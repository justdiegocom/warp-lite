//! What a pane's screen says, for the phone: the question it asks with its options (Claude Code's permission prompts,
//! trust dialog and menus, Codex's approvals), whether its program is working, which agent draws it and which tmux
//! session it shows. Plain text in, nothing kept.
//!
//! A question is a group of option lines near the bottom of the screen, one of them behind a cursor (`❯`, `›`):
//! numbered `1.`, `2.`, … (permission prompts, menus, Codex), or unnumbered with a footer such as «Enter to confirm»
//! under them (the trust dialog). Answering moves the cursor with ↑/↓ to the option and presses Enter, which works
//! the same in all of them (a digit alone picks the option in some and only moves to it in others).

use crate::proto::{Prompt, PromptOption};

/// What programs draw in front of the selected option.
const CURSORS: [char; 3] = ['❯', '›', '▶'];

/// Lines under the options of a question that say how to answer it.
const FOOTERS: [&str; 6] = ["enter to confirm", "enter to select", "to navigate", "esc to cancel", "press enter", "tab to amend"];

/// Box borders around and inside a dialog, dropped from both ends of a line (with the space after the left one, so
/// what is inside keeps its indentation).
fn strip_frame(line: &str) -> &str {
    let mut t = line.trim_end();
    if let Some(s) = t.strip_suffix(['│', '┃', '║']) {
        t = s.trim_end();
    }
    let lead = t.len() - t.trim_start().len();
    match t[lead..].strip_prefix(['│', '┃', '║']) {
        Some(rest) => rest.strip_prefix(' ').unwrap_or(rest),
        None => t,
    }
}

/// A line that is only a rule (───, ╌╌╌, ━━━), the edges of a dialog.
fn is_rule(line: &str) -> bool {
    let t = line.trim();
    t.chars().count() >= 8 && t.chars().all(|c| matches!(c, '─' | '━' | '╌' | '┄' | '┈' | '═' | '-' | '╭' | '╮' | '╰' | '╯' | '┌' | '┐' | '└' | '┘'))
}

fn is_solid_rule(line: &str) -> bool {
    is_rule(line) && !line.contains(['╌', '┄', '┈'])
}

#[derive(Debug, Clone)]
struct OptLine {
    row: usize,
    /// Column where its text starts (after the cursor and the number).
    col: usize,
    cursor: bool,
    num: Option<u32>,
    label: String,
}

fn lead(s: &str) -> usize {
    s.chars().take_while(|c| *c == ' ').count()
}

fn parse_option(row: usize, line: &str) -> Option<OptLine> {
    let mut col = lead(line);
    let mut rest = line.trim_start();
    let mut cursor = false;
    if let Some(c) = rest.chars().next()
        && CURSORS.contains(&c)
    {
        cursor = true;
        rest = &rest[c.len_utf8()..];
        col += 1 + lead(rest);
        rest = rest.trim_start();
    }
    let digits: String = rest.chars().take_while(|c| c.is_ascii_digit()).collect();
    let mut num = None;
    if !digits.is_empty() && digits.len() <= 2 && rest[digits.len()..].starts_with(". ") {
        num = digits.parse().ok();
        let after = &rest[digits.len() + 1..];
        col += digits.len() + 1 + lead(after);
        rest = after.trim_start();
    }
    if rest.is_empty() {
        return None;
    }
    Some(OptLine { row, col, cursor, num, label: rest.trim_end().to_string() })
}

/// The question on the screen, if any. `rows`: the visible screen, top to bottom.
pub fn prompt(rows: &[String]) -> Option<Prompt> {
    let lines: Vec<&str> = rows.iter().map(|l| strip_frame(l)).collect();
    numbered(&lines).or_else(|| unnumbered(&lines))
}

/// Non-empty lines from `row` (excluded) to the bottom: a question sits at the bottom of the screen, under it only
/// its footer, an input box or tmux's status line.
fn below(lines: &[&str], row: usize) -> usize {
    lines[row + 1..].iter().filter(|l| !l.trim().is_empty()).count()
}

fn numbered(lines: &[&str]) -> Option<Prompt> {
    // The last numbered option line, then up through n-1, ..., 1 (with wrapped text or descriptions in between).
    let last = (0..lines.len()).rev().find_map(|r| parse_option(r, lines[r]).filter(|o| o.num.is_some()))?;
    if below(lines, last.row) > 6 {
        return None;
    }
    let mut opts = vec![last.clone()];
    let mut want = last.num? - 1;
    let mut r = last.row;
    while want >= 1 && r > 0 {
        r -= 1;
        let line = lines[r];
        if line.trim().is_empty() {
            if r + 2 < lines.len() && lines[r + 1].trim().is_empty() {
                return None;  // two blank lines: not one group
            }
            continue;
        }
        match parse_option(r, line) {
            Some(o) if o.num == Some(want) => {
                opts.push(o);
                want -= 1;
            }
            // Wrapped text or a description: indented at least as far as the option's text.
            _ if lead(line) >= opts.last().unwrap().col.saturating_sub(3).max(1) => {}
            _ => return None,
        }
    }
    if want != 0 || opts.len() < 2 {
        return None;
    }
    opts.reverse();
    build(lines, opts)
}

fn unnumbered(lines: &[&str]) -> Option<Prompt> {
    // The footer, then the block of options right above it with the cursor on one of them.
    let footer = (0..lines.len()).rev().find(|&r| {
        let l = lines[r].to_lowercase();
        FOOTERS.iter().any(|f| l.contains(f))
    })?;
    if below(lines, footer) > 4 {
        return None;
    }
    let mut r = footer;
    while r > 0 && lines[r - 1].trim().is_empty() {
        r -= 1;
    }
    let end = r;  // exclusive
    let mut start = end;
    while start > 0 && !lines[start - 1].trim().is_empty() && !is_rule(lines[start - 1]) && end - start < 12 {
        start -= 1;
    }
    let opts: Vec<OptLine> = (start..end).filter_map(|r| parse_option(r, lines[r])).collect();
    let cur = opts.iter().find(|o| o.cursor)?;
    // Siblings: the lines of the block whose text starts where the cursor's does.
    let opts: Vec<OptLine> = opts.iter().filter(|o| o.cursor || lead(lines[o.row]) == cur.col).cloned().collect();
    if opts.len() < 2 || opts.iter().filter(|o| o.cursor).count() != 1 || opts.len() != end - start {
        return None;
    }
    build(lines, opts)
}

fn build(lines: &[&str], opts: Vec<OptLine>) -> Option<Prompt> {
    let selected = opts.iter().position(|o| o.cursor)? as u32;
    if opts.iter().filter(|o| o.cursor).count() != 1 {
        return None;
    }
    // Each option's label, and the lines under it (until the next option) as its hint.
    let mut options = Vec::new();
    for (i, o) in opts.iter().enumerate() {
        let end = opts.get(i + 1).map(|n| n.row).unwrap_or_else(|| {
            let mut e = o.row + 1;
            while e < lines.len() && !lines[e].trim().is_empty() && lead(lines[e]) >= o.col && parse_option(e, lines[e]).is_none_or(|p| p.num.is_none() && !p.cursor) && !footerish(lines[e]) {
                e += 1;
            }
            e
        });
        let hint: Vec<&str> = (o.row + 1..end).map(|r| lines[r].trim()).filter(|l| !l.is_empty()).collect();
        options.push(PromptOption { label: o.label.clone(), hint: hint.join(" ") });
    }
    // The question: the paragraph right above the options (the one with a «?» if there is one close by).
    let first = opts[0].row;
    // Paragraphs above the options, nearest first, up to the dialog's top edge.
    let mut paras: Vec<Vec<&str>> = Vec::new();
    let mut cur: Vec<&str> = Vec::new();
    let mut r = first;
    while r > 0 && first - r < 14 {
        r -= 1;
        let l = lines[r].trim();
        if is_solid_rule(lines[r]) {
            break;
        }
        if l.is_empty() || is_rule(lines[r]) {
            if !cur.is_empty() {
                cur.reverse();
                paras.push(std::mem::take(&mut cur));
            }
        } else {
            cur.push(l);
        }
    }
    if !cur.is_empty() {
        cur.reverse();
        paras.push(cur);
    }
    let qi = paras.iter().position(|p| p.iter().any(|l| l.contains('?'))).filter(|&i| i < 3).unwrap_or(0);
    let question = paras.get(qi).map(|p| p.join(" ")).unwrap_or_default();
    // What it is about (the command, the file): what is above the question, then what is between it and the options.
    let mut context = Vec::new();
    for p in paras.iter().skip(qi + 1).rev().chain(paras.iter().take(qi).rev()) {
        for l in p {
            let l = l.trim_start_matches(['│', '┃']).trim();
            if !l.is_empty() && !l.starts_with("Tip:") && context.len() < 8 {
                context.push(l.to_string());
            }
        }
    }
    let mut h = Fnv::default();
    h.add(&question);
    for o in &options {
        h.add(&o.label);
    }
    Some(Prompt { id: format!("{:016x}", h.0), question, context, options, selected })
}

fn footerish(line: &str) -> bool {
    let l = line.to_lowercase();
    FOOTERS.iter().any(|f| l.contains(f))
}

struct Fnv(u64);

impl Default for Fnv {
    fn default() -> Self {
        Fnv(0xcbf29ce484222325)
    }
}

impl Fnv {
    fn add(&mut self, s: &str) {
        for b in s.bytes().chain(std::iter::once(0)) {
            self.0 ^= b as u64;
            self.0 = self.0.wrapping_mul(0x100000001b3);
        }
    }
}

/// The keys that choose option `to` of a question whose selected option is `from`.
pub fn keys_for(from: u32, to: u32) -> Vec<&'static str> {
    let mut k = Vec::new();
    let key = if to > from { "down" } else { "up" };
    for _ in 0..from.abs_diff(to) {
        k.push(key);
    }
    k.push("enter");
    k
}

/// The last few non-empty rows, where programs draw their status.
fn bottom(rows: &[String], n: usize) -> Vec<&str> {
    rows.iter().rev().map(|l| l.trim()).filter(|l| !l.is_empty()).take(n).collect()
}

/// Whether the agent is working: Claude Code and Codex both say «esc to interrupt» while they do.
pub fn working(rows: &[String]) -> bool {
    bottom(rows, 8).iter().any(|l| l.to_lowercase().contains("esc to interrupt"))
}

/// The pane's status for the phone (see [`crate::proto::PaneView::status`]).
pub fn status(rows: &[String], agent: &str, prompt: bool, running: bool) -> &'static str {
    if prompt {
        "waiting"
    } else if !agent.is_empty() && working(rows) {
        "working"
    } else if !agent.is_empty() {
        "finished"
    } else if running {
        "running"
    } else {
        "idle"
    }
}

/// Which agent draws this screen, from its footer (for a pane running ssh, where Warp can't tell).
pub fn agent(rows: &[String]) -> &'static str {
    let b = bottom(rows, 8).join("\n");
    const CLAUDE: [&str; 8] = ["? for shortcuts", "mode on", "⏵⏵", "⏸", "shift+tab to cycle", "Tab to amend", "/effort", "Claude Code"];
    const CODEX: [&str; 4] = ["context left", "OpenAI Codex", "⏎ send", "Ctrl+J newline"];
    if CODEX.iter().any(|m| b.contains(m)) {
        "codex"
    } else if CLAUDE.iter().any(|m| b.contains(m)) || b.to_lowercase().contains("esc to interrupt") {
        "claude"
    } else {
        ""
    }
}

/// The tmux session the screen shows: the default status line starts with «[name] ». tmux cuts it at 10 characters
/// (status-left-length), so a longer name shows as its first 9 («[phone-dem0:zsh*»): then a pattern, «phone-dem*».
pub fn tmux_session(rows: &[String]) -> Option<String> {
    let last = rows.iter().rev().find(|l| !l.trim().is_empty())?;
    let rest = last.strip_prefix('[')?;
    if let Some(end) = rest.find("] ") {
        let name = &rest[..end];
        return valid_name(name).then(|| name.to_string());
    }
    let start: String = rest.chars().take(9).collect();
    (start.chars().count() == 9 && valid_name(&start)).then(|| format!("{start}*"))
}

/// The tmux session to look in: the screen's, or the command's when the screen's is cut and the command's fits it.
pub fn pick_tmux(screen: Option<String>, command: Option<String>) -> Option<String> {
    match (screen, command) {
        (Some(s), Some(c)) if s.ends_with('*') && c.starts_with(s.trim_end_matches('*')) => Some(c),
        (Some(s), _) => Some(s),
        (None, c) => c,
    }
}

/// A tmux session or host name that is safe to pass on: letters, digits and `_ . -` (and `@` for user@host).
pub fn valid_name(s: &str) -> bool {
    !s.is_empty() && s.len() <= 64 && !s.starts_with('-') && s.chars().all(|c| c.is_ascii_alphanumeric() || "_.-@".contains(c))
}

#[cfg(test)]
mod tests {
    use super::*;

    fn rows(s: &str) -> Vec<String> {
        s.lines().map(|l| l.to_string()).collect()
    }

    // Claude Code 2.1, inside tmux (status line at the bottom), as captured.
    const PERMISSION: &str = "\
❯ Create a file named bye.txt containing bye, then run ls -la

  Creating bye.txt and listing the directory
  ⎿  $ printf 'bye\\n' > /tmp/calib/bye.txt && ls -la /tmp/calib

────────────────────────────────────────────────────────────────────
 Bash command
 Tip: auto mode handles these prompts for you — choose \"switch to auto mode\" below
 Create bye.txt and list the directory
╌╌╌╌╌╌╌╌╌╌╌╌╌╌╌╌╌╌╌╌╌╌╌╌╌╌╌╌╌╌╌╌╌╌╌╌╌╌╌╌╌╌╌╌╌╌╌╌╌╌╌╌╌╌╌╌╌╌╌╌╌╌╌╌╌╌╌╌
 │ printf 'bye\\n' > /tmp/calib/bye.txt &&
 │ ls -la /tmp/calib
╌╌╌╌╌╌╌╌╌╌╌╌╌╌╌╌╌╌╌╌╌╌╌╌╌╌╌╌╌╌╌╌╌╌╌╌╌╌╌╌╌╌╌╌╌╌╌╌╌╌╌╌╌╌╌╌╌╌╌╌╌╌╌╌╌╌╌╌
 Do you want to proceed?
 ❯ 1. Yes
   2. Yes, and always allow access to
      /tmp/calib from this
      project
   3. Yes, and switch to auto mode · auto mode handles these prompts for you
   4. No

 Esc to cancel · Tab to amend
[calib] 0:claude*                                          \"host\" 18:51 23-Oct-26";

    const TRUST: &str = "\
 Accessing workspace:

 /tmp/calib

 Quick safety check: Is this a project you created or one you trust? (Like your own code, a
 well-known open source project, or work from your team). If not, take a moment to review what's in
 this folder first.

 Claude Code'll be able to read, edit, and execute files here.

 Security guide

 ❯ No, exit
   Yes, I trust this folder

 Enter to confirm · Esc to cancel
";

    const IDLE: &str = "\
❯ Create a file named hello.txt containing hi, then run ls

  Listed 1 directory

● I created hello.txt containing hi, then ran ls there.

✻ Cogitated for 5s · done 6:51 PM

────────────────────────────────────────────────────────────────────
❯
────────────────────────────────────────────────────────────────────
  ⏸ manual mode on · ? for shortcuts · ← 1 agent
";

    const WORKING: &str = "\
❯ Create a file named bye.txt

✻ Baking… (3s)

────────────────────────────────────────────────────────────────────
❯
────────────────────────────────────────────────────────────────────
  ⏸ manual mode on · esc to interrupt · ← 1 agent
";

    #[test]
    fn permission_prompt() {
        let r = rows(PERMISSION);
        let p = prompt(&r).expect("prompt");
        assert_eq!(p.question, "Do you want to proceed?");
        let labels: Vec<&str> = p.options.iter().map(|o| o.label.as_str()).collect();
        assert_eq!(labels, ["Yes", "Yes, and always allow access to", "Yes, and switch to auto mode · auto mode handles these prompts for you", "No"]);
        assert_eq!(p.options[1].hint, "/tmp/calib from this project");
        assert_eq!(p.selected, 0);
        assert_eq!(p.context, ["Bash command", "Create bye.txt and list the directory", "printf 'bye\\n' > /tmp/calib/bye.txt &&", "ls -la /tmp/calib"]);
        assert_eq!(status(&r, "claude", true, true), "waiting");
        assert_eq!(agent(&r), "claude");
        assert_eq!(tmux_session(&r).as_deref(), Some("calib"));
        // Moving the cursor changes the selection, not the id.
        let moved = PERMISSION.replace(" ❯ 1. Yes", "   1. Yes").replace("   2. Yes, and always", " ❯ 2. Yes, and always");
        let q = prompt(&rows(&moved)).unwrap();
        assert_eq!((q.selected, &q.id), (1, &p.id));
        assert_eq!(keys_for(1, 3), ["down", "down", "enter"]);
        assert_eq!(keys_for(2, 0), ["up", "up", "enter"]);
        assert_eq!(keys_for(0, 0), ["enter"]);
    }

    #[test]
    fn trust_dialog() {
        let p = prompt(&rows(TRUST)).expect("prompt");
        assert!(p.question.starts_with("Quick safety check: Is this a project you created"), "{}", p.question);
        assert!(p.question.ends_with("this folder first."), "{}", p.question);
        let labels: Vec<&str> = p.options.iter().map(|o| o.label.as_str()).collect();
        assert_eq!(labels, ["No, exit", "Yes, I trust this folder"]);
        assert_eq!(p.selected, 0);
        assert_eq!(keys_for(p.selected, 1), ["down", "enter"]);
    }

    #[test]
    fn no_prompt_when_idle_or_working() {
        // The echo of the message («❯ Create…») and the empty input box are not options.
        let r = rows(IDLE);
        assert!(prompt(&r).is_none());
        assert_eq!(status(&r, "claude", false, true), "finished");
        assert_eq!(agent(&r), "claude");
        let w = rows(WORKING);
        assert!(prompt(&w).is_none());
        assert_eq!(status(&w, "claude", false, true), "working");
        assert_eq!(tmux_session(&w), None);
        // A numbered list in the agent's answer has no cursor.
        let list = rows("Steps:\n  1. Build it\n  2. Test it\n  3. Ship it\n\n❯ \n");
        assert!(prompt(&list).is_none());
        // A shell.
        let sh = rows("PS C:\\> dir\n\n    Directory: C:\\\n\nPS C:\\> ");
        assert!(prompt(&sh).is_none());
        assert_eq!(status(&sh, "", false, false), "idle");
        assert_eq!(agent(&sh), "");
    }

    #[test]
    fn codex_and_menus() {
        let codex = rows(
            "Would you like to run the following command?\n\n  $ cargo test\n\n› 1. Yes, proceed (y)\n  2. Yes, and don't ask again for this command (a)\n  3. No, and tell Codex what to do differently (esc)\n\n  Press enter to confirm or esc to cancel\n",
        );
        let p = prompt(&codex).expect("codex");
        assert_eq!(p.question, "Would you like to run the following command?");
        assert_eq!(p.context, ["$ cargo test"]);
        assert_eq!(p.options.len(), 3);
        assert_eq!(p.options[2].label, "No, and tell Codex what to do differently (esc)");
        // AskUserQuestion: options with descriptions under them, inside a box.
        let ask = rows(
            "╭──────────────────────────────────────────╮\n│ Which database should we use?            │\n│                                          │\n│ ❯ 1. Postgres                            │\n│      Robust and boring                   │\n│   2. SQLite                              │\n│      One file                            │\n│   3. Type something.                     │\n╰──────────────────────────────────────────╯\n Enter to select · ↑/↓ to navigate · Esc to cancel\n",
        );
        let p = prompt(&ask).expect("menu");
        assert_eq!(p.question, "Which database should we use?");
        assert_eq!(p.options.iter().map(|o| o.label.as_str()).collect::<Vec<_>>(), ["Postgres", "SQLite", "Type something."]);
        assert_eq!(p.options[0].hint, "Robust and boring");
        assert!(valid_name("phone-demo") && valid_name("me@fr") && !valid_name("-oProxyCommand=x") && !valid_name("a;b"));
        // tmux's status line, whole and cut (as captured over ssh).
        let cut = rows("❯ \n[phone-dem0:zsh*                 agents: 4 blocked · 6 idle \"ubuntu@vnic-main:/tmp\" 20:05 23-Oct-26");
        assert_eq!(tmux_session(&cut).as_deref(), Some("phone-dem*"));
        assert_eq!(pick_tmux(tmux_session(&cut), Some("phone-demo".into())).as_deref(), Some("phone-demo"));
        assert_eq!(pick_tmux(tmux_session(&cut), Some("other".into())).as_deref(), Some("phone-dem*"));
        assert_eq!(pick_tmux(None, Some("x".into())).as_deref(), Some("x"));
        assert_eq!(tmux_session(&rows("[a;b] 0:zsh*")), None);
    }
}
