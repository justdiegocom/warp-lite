//! A conversation as the phone shows it, from the transcript Claude Code or Codex writes as it goes (JSON lines):
//! what you wrote, the agent's answers (markdown) and its tool calls as one line each («Ran cargo test», «Edited
//! main.rs») with what they did as detail.
//!
//! - Claude Code: `~/.claude/projects/<folder>/<session>.jsonl`. Records `user` (a string you typed, or tool results),
//!   `assistant` (text, tool_use and thinking blocks, one record per block), `system`, `ai-title`; subagents
//!   (`isSidechain`), meta records and thinking are left out.
//! - Codex: `~/.codex/sessions/YYYY/MM/DD/rollout-*.jsonl`. `event_msg` `user_message` / `agent_message`, and
//!   `response_item` `function_call` / `custom_tool_call` with their `*_output`.
//!
//! Lines that don't parse (the cut first line of a tail, records of a newer version) are skipped. Nothing is logged.

use std::collections::HashMap;

use serde_json::Value;

use crate::proto::ChatItem;

/// Longest text kept for a message, and for the detail of a tool call (characters).
const MAX_TEXT: usize = 12_000;
const MAX_DETAIL: usize = 1_500;
/// Items kept per conversation (the oldest are dropped).
const MAX_ITEMS: usize = 1_000;

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

pub struct Chat {
    agent: Agent,
    entries: Vec<Entry>,
    /// Tool call id → its item's id (to attach the result).
    calls: HashMap<String, u64>,
    next_id: u64,
    pub title: String,
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

/// The text of a tool result (a string, or blocks of text).
fn result_text(v: &Value) -> String {
    match v {
        Value::String(t) => t.clone(),
        Value::Array(a) => a.iter().filter_map(|b| b.get("text").and_then(Value::as_str)).collect::<Vec<_>>().join("\n"),
        Value::Object(_) => v.get("output").map(result_text).unwrap_or_default(),
        _ => String::new(),
    }
}

impl Chat {
    pub fn new(agent: Agent) -> Chat {
        Chat { agent, entries: Vec::new(), calls: HashMap::new(), next_id: 1, title: String::new() }
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

    fn push(&mut self, role: &str, text: String, detail: String, rev: u64) -> u64 {
        let id = self.next_id;
        self.next_id += 1;
        self.entries.push(Entry { item: ChatItem { id, role: role.into(), text, detail, ..Default::default() }, rev });
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

    /// One line of the transcript. `rev`: the revision items changed by it get.
    pub fn feed(&mut self, line: &[u8], rev: u64) {
        let Ok(v) = serde_json::from_slice::<Value>(line) else { return };
        match self.agent {
            Agent::Claude => self.claude(&v, rev),
            Agent::Codex => self.codex(&v, rev),
        }
    }

    fn user_text(&mut self, t: &str, rev: u64) {
        let t = t.trim();
        if t.is_empty() {
            return;
        }
        if t.starts_with("[Request interrupted") {
            self.push("note", "Interrupted".into(), String::new(), rev);
            return;
        }
        if let Some(rest) = t.strip_prefix("<command-name>") {
            // A slash command: «/model», with its arguments.
            let name = rest.split("</command-name>").next().unwrap_or("").trim();
            let args = t.split("<command-args>").nth(1).and_then(|a| a.split("</command-args>").next()).unwrap_or("").trim();
            if !name.is_empty() {
                let text = if args.is_empty() { name.to_string() } else { format!("{name} {args}") };
                self.push("user", cut(&text, 400), String::new(), rev);
            }
            return;
        }
        if t.starts_with('<') {
            return;  // command output, caveats, task notifications, reminders: not something you wrote
        }
        self.push("user", cut(t, MAX_TEXT), String::new(), rev);
    }

    fn tool(&mut self, call: &str, text: String, detail: String, rev: u64) {
        let id = self.push("tool", text, detail, rev);
        self.update(id, rev, |i| i.pending = true);
        if !call.is_empty() {
            self.calls.insert(call.to_string(), id);
        }
    }

    fn result(&mut self, call: &str, out: &str, failed: bool, rev: u64) {
        let Some(&id) = self.calls.get(call) else { return };
        self.calls.remove(call);
        let out = out.trim();
        self.update(id, rev, |i| {
            i.pending = false;
            i.failed = failed;
            if !out.is_empty() {
                let room = MAX_DETAIL.saturating_sub(i.detail.chars().count()).max(300);
                if !i.detail.is_empty() {
                    i.detail.push_str("\n\n");
                }
                i.detail.push_str(&cut(out, room));
            }
        });
    }

    fn claude(&mut self, v: &Value, rev: u64) {
        if v.get("isSidechain").and_then(Value::as_bool) == Some(true) || v.get("isMeta").and_then(Value::as_bool) == Some(true) {
            return;
        }
        let content = &v["message"]["content"];
        match s(v, "type") {
            "ai-title" => self.title = cut(s(v, "aiTitle"), 120),
            "user" => {
                if let Some(t) = content.as_str() {
                    self.user_text(t, rev);
                }
                for b in content.as_array().into_iter().flatten() {
                    match s(b, "type") {
                        "text" => self.user_text(s(b, "text"), rev),
                        "image" => {
                            self.push("user", "[image]".into(), String::new(), rev);
                        }
                        "tool_result" => {
                            let failed = b.get("is_error").and_then(Value::as_bool) == Some(true);
                            self.result(s(b, "tool_use_id"), &result_text(&b["content"]), failed, rev);
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
                                self.push("assistant", cut(t, MAX_TEXT), String::new(), rev);
                            }
                        }
                        "tool_use" => {
                            let (text, detail) = claude_tool(s(b, "name"), &b["input"]);
                            self.tool(s(b, "id"), text, detail, rev);
                        }
                        _ => {}  // thinking
                    }
                }
            }
            _ => {}
        }
    }

    fn codex(&mut self, v: &Value, rev: u64) {
        let p = &v["payload"];
        match (s(v, "type"), s(p, "type")) {
            ("event_msg", "user_message") => self.user_text(s(p, "message"), rev),
            ("event_msg", "agent_message") => {
                let t = s(p, "message").trim();
                if !t.is_empty() {
                    self.push("assistant", cut(t, MAX_TEXT), String::new(), rev);
                }
            }
            ("event_msg", "turn_aborted") => {
                self.push("note", "Interrupted".into(), String::new(), rev);
            }
            ("response_item", "function_call") => {
                let args: Value = serde_json::from_str(s(p, "arguments")).unwrap_or(Value::Null);
                let (text, detail) = codex_tool(s(p, "name"), &args, "");
                self.tool(s(p, "call_id"), text, detail, rev);
            }
            ("response_item", "custom_tool_call") => {
                let (text, detail) = codex_tool(s(p, "name"), &Value::Null, s(p, "input"));
                self.tool(s(p, "call_id"), text, detail, rev);
            }
            ("response_item", "function_call_output" | "custom_tool_call_output") => {
                let out = match &p["output"] {
                    Value::String(t) => serde_json::from_str::<Value>(t).ok().map(|o| result_text(&o)).filter(|o| !o.is_empty()).unwrap_or_else(|| t.clone()),
                    o => result_text(o),
                };
                self.result(s(p, "call_id"), &out, false, rev);
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
            let line = format!("Ran {}", first_line(cmd, 80));
            (line, detail(format!("$ {cmd}")))
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
        "Agent" | "Task" => (format!("Agent: {}", first_line(s(input, "description"), 70)), detail(s(input, "prompt").to_string())),
        "TodoWrite" => ("Updated the to-do list".into(), String::new()),
        "AskUserQuestion" => {
            let q = input["questions"].get(0).map(|q| s(q, "question")).unwrap_or("");
            (format!("Asked: {}", first_line(q, 80)), String::new())
        }
        "ExitPlanMode" => ("Proposed a plan".into(), detail(s(input, "plan").to_string())),
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
            r#"{"type":"user","message":{"role":"user","content":"Create bye.txt, then run ls"},"isSidechain":false}"#,
            r#"{"type":"user","isMeta":true,"message":{"content":"<local-command-caveat>x</local-command-caveat>"}}"#,
            r#"{"type":"assistant","message":{"content":[{"type":"thinking","thinking":"hmm"}]}}"#,
            r#"{"type":"assistant","message":{"content":[{"type":"tool_use","id":"t1","name":"Bash","input":{"command":"printf 'bye\\n' > bye.txt && ls -la","description":"Create and list"}}]}}"#,
            r#"{"type":"assistant","isSidechain":true,"message":{"content":[{"type":"text","text":"subagent"}]}}"#,
            r#"{"type":"user","message":{"content":[{"type":"tool_result","tool_use_id":"t1","content":"total 16\nbye.txt","is_error":false}]}}"#,
            r#"{"type":"assistant","message":{"content":[{"type":"tool_use","id":"t2","name":"Edit","input":{"file_path":"/src/main.rs","old_string":"a","new_string":"b"}}]}}"#,
            r#"{"type":"user","message":{"content":[{"type":"tool_result","tool_use_id":"t2","content":[{"type":"text","text":"denied"}],"is_error":true}]}}"#,
            r#"{"type":"assistant","message":{"content":[{"type":"text","text":"I created **bye.txt**."}]}}"#,
            r#"{"type":"user","message":{"content":"<command-name>/model</command-name>\n<command-args>haiku</command-args>"}}"#,
            r#"{"type":"user","message":{"content":"<task-notification>done</task-notification>"}}"#,
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
        assert!(items[1].detail.contains("total 16") && !items[1].failed && !items[1].pending);
        assert!(items[2].failed && items[2].detail.contains("- a\n+ b"));
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

    #[test]
    fn codex_transcript() {
        let mut c = Chat::new(Agent::Codex);
        feed(&mut c, &[
            r#"{"type":"session_meta","payload":{"id":"x","cwd":"/src","source":"cli"}}"#,
            r#"{"type":"event_msg","payload":{"type":"user_message","message":"run the tests"}}"#,
            r#"{"type":"response_item","payload":{"type":"function_call","name":"shell","arguments":"{\"command\":[\"bash\",\"-lc\",\"cargo test\"]}","call_id":"c1"}}"#,
            r#"{"type":"response_item","payload":{"type":"function_call_output","call_id":"c1","output":"{\"output\":\"ok. 3 passed\",\"metadata\":{}}"}}"#,
            r#"{"type":"response_item","payload":{"type":"custom_tool_call","name":"apply_patch","call_id":"c2","input":"*** Begin Patch\n*** Update File: src/lib.rs\n@@\n-a\n+b\n*** End Patch"}}"#,
            r#"{"type":"response_item","payload":{"type":"custom_tool_call","name":"exec","call_id":"c3","input":"const r = tools.exec_command({cmd:\"rg -n foo\"});"}}"#,
            r#"{"type":"event_msg","payload":{"type":"agent_message","message":"All **3** tests pass."}}"#,
            r#"{"type":"response_item","payload":{"type":"message","role":"assistant","content":[{"type":"output_text","text":"All **3** tests pass."}]}}"#,
        ]);
        let (items, _) = c.since(0, 100, 1 << 20);
        let got: Vec<(&str, &str)> = items.iter().map(|i| (i.role.as_str(), i.text.as_str())).collect();
        assert_eq!(got, [
            ("user", "run the tests"),
            ("tool", "Ran cargo test"),
            ("tool", "Edited lib.rs"),
            ("tool", "Ran rg -n foo"),
            ("assistant", "All **3** tests pass."),
        ]);
        assert!(items[1].detail.contains("3 passed") && !items[1].pending);
        assert!(items[2].pending);
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
