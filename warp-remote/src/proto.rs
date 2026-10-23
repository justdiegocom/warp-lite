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

use iroh::endpoint::{ReadExactError, RecvStream, SendStream};
use serde::{Deserialize, Serialize, de::DeserializeOwned};

use crate::{Result, err};

/// Protocol version this build speaks.
pub const VERSION: u32 = 1;
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
}

#[derive(Debug, Clone, Serialize, Deserialize, PartialEq)]
#[serde(tag = "type", rename_all = "snake_case")]
pub enum Response {
    Hello { version: u32, host: String, paired: bool },
    Paired,
    Sessions { sessions: Vec<Session> },
    Pong { id: u64 },
    Error { code: String, message: String },
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
    }
}
