//! Protocol v1.
//!
//! The client opens one bidirectional QUIC stream right after connecting (ALPN `warp-remote/1`) and
//! sends requests on it; the host answers each one in order. Every message is a frame: a 4-byte
//! big-endian length followed by that many bytes of UTF-8 JSON (at most [`MAX_FRAME`]). The JSON
//! object's `type` field says which message it is.
//!
//! ```text
//! client                                    host
//! {"type":"hello","version":1,"name":"iPhone"}
//!                                           {"type":"hello","version":1,"host":"DESKTOP","paired":true}
//! {"type":"sessions"}
//!                                           {"type":"sessions","sessions":[{...}, ...]}
//! {"type":"ping","id":7}
//!                                           {"type":"pong","id":7}
//! ```
//!
//! `hello` always comes first. A device that isn't paired gets `"paired":false` only while a
//! pairing ticket is active, and must then send `{"type":"pair","secret":"<hex>"}` with the secret
//! from the ticket; anything else closes the connection. With no ticket active, an unknown device
//! is disconnected before any stream is read (QUIC close code 1, reason `not_paired`).
//!
//! Errors come back as `{"type":"error","code":...,"message":...}`; codes are in [`code`].
//! Fields added later are optional, so older apps and hosts keep understanding each other within v1.
//!
//! Version 2 (same ALPN; `hello` answers the lower of both versions, and a v2 app only sends these to a v2 host)
//! adds a pane's content and typing into it. Panes are addressed by `window` and `pane_id` (see [`Session`]):
//!
//! ```text
//! {"type":"watch","window":..,"pane_id":3,"since":0}
//!                                           {"type":"pane","rev":41,"kind":"chat","items":[...],"prompt":{...},...}
//! {"type":"watch","window":..,"pane_id":3,"since":41}     (about once a second while it's on screen: only what changed)
//! {"type":"input","window":..,"pane_id":3,"text":"go on","keys":["enter"]}
//!                                           {"type":"ok"}
//! {"type":"unwatch","window":..,"pane_id":3}
//! ```
//!
//! The first `watch` of a pane subscribes to it: only then does the host read its screen and follow its Claude Code or
//! Codex conversation (a file on this computer, or over the user's own `ssh` for a pane running `ssh host`), and it
//! stops on `unwatch`, when the connection ends or when nobody asked for a minute. `input` needs the device to be
//! allowed to type (Warp: Paired devices → the device → Allow typing; error `typing_off` otherwise).
//!
//! Version 3 adds a subagent's own conversation, read-only: `watch` and `unwatch` with `agent` (an [`AgentInfo`]'s
//! `id`, from the pane's `agents` or an item's `agent`) follow that subagent of the pane's Claude Code. A v2 host would
//! ignore `agent` and answer with the pane itself, so an app only sends it to a v3 host.

use iroh::endpoint::{ReadExactError, RecvStream, SendStream};
use serde::{Deserialize, Serialize, de::DeserializeOwned};

use crate::{Result, err};

/// Protocol version this build speaks.
pub const VERSION: u32 = 3;
/// Largest frame accepted (the host is reachable from the internet: never trust a length).
pub const MAX_FRAME: usize = 1 << 20;

#[derive(Debug, Clone, Serialize, Deserialize, PartialEq)]
#[serde(tag = "type", rename_all = "snake_case")]
pub enum Request {
    /// First message of every connection. `name` is the device's name, shown in Warp's list.
    Hello { version: u32, name: String },
    /// Pairing: the secret of the ticket (hex), right after `hello` answered `paired: false`.
    Pair { secret: String },
    /// Every tab and pane of every window of this Warp.
    Sessions,
    /// Answered at once with `pong` and the same id (round-trip time).
    Ping { id: u64 },
    /// v2. What that pane shows (a [`PaneView`]); subscribes to it. `since`: the `rev` of the last answer (0: all of
    /// it). `before`: instead, the chat items older than that item id (pulling for older ones).
    Watch {
        window: u64,
        pane_id: u32,
        #[serde(default)]
        since: u64,
        #[serde(default, skip_serializing_if = "Option::is_none")]
        before: Option<u64>,
        /// v3. That subagent of the pane's Claude Code instead (read-only: no screen, no questions, no typing).
        #[serde(default, skip_serializing_if = "String::is_empty")]
        agent: String,
    },
    /// v2. Stop following that pane (v3: or that subagent of it).
    Unwatch {
        window: u64,
        pane_id: u32,
        #[serde(default, skip_serializing_if = "String::is_empty")]
        agent: String,
    },
    /// v2. Typing into that pane, as if on the computer: `text` is pasted (one line), then `keys` are pressed (see
    /// [`KEYS`]). `option`: choose that option (0-based) of the question the pane shows, if it is still the one
    /// with that `prompt` id.
    Input {
        window: u64,
        pane_id: u32,
        #[serde(default, skip_serializing_if = "String::is_empty")]
        text: String,
        #[serde(default, skip_serializing_if = "Vec::is_empty")]
        keys: Vec<String>,
        #[serde(default, skip_serializing_if = "Option::is_none")]
        option: Option<u32>,
        #[serde(default, skip_serializing_if = "String::is_empty")]
        prompt: String,
    },
}

/// Keys a phone may press in a pane.
pub const KEYS: [&str; 9] = ["enter", "esc", "up", "down", "left", "right", "tab", "backspace", "ctrl_c"];
/// Longest text a phone may type at once (characters).
pub const MAX_TEXT: usize = 4000;

#[derive(Debug, Clone, Serialize, Deserialize, PartialEq)]
#[serde(tag = "type", rename_all = "snake_case")]
pub enum Response {
    Hello { version: u32, host: String, paired: bool },
    Paired,
    Sessions { sessions: Vec<Session> },
    Pong { id: u64 },
    Error { code: String, message: String },
    /// v2: the answer to `watch`.
    Pane(Box<PaneView>),
    /// v2: done (`unwatch`, `input`).
    Ok,
}

/// What a pane shows, for the phone. A pane running Claude Code or Codex whose conversation was found is a chat
/// (`kind` "chat", from its transcript); any other is its screen as text (`kind` "screen").
#[derive(Debug, Clone, Default, Serialize, Deserialize, PartialEq)]
pub struct PaneView {
    pub window: u64,
    pub pane_id: u32,
    /// Ask again with `since` = this to get only what changed.
    pub rev: u64,
    pub kind: String,
    /// "claude", "codex" or "".
    #[serde(default)]
    pub agent: String,
    /// "working", "waiting" (a question is waiting for an answer), "finished" (the agent is done, your turn),
    /// "running" (some other program), "idle" (at the shell's prompt).
    #[serde(default)]
    pub status: String,
    /// The conversation's title, when the agent gave it one.
    #[serde(default, skip_serializing_if = "String::is_empty")]
    pub title: String,
    /// Chat items new or changed since `since` (oldest first); all the recent ones when `reset`.
    #[serde(default)]
    pub items: Vec<ChatItem>,
    /// Drop what you had for this pane (first answer, another conversation, or the kind changed).
    #[serde(default)]
    pub reset: bool,
    /// Older items exist (ask with `before`).
    #[serde(default)]
    pub more: bool,
    /// The screen's text, one line per row (scrollback first), when it changed since `since`.
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub screen: Option<Vec<String>>,
    /// The question the pane is asking now, if any.
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub prompt: Option<Prompt>,
    /// This device may type here.
    #[serde(default)]
    pub typing: bool,
    /// Where the conversation comes from ("this computer", "ssh fr"), or why there is no chat.
    #[serde(default, skip_serializing_if = "String::is_empty")]
    pub note: String,
    /// The subagents of this conversation (running ones first), when they changed since `since` (absent: unchanged).
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub agents: Option<Vec<AgentInfo>>,
    /// When the turn going on (or the last one) started: the last message you wrote (Unix ms; 0: unknown).
    #[serde(default, skip_serializing_if = "is_zero64")]
    pub turn_start: u64,
    /// What the agent says it is doing now («Baking… (3m 12s · ↓ 4.2k tokens)», «Waiting for 3 background agents to
    /// finish»), or the tool call that is running.
    #[serde(default, skip_serializing_if = "String::is_empty")]
    pub activity: String,
}

/// A subagent (Claude Code's Agent tool: a foreground or background agent with its own conversation).
#[derive(Debug, Clone, Default, Serialize, Deserialize, PartialEq)]
pub struct AgentInfo {
    /// Its id (`watch` with `agent`); empty until it is known (the launch's answer says it).
    #[serde(default)]
    pub id: String,
    /// Its type: "general-purpose", "Explore", "executor"…
    pub name: String,
    #[serde(default)]
    pub description: String,
    /// "running", "done" or "failed".
    pub status: String,
    /// Started (or resumed), and when it last stopped (Unix ms; 0: unknown).
    #[serde(default, skip_serializing_if = "is_zero64")]
    pub started: u64,
    #[serde(default, skip_serializing_if = "is_zero64")]
    pub ended: u64,
    /// What its last run took, as it reported when it finished (0: unknown).
    #[serde(default, skip_serializing_if = "is_zero64")]
    pub duration_ms: u64,
    #[serde(default, skip_serializing_if = "is_zero64")]
    pub tokens: u64,
    #[serde(default, skip_serializing_if = "is_zero64")]
    pub tool_uses: u64,
    #[serde(default, skip_serializing_if = "std::ops::Not::not")]
    pub background: bool,
}

/// One message of a conversation.
#[derive(Debug, Clone, Default, Serialize, Deserialize, PartialEq)]
pub struct ChatItem {
    /// Stable for this item while the pane is watched (an item that changes comes again with the same id).
    pub id: u64,
    /// "user", "assistant" (markdown), "tool" (one line, e.g. "Edited main.cpp", with `detail`) or "note".
    pub role: String,
    pub text: String,
    #[serde(default, skip_serializing_if = "String::is_empty")]
    pub detail: String,
    /// A tool call that failed.
    #[serde(default, skip_serializing_if = "std::ops::Not::not")]
    pub failed: bool,
    /// A tool call still running (or waiting for permission).
    #[serde(default, skip_serializing_if = "std::ops::Not::not")]
    pub pending: bool,
    /// When it was written (Unix ms; 0: unknown).
    #[serde(default, skip_serializing_if = "is_zero64")]
    pub ts: u64,
    /// v3, a tool call: which tool ("Bash", "Edit", "Agent", "AskUserQuestion", "ExitPlanMode", Codex's "shell"…).
    #[serde(default, skip_serializing_if = "String::is_empty")]
    pub tool: String,
    /// v3, a tool call: what it came to, short («+12 −3», «exit 1», «120 lines», «green»).
    #[serde(default, skip_serializing_if = "String::is_empty")]
    pub badge: String,
    /// v3: the subagent an Agent call started (or a message about it is from).
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub agent: Option<AgentInfo>,
    /// v3, an AskUserQuestion call: the options of its (first) question, with their descriptions.
    #[serde(default, skip_serializing_if = "Vec::is_empty")]
    pub options: Vec<PromptOption>,
}

/// A question with options on the pane's screen (Claude Code's permission prompts and menus, Codex's approvals).
#[derive(Debug, Clone, Default, Serialize, Deserialize, PartialEq)]
pub struct Prompt {
    /// Changes when the question or its options change; `input` with `option` must carry it.
    pub id: String,
    pub question: String,
    /// What it is about (the command, the file), a few lines.
    #[serde(default, skip_serializing_if = "Vec::is_empty")]
    pub context: Vec<String>,
    pub options: Vec<PromptOption>,
    /// The option selected now (0-based).
    #[serde(default)]
    pub selected: u32,
}

#[derive(Debug, Clone, Default, Serialize, Deserialize, PartialEq)]
pub struct PromptOption {
    pub label: String,
    /// More text under the label (its description, or the rest of a long label).
    #[serde(default, skip_serializing_if = "String::is_empty")]
    pub hint: String,
}

/// One terminal pane of a Warp window.
#[derive(Debug, Clone, Default, Serialize, Deserialize, PartialEq)]
pub struct Session {
    /// Which Warp window it is in (stable while that window is open).
    pub window: u64,
    /// Tab index in its window, and pane index in its tab.
    pub tab: u32,
    pub pane: u32,
    /// The tab's name, as shown in its window.
    pub tab_name: String,
    /// The pane's title: the Claude Code conversation, what is running, or the shell.
    pub title: String,
    pub cwd: String,
    /// Something is running in it (not at the prompt).
    pub busy: bool,
    #[serde(default, skip_serializing_if = "String::is_empty")]
    pub running: String,
    /// "claude" or "codex" when one of them runs there.
    #[serde(default, skip_serializing_if = "String::is_empty")]
    pub agent: String,
    /// The pane's id in its window, for `watch` and `input` (stable while it is open; 0 from an older Warp).
    #[serde(default, skip_serializing_if = "is_zero")]
    pub pane_id: u32,
    /// Finishes and questions not seen yet on the computer (Warp's unread badge; 0 from an older Warp).
    #[serde(default, skip_serializing_if = "is_zero")]
    pub unread: u32,
    /// An agent's pane: "working", "waiting" (a question waits for an answer) or "finished", from its screen.
    #[serde(default, skip_serializing_if = "String::is_empty")]
    pub status: String,
}

fn is_zero(n: &u32) -> bool {
    *n == 0
}

fn is_zero64(n: &u64) -> bool {
    *n == 0
}

/// Error codes of [`Response::Error`] (and QUIC close reasons).
pub mod code {
    pub const NOT_PAIRED: &str = "not_paired";
    pub const BAD_SECRET: &str = "bad_secret";
    pub const TICKET_EXPIRED: &str = "ticket_expired";
    pub const UNSUPPORTED_VERSION: &str = "unsupported_version";
    pub const BAD_REQUEST: &str = "bad_request";
    pub const UNAVAILABLE: &str = "unavailable";
    pub const REVOKED: &str = "revoked";
    /// v2: this device may not type here (Warp: Paired devices → the device → Allow typing).
    pub const TYPING_OFF: &str = "typing_off";
    /// v2: the pane isn't there any more.
    pub const NO_PANE: &str = "no_pane";
    /// v2: the question answered is no longer on the screen.
    pub const PROMPT_GONE: &str = "prompt_gone";
}

/// QUIC close codes used by the host.
pub const CLOSE_NOT_PAIRED: u32 = 1;
pub const CLOSE_REVOKED: u32 = 2;
pub const CLOSE_PROTOCOL: u32 = 3;
pub const CLOSE_BYE: u32 = 0;

pub fn encode<T: Serialize>(msg: &T) -> Result<Vec<u8>> {
    let body = serde_json::to_vec(msg)?;
    if body.len() > MAX_FRAME {
        return Err(err("frame too large"));
    }
    let mut buf = Vec::with_capacity(4 + body.len());
    buf.extend_from_slice(&(body.len() as u32).to_be_bytes());
    buf.extend_from_slice(&body);
    Ok(buf)
}

pub async fn write_frame<T: Serialize>(send: &mut SendStream, msg: &T) -> Result<()> {
    send.write_all(&encode(msg)?).await?;
    Ok(())
}

/// The next frame, or `None` when the other side finished the stream cleanly between frames.
pub async fn read_frame<T: DeserializeOwned>(recv: &mut RecvStream) -> Result<Option<T>> {
    let mut len = [0u8; 4];
    match recv.read_exact(&mut len).await {
        Ok(()) => {}
        Err(ReadExactError::FinishedEarly(0)) => return Ok(None),
        Err(e) => return Err(e.into()),
    }
    let n = u32::from_be_bytes(len) as usize;
    if n > MAX_FRAME {
        return Err(err(format!("frame of {n} bytes is too large")));
    }
    let mut body = vec![0u8; n];
    recv.read_exact(&mut body).await?;
    Ok(Some(serde_json::from_slice(&body)?))
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn wire_format() {
        let b = encode(&Request::Ping { id: 7 }).unwrap();
        assert_eq!(&b[..4], &(b.len() as u32 - 4).to_be_bytes());
        assert_eq!(&b[4..], br#"{"type":"ping","id":7}"#);
        let r: Response = serde_json::from_str(r#"{"type":"hello","version":1,"host":"x","paired":false}"#).unwrap();
        assert_eq!(r, Response::Hello { version: 1, host: "x".into(), paired: false });
        // Fields a newer host adds are ignored; optional ones may be missing.
        let s: Session = serde_json::from_str(
            r#"{"window":1,"tab":0,"pane":0,"tab_name":"a","title":"b","cwd":"c","busy":false,"future":1}"#,
        )
        .unwrap();
        assert_eq!(s.agent, "");
        assert_eq!(s.pane_id, 0);
    }

    #[test]
    fn v2_messages() {
        // A v1 session list (no pane_id) still reads, and pane_id 0 is left out.
        let s = Session { window: 1, tab: 0, pane: 0, ..Default::default() };
        assert!(!serde_json::to_string(&s).unwrap().contains("pane_id"));
        let w: Request = serde_json::from_str(r#"{"type":"watch","window":5,"pane_id":3}"#).unwrap();
        assert_eq!(w, Request::Watch { window: 5, pane_id: 3, since: 0, before: None, agent: String::new() });
        // v3: a subagent; left out when empty, so a v2 host reads what it always did.
        let sub = Request::Watch { window: 5, pane_id: 3, since: 0, before: None, agent: "a6d5e25a0889e3ee4".into() };
        assert!(serde_json::to_string(&sub).unwrap().contains(r#""agent":"a6d5e25a0889e3ee4""#));
        assert_eq!(&encode(&Request::Unwatch { window: 5, pane_id: 3, agent: String::new() }).unwrap()[4..], br#"{"type":"unwatch","window":5,"pane_id":3}"#);
        let i: Request = serde_json::from_str(r#"{"type":"input","window":5,"pane_id":3,"keys":["enter"]}"#).unwrap();
        assert!(matches!(i, Request::Input { ref text, ref keys, option: None, .. } if text.is_empty() && keys == &["enter"]));
        let ok = encode(&Response::Ok).unwrap();
        assert_eq!(&ok[4..], br#"{"type":"ok"}"#);
        let p: Response = serde_json::from_str(r#"{"type":"pane","window":5,"pane_id":3,"rev":2,"kind":"screen"}"#).unwrap();
        assert!(matches!(p, Response::Pane(ref v) if v.rev == 2 && v.items.is_empty() && v.prompt.is_none()));
    }
}
