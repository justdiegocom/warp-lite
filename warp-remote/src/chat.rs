//! A conversation as the phone shows it, from the transcript Claude Code or Codex writes as it goes (JSON lines):
//! what you wrote, the agent's answers (markdown) and its tool calls as one line each («Ran cargo test», «Edited
//! main.rs») with what they did as detail and a short result («exit 1», «+12 −3»), and its subagents.
//!
//! - Claude Code: `~/.claude/projects/<folder>/<session>.jsonl`. Records `user` (a string you typed, tool results with
//!   `toolUseResult`, task notifications), `assistant` (text, tool_use and thinking blocks, one record per block),
//!   `ai-title`; meta records and thinking are left out, and so are a subagent's records (`isSidechain`) except in that
//!   subagent's own conversation, `<session>/subagents/agent-<id>.jsonl` (see [`Chat::subagent`]).
//! - Subagents: an `Agent` (or `Task`) call starts one; its result says its id (and, for one in the foreground, what it
//!   took); a background one reports in a `<task-notification>` each time it stops; `SendMessage` resumes one. What the
//!   host lists of `<session>/subagents` ([`Listed`]: running or not, from the end of its file) fills in the ones that
//!   started before the part of the transcript that was read.
//! - Codex: `~/.codex/sessions/YYYY/MM/DD/rollout-*.jsonl`. `event_msg` `user_message` / `agent_message`, and
//!   `response_item` `function_call` / `custom_tool_call` with their `*_output`.
//!
//! Lines that don't parse (the cut first line of a tail, records of a newer version) are skipped. Nothing is logged.

use std::collections::HashMap;

use serde_json::Value;

use crate::proto::{AgentInfo, ChatItem, PromptOption};

/// Longest text kept for a message, for the detail of a tool call, and for a subagent's report (characters).
const MAX_TEXT: usize = 12_000;
const MAX_DETAIL: usize = 1_500;
const MAX_REPORT: usize = 4_000;
/// Items kept per conversation (the oldest are dropped).
const MAX_ITEMS: usize = 1_000;
/// Subagents kept per conversation (the ones that stopped longest ago are dropped).
const MAX_AGENTS: usize = 40;

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum Agent {
    Claude,
    Codex,
}

impl Agent {
    pub fn name(self) -> &'static str {
        match self {
            Agent::Claude => "claude",
            Agent::Codex => "codex",
        }
    }
}

#[derive(Debug, Clone)]
struct Entry {
    item: ChatItem,
    rev: u64,
}

/// A tool call waiting for its result: its item and its tool.
struct Call {
    item: u64,
    tool: String,
}

/// A subagent: what the phone sees of it, the Agent call that started it, and the items that show it.
struct Sub {
    info: AgentInfo,
    call: String,
    items: Vec<u64>,
}

/// One subagent as the host found it in `<session>/subagents`: its id, whether it is still working (its file doesn't end
/// with its final answer), when it started (its first record) and last wrote (Unix ms; 0: unknown), and its
/// `agent-<id>.meta.json` (`agentType`, `description`, `toolUseId`).
#[derive(Debug, Clone, Default, PartialEq)]
pub struct Listed {
    pub id: String,
    pub running: bool,
    pub started: u64,
    pub modified: u64,
    pub meta: Value,
}

pub struct Chat {
    agent: Agent,
    /// A subagent's own conversation: its records are all sidechain ones.
    sidechain: bool,
    entries: Vec<Entry>,
    /// Tool call id → its item and tool (to attach the result).
    calls: HashMap<String, Call>,
    next_id: u64,
    pub title: String,
    /// When the last message you wrote was sent (Unix ms; 0: unknown): the turn going on started then.
    pub turn_start: u64,
    subs: Vec<Sub>,
    /// Revision of the last change to [`Chat::agents`].
    pub agents_rev: u64,
}

fn cut(s: &str, max: usize) -> String {
    match s.char_indices().nth(max) {
        Some((i, _)) => format!("{}…", &s[..i]),
        None => s.to_string(),
    }
}

fn first_line(s: &str, max: usize) -> String {
    let l = s.trim().lines().next().unwrap_or("");
    let more = s.trim().lines().nth(1).is_some();
    let c = cut(l, max);
    if more && !c.ends_with('…') { format!("{c} …") } else { c }
}

fn base(path: &str) -> String {
    path.rsplit(['/', '\\']).next().unwrap_or(path).to_string()
}

fn s<'a>(v: &'a Value, k: &str) -> &'a str {
    v.get(k).and_then(Value::as_str).unwrap_or("")
}

fn n(v: &Value, k: &str) -> u64 {
    v.get(k).and_then(Value::as_u64).unwrap_or(0)
}

/// A flag that may come as `true` or `"true"`.
fn flag(v: &Value) -> bool {
    v.as_bool() == Some(true) || v.as_str() == Some("true")
}

/// The text of a tool result (a string, or blocks of text).
fn result_text(v: &Value) -> String {
    match v {
        Value::String(t) => t.clone(),
        Value::Array(a) => a.iter().filter_map(|b| b.get("text").and_then(Value::as_str)).collect::<Vec<_>>().join("\n"),
        Value::Object(_) => v.get("output").map(result_text).unwrap_or_default(),
        _ => String::new(),
    }
}

/// An RFC 3339 time in UTC as the transcripts write it («2026-10-23T20:44:59.816Z») → Unix ms (0 if it isn't one).
pub fn parse_ts(t: &str) -> u64 {
    let b = t.as_bytes();
    if b.len() < 20 || b[4] != b'-' || b[7] != b'-' || b[10] != b'T' || b[13] != b':' || b[16] != b':' {
        return 0;
    }
    let num = |r: std::ops::Range<usize>| t.get(r).and_then(|x| x.parse::<i64>().ok());
    let (Some(y), Some(mo), Some(d), Some(h), Some(mi), Some(sec)) = (num(0..4), num(5..7), num(8..10), num(11..13), num(14..16), num(17..19)) else {
        return 0;
    };
    let mut ms = 0;
    if b[19] == b'.' {
        let frac: String = t[20..].chars().take_while(|c| c.is_ascii_digit()).collect();
        ms = format!("{:0<3}", &frac[..frac.len().min(3)]).parse().unwrap_or(0);
    }
    // Days from 1970-01-01 (Howard Hinnant's days_from_civil).
    let y = if mo <= 2 { y - 1 } else { y };
    let era = y.div_euclid(400);
    let yoe = y - era * 400;
    let doy = (153 * (mo + if mo > 2 { -3 } else { 9 }) + 2) / 5 + d - 1;
    let doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    let days = era * 146_097 + doe - 719_468;
    let secs = days * 86_400 + h * 3_600 + mi * 60 + sec;
    if secs < 0 { 0 } else { secs as u64 * 1000 + ms }
}

/// The text between `<tag>` and `</tag>`.
fn tag<'a>(t: &'a str, name: &str) -> &'a str {
    let open = format!("<{name}>");
    let close = format!("</{name}>");
    t.split_once(&open).and_then(|(_, r)| r.split_once(&close)).map(|(x, _)| x.trim()).unwrap_or("")
}

/// Lines added and removed, from an Edit's or a Write's `structuredPatch` («+12 −3»).
fn patch_badge(patch: &Value) -> Option<String> {
    let hunks = patch.as_array()?;
    let (mut add, mut del) = (0, 0);
    for l in hunks.iter().flat_map(|h| h["lines"].as_array().into_iter().flatten()).filter_map(Value::as_str) {
        if l.starts_with('+') {
            add += 1;
        } else if l.starts_with('-') {
            del += 1;
        }
    }
    Some(format!("+{add} −{del}"))
}

impl Chat {
    pub fn new(agent: Agent) -> Chat {
        Chat {
            agent,
            sidechain: false,
            entries: Vec::new(),
            calls: HashMap::new(),
            next_id: 1,
            title: String::new(),
            turn_start: 0,
            subs: Vec::new(),
            agents_rev: 0,
        }
    }

    /// A subagent's own conversation (`agent-<id>.jsonl`): its task comes first, as the message it was given.
    pub fn subagent() -> Chat {
        Chat { sidechain: true, ..Chat::new(Agent::Claude) }
    }

    pub fn agent(&self) -> Agent {
        self.agent
    }

    pub fn len(&self) -> usize {
        self.entries.len()
    }

    pub fn is_empty(&self) -> bool {
        self.entries.is_empty()
    }

    fn push(&mut self, role: &str, text: String, detail: String, ts: u64, rev: u64) -> u64 {
        let id = self.next_id;
        self.next_id += 1;
        self.entries.push(Entry { item: ChatItem { id, role: role.into(), text, detail, ts, ..Default::default() }, rev });
        if self.entries.len() > MAX_ITEMS {
            self.entries.drain(..self.entries.len() - MAX_ITEMS);
        }
        id
    }

    fn update(&mut self, id: u64, rev: u64, f: impl FnOnce(&mut ChatItem)) {
        if let Some(e) = self.entries.iter_mut().rev().find(|e| e.item.id == id) {
            f(&mut e.item);
            e.rev = rev;
        }
    }

    /// The tool call still running, the latest one (its line).
    pub fn pending_tool(&self) -> Option<&str> {
        self.entries.iter().rev().take(200).find(|e| e.item.pending).map(|e| e.item.text.as_str())
    }

    /// The subagents: running ones first (the latest started first), then the ones that stopped (latest first).
    pub fn agents(&self) -> Vec<AgentInfo> {
        let mut out: Vec<AgentInfo> = self.subs.iter().map(|s| s.info.clone()).collect();
        out.sort_by_key(|a| (a.status != "running", std::cmp::Reverse(if a.status == "running" { a.started } else { a.ended.max(a.started) })));
        out
    }

    /// The subagent with that id, if this conversation knows it.
    pub fn agent_info(&self, id: &str) -> Option<&AgentInfo> {
        self.subs.iter().find(|s| s.info.id == id).map(|s| &s.info)
    }

    /// One line of the transcript. `rev`: the revision items changed by it get.
    pub fn feed(&mut self, line: &[u8], rev: u64) {
        let Ok(v) = serde_json::from_slice::<Value>(line) else { return };
        match self.agent {
            Agent::Claude => self.claude(&v, rev),
            Agent::Codex => self.codex(&v, rev),
        }
    }

    fn user_text(&mut self, t: &str, ts: u64, rev: u64) {
        let t = t.trim();
        if t.is_empty() {
            return;
        }
        if t.starts_with("[Request interrupted") {
            self.push("note", "Interrupted".into(), String::new(), ts, rev);
            return;
        }
        if t.starts_with("<task-notification>") {
            self.notification(t, ts, rev);
            return;
        }
        if let Some(rest) = t.strip_prefix("<command-name>") {
            // A slash command: «/model», with its arguments.
            let name = rest.split("</command-name>").next().unwrap_or("").trim();
            let args = tag(t, "command-args");
            if !name.is_empty() {
                let text = if args.is_empty() { name.to_string() } else { format!("{name} {args}") };
                self.push("user", cut(&text, 400), String::new(), ts, rev);
                self.turn_start = ts;
            }
            return;
        }
        if t.starts_with("<bash-input>") {
            // A shell command typed with «!».
            self.push("user", format!("! {}", cut(tag(t, "bash-input"), 400)), String::new(), ts, rev);
            return;
        }
        if t.starts_with('<') {
            return;  // command output, caveats, reminders: not something you wrote
        }
        self.push("user", cut(t, MAX_TEXT), String::new(), ts, rev);
        if ts != 0 {
            self.turn_start = ts;
        }
    }

    fn tool(&mut self, call: &str, tool: &str, text: String, detail: String, ts: u64, rev: u64) -> u64 {
        let id = self.push("tool", text, detail, ts, rev);
        self.update(id, rev, |i| {
            i.pending = true;
            i.tool = tool.to_string();
        });
        if !call.is_empty() {
            self.calls.insert(call.to_string(), Call { item: id, tool: tool.to_string() });
        }
        id
    }

    fn result(&mut self, call: &str, out: &str, failed: bool, extra: &Value, ts: u64, rev: u64) {
        let Some(c) = self.calls.remove(call) else { return };
        let out = out.trim();
        let badge = match c.tool.as_str() {
            "Bash" if failed => out.strip_prefix("Exit code ").and_then(|r| r.split_whitespace().next()).map(|code| format!("exit {code}")),
            "Bash" if flag(&extra["interrupted"]) => Some("interrupted".into()),
            "Edit" | "MultiEdit" | "Write" if !failed => patch_badge(&extra["structuredPatch"]).filter(|_| s(extra, "type") != "create"),
            "Read" => Some(n(&extra["file"], "numLines")).filter(|&l| l > 0).map(|l| format!("{l} lines")),
            "Grep" | "Glob" => Some(n(extra, "numFiles")).filter(|&f| f > 0).map(|f| format!("{f} file{}", if f == 1 { "" } else { "s" })),
            "AskUserQuestion" => extra["answers"].as_object().map(|a| cut(&a.values().filter_map(Value::as_str).collect::<Vec<_>>().join(", "), 60)),
            "ExitPlanMode" => Some(if failed { "not approved" } else { "approved" }.into()),
            _ => None,
        };
        self.update(c.item, rev, |i| {
            i.pending = false;
            i.failed = failed;
            if let Some(b) = badge.filter(|b| !b.is_empty()) {
                i.badge = b;
            }
            // An agent's launch, an answered question, an approved plan: what the tool says back says nothing new.
            if !out.is_empty() && !matches!(c.tool.as_str(), "Agent" | "Task" | "AskUserQuestion" | "ExitPlanMode") {
                let room = MAX_DETAIL.saturating_sub(i.detail.chars().count()).max(300);
                if !i.detail.is_empty() {
                    i.detail.push_str("\n\n");
                }
                i.detail.push_str(&cut(out, room));
            }
        });
        match c.tool.as_str() {
            "Agent" | "Task" => {
                // Background: «async_launched» with its id; foreground: what it took, and its report as the detail.
                let id = s(extra, "agentId").to_string();
                let done = s(extra, "status") == "completed";
                let report = if done { out.to_string() } else { String::new() };
                self.change_sub(|sub| sub.call == call, rev, |a| {
                    if !id.is_empty() {
                        a.id = id.clone();
                    }
                    if failed {
                        a.status = "failed".into();
                        a.ended = ts;
                    } else if done {
                        a.status = "done".into();
                        a.ended = ts;
                        a.duration_ms = n(extra, "totalDurationMs");
                        a.tokens = n(extra, "totalTokens");
                        a.tool_uses = n(extra, "totalToolUseCount");
                    }
                });
                if !report.is_empty() {
                    self.update(c.item, rev, |i| i.detail = cut(&report, MAX_REPORT));
                }
                // A subagent the host listed before its call was read: the same one.
                self.merge_duplicate(&id, rev);
            }
            "SendMessage" => {
                let id = s(extra, "resumedAgentId");
                if !id.is_empty() {
                    self.change_sub(|sub| sub.info.id == id, rev, |a| {
                        if a.status != "running" {
                            a.started = ts;
                        }
                        a.status = "running".into();
                    });
                }
            }
            _ => {}
        }
    }

    /// Changes the subagent that `which` finds (nothing if none) and the items that show it.
    fn change_sub(&mut self, which: impl Fn(&Sub) -> bool, rev: u64, f: impl FnOnce(&mut AgentInfo)) {
        let Some(k) = self.subs.iter().position(which) else { return };
        let before = self.subs[k].info.clone();
        f(&mut self.subs[k].info);
        if self.subs[k].info == before {
            return;
        }
        self.agents_rev = rev;
        let (info, items) = (self.subs[k].info.clone(), self.subs[k].items.clone());
        for id in items {
            self.update(id, rev, |i| i.agent = Some(info.clone()));
        }
    }

    /// Two entries for one subagent (one from its call, one from the host's list or a notification): kept as one.
    fn merge_duplicate(&mut self, id: &str, rev: u64) {
        if id.is_empty() {
            return;
        }
        let same: Vec<usize> = (0..self.subs.len()).filter(|&k| self.subs[k].info.id == id).collect();
        if same.len() < 2 {
            return;
        }
        let keep = same[0];
        for &k in same[1..].iter().rev() {
            let other = self.subs.remove(k);
            let s = &mut self.subs[keep];
            s.items.extend(other.items);
            if s.call.is_empty() {
                s.call = other.call;
            }
            let (a, b) = (&mut s.info, other.info);
            if a.name.is_empty() || a.name == "agent" {
                a.name = b.name;
            }
            if a.description.is_empty() {
                a.description = b.description;
            }
            if a.started == 0 || (b.started != 0 && b.started < a.started) {
                a.started = b.started;
            }
            a.ended = a.ended.max(b.ended);
            a.duration_ms = a.duration_ms.max(b.duration_ms);
            a.tokens = a.tokens.max(b.tokens);
            a.tool_uses = a.tool_uses.max(b.tool_uses);
            a.background |= b.background;
        }
        self.agents_rev = rev;
        let (info, items) = (self.subs[keep].info.clone(), self.subs[keep].items.clone());
        for i in items {
            self.update(i, rev, |it| it.agent = Some(info.clone()));
        }
    }

    fn new_sub(&mut self, info: AgentInfo, call: &str, rev: u64) -> usize {
        self.subs.push(Sub { info, call: call.to_string(), items: Vec::new() });
        if self.subs.len() > MAX_AGENTS {
            // The one that stopped longest ago goes.
            if let Some(k) = (0..self.subs.len()).filter(|&k| self.subs[k].info.status != "running").min_by_key(|&k| self.subs[k].info.ended.max(self.subs[k].info.started)) {
                self.subs.remove(k);
            }
        }
        self.agents_rev = rev;
        self.subs.len() - 1
    }

    /// A background task stopped: a subagent's report (its card is updated, and the report comes as a new item), or a
    /// background command that ended (a note).
    fn notification(&mut self, t: &str, ts: u64, rev: u64) {
        let (task, call, status, summary) = (tag(t, "task-id"), tag(t, "tool-use-id"), tag(t, "status"), tag(t, "summary"));
        let usage = tag(t, "usage");
        let num = |k: &str| tag(usage, k).parse::<u64>().unwrap_or(0);
        let known = self.subs.iter().position(|s| (!task.is_empty() && s.info.id == task) || (!call.is_empty() && s.call == call));
        let is_agent = known.is_some() || summary.starts_with("Agent ");
        if !is_agent {
            if !summary.is_empty() {
                self.push("note", cut(summary, 300), String::new(), ts, rev);
            }
            return;
        }
        let k = match known {
            Some(k) => k,
            None => {
                // Started before the part of the transcript that was read: what the summary says of it.
                let desc = summary.split('"').nth(1).unwrap_or("").to_string();
                let info = AgentInfo { id: task.to_string(), name: "agent".into(), description: desc, status: "running".into(), background: true, ..Default::default() };
                self.new_sub(info, call, rev)
            }
        };
        let ok = status == "completed";
        let tokens = num("subagent_tokens").max(num("total_tokens"));
        let (dur, tools) = (num("duration_ms"), num("tool_uses"));
        let id = self.subs[k].info.id.clone();
        let sub_call = self.subs[k].call.clone();
        let task_id = task.to_string();
        self.change_sub(
            |s| (!id.is_empty() && s.info.id == id) || (!sub_call.is_empty() && s.call == sub_call),
            rev,
            |a| {
                if a.id.is_empty() {
                    a.id = task_id;
                }
                a.status = if ok { "done" } else { "failed" }.into();
                a.ended = ts;
                if dur > 0 {
                    a.duration_ms = dur;
                }
                if tokens > 0 {
                    a.tokens = tokens;
                }
                if tools > 0 {
                    a.tool_uses = tools;
                }
            },
        );
        let k = self.subs.iter().position(|s| (!id.is_empty() && s.info.id == id) || (!sub_call.is_empty() && s.call == sub_call)).unwrap_or(k);
        let info = self.subs[k].info.clone();
        let text = if summary.is_empty() { format!("Agent “{}” {}", info.description, if ok { "finished" } else { "stopped" }) } else { cut(summary, 200) };
        let item = self.push("tool", text, cut(tag(t, "result"), MAX_REPORT), ts, rev);
        self.update(item, rev, |i| {
            i.tool = "Agent".into();
            i.failed = !ok;
            i.agent = Some(info);
        });
        self.subs[k].items.push(item);
    }

    /// What the host found in `<session>/subagents`: subagents that started before the part of the transcript that was
    /// read get an entry, and whether each one is still working comes from its own file.
    pub fn merge_listed(&mut self, list: &[Listed], rev: u64) {
        for l in list {
            let call = s(&l.meta, "toolUseId").to_string();
            let k = match self.subs.iter().position(|s| s.info.id == l.id).or_else(|| self.subs.iter().position(|s| !call.is_empty() && s.call == call)) {
                Some(k) => k,
                None => {
                    let info = AgentInfo {
                        id: l.id.clone(),
                        name: s(&l.meta, "agentType").to_string(),
                        description: s(&l.meta, "description").to_string(),
                        status: if l.running { "running" } else { "done" }.into(),
                        started: l.started,
                        ended: if l.running { 0 } else { l.modified },
                        background: s(&l.meta, "requestShape") == "background",
                        ..Default::default()
                    };
                    self.new_sub(info, &call, rev);
                    continue;
                }
            };
            let (id, running, started, modified) = (l.id.clone(), l.running, l.started, l.modified);
            let (name, desc) = (s(&l.meta, "agentType").to_string(), s(&l.meta, "description").to_string());
            let key_call = self.subs[k].call.clone();
            let key_id = self.subs[k].info.id.clone();
            self.change_sub(|s| (!key_id.is_empty() && s.info.id == key_id) || (!key_call.is_empty() && s.call == key_call), rev, |a| {
                a.id = id;
                if a.name.is_empty() || a.name == "agent" {
                    a.name = name;
                }
                if a.description.is_empty() {
                    a.description = desc;
                }
                if running {
                    a.status = "running".into();
                    a.ended = 0;
                } else if a.status == "running" {
                    a.status = "done".into();
                    a.ended = modified;
                }
                if a.started == 0 {
                    a.started = started;
                }
            });
            self.merge_duplicate(&l.id, rev);
        }
    }

    fn claude(&mut self, v: &Value, rev: u64) {
        if (!self.sidechain && v.get("isSidechain").and_then(Value::as_bool) == Some(true)) || v.get("isMeta").and_then(Value::as_bool) == Some(true) {
            return;
        }
        let ts = parse_ts(s(v, "timestamp"));
        let content = &v["message"]["content"];
        match s(v, "type") {
            "ai-title" => self.title = cut(s(v, "aiTitle"), 120),
            "user" => {
                if let Some(t) = content.as_str() {
                    self.user_text(t, ts, rev);
                }
                for b in content.as_array().into_iter().flatten() {
                    match s(b, "type") {
                        "text" => self.user_text(s(b, "text"), ts, rev),
                        "image" => {
                            self.push("user", "[image]".into(), String::new(), ts, rev);
                        }
                        "tool_result" => {
                            let failed = b.get("is_error").and_then(Value::as_bool) == Some(true);
                            self.result(s(b, "tool_use_id"), &result_text(&b["content"]), failed, &v["toolUseResult"], ts, rev);
                        }
                        _ => {}
                    }
                }
            }
            "assistant" => {
                for b in content.as_array().into_iter().flatten() {
                    match s(b, "type") {
                        "text" => {
                            let t = s(b, "text").trim();
                            if !t.is_empty() && t != "(no content)" {
                                self.push("assistant", cut(t, MAX_TEXT), String::new(), ts, rev);
                            }
                        }
                        "tool_use" => self.claude_call(b, ts, rev),
                        _ => {}  // thinking
                    }
                }
            }
            _ => {}
        }
    }

    fn claude_call(&mut self, b: &Value, ts: u64, rev: u64) {
        let (name, input, call) = (s(b, "name"), &b["input"], s(b, "id"));
        let (text, detail) = claude_tool(name, input);
        let item = self.tool(call, name, text, detail, ts, rev);
        match name {
            "Agent" | "Task" => {
                let kind = s(input, "subagent_type");
                let info = AgentInfo {
                    name: if kind.is_empty() { "general-purpose".into() } else { kind.to_string() },
                    description: s(input, "description").to_string(),
                    status: "running".into(),
                    started: ts,
                    background: flag(&input["run_in_background"]),
                    ..Default::default()
                };
                let k = self.new_sub(info.clone(), call, rev);
                self.subs[k].items.push(item);
                self.update(item, rev, |i| i.agent = Some(info));
            }
            "AskUserQuestion" => {
                let q = &input["questions"][0];
                let opts: Vec<PromptOption> = q["options"]
                    .as_array()
                    .into_iter()
                    .flatten()
                    .map(|o| PromptOption { label: cut(s(o, "label"), 200), hint: cut(s(o, "description"), 300) })
                    .collect();
                self.update(item, rev, |i| i.options = opts);
            }
            "Edit" | "MultiEdit" | "Write" => {
                // Until its result says exactly: the lines it writes and replaces.
                let lines = |t: &str| t.lines().count();
                let (add, del) = match name {
                    "Write" => (lines(s(input, "content")), 0),
                    "MultiEdit" => input["edits"].as_array().into_iter().flatten().fold((0, 0), |(a, d), e| (a + lines(s(e, "new_string")), d + lines(s(e, "old_string")))),
                    _ => (lines(s(input, "new_string")), lines(s(input, "old_string"))),
                };
                if add + del > 0 {
                    self.update(item, rev, |i| i.badge = if name == "Write" { format!("+{add}") } else { format!("+{add} −{del}") });
                }
            }
            _ => {}
        }
    }

    fn codex(&mut self, v: &Value, rev: u64) {
        let p = &v["payload"];
        let ts = parse_ts(s(v, "timestamp"));
        match (s(v, "type"), s(p, "type")) {
            ("event_msg", "user_message") => self.user_text(s(p, "message"), ts, rev),
            ("event_msg", "agent_message") => {
                let t = s(p, "message").trim();
                if !t.is_empty() {
                    self.push("assistant", cut(t, MAX_TEXT), String::new(), ts, rev);
                }
            }
            ("event_msg", "turn_aborted") => {
                self.push("note", "Interrupted".into(), String::new(), ts, rev);
            }
            ("response_item", "function_call") => {
                let args: Value = serde_json::from_str(s(p, "arguments")).unwrap_or(Value::Null);
                let (text, detail) = codex_tool(s(p, "name"), &args, "");
                self.tool(s(p, "call_id"), s(p, "name"), text, detail, ts, rev);
            }
            ("response_item", "custom_tool_call") => {
                let (text, detail) = codex_tool(s(p, "name"), &Value::Null, s(p, "input"));
                self.tool(s(p, "call_id"), s(p, "name"), text, detail, ts, rev);
            }
            ("response_item", "function_call_output" | "custom_tool_call_output") => {
                let (out, code) = match &p["output"] {
                    Value::String(t) => match serde_json::from_str::<Value>(t) {
                        Ok(o) => (Some(result_text(&o)).filter(|o| !o.is_empty()).unwrap_or_else(|| t.clone()), o["metadata"]["exit_code"].as_i64()),
                        Err(_) => (t.clone(), None),
                    },
                    o => (result_text(o), None),
                };
                let failed = code.is_some_and(|c| c != 0);
                let item = self.calls.get(s(p, "call_id")).map(|c| c.item);
                self.result(s(p, "call_id"), &out, failed, &Value::Null, ts, rev);
                if let (Some(c), Some(item)) = (code.filter(|&c| c != 0), item) {
                    self.update(item, rev, |i| i.badge = format!("exit {c}"));
                }
            }
            _ => {}
        }
    }

    /// Items changed after revision `since` (all of the last `limit` when `since` is 0), oldest first, within about
    /// `budget` bytes; and whether older items are left out.
    pub fn since(&self, since: u64, limit: usize, budget: usize) -> (Vec<ChatItem>, bool) {
        if since == 0 {
            return self.page(self.entries.len(), limit, budget);
        }
        let mut out: Vec<ChatItem> = self.entries.iter().filter(|e| e.rev > since).map(|e| e.item.clone()).collect();
        let mut size = 0;
        let mut keep = out.len();
        for (i, it) in out.iter().enumerate().rev() {
            size += it.text.len() + it.detail.len() + 64;
            if size > budget {
                keep = out.len() - i - 1;
                break;
            }
        }
        let n = out.len();
        out.drain(..n - keep);
        (out, false)
    }

    /// Up to `limit` items older than item `before`, oldest first, and whether there are even older ones.
    pub fn before(&self, before: u64, limit: usize, budget: usize) -> (Vec<ChatItem>, bool) {
        let end = self.entries.iter().position(|e| e.item.id >= before).unwrap_or(self.entries.len());
        self.page(end, limit, budget)
    }

    fn page(&self, end: usize, limit: usize, budget: usize) -> (Vec<ChatItem>, bool) {
        let mut start = end;
        let mut size = 0;
        while start > 0 && end - start < limit {
            let it = &self.entries[start - 1].item;
            size += it.text.len() + it.detail.len() + 64;
            if size > budget && start < end {
                break;
            }
            start -= 1;
        }
        (self.entries[start..end].iter().map(|e| e.item.clone()).collect(), start > 0)
    }
}

/// A Claude Code tool call as one line, and its input as detail.
fn claude_tool(name: &str, input: &Value) -> (String, String) {
    let path = || base(["file_path", "notebook_path", "path"].iter().map(|k| s(input, k)).find(|p| !p.is_empty()).unwrap_or(""));
    let detail = |d: String| cut(&d, MAX_DETAIL);
    match name {
        "Bash" => {
            let cmd = s(input, "command");
            let what = s(input, "description");
            let line = format!("Ran {}", first_line(cmd, 80));
            let d = if what.is_empty() { format!("$ {cmd}") } else { format!("{what}\n$ {cmd}") };
            (line, detail(d))
        }
        "Read" => (format!("Read {}", path()), s(input, "file_path").to_string()),
        "Edit" | "MultiEdit" | "NotebookEdit" => {
            let mut d = s(input, "file_path").to_string();
            let (old, new) = (s(input, "old_string"), s(input, "new_string"));
            if !old.is_empty() || !new.is_empty() {
                let minus: Vec<String> = old.lines().take(12).map(|l| format!("- {l}")).collect();
                let plus: Vec<String> = new.lines().take(12).map(|l| format!("+ {l}")).collect();
                d = format!("{d}\n{}\n{}", minus.join("\n"), plus.join("\n"));
            }
            (format!("Edited {}", path()), detail(d))
        }
        "Write" => {
            let body: Vec<&str> = s(input, "content").lines().take(20).collect();
            (format!("Wrote {}", path()), detail(format!("{}\n{}", s(input, "file_path"), body.join("\n"))))
        }
        "Grep" => (format!("Searched for “{}”", first_line(s(input, "pattern"), 60)), detail(input.to_string())),
        "Glob" => (format!("Listed {}", first_line(s(input, "pattern"), 60)), String::new()),
        "WebFetch" => {
            let url = s(input, "url");
            let host = url.split("://").nth(1).unwrap_or(url).split('/').next().unwrap_or(url);
            (format!("Fetched {host}"), url.to_string())
        }
        "WebSearch" => (format!("Searched the web for “{}”", first_line(s(input, "query"), 60)), String::new()),
        "Agent" | "Task" => (first_line(s(input, "description"), 70), detail(s(input, "prompt").to_string())),
        "SendMessage" => {
            let what = [s(input, "summary"), s(input, "message"), s(input, "content")].into_iter().find(|t| !t.is_empty()).unwrap_or("");
            (format!("Sent a message: {}", first_line(what, 70)), detail(s(input, "message").to_string()))
        }
        "TodoWrite" => ("Updated the to-do list".into(), String::new()),
        "AskUserQuestion" => {
            let qs = input["questions"].as_array().cloned().unwrap_or_default();
            let q = qs.first().map(|q| s(q, "question")).unwrap_or("");
            // The other questions, if it asks several at once.
            let rest: Vec<String> = qs.iter().skip(1).map(|q| format!("{}: {}", s(q, "header"), s(q, "question"))).collect();
            (cut(q.trim(), 400), detail(rest.join("\n")))
        }
        "ExitPlanMode" => ("Proposed a plan".into(), cut(s(input, "plan"), MAX_TEXT)),
        "Skill" => (format!("Used the {} skill", s(input, "skill")), String::new()),
        _ => {
            let short = name.rsplit("__").next().unwrap_or(name);
            (format!("Used {short}"), detail(input.to_string()))
        }
    }
}

/// A Codex tool call as one line, and its input as detail.
fn codex_tool(name: &str, args: &Value, input: &str) -> (String, String) {
    let detail = |d: &str| cut(d, MAX_DETAIL);
    match name {
        "shell" | "container.exec" | "local_shell" | "exec_command" | "shell_command" => {
            let cmd = match &args["command"] {
                Value::Array(a) => {
                    let parts: Vec<&str> = a.iter().filter_map(Value::as_str).collect();
                    // ["bash", "-lc", "<script>"]: the script is what was run.
                    if parts.len() == 3 && parts[1].starts_with('-') { parts[2].to_string() } else { parts.join(" ") }
                }
                Value::String(c) => c.clone(),
                _ => s(args, "cmd").to_string(),
            };
            (format!("Ran {}", first_line(&cmd, 80)), detail(&format!("$ {cmd}")))
        }
        "apply_patch" => {
            let patch = if input.is_empty() { s(args, "input") } else { input };
            let files: Vec<String> = patch
                .lines()
                .filter_map(|l| l.strip_prefix("*** Update File: ").or(l.strip_prefix("*** Add File: ")).or(l.strip_prefix("*** Delete File: ")))
                .map(|f| base(f.trim()))
                .collect();
            let what = if files.is_empty() { "files".to_string() } else { files.join(", ") };
            (format!("Edited {}", cut(&what, 80)), detail(patch))
        }
        "exec" => {
            // Code that calls tools: the first command it runs says what it does.
            let cmd = input.split("cmd:\"").nth(1).and_then(|r| r.split('"').next()).unwrap_or("");
            let line = if cmd.is_empty() { "Ran code".to_string() } else { format!("Ran {}", first_line(cmd, 80)) };
            (line, detail(input))
        }
        _ => {
            let short = name.rsplit("__").next().unwrap_or(name);
            (format!("Used {short}"), detail(&args.to_string()))
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    fn feed(c: &mut Chat, lines: &[&str]) {
        for (i, l) in lines.iter().enumerate() {
            c.feed(l.as_bytes(), i as u64 + 1);
        }
    }

    #[test]
    fn claude_transcript() {
        let mut c = Chat::new(Agent::Claude);
        feed(&mut c, &[
            r#"pe":"assistant","message":{"content":[{"type":"text","text":"cut"}]}}"#,
            r#"{"type":"user","message":{"role":"user","content":"Create bye.txt, then run ls"},"isSidechain":false,"timestamp":"2026-10-23T20:44:59.816Z"}"#,
            r#"{"type":"user","isMeta":true,"message":{"content":"<local-command-caveat>x</local-command-caveat>"}}"#,
            r#"{"type":"assistant","message":{"content":[{"type":"thinking","thinking":"hmm"}]}}"#,
            r#"{"type":"assistant","message":{"content":[{"type":"tool_use","id":"t1","name":"Bash","input":{"command":"printf 'bye\\n' > bye.txt && ls -la","description":"Create and list"}}]}}"#,
            r#"{"type":"assistant","isSidechain":true,"message":{"content":[{"type":"text","text":"subagent"}]}}"#,
            r#"{"type":"user","message":{"content":[{"type":"tool_result","tool_use_id":"t1","content":"total 16\nbye.txt","is_error":false}]}}"#,
            r#"{"type":"assistant","message":{"content":[{"type":"tool_use","id":"t2","name":"Edit","input":{"file_path":"/src/main.rs","old_string":"a","new_string":"b"}}]}}"#,
            r#"{"type":"user","message":{"content":[{"type":"tool_result","tool_use_id":"t2","content":[{"type":"text","text":"denied"}],"is_error":true}]}}"#,
            r#"{"type":"assistant","message":{"content":[{"type":"text","text":"I created **bye.txt**."}]}}"#,
            r#"{"type":"user","message":{"content":"<command-name>/model</command-name>\n<command-args>haiku</command-args>"}}"#,
            r#"{"type":"user","message":{"content":"<system-reminder>x</system-reminder>"}}"#,
            r#"{"type":"user","message":{"content":[{"type":"text","text":"[Request interrupted by user]"}]}}"#,
            r#"{"type":"ai-title","aiTitle":"Create bye.txt"}"#,
            r#"{"type":"system","subtype":"turn_duration","durationMs":5000}"#,
        ]);
        let (items, more) = c.since(0, 100, 1 << 20);
        assert!(!more);
        let got: Vec<(&str, &str)> = items.iter().map(|i| (i.role.as_str(), i.text.as_str())).collect();
        assert_eq!(got, [
            ("user", "Create bye.txt, then run ls"),
            ("tool", "Ran printf 'bye\\n' > bye.txt && ls -la"),
            ("tool", "Edited main.rs"),
            ("assistant", "I created **bye.txt**."),
            ("user", "/model haiku"),
            ("note", "Interrupted"),
        ]);
        assert!(items[1].detail.contains("total 16") && !items[1].failed && !items[1].pending && items[1].tool == "Bash");
        assert!(items[2].failed && items[2].detail.contains("- a\n+ b"));
        assert_eq!(items[0].ts, 1_792_788_299_816);
        assert_eq!(c.title, "Create bye.txt");
        // Incremental: only what changed after revision 7 (the Edit's result came at 9).
        let (later, _) = c.since(7, 100, 1 << 20);
        assert_eq!(later.iter().map(|i| i.text.as_str()).collect::<Vec<_>>(), ["Edited main.rs", "I created **bye.txt**.", "/model haiku", "Interrupted"]);
        // Pages: the last 2, then the ones before them.
        let (last, more) = c.since(0, 2, 1 << 20);
        assert!(more && last.len() == 2 && last[0].text == "/model haiku");
        let (older, more) = c.before(last[0].id, 10, 1 << 20);
        assert!(!more && older.len() == 4);
    }

    /// Tool results as Claude Code writes them: the exit code of a failed command, the lines an edit changed, what was
    /// answered to a question, a plan.
    #[test]
    fn results_and_questions() {
        let mut c = Chat::new(Agent::Claude);
        feed(&mut c, &[
            r#"{"type":"assistant","message":{"content":[{"type":"tool_use","id":"b","name":"Bash","input":{"command":"./build.sh"}}]}}"#,
            r#"{"type":"user","message":{"content":[{"type":"tool_result","tool_use_id":"b","content":"Exit code 2\nerror: no such file","is_error":true}]},"toolUseResult":"Error: Exit code 2"}"#,
            r#"{"type":"assistant","message":{"content":[{"type":"tool_use","id":"e","name":"Edit","input":{"file_path":"/x/main.cpp","old_string":"a\nb","new_string":"c"}}]}}"#,
            r#"{"type":"user","message":{"content":[{"type":"tool_result","tool_use_id":"e","content":"ok"}]},"toolUseResult":{"filePath":"/x/main.cpp","structuredPatch":[{"oldStart":1,"lines":[" x","-a","-b","-q","+c"]}]}}"#,
            r#"{"type":"assistant","message":{"content":[{"type":"tool_use","id":"q","name":"AskUserQuestion","input":{"questions":[{"question":"Which color?","header":"Color","multiSelect":false,"options":[{"label":"red","description":"Warm"},{"label":"green","description":"Calm"}]},{"question":"Size?","header":"Size","options":[]}]}}]}}"#,
            r#"{"type":"user","message":{"content":[{"type":"tool_result","tool_use_id":"q","content":"Your questions have been answered"}]},"toolUseResult":{"questions":[],"answers":{"Which color?":"green"}}}"#,
            r#"{"type":"assistant","message":{"content":[{"type":"tool_use","id":"p","name":"ExitPlanMode","input":{"plan":"1. Build\n2. Test"}}]}}"#,
        ]);
        let (items, _) = c.since(0, 100, 1 << 20);
        assert_eq!((items[0].badge.as_str(), items[0].failed), ("exit 2", true));
        assert_eq!(items[1].badge, "+1 −3");
        assert_eq!((items[2].text.as_str(), items[2].tool.as_str(), items[2].badge.as_str()), ("Which color?", "AskUserQuestion", "green"));
        assert_eq!(items[2].options.iter().map(|o| (o.label.as_str(), o.hint.as_str())).collect::<Vec<_>>(), [("red", "Warm"), ("green", "Calm")]);
        assert_eq!(items[2].detail, "Size: Size?");
        assert!(items[3].pending && items[3].detail == "1. Build\n2. Test" && items[3].tool == "ExitPlanMode");
        assert_eq!(c.pending_tool(), Some("Proposed a plan"));
    }

    /// A background subagent: started by an Agent call, its id from the call's answer, then its report in a task
    /// notification (a new item, and its card shows it finished); a SendMessage resumes it. One that started before the
    /// part read comes from the host's list of the session's subagents.
    #[test]
    fn subagents() {
        let mut c = Chat::new(Agent::Claude);
        feed(&mut c, &[
            r#"{"type":"user","message":{"content":"port the skins"},"timestamp":"2026-10-23T16:00:00.000Z"}"#,
            r#"{"type":"assistant","timestamp":"2026-10-23T16:31:53.393Z","message":{"content":[{"type":"tool_use","id":"toolu_1","name":"Agent","input":{"description":"Port skins","subagent_type":"executor","run_in_background":"true","prompt":"Task: port them"}}]}}"#,
            r#"{"type":"user","timestamp":"2026-10-23T16:31:54.000Z","message":{"content":[{"type":"tool_result","tool_use_id":"toolu_1","content":[{"type":"text","text":"Async agent launched successfully. agentId: a6d5"}]}]},"toolUseResult":{"isAsync":true,"status":"async_launched","agentId":"a6d5e25a0889e3ee4","description":"Port skins"}}"#,
            r#"{"type":"user","origin":{"kind":"task-notification"},"timestamp":"2026-10-23T17:31:54.000Z","message":{"content":"<task-notification>\n<task-id>a6d5e25a0889e3ee4</task-id>\n<tool-use-id>toolu_1</tool-use-id>\n<status>completed</status>\n<summary>Agent \"Port skins\" finished</summary>\n<result>Done: **3 skins** ported.</result>\n<usage><subagent_tokens>616302</subagent_tokens><tool_uses>372</tool_uses><duration_ms>3600000</duration_ms></usage>\n</task-notification>"}}"#,
            r#"{"type":"user","origin":{"kind":"task-notification"},"message":{"content":"<task-notification>\n<task-id>bx3iwo33k</task-id>\n<status>completed</status>\n<summary>Background command \"npm test\" completed (exit code 0)</summary>\n</task-notification>"}}"#,
        ]);
        let (items, _) = c.since(0, 100, 1 << 20);
        let card = items[1].agent.as_ref().unwrap();
        assert_eq!((items[1].text.as_str(), items[1].tool.as_str()), ("Port skins", "Agent"));
        assert_eq!((card.id.as_str(), card.name.as_str(), card.status.as_str(), card.background), ("a6d5e25a0889e3ee4", "executor", "done", true));
        assert_eq!((card.tokens, card.tool_uses, card.duration_ms), (616_302, 372, 3_600_000));
        assert_eq!((items[2].text.as_str(), items[2].detail.as_str()), ("Agent \"Port skins\" finished", "Done: **3 skins** ported."));
        assert_eq!(items[3].role, "note");
        assert_eq!(c.turn_start, 1_792_771_200_000);
        assert_eq!(c.agents().len(), 1);
        // Resumed with a message: running again.
        feed(&mut c, &[
            r#"{"type":"assistant","message":{"content":[{"type":"tool_use","id":"s1","name":"SendMessage","input":{"to":"a6d5e25a0889e3ee4","summary":"one more fix","message":"also the arms"}}]}}"#,
            r#"{"type":"user","timestamp":"2026-10-23T18:00:00.000Z","message":{"content":[{"type":"tool_result","tool_use_id":"s1","content":"ok"}]},"toolUseResult":{"success":true,"resumedAgentId":"a6d5e25a0889e3ee4"}}"#,
        ]);
        let a = c.agent_info("a6d5e25a0889e3ee4").unwrap();
        assert_eq!((a.status.as_str(), a.started), ("running", 1_792_778_400_000));
        // The host's list: one more, started long ago and still working; and this one stopped.
        let before = c.agents_rev;
        c.merge_listed(
            &[
                Listed { id: "ac9fc284ab2b0f242".into(), running: true, started: 1_792_700_000_000, modified: 0, meta: serde_json::json!({"agentType":"general-purpose","description":"Rig the skin","toolUseId":"toolu_0"}) },
                Listed { id: "a6d5e25a0889e3ee4".into(), running: false, started: 0, modified: 1_792_779_000_000, meta: serde_json::json!({"agentType":"executor","description":"Port skins","toolUseId":"toolu_1"}) },
            ],
            99,
        );
        assert_eq!(c.agents_rev, 99);
        assert!(before < 99);
        let list = c.agents();
        assert_eq!(list.iter().map(|a| (a.id.as_str(), a.status.as_str())).collect::<Vec<_>>(), [("ac9fc284ab2b0f242", "running"), ("a6d5e25a0889e3ee4", "done")]);
        assert_eq!((list[0].name.as_str(), list[0].description.as_str()), ("general-purpose", "Rig the skin"));
        // Its card in the chat changed with it.
        let (later, _) = c.since(98, 100, 1 << 20);
        assert!(later.iter().any(|i| i.agent.as_ref().is_some_and(|a| a.id == "a6d5e25a0889e3ee4" && a.status == "done")));
    }

    /// A subagent's own file: every record is a sidechain one, and its task comes first.
    #[test]
    fn subagent_conversation() {
        let mut c = Chat::subagent();
        feed(&mut c, &[
            r#"{"type":"user","isSidechain":true,"agentId":"a6d5","message":{"role":"user","content":"Task #89: fix the shoulders"},"timestamp":"2026-10-23T20:44:59.816Z"}"#,
            r#"{"type":"assistant","isSidechain":true,"agentId":"a6d5","message":{"content":[{"type":"tool_use","id":"t","name":"Read","input":{"file_path":"/r/rig.py"}}]}}"#,
            r#"{"type":"user","isSidechain":true,"message":{"content":[{"type":"tool_result","tool_use_id":"t","content":"x"}]},"toolUseResult":{"type":"text","file":{"filePath":"/r/rig.py","numLines":120}}}"#,
            r#"{"type":"assistant","isSidechain":true,"message":{"content":[{"type":"text","text":"Fixed."}]}}"#,
        ]);
        let (items, _) = c.since(0, 100, 1 << 20);
        assert_eq!(items.iter().map(|i| i.text.as_str()).collect::<Vec<_>>(), ["Task #89: fix the shoulders", "Read rig.py", "Fixed."]);
        assert_eq!(items[1].badge, "120 lines");
    }

    #[test]
    fn times() {
        assert_eq!(parse_ts("1970-01-01T00:00:00Z"), 0);
        assert_eq!(parse_ts("2000-03-01T00:00:01.5Z"), 951_868_801_500);
        assert_eq!(parse_ts("2026-10-23T20:44:59.816Z"), 1_792_788_299_816);
        assert_eq!(parse_ts("yesterday"), 0);
    }

    #[test]
    fn codex_transcript() {
        let mut c = Chat::new(Agent::Codex);
        feed(&mut c, &[
            r#"{"type":"session_meta","payload":{"id":"x","cwd":"/src","source":"cli"}}"#,
            r#"{"type":"event_msg","payload":{"type":"user_message","message":"run the tests"}}"#,
            r#"{"type":"response_item","payload":{"type":"function_call","name":"shell","arguments":"{\"command\":[\"bash\",\"-lc\",\"cargo test\"]}","call_id":"c1"}}"#,
            r#"{"type":"response_item","payload":{"type":"function_call_output","call_id":"c1","output":"{\"output\":\"ok. 3 passed\",\"metadata\":{\"exit_code\":0}}"}}"#,
            r#"{"type":"response_item","payload":{"type":"custom_tool_call","name":"apply_patch","call_id":"c2","input":"*** Begin Patch\n*** Update File: src/lib.rs\n@@\n-a\n+b\n*** End Patch"}}"#,
            r#"{"type":"response_item","payload":{"type":"custom_tool_call","name":"exec","call_id":"c3","input":"const r = tools.exec_command({cmd:\"rg -n foo\"});"}}"#,
            r#"{"type":"event_msg","payload":{"type":"agent_message","message":"All **3** tests pass."}}"#,
            r#"{"type":"response_item","payload":{"type":"message","role":"assistant","content":[{"type":"output_text","text":"All **3** tests pass."}]}}"#,
            r#"{"type":"response_item","payload":{"type":"function_call","name":"shell","arguments":"{\"command\":[\"bash\",\"-lc\",\"false\"]}","call_id":"c4"}}"#,
            r#"{"type":"response_item","payload":{"type":"function_call_output","call_id":"c4","output":"{\"output\":\"\",\"metadata\":{\"exit_code\":1}}"}}"#,
        ]);
        let (items, _) = c.since(0, 100, 1 << 20);
        let got: Vec<(&str, &str)> = items.iter().map(|i| (i.role.as_str(), i.text.as_str())).collect();
        assert_eq!(got, [
            ("user", "run the tests"),
            ("tool", "Ran cargo test"),
            ("tool", "Edited lib.rs"),
            ("tool", "Ran rg -n foo"),
            ("assistant", "All **3** tests pass."),
            ("tool", "Ran false"),
        ]);
        assert!(items[1].detail.contains("3 passed") && !items[1].pending && !items[1].failed);
        assert!(items[2].pending);
        assert_eq!((items[5].failed, items[5].badge.as_str()), (true, "exit 1"));
    }

    #[test]
    fn budget_and_cuts() {
        let mut c = Chat::new(Agent::Claude);
        let big = "x".repeat(50_000);
        for i in 0..10 {
            let l = serde_json::json!({"type":"assistant","message":{"content":[{"type":"text","text":big}]}}).to_string();
            c.feed(l.as_bytes(), i + 1);
        }
        let (items, more) = c.since(0, 100, 40_000);
        assert!(more && items.len() == 3, "{} items", items.len());
        assert!(items[0].text.chars().count() <= MAX_TEXT + 1);
    }
}
