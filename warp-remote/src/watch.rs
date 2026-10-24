//! Following one pane for a phone (protocol v2 `watch`): its screen, which Warp sends on every poll, and the
//! conversation of the Claude Code or Codex running in it, from its transcript:
//!
//! - on this computer: the file Warp names (Claude Code's session of that pane's process, or Codex's newest
//!   conversation in its folder), read from where it was left on each poll;
//! - over ssh (a pane running `ssh host`, also through a wrapper such as a `.bat` or a PowerShell function, which
//!   Warp sees through to the ssh it starts; also with tmux): with the user's own ssh setup, `ssh -T -n -o
//!   BatchMode=yes host` runs [`REMOTE_SCRIPT`], which finds the conversation (Claude Code's `sessions/<pid>.json` of a
//!   live process in the tmux session the pane shows, or the file the Codex running there has open) and prints what
//!   is new in it since the last time, and ends. One such ssh when the pane's screen changes (at most every 1.5 s,
//!   and once more a moment later) and every 15 s otherwise. It only reads, and nothing of it stays on that host.
//!
//! A subagent of that Claude Code (protocol v3, `watch` with `agent`) is followed the same way from its own file,
//! `<session>/subagents/agent-<id>.jsonl`, read-only. Every 15 s the pane's watch also lists that folder (which
//! subagents are working, see [`crate::chat::Listed`]).
//!
//! Nothing runs while no phone watches a pane: a watch exists only between `watch` and `unwatch` (or the end of the
//! connection, or a minute without a poll).

use std::{
    io::{Read, Seek, SeekFrom},
    path::{Path, PathBuf},
    sync::atomic::{AtomicU64, Ordering},
    time::{Duration, Instant, SystemTime},
};

#[cfg(not(windows))]
use std::process::{Command, Stdio};

use data_encoding::BASE64;

use crate::{
    chat::{Agent, Chat, Listed, parse_ts},
    proto::{AgentInfo, PaneView},
    screen,
};

/// A pane's text as Warp sends it: scrollback rows first, then the `rows` rows of the screen.
#[derive(Debug, Clone, Default, PartialEq)]
pub struct PaneScreen {
    pub rows: usize,
    pub lines: Vec<String>,
}

impl PaneScreen {
    /// The rows on screen now.
    pub fn visible(&self) -> &[String] {
        &self.lines[self.lines.len().saturating_sub(self.rows.max(1))..]
    }
}

/// What Warp knows about a pane: the agent it runs (Claude Code / Codex, when Warp can tell), the command running,
/// its folder, for an agent running on this computer the path of its transcript, and the command line of the ssh
/// running under it (also when the command is a wrapper around ssh; empty from an older Warp).
#[derive(Debug, Clone, Default, PartialEq)]
pub struct PaneInfo {
    pub agent: String,
    pub running: String,
    pub cwd: String,
    pub transcript: String,
    pub ssh: String,
}

/// Typing into a pane: pasted text, or one of [`crate::proto::KEYS`].
#[derive(Debug, Clone, PartialEq)]
pub enum PaneInput {
    Text(String),
    Key(String),
}

/// How much of a transcript is read when a watch starts (its end; older items come from this part only).
const INITIAL_TAIL: u64 = 3_000_000;
/// Longest transcript line kept (a longer one is skipped).
const MAX_LINE: usize = 4 << 20;
/// Size of the chat items of one answer, about (a frame is at most 1 MB).
const BUDGET: usize = 500_000;
/// Rows of text sent for a pane shown as a screen, and their length.
const MAX_ROWS: usize = 400;
const MAX_COLS: usize = 400;
/// How often what Warp knows about the pane is asked again (the agent may start, a conversation may be cleared).
const INFO_EVERY: Duration = Duration::from_secs(10);
/// How long before a remote search that found nothing is tried again (longer after each miss, up to 5 times): an
/// agent writes where its conversation is only once it is past its first dialog.
const RETRY_REMOTE: Duration = Duration::from_secs(6);
/// A fetch over ssh: after the screen changed, at most this often (a subagent's while it works); once more this long
/// after that (the transcript may be written a moment after the screen); otherwise this often; and an ssh that takes
/// longer than this is ended.
const FETCH_MIN: Duration = Duration::from_millis(1500);
const FETCH_SUB: Duration = Duration::from_secs(4);
const FETCH_TRAIL: Duration = Duration::from_millis(2500);
const FETCH_MAX: Duration = Duration::from_secs(15);
const FETCH_TIMEOUT: Duration = Duration::from_secs(30);
/// How often the session's subagents are listed (with a fetch over ssh, or from the folder on this computer).
const LIST_EVERY: Duration = Duration::from_secs(15);
/// Subagents listed: the ones that wrote in the last 12 hours, at most this many.
const LIST_MAX: usize = 30;
const LIST_AGE: Duration = Duration::from_secs(12 * 3600);

/// Runs on the ssh host (POSIX sh, any shell as login shell), reads and ends. $1: the tmux session the pane shows
/// ("-": none; a name ending in `*` is the start of one, as tmux's status line cut it), $2: claude, codex or auto,
/// $3: the transcript ("-": find it), $4: where the last fetch ended in it (bytes; negative: that much before its
/// end), $5: 1 to list the session's subagents.
///
/// Finding it prints `@claude <path>`, `@codex <path>` or `@none`; listing prints `@agent <id> <1 working | 0> <first
/// record's time | -> <last write, Unix s | -> <its meta.json>` for each subagent that wrote in the last 12 hours; then
/// come the transcript's lines from $4 (from the last 3 MB the first time; only the record types the phone shows, and
/// only whole lines) and `@next <offset>`, where the next fetch starts. It reads only transcripts (`.jsonl` under
/// `.claude/projects` or `.codex/sessions`) and what Claude Code writes about them; for Codex in tmux, `tmux
/// list-panes` and `ps` say which process runs in that session, and `/proc` which conversation it has open.
pub const REMOTE_SCRIPT: &str = r#"h=$1 a=$2 t=$3 o=$4 l=$5
if [ "$t" = - ]; then
  d=${CLAUDE_CONFIG_DIR:-$HOME/.claude}
  t= k= b=-1
  if [ "$a" != codex ]; then
    for f in "$d"/sessions/*.json; do
      p=${f##*/}; p=${p%.json}
      case $p in ''|*[!0-9]*) continue ;; esac
      kill -0 "$p" 2>/dev/null || continue
      j=$(head -c 65536 "$f" | tr -d '\n')
      m=$(printf '%s' "$j" | sed -n 's/.*"tmux":"\([^":]*\):.*/\1/p')
      if [ "$h" = - ]; then [ -z "$m" ] || continue; else case $m in $h) ;; *) continue ;; esac; fi
      s=$(printf '%s' "$j" | sed -n 's/.*"sessionId":"\([^"]*\)".*/\1/p')
      c=$(printf '%s' "$j" | sed -n 's/.*"cwd":"\([^"]*\)".*/\1/p')
      u=$(printf '%s' "$j" | sed -n 's/.*"updatedAt":\([0-9]*\).*/\1/p')
      [ -n "$s" ] && [ -n "$c" ] && [ "${u:-0}" -gt "$b" ] || continue
      b=${u:-0} k=claude
      t=$d/projects/$(printf '%s' "$c" | sed 's/[^A-Za-z0-9]/-/g')/$s.jsonl
    done
  fi
  if [ -z "$t" ] && [ "$a" != claude ]; then
    x=${CODEX_HOME:-$HOME/.codex}/sessions
    if [ "$h" != - ] && [ -d /proc/self/fd ]; then
      r=$(tmux list-panes -a -F '#{session_name} #{pane_pid}' 2>/dev/null | while read -r n q; do case $n in ($h) printf '%s ' "$q" ;; esac; done)
      if [ -n "$r" ]; then
        for q in $(ps -A -o pid= -o ppid= 2>/dev/null | awk -v r="$r" 'BEGIN { n = split(r, w, " "); for (i = 1; i <= n; i++) s[w[i]] = 1 } { pp[$1] = $2 } END { do { c = 0; for (x in pp) if (!(x in s) && (pp[x] in s)) { s[x] = 1; c = 1 } } while (c); for (x in s) print x }'); do
          for f in /proc/"$q"/fd/*; do
            f=$(readlink "$f" 2>/dev/null) || continue
            case $f in "$x"/*/rollout-*.jsonl) t=$f k=codex; break 2 ;; esac
          done
        done
      fi
    fi
    if [ -z "$t" ]; then
      for f in $(ls -t "$x"/*/*/*/rollout-*.jsonl 2>/dev/null | head -n 20); do
        head -c 4096 "$f" | grep -q '"source":"exec"' && continue
        t=$f k=codex
        break
      done
    fi
  fi
  [ -n "$t" ] || { echo @none; exit 0; }
  echo "@$k $t"
  o=-3000000
fi
case $t in */.claude/projects/*.jsonl) e='"type":"(user|assistant|ai-title)"' ;; */.codex/sessions/*.jsonl) e='"type":"(event_msg|response_item)"' ;; *) echo @none; exit 0 ;; esac
g=${t%.jsonl}/subagents
if [ "$l" = 1 ] && [ -d "$g" ]; then
  for f in $(find "$g" -name 'agent-*.jsonl' -mmin -720 2>/dev/null | head -n 30); do
    i=${f##*/agent-}; i=${i%.jsonl}
    case $i in ''|*[!A-Za-z0-9]*) continue ;; esac
    z=1 w=
    tail -c 65536 "$f" | grep -E '"type":"(user|assistant)"' | tail -n 1 | grep -q '"stop_reason":"end_turn"' && z=0
    [ $z = 1 ] && w=$(head -n 1 "$f" | head -c 300000 | grep -o '"timestamp":"[^"]*"' | tail -n 1 | cut -d'"' -f4)
    m=$(date -r "$f" +%s 2>/dev/null)
    printf '@agent %s %s %s %s %s\n' "$i" "$z" "${w:--}" "${m:--}" "$(head -c 4000 "$g/agent-$i.meta.json" 2>/dev/null | tr -d '\n')"
  done
fi
s=$(wc -c < "$t" 2>/dev/null | tr -d ' ')
[ -n "$s" ] || { echo "@next 0"; exit 0; }
if [ "$o" -lt 0 ]; then o=$((s + o)); [ "$o" -ge 0 ] || o=0; fi
[ "$o" -le "$s" ] || o=0
tail -c +$((o + 1)) "$t" | head -c $((s - o)) | LC_ALL=C awk -v o="$o" -v total=$((s - o)) -v e="$e" '
function out(l) { if (l ~ e && (substr(l, 1, 9) != "{\"type\":\"" || l ~ ("^[{]" e))) print l }
NR > 1 { out(prev) }
{ prev = $0; n += length($0) + 1 }
END { if (NR == 0) print "@next " o; else if (n == total) { out(prev); print "@next " (o + n) } else print "@next " (o + n - length(prev) - 1) }'
"#;

/// The ssh a pane runs: the options to reuse (port, user, key, jump host, config file), the host, the tmux session its
/// remote command opens (`tmux new -A -s name`, `tmux attach -t name`) and the words of that command.
#[derive(Debug, Clone, PartialEq)]
pub struct SshTarget {
    pub opts: Vec<String>,
    pub host: String,
    pub tmux: Option<String>,
    pub words: Vec<String>,
}

/// The ssh command a pane runs. None for anything that isn't a plain `ssh host [command]`.
pub fn ssh_target(running: &str) -> Option<SshTarget> {
    let args = split_args(running);
    let exe = args.first()?.rsplit(['/', '\\']).next()?.to_ascii_lowercase();
    if exe != "ssh" && exe != "ssh.exe" {
        return None;
    }
    const WITH_VALUE: &str = "BbcDEeFIiJLlmOoPpQRSWw";
    const KEEP: &str = "pliJF";
    let mut keep = Vec::new();
    let mut i = 1;
    while i < args.len() {
        let a = &args[i];
        if a == "--" {
            i += 1;
            break;
        }
        let Some(opts) = a.strip_prefix('-') else { break };
        for (at, c) in opts.char_indices() {
            if WITH_VALUE.contains(c) {
                let inline = &opts[at + 1..];
                let value = if inline.is_empty() {
                    i += 1;
                    args.get(i)?.clone()
                } else {
                    inline.to_string()
                };
                if KEEP.contains(c) {
                    if value.starts_with('-') || value.chars().any(|ch| ch.is_control()) {
                        return None;
                    }
                    keep.push(format!("-{c}"));
                    keep.push(value);
                }
                break;
            }
        }
        i += 1;
    }
    let host = args.get(i)?.clone();
    if !screen::valid_name(&host) {
        return None;
    }
    let words: Vec<String> = args[i + 1..].iter().flat_map(|a| a.split_whitespace()).map(String::from).collect();
    let tmux = words.iter().position(|w| w.ends_with("tmux")).and_then(|t| {
        words[t..].windows(2).find(|w| w[0] == "-s" || w[0] == "-t").map(|w| w[1].trim_start_matches('=').to_string())
    });
    Some(SshTarget { opts: keep, host, tmux: tmux.filter(|n| screen::valid_name(n)), words })
}

/// Splits a command line on spaces, keeping quoted parts together (without their quotes).
fn split_args(s: &str) -> Vec<String> {
    let mut out = Vec::new();
    let mut cur = String::new();
    let mut quote = None;
    let mut any = false;
    for c in s.chars() {
        match quote {
            Some(q) if c == q => quote = None,
            Some(_) => cur.push(c),
            None if c == '"' || c == '\'' => {
                quote = Some(c);
                any = true;
            }
            None if c == ' ' || c == '\t' => {
                if any || !cur.is_empty() {
                    out.push(std::mem::take(&mut cur));
                }
                any = false;
            }
            None => cur.push(c),
        }
    }
    if any || !cur.is_empty() {
        out.push(cur);
    }
    out
}

/// A conversation on another host, fetched over ssh: one `ssh` per fetch, which prints what is new and ends.
struct Remote {
    dir: PathBuf,
    opts: Vec<String>,
    host: String,
    tmux: Option<String>,
    agent: Option<Agent>,
    /// The transcript found there: whose and where, and where the next fetch starts in it (negative: before its end).
    found: Option<(Agent, String)>,
    offset: i64,
    /// The last search found nothing; searches in a row that found nothing.
    none: bool,
    misses: u32,
    error: String,
    fetch: Option<Fetch>,
    last: Option<Instant>,
    /// The screen changed since the last fetch; one more fetch is due a moment after the last.
    dirty: bool,
    trail: bool,
    /// Fetches after a change no closer than this.
    min: Duration,
    /// Subagents are listed (the pane's own conversation); when they were last, and what the last listing found.
    lists: bool,
    listed_at: Option<Instant>,
    listing: Option<Vec<Listed>>,
}

/// One ssh running, and the files it writes to (in the sidecar's folder, read once it has ended, then deleted).
struct Fetch {
    proc: Proc,
    out: PathBuf,
    err: PathBuf,
    started: Instant,
    /// It lists the subagents.
    lists: bool,
}

impl Drop for Fetch {
    fn drop(&mut self) {
        if !self.proc.exited() {
            self.proc.end();
        }
        let _ = std::fs::remove_file(&self.out);
        let _ = std::fs::remove_file(&self.err);
    }
}

/// A transcript path as the remote script prints it, safe to send back to it: only `[A-Za-z0-9/._-]`, a `.jsonl`.
fn safe_path(p: &str) -> bool {
    p.ends_with(".jsonl") && !p.contains("..") && p.chars().all(|c| c.is_ascii_alphanumeric() || "/._-".contains(c))
}

/// A subagent id as Claude Code makes them (`a6d5e25a0889e3ee4`): only letters and digits.
fn valid_agent_id(id: &str) -> bool {
    !id.is_empty() && id.len() <= 64 && id.chars().all(|c| c.is_ascii_alphanumeric())
}

/// The file of subagent `id` of the conversation in `transcript` (`<session>.jsonl` → `<session>/subagents/agent-<id>.jsonl`).
fn subagent_path(transcript: &str, id: &str) -> Option<String> {
    let stem = transcript.strip_suffix(".jsonl")?;
    let sep = if transcript.contains('\\') && !transcript.contains('/') { '\\' } else { '/' };
    valid_agent_id(id).then(|| format!("{stem}{sep}subagents{sep}agent-{id}.jsonl"))
}

/// One `@agent` line of the remote script.
fn parse_listed(line: &str) -> Option<Listed> {
    let mut f = line.splitn(6, ' ');
    let (_, id, z, ts, m) = (f.next()?, f.next()?, f.next()?, f.next()?, f.next()?);
    let meta = serde_json::from_str(f.next().unwrap_or("").trim()).unwrap_or(serde_json::Value::Null);
    valid_agent_id(id).then(|| Listed {
        id: id.to_string(),
        running: z == "1",
        started: parse_ts(ts),
        modified: m.parse::<u64>().map(|s| s * 1000).unwrap_or(0),
        meta,
    })
}

impl Remote {
    fn new(dir: &Path, t: &SshTarget, tmux: Option<String>, agent: Option<Agent>) -> Remote {
        Remote {
            dir: dir.to_path_buf(),
            opts: t.opts.clone(),
            host: t.host.clone(),
            tmux,
            agent,
            found: None,
            offset: 0,
            none: false,
            misses: 0,
            error: String::new(),
            fetch: None,
            last: None,
            dirty: false,
            trail: false,
            min: FETCH_MIN,
            lists: true,
            listed_at: None,
            listing: None,
        }
    }

    /// A subagent's file on the same host: no search, from its last 3 MB, no listing.
    fn sub(&self, path: String) -> Remote {
        Remote {
            found: Some((Agent::Claude, path)),
            offset: -(INITIAL_TAIL as i64),
            min: FETCH_SUB,
            lists: false,
            ..Remote::new(&self.dir, &SshTarget { opts: self.opts.clone(), host: self.host.clone(), tmux: None, words: Vec::new() }, None, Some(Agent::Claude))
        }
    }

    /// Collects what the last fetch brought (once its ssh has ended) and starts the next one when it is due.
    fn step(&mut self, screen_changed: bool) -> Vec<Vec<u8>> {
        let mut lines = Vec::new();
        self.dirty |= screen_changed;
        if let Some(f) = self.fetch.as_mut() {
            if f.proc.exited() {
                let f = self.fetch.take().unwrap();
                self.finish(&f, &mut lines);
            } else if f.started.elapsed() > FETCH_TIMEOUT {
                self.fetch = None;  // ends it
                self.error = "no answer".into();
            } else {
                return lines;
            }
        }
        let due = match self.last.map(|t| t.elapsed()) {
            None => true,
            Some(e) if self.found.is_none() => e >= RETRY_REMOTE * (1 + self.misses.min(4)),
            Some(e) => (self.dirty && e >= self.min) || (self.trail && e >= FETCH_TRAIL) || e >= FETCH_MAX,
        };
        if due {
            self.trail = self.dirty;
            self.dirty = false;
            self.last = Some(Instant::now());
            match self.start() {
                Ok(f) => self.fetch = Some(f),
                Err(e) => self.error = format!("couldn't start ssh: {e}"),
            }
        }
        lines
    }

    fn start(&mut self) -> std::io::Result<Fetch> {
        static N: AtomicU64 = AtomicU64::new(0);
        let base = format!("watch-{}-{}", std::process::id(), N.fetch_add(1, Ordering::Relaxed));
        let (out, err) = (self.dir.join(format!("{base}-out.tmp")), self.dir.join(format!("{base}-err.tmp")));
        let (path, offset) = match &self.found {
            Some((_, p)) => (p.as_str(), self.offset),
            None => ("-", 0),
        };
        let lists = self.lists && self.listed_at.is_none_or(|t| t.elapsed() >= LIST_EVERY);
        // Everything in it is checked (names, the path) or fixed; the tmux session and the path in single quotes.
        let remote = format!(
            "sh -c \"$(printf %s {} | base64 -d)\" warp '{}' {} '{path}' {offset} {}",
            BASE64.encode(REMOTE_SCRIPT.as_bytes()),
            self.tmux.as_deref().unwrap_or("-"),
            self.agent.map(|a| a.name()).unwrap_or("auto"),
            if lists { 1 } else { 0 }
        );
        let mut args: Vec<String> = ["-T", "-n", "-o", "BatchMode=yes", "-o", "ConnectTimeout=10", "-o", "LogLevel=ERROR"]
            .iter()
            .chain(&["-o", "ClearAllForwardings=yes"])
            .map(|a| a.to_string())
            .collect();
        args.extend(self.opts.iter().cloned());
        args.extend(["--".to_string(), self.host.clone(), remote]);
        let o = output_file(&out)?;
        let e = match output_file(&err) {
            Ok(e) => e,
            Err(x) => {
                let _ = std::fs::remove_file(&out);
                return Err(x);
            }
        };
        match spawn_ssh(&args, o, e) {
            Ok(proc) => Ok(Fetch { proc, out, err, started: Instant::now(), lists }),
            Err(x) => {
                let _ = std::fs::remove_file(&out);
                let _ = std::fs::remove_file(&err);
                Err(x)
            }
        }
    }

    fn finish(&mut self, f: &Fetch, lines: &mut Vec<Vec<u8>>) {
        let text = std::fs::read(&f.out).unwrap_or_default();
        let mut any = false;
        let mut listing = Vec::new();
        for l in text.split(|b| *b == b'\n') {
            let l = l.strip_suffix(b"\r").unwrap_or(l);
            if l.first() == Some(&b'@') {
                any = true;
                let h = String::from_utf8_lossy(l);
                if let Some(n) = h.strip_prefix("@next ") {
                    self.offset = n.trim().parse().unwrap_or(self.offset);
                } else if h.starts_with("@agent ") {
                    listing.extend(parse_listed(&h));
                } else if h == "@none" {
                    self.none = true;
                    self.misses += 1;
                } else if let Some((who, p)) = h.strip_prefix("@claude ").map(|p| (Agent::Claude, p)).or_else(|| h.strip_prefix("@codex ").map(|p| (Agent::Codex, p))) {
                    if safe_path(p) {
                        self.found = Some((who, p.to_string()));
                        self.none = false;
                        self.misses = 0;
                    } else {
                        self.error = "the conversation's path has characters it can't be read with".into();
                    }
                }
            } else if !l.is_empty() && l.len() <= MAX_LINE && self.found.is_some() {
                lines.push(l.to_vec());
            }
        }
        if f.lists && any && self.found.is_some() {
            self.listed_at = Some(Instant::now());
            self.listing = Some(listing);
        }
        // ssh's own messages (can't connect, host key, permission denied): the last one says why.
        let err = std::fs::read_to_string(&f.err).unwrap_or_default();
        let last = err.lines().rev().find(|l| !l.trim().is_empty()).unwrap_or("").trim();
        if any {
            self.error.clear();
        } else {
            self.error = if last.is_empty() { "no answer".into() } else { last.chars().take(200).collect() };
        }
    }
}

/// A new file for what ssh writes. On Windows opened for overlapped I/O, which is how ssh (OpenSSH for Windows) writes
/// its output: to a handle opened without it, it stops after 100 to 200 KB (measured), and a conversation's first fetch
/// is often more (opened with it, 12 MB came in 0.7 s).
fn output_file(path: &Path) -> std::io::Result<std::fs::File> {
    let mut o = std::fs::OpenOptions::new();
    o.write(true).create_new(true);
    #[cfg(windows)]
    std::os::windows::fs::OpenOptionsExt::custom_flags(&mut o, 0x4000_0000);  // FILE_FLAG_OVERLAPPED
    o.open(path)
}

/// Starts ssh with these arguments, writing to these files.
#[cfg(not(windows))]
fn spawn_ssh(args: &[String], out: std::fs::File, err: std::fs::File) -> std::io::Result<Proc> {
    Command::new("ssh").args(args).stdin(Stdio::null()).stdout(Stdio::from(out)).stderr(Stdio::from(err)).spawn().map(Proc)
}

#[cfg(not(windows))]
struct Proc(std::process::Child);

#[cfg(not(windows))]
impl Proc {
    fn exited(&mut self) -> bool {
        matches!(self.0.try_wait(), Ok(Some(_)))
    }

    fn end(&mut self) {
        let _ = self.0.kill();
        let _ = self.0.wait();
    }
}

/// On Windows, ssh (OpenSSH for Windows 9.5) writes all it receives only to a file nobody else has open, opened for
/// overlapped I/O (see [`output_file`]): to a pipe it stops after the first write. It gets a hidden console
/// (CREATE_NEW_CONSOLE with SW_HIDE, as `Start-Process -WindowStyle Hidden`: never a window on the screen), and what it
/// wrote is read once it has ended.
#[cfg(windows)]
fn spawn_ssh(args: &[String], out: std::fs::File, err: std::fs::File) -> std::io::Result<Proc> {
    use std::os::windows::io::AsRawHandle;
    use windows_sys::Win32::{
        Foundation::{CloseHandle, HANDLE_FLAG_INHERIT, SetHandleInformation},
        System::Threading::{CREATE_NEW_CONSOLE, CreateProcessW, PROCESS_INFORMATION, STARTF_USESHOWWINDOW, STARTF_USESTDHANDLES, STARTUPINFOW},
    };
    let exe = std::env::var_os("PATH")
        .into_iter()
        .flat_map(|p| std::env::split_paths(&p).collect::<Vec<_>>())
        .filter(|d| d.is_absolute())
        .map(|d| d.join("ssh.exe"))
        .find(|f| f.is_file())
        .ok_or_else(|| std::io::Error::new(std::io::ErrorKind::NotFound, "ssh.exe is not in PATH"))?;
    let nul = std::fs::File::open("NUL")?;
    let wide = |s: &str| s.encode_utf16().chain(std::iter::once(0)).collect::<Vec<u16>>();
    let exe = exe.to_string_lossy().to_string();
    let mut line = wide(&std::iter::once(exe.as_str()).chain(args.iter().map(|a| a.as_str())).map(quote_arg).collect::<Vec<_>>().join(" "));
    let mut pi: PROCESS_INFORMATION = unsafe { std::mem::zeroed() };
    let ok = unsafe {
        // Only these three handles are inheritable, and they are closed right after: nothing else of this process leaks.
        for h in [nul.as_raw_handle(), out.as_raw_handle(), err.as_raw_handle()] {
            SetHandleInformation(h, HANDLE_FLAG_INHERIT, HANDLE_FLAG_INHERIT);
        }
        let mut si: STARTUPINFOW = std::mem::zeroed();
        si.cb = std::mem::size_of::<STARTUPINFOW>() as u32;
        si.dwFlags = STARTF_USESTDHANDLES | STARTF_USESHOWWINDOW;
        si.wShowWindow = 0;  // SW_HIDE
        si.hStdInput = nul.as_raw_handle();
        si.hStdOutput = out.as_raw_handle();
        si.hStdError = err.as_raw_handle();
        let app = wide(&exe);
        CreateProcessW(app.as_ptr(), line.as_mut_ptr(), std::ptr::null(), std::ptr::null(), 1, CREATE_NEW_CONSOLE, std::ptr::null(), std::ptr::null(), &si, &mut pi)
    };
    if ok == 0 {
        return Err(std::io::Error::last_os_error());
    }
    unsafe { CloseHandle(pi.hThread) };
    Ok(Proc(pi.hProcess as isize))
}

/// An argument as the Windows C runtime (and so ssh) splits a command line.
#[cfg(windows)]
fn quote_arg(a: &str) -> String {
    if !a.is_empty() && !a.contains([' ', '\t', '"']) {
        return a.to_string();
    }
    let mut out = String::from("\"");
    let mut slashes = 0;
    for c in a.chars() {
        match c {
            '\\' => slashes += 1,
            '"' => {
                out.extend(std::iter::repeat_n('\\', slashes * 2 + 1));
                out.push('"');
                slashes = 0;
            }
            _ => {
                out.extend(std::iter::repeat_n('\\', slashes));
                out.push(c);
                slashes = 0;
            }
        }
    }
    out.extend(std::iter::repeat_n('\\', slashes * 2));
    out.push('"');
    out
}

/// ssh's process (its handle).
#[cfg(windows)]
struct Proc(isize);

#[cfg(windows)]
impl Proc {
    fn exited(&mut self) -> bool {
        use windows_sys::Win32::{Foundation::WAIT_OBJECT_0, System::Threading::WaitForSingleObject};
        unsafe { WaitForSingleObject(self.0 as _, 0) == WAIT_OBJECT_0 }
    }

    fn end(&mut self) {
        use windows_sys::Win32::System::Threading::{TerminateProcess, WaitForSingleObject};
        unsafe {
            TerminateProcess(self.0 as _, 1);
            WaitForSingleObject(self.0 as _, 3000);
        }
    }
}

#[cfg(windows)]
impl Drop for Proc {
    fn drop(&mut self) {
        unsafe { windows_sys::Win32::Foundation::CloseHandle(self.0 as _) };
    }
}

/// Removes what fetches left in the sidecar's folder (it never keeps any between runs).
pub fn clean_up(dir: &std::path::Path) {
    for e in std::fs::read_dir(dir).into_iter().flatten().flatten() {
        let name = e.file_name().to_string_lossy().to_string();
        if name.starts_with("watch-") && name.ends_with(".tmp") {
            let _ = std::fs::remove_file(e.path());
        }
    }
}

/// The subagents of the conversation in `transcript` on this computer, as [`REMOTE_SCRIPT`] lists them over ssh.
fn local_listing(transcript: &Path) -> Vec<Listed> {
    let dir = transcript.with_extension("").join("subagents");
    let Ok(rd) = std::fs::read_dir(&dir) else { return Vec::new() };
    let now = SystemTime::now();
    let mut out: Vec<Listed> = Vec::new();
    for e in rd.flatten() {
        let name = e.file_name().to_string_lossy().to_string();
        let Some(id) = name.strip_prefix("agent-").and_then(|n| n.strip_suffix(".jsonl")) else { continue };
        let Some(modified) = e.metadata().ok().and_then(|m| m.modified().ok()) else { continue };
        if !valid_agent_id(id) || now.duration_since(modified).unwrap_or_default() > LIST_AGE {
            continue;
        }
        let path = e.path();
        let running = !ends_its_turn(&path);
        let started = if running { first_time(&path) } else { 0 };
        let meta = std::fs::File::open(dir.join(format!("agent-{id}.meta.json")))
            .ok()
            .and_then(|f| {
                let mut b = Vec::new();
                f.take(4000).read_to_end(&mut b).ok()?;
                serde_json::from_slice(&b).ok()
            })
            .unwrap_or(serde_json::Value::Null);
        let modified = modified.duration_since(SystemTime::UNIX_EPOCH).map(|d| d.as_millis() as u64).unwrap_or(0);
        out.push(Listed { id: id.to_string(), running, started, modified, meta });
    }
    out.sort_by_key(|l| std::cmp::Reverse(l.modified));
    out.truncate(LIST_MAX);
    out
}

/// A subagent's file ends with its final answer (its last user or assistant record ended the turn): it stopped.
fn ends_its_turn(path: &Path) -> bool {
    let Ok(mut f) = std::fs::File::open(path) else { return true };
    let len = f.metadata().map(|m| m.len()).unwrap_or(0);
    let mut buf = Vec::new();
    if f.seek(SeekFrom::Start(len.saturating_sub(65_536))).is_err() || f.read_to_end(&mut buf).is_err() {
        return true;
    }
    let text = String::from_utf8_lossy(&buf);
    let last = text.lines().rev().find(|l| l.contains("\"type\":\"user\"") || l.contains("\"type\":\"assistant\""));
    last.is_some_and(|l| l.contains("\"stop_reason\":\"end_turn\""))
}

/// When a subagent started: the time of its first record (its task).
fn first_time(path: &Path) -> u64 {
    let Ok(f) = std::fs::File::open(path) else { return 0 };
    let mut buf = Vec::new();
    if f.take(300_000).read_to_end(&mut buf).is_err() {
        return 0;
    }
    let first = buf.split(|b| *b == b'\n').next().unwrap_or(&[]);
    let text = String::from_utf8_lossy(first);
    text.rfind("\"timestamp\":\"").map(|i| parse_ts(text[i + 13..].split('"').next().unwrap_or(""))).unwrap_or(0)
}

enum Source {
    None,
    /// A transcript on this computer: where it was left, and the start of a line not finished yet; when its subagents
    /// were last listed.
    Local { path: PathBuf, offset: Option<u64>, carry: Vec<u8>, listed_at: Option<Instant> },
    Remote(Box<Remote>),
}

pub struct Watch {
    pub window: u64,
    pub pane_id: u32,
    pub last_poll: Instant,
    /// The sidecar's data folder (where ssh for Windows writes what it gets, see [`spawn_ssh`]).
    dir: PathBuf,
    info: PaneInfo,
    info_at: Instant,
    agent: String,
    source: Source,
    chat: Option<Chat>,
    /// A subagent's conversation (by its id), and what the pane's conversation says of it now.
    sub: Option<String>,
    sub_info: Option<AgentInfo>,
    /// Revision: counts every change this phone hasn't seen.
    rev: u64,
    screen_hash: u64,
    screen_rev: u64,
    kind: &'static str,
    kind_rev: u64,
    note: String,
}

fn hash_lines(lines: &[String]) -> u64 {
    let mut h: u64 = 0xcbf29ce484222325;
    for l in lines {
        for b in l.bytes().chain(std::iter::once(b'\n')) {
            h = (h ^ b as u64).wrapping_mul(0x100000001b3);
        }
    }
    h
}

impl Watch {
    pub fn new(dir: &Path, window: u64, pane_id: u32, info: PaneInfo, screen: &PaneScreen) -> Watch {
        let mut w = Watch::empty(dir, window, pane_id);
        w.set_info(info, screen);
        w
    }

    fn empty(dir: &Path, window: u64, pane_id: u32) -> Watch {
        // Revisions start at the clock (ms): a watch made after a reconnection is always newer than what the phone
        // has, so its first answer starts the pane over.
        let start = SystemTime::now().duration_since(SystemTime::UNIX_EPOCH).map(|d| d.as_millis() as u64).unwrap_or(1);
        Watch {
            window,
            pane_id,
            last_poll: Instant::now(),
            dir: dir.to_path_buf(),
            info: PaneInfo::default(),
            info_at: Instant::now(),
            agent: String::new(),
            source: Source::None,
            chat: None,
            sub: None,
            sub_info: None,
            rev: start,
            screen_hash: 0,
            screen_rev: 0,
            kind: "screen",
            kind_rev: start,
            note: String::new(),
        }
    }

    /// The conversation of subagent `id` of this pane's Claude Code, from its own file next to the pane's (on this
    /// computer or over the same ssh). Err: why not.
    pub fn subagent(&self, id: &str) -> Result<Watch, String> {
        if !valid_agent_id(id) {
            return Err("no such subagent".into());
        }
        let mut w = Watch::empty(&self.dir, self.window, self.pane_id);
        w.source = match &self.source {
            Source::Local { path, .. } if self.chat.as_ref().is_some_and(|c| c.agent() == Agent::Claude) => {
                let p = subagent_path(&path.to_string_lossy(), id).ok_or("no such subagent")?;
                Source::Local { path: PathBuf::from(p), offset: None, carry: Vec::new(), listed_at: None }
            }
            Source::Remote(r) if matches!(r.found, Some((Agent::Claude, _))) => {
                let p = r.found.as_ref().and_then(|(_, t)| subagent_path(t, id)).filter(|p| safe_path(p)).ok_or("no such subagent")?;
                Source::Remote(Box::new(r.sub(p)))
            }
            _ => return Err("this pane's conversation isn't open".into()),
        };
        w.agent = "claude".into();
        w.chat = Some(Chat::subagent());
        w.sub = Some(id.to_string());
        w.sub_info = self.chat.as_ref().and_then(|c| c.agent_info(id)).cloned();
        w.kind = "chat";
        w.note = "subagent".into();
        Ok(w)
    }

    /// What the pane's conversation says of this subagent now (its status, what it took).
    pub fn set_sub_info(&mut self, info: Option<AgentInfo>) {
        if info.is_some() {
            self.sub_info = info;
        }
    }

    /// The subagent `id` as this pane's conversation knows it.
    pub fn agent_info(&self, id: &str) -> Option<AgentInfo> {
        self.chat.as_ref().and_then(|c| c.agent_info(id)).cloned()
    }

    /// Time to ask Warp about the pane again.
    pub fn info_due(&self) -> bool {
        self.sub.is_none() && self.info_at.elapsed() >= INFO_EVERY
    }

    /// What Warp says about the pane now: the agent may have started or ended, or moved to another conversation.
    pub fn set_info(&mut self, info: PaneInfo, screen: &PaneScreen) {
        if self.sub.is_some() {
            return;
        }
        self.info_at = Instant::now();
        // The ssh under the pane (Warp sees through a wrapper such as `fr fnviewer` to it), or the command itself.
        let ssh = ssh_target(if info.ssh.is_empty() { &info.running } else { &info.ssh });
        let visible = screen.visible();
        let found = match &self.source {
            Source::Remote(r) => r.found.as_ref().map(|(a, _)| a.name()),
            _ => None,
        };
        let agent = if !info.agent.is_empty() {
            info.agent.clone()
        } else if ssh.is_some() {
            let seen = screen::agent(visible);
            if seen.is_empty() { found.unwrap_or("").to_string() } else { seen.to_string() }
        } else {
            String::new()
        };
        let kind = match agent.as_str() {
            "claude" => Some(Agent::Claude),
            "codex" => Some(Agent::Codex),
            _ => None,
        };
        let tmux = ssh.as_ref().and_then(|t| screen::pick_tmux(screen::tmux_session(visible), t.tmux.clone(), &t.words));
        let local = !info.transcript.is_empty() && kind.is_some();
        // Over ssh: an agent the screen shows, or one that may be in the tmux session it shows (looked for there).
        let remote = !local && ssh.is_some() && (kind.is_some() || tmux.is_some());
        let keep = match &self.source {
            Source::Local { path, .. } => local && path.as_os_str() == info.transcript.as_str(),
            Source::Remote(r) => remote && ssh.as_ref().is_some_and(|t| t.host == r.host) && r.tmux == tmux,
            Source::None => !local && !remote,
        };
        self.info = info;
        self.agent = agent;
        if keep {
            return;
        }
        self.chat = None;
        self.note.clear();
        self.source = Source::None;
        if local {
            self.source = Source::Local { path: PathBuf::from(&self.info.transcript), offset: None, carry: Vec::new(), listed_at: None };
            self.chat = Some(Chat::new(kind.unwrap()));
            self.note = "this computer".into();
        } else if let (true, Some(t)) = (remote, ssh) {
            self.note = format!("Looking for the conversation on {}…", t.host);
            self.source = Source::Remote(Box::new(Remote::new(&self.dir, &t, tmux, kind)));
        }
    }

    /// New lines of the transcript, into the chat, and the subagents listed. `screen_changed`: a fetch over ssh may be due.
    fn read_transcript(&mut self, screen_changed: bool) {
        let mut lines: Vec<Vec<u8>> = Vec::new();
        let mut listing = None;
        match &mut self.source {
            Source::None => return,
            Source::Local { path, offset, carry, listed_at } => {
                let Ok(mut f) = std::fs::File::open(&*path) else { return };
                let len = f.metadata().map(|m| m.len()).unwrap_or(0);
                let mut skip_first = false;
                let at = match *offset {
                    Some(o) if o <= len => o,
                    _ => {
                        // First read, or the file started over: from its last part.
                        carry.clear();
                        skip_first = len > INITIAL_TAIL;
                        len.saturating_sub(INITIAL_TAIL)
                    }
                };
                let want = (len - at).min(8 << 20);
                let mut buf = Vec::with_capacity(want as usize);
                if f.seek(SeekFrom::Start(at)).is_err() || f.take(want).read_to_end(&mut buf).is_err() {
                    return;
                }
                *offset = Some(at + buf.len() as u64);
                carry.extend_from_slice(&buf);
                let mut start = 0;
                while let Some(n) = carry[start..].iter().position(|b| *b == b'\n') {
                    if skip_first {
                        skip_first = false;
                    } else if n <= MAX_LINE {
                        lines.push(carry[start..start + n].to_vec());
                    }
                    start += n + 1;
                }
                carry.drain(..start);
                if carry.len() > MAX_LINE {
                    carry.clear();
                }
                if self.sub.is_none() && listed_at.is_none_or(|t| t.elapsed() >= LIST_EVERY) && self.chat.as_ref().is_some_and(|c| c.agent() == Agent::Claude) {
                    *listed_at = Some(Instant::now());
                    listing = Some(local_listing(path));
                }
            }
            Source::Remote(remote) => {
                lines = remote.step(screen_changed);
                listing = remote.listing.take();
                if self.chat.is_none()
                    && let Some((who, _)) = &remote.found
                {
                    self.chat = Some(Chat::new(*who));
                    if self.agent.is_empty() {
                        self.agent = who.name().into();
                    }
                }
                self.note = if self.sub.is_some() {
                    if remote.error.is_empty() { "subagent".into() } else { format!("ssh: {}", remote.error) }
                } else if remote.found.is_some() {
                    "over ssh".into()
                } else if remote.none {
                    format!("No {} conversation found there (yet).", if self.agent == "codex" { "Codex" } else { "Claude Code" })
                } else if !remote.error.is_empty() {
                    format!("ssh: {}", remote.error)
                } else {
                    format!("Looking for the conversation on {}…", remote.host)
                };
            }
        }
        let Some(chat) = self.chat.as_mut() else { return };
        if !lines.is_empty() {
            self.rev += 1;
            for l in &lines {
                chat.feed(l, self.rev);
            }
        }
        if let Some(list) = listing {
            chat.merge_listed(&list, self.rev + 1);
            if chat.agents_rev == self.rev + 1 {
                self.rev += 1;
            }
        }
    }

    /// The pane for the phone: what changed since revision `since`, or the items before `before`.
    pub fn poll(&mut self, screen: &PaneScreen, since: u64, before: Option<u64>, typing: bool) -> PaneView {
        self.last_poll = Instant::now();
        if self.sub.is_some() {
            return self.poll_sub(since, before);
        }
        let unknown = since > self.rev;  // a revision of an earlier watch (the clock went back): start over
        let visible = screen.visible();
        let h = hash_lines(&screen.lines);
        let changed = h != self.screen_hash;
        if changed {
            self.screen_hash = h;
            self.rev += 1;
            self.screen_rev = self.rev;
        }
        self.read_transcript(changed);
        let kind = if self.chat.is_some() { "chat" } else { "screen" };
        if kind != self.kind {
            self.kind = kind;
            self.rev += 1;
            self.kind_rev = self.rev;
        }
        let prompt = screen::prompt(visible);
        let status = screen::status(visible, &self.agent, prompt.is_some(), !self.info.running.is_empty());
        let reset = before.is_none() && (since == 0 || since < self.kind_rev || unknown);
        let mut activity = if self.agent == "claude" { screen::activity(visible) } else { String::new() };
        if activity.is_empty() && status == "working" {
            activity = self.chat.as_ref().and_then(|c| c.pending_tool()).unwrap_or("").to_string();
        }
        let mut v = PaneView {
            window: self.window,
            pane_id: self.pane_id,
            rev: self.rev,
            kind: kind.into(),
            agent: self.agent.clone(),
            status: status.into(),
            reset,
            prompt,
            typing,
            note: self.note.clone(),
            activity,
            ..Default::default()
        };
        if let Some(chat) = &self.chat {
            self.fill(&mut v, since, before, reset);
            v.title = chat.title.clone();
        } else if before.is_none() && (reset || self.screen_rev > since) {
            let from = screen.lines.len().saturating_sub(MAX_ROWS);
            v.screen = Some(screen.lines[from..].iter().map(|l| l.chars().take(MAX_COLS).collect()).collect());
        }
        v
    }

    /// The chat's items (and subagents, and when the turn started) for an answer.
    fn fill(&self, v: &mut PaneView, since: u64, before: Option<u64>, reset: bool) {
        let Some(chat) = &self.chat else { return };
        let (items, more) = match before {
            Some(b) => chat.before(b, 60, BUDGET),
            None if reset => chat.since(0, 80, BUDGET),
            None => chat.since(since, usize::MAX, BUDGET),
        };
        v.items = items;
        v.more = more;
        v.turn_start = chat.turn_start;
        if before.is_none() && (reset || chat.agents_rev > since) {
            v.agents = Some(chat.agents());
        }
    }

    /// A subagent's conversation: read-only, its status from what the pane's conversation says of it.
    fn poll_sub(&mut self, since: u64, before: Option<u64>) -> PaneView {
        let unknown = since > self.rev;
        let running = self.sub_info.as_ref().is_none_or(|a| a.status == "running");
        self.read_transcript(running);
        let reset = before.is_none() && (since == 0 || since < self.kind_rev || unknown);
        let info = self.sub_info.clone().unwrap_or_default();
        let mut v = PaneView {
            window: self.window,
            pane_id: self.pane_id,
            rev: self.rev,
            kind: "chat".into(),
            agent: "claude".into(),
            status: if running { "working" } else { "finished" }.into(),
            reset,
            note: self.note.clone(),
            activity: if running { self.chat.as_ref().and_then(|c| c.pending_tool()).unwrap_or("").to_string() } else { String::new() },
            ..Default::default()
        };
        self.fill(&mut v, since, before, reset);
        v.agents = None;
        v.title = if info.description.is_empty() { info.name } else { info.description };
        v
    }
}

#[cfg(test)]
mod tests {
    #[cfg(unix)]
    use std::io::Write;

    use super::*;

    fn target(t: Option<SshTarget>) -> Option<(Vec<String>, String, Option<String>)> {
        t.map(|t| (t.opts, t.host, t.tmux))
    }

    #[test]
    fn ssh_commands() {
        assert_eq!(target(ssh_target("ssh fr")), Some((vec![], "fr".into(), None)));
        assert_eq!(
            target(ssh_target(r#"ssh -t fr "tmux -S /tmp/claude-1001/demo.sock new -A -s phone-demo""#)),
            Some((vec![], "fr".into(), Some("phone-demo".into())))
        );
        assert_eq!(
            target(ssh_target("C:\\Windows\\System32\\OpenSSH\\ssh.exe -p 2222 -i C:\\k\\id -L 80:x:80 -A me@server tmux attach -t work")),
            Some((vec!["-p".into(), "2222".into(), "-i".into(), "C:\\k\\id".into()], "me@server".into(), Some("work".into())))
        );
        assert_eq!(ssh_target("ssh -oProxyCommand=evil host").map(|t| t.host), Some("host".into()));
        assert_eq!(ssh_target("sshfs host:/ x"), None);
        assert_eq!(ssh_target("ssh -p -oX host"), None);
        assert_eq!(ssh_target("ssh 'a;b'"), None);
        assert_eq!(ssh_target("claude"), None);
        // A wrapper (`fr fnviewer`, a .bat or a PowerShell function) isn't ssh: Warp sends the ssh it started, as Windows
        // has its command line, and the session's name is a word of its remote command.
        assert_eq!(ssh_target("fr fnviewer"), None);
        let t = ssh_target(r#""C:\WINDOWS\System32\OpenSSH\ssh.exe" -t fr "~/bin/fr-session fnviewer""#).unwrap();
        assert_eq!((t.host.as_str(), t.tmux.as_deref(), t.words.as_slice()), ("fr", None, &["~/bin/fr-session".to_string(), "fnviewer".to_string()][..]));
        assert_eq!(ssh_target("ssh fr tmux attach -r -t =fnviewer").and_then(|t| t.tmux).as_deref(), Some("fnviewer"));
        assert_eq!(subagent_path("/h/.claude/projects/-h/s1.jsonl", "a6d5").as_deref(), Some("/h/.claude/projects/-h/s1/subagents/agent-a6d5.jsonl"));
        assert_eq!(subagent_path("C:\\u\\.claude\\projects\\C--x\\s1.jsonl", "a1").as_deref(), Some("C:\\u\\.claude\\projects\\C--x\\s1\\subagents\\agent-a1.jsonl"));
        assert_eq!(subagent_path("/x/s.jsonl", "../a"), None);
    }

    /// Where sh runs: the script itself against a home folder made here.
    #[cfg(unix)]
    fn run_script(home: &Path, args: &[&str]) -> Vec<String> {
        let o = std::process::Command::new("sh")
            .args(["-c", REMOTE_SCRIPT, "warp"])
            .args(args)
            .env("HOME", home)
            .env_remove("CLAUDE_CONFIG_DIR")
            .env_remove("CODEX_HOME")
            .output()
            .unwrap();
        String::from_utf8_lossy(&o.stdout).lines().map(String::from).collect()
    }

    /// The script: only whole lines of the record types the phone shows (not other records that merely contain such a
    /// type, like Claude Code's request copies), and the next fetch starts right after the last whole line, so a line
    /// still being written comes whole next time.
    #[cfg(unix)]
    #[test]
    fn remote_script_reads_whole_lines() {
        let dir = std::env::temp_dir().join(format!("warp-remote-script-{}", std::process::id()));
        let proj = dir.join(".claude/projects/x");
        std::fs::create_dir_all(&proj).unwrap();
        let t = proj.join("t.jsonl");
        let (a, b) = (r#"{"parentUuid":null,"type":"user","message":{"content":"one"}}"#, r#"{"type":"api-request-blob","messages":[{"type":"user","content":"copy"}]}"#);
        let c = r#"{"parentUuid":"1","type":"assistant","message":{"content":[{"type":"text","text":"two"}]}}"#;
        let title = r#"{"type":"ai-title","aiTitle":"T"}"#;
        std::fs::write(&t, format!("{a}\n{b}\n{title}\n{}", &c[..20])).unwrap();
        let p = t.to_str().unwrap();
        let first = a.len() + 1 + b.len() + 1 + title.len() + 1;
        assert_eq!(run_script(&dir, &["-", "claude", p, "0"]), [a.to_string(), title.to_string(), format!("@next {first}")]);
        std::fs::OpenOptions::new().append(true).open(&t).unwrap().write_all(format!("{}\n", &c[20..]).as_bytes()).unwrap();
        let end = first + c.len() + 1;
        assert_eq!(run_script(&dir, &["-", "claude", p, &first.to_string()]), [c.to_string(), format!("@next {end}")]);
        assert_eq!(run_script(&dir, &["-", "claude", p, &end.to_string()]), [format!("@next {end}")]);
        // From the end: negative.
        assert_eq!(run_script(&dir, &["-", "claude", p, &format!("-{}", c.len() + 1)]), [c.to_string(), format!("@next {end}")]);
        // Not a transcript: nothing is read.
        assert_eq!(run_script(&dir, &["-", "claude", "/etc/passwd", "0"]), ["@none"]);
        assert!(safe_path("/home/u/.claude/projects/-tmp-x/9d09-21b2.jsonl") && !safe_path("/x/'a'.jsonl") && !safe_path("/x/../y.jsonl"));
        let _ = std::fs::remove_dir_all(&dir);
    }

    /// The script finds the conversation of the Claude Code in a tmux session by its `sessions/<pid>.json` (a live
    /// process: this test's), and lists its subagents: which one is still working, when it started, its meta.json.
    #[cfg(unix)]
    #[test]
    fn remote_script_finds_and_lists() {
        let home = std::env::temp_dir().join(format!("warp-remote-find-{}", std::process::id()));
        let _ = std::fs::remove_dir_all(&home);
        let sessions = home.join(".claude/sessions");
        let proj = home.join(".claude/projects/-home-u");
        let subs = proj.join("s1/subagents");
        std::fs::create_dir_all(&sessions).unwrap();
        std::fs::create_dir_all(&subs).unwrap();
        let me = std::process::id();
        std::fs::write(sessions.join(format!("{me}.json")), r#"{"pid":1,"sessionId":"s1","cwd":"/home/u","tmux":"fnviewer:@6.%6","updatedAt":5}"#).unwrap();
        std::fs::write(proj.join("s1.jsonl"), "{\"parentUuid\":null,\"type\":\"user\",\"message\":{\"content\":\"hi\"}}\n").unwrap();
        std::fs::write(subs.join("agent-a1.jsonl"), "{\"type\":\"user\",\"isSidechain\":true,\"message\":{\"content\":\"task\"},\"timestamp\":\"2026-10-23T20:44:59.816Z\"}\n{\"type\":\"assistant\",\"message\":{\"stop_reason\":\"tool_use\",\"content\":[]}}\n").unwrap();
        std::fs::write(subs.join("agent-a1.meta.json"), r#"{"agentType":"executor","description":"Fix it","toolUseId":"toolu_1"}"#).unwrap();
        std::fs::write(subs.join("agent-b2.jsonl"), "{\"type\":\"assistant\",\"message\":{\"stop_reason\":\"end_turn\",\"content\":[]}}\n").unwrap();
        let path = proj.join("s1.jsonl").to_string_lossy().to_string();
        let out = run_script(&home, &["fnviewer", "auto", "-", "0", "1"]);
        assert_eq!(out[0], format!("@claude {path}"));
        let mut listed: Vec<Listed> = out.iter().filter(|l| l.starts_with("@agent ")).filter_map(|l| parse_listed(l)).collect();
        listed.sort_by(|a, b| a.id.cmp(&b.id));
        assert_eq!(listed.iter().map(|l| (l.id.as_str(), l.running, l.started)).collect::<Vec<_>>(), [("a1", true, 1_792_788_299_816), ("b2", false, 0)]);
        assert_eq!(listed[0].meta["agentType"], "executor");
        assert!(listed[0].modified > 0);
        // The same, read here (a pane on this computer).
        let mut local = local_listing(Path::new(&path));
        local.sort_by(|a, b| a.id.cmp(&b.id));
        assert_eq!(local.iter().map(|l| (l.id.as_str(), l.running, l.started)).collect::<Vec<_>>(), [("a1", true, 1_792_788_299_816), ("b2", false, 0)]);
        // Another session's name, or the start of a longer one as tmux's status line cut it.
        assert_eq!(run_script(&home, &["radarnite", "claude", "-", "0"]), ["@none"]);
        assert_eq!(run_script(&home, &["fnviewe*", "claude", "-", "0"])[0], format!("@claude {path}"));
        let _ = std::fs::remove_dir_all(&home);
    }

    #[test]
    fn local_transcript() {
        let dir = std::env::temp_dir().join(format!("warp-remote-watch-{}", std::process::id()));
        let _ = std::fs::create_dir_all(&dir);
        let path = dir.join("t.jsonl");
        std::fs::write(&path, "{\"type\":\"user\",\"message\":{\"content\":\"hello\"}}\n{\"type\":\"assistant\",\"mess").unwrap();
        let info = PaneInfo { agent: "claude".into(), running: "claude".into(), cwd: "C:\\x".into(), transcript: path.display().to_string(), ..Default::default() };
        let screen = PaneScreen { rows: 3, lines: vec!["❯ hello".into(), String::new(), "  ? for shortcuts".into()] };
        let mut w = Watch::new(&dir, 7, 3, info, &screen);
        let v = w.poll(&screen, 0, None, false);
        assert_eq!((v.kind.as_str(), v.status.as_str(), v.reset, v.items.len()), ("chat", "finished", true, 1));
        assert!(v.screen.is_none());
        assert_eq!(v.agents.as_deref(), Some(&[][..]));
        // The rest of the line arrives: only the new item comes, and the same rev means nothing new.
        let mut f = std::fs::OpenOptions::new().append(true).open(&path).unwrap();
        std::io::Write::write_all(&mut f, b"age\":{\"content\":[{\"type\":\"text\",\"text\":\"hi!\"}]}}\n").unwrap();
        let v2 = w.poll(&screen, v.rev, None, true);
        assert_eq!(v2.items.iter().map(|i| i.text.as_str()).collect::<Vec<_>>(), ["hi!"]);
        assert!(!v2.reset && v2.typing && v2.agents.is_none());
        let v3 = w.poll(&screen, v2.rev, None, true);
        assert!(v3.items.is_empty() && v3.rev == v2.rev);
        // After a reconnection the phone asks a new watch with its old revision: it starts over.
        let mut again = Watch::new(&dir, 7, 3, PaneInfo { transcript: path.display().to_string(), agent: "claude".into(), ..Default::default() }, &screen);
        let v4 = again.poll(&screen, v3.rev, None, true);
        assert!(v4.reset && v4.items.len() == 2, "{v4:?}");
        let _ = std::fs::remove_dir_all(&dir);
    }

    /// A subagent of a conversation on this computer: its own file, read-only, its card's status.
    #[test]
    fn local_subagent() {
        let dir = std::env::temp_dir().join(format!("warp-remote-sub-{}", std::process::id()));
        let subs = dir.join("s1/subagents");
        std::fs::create_dir_all(&subs).unwrap();
        let path = dir.join("s1.jsonl");
        std::fs::write(&path, concat!(
            r#"{"type":"assistant","message":{"content":[{"type":"tool_use","id":"toolu_1","name":"Agent","input":{"description":"Fix it","subagent_type":"executor","run_in_background":true,"prompt":"p"}}]}}"#, "\n",
            r#"{"type":"user","message":{"content":[{"type":"tool_result","tool_use_id":"toolu_1","content":"launched"}]},"toolUseResult":{"status":"async_launched","agentId":"a1"}}"#, "\n",
        )).unwrap();
        std::fs::write(subs.join("agent-a1.jsonl"), concat!(
            r#"{"type":"user","isSidechain":true,"message":{"content":"Task: fix it"},"timestamp":"2026-10-23T20:44:59.816Z"}"#, "\n",
            r#"{"type":"assistant","isSidechain":true,"message":{"stop_reason":"tool_use","content":[{"type":"tool_use","id":"r","name":"Read","input":{"file_path":"/x/a.rs"}}]}}"#, "\n",
        )).unwrap();
        std::fs::write(subs.join("agent-a1.meta.json"), r#"{"agentType":"executor","description":"Fix it","toolUseId":"toolu_1"}"#).unwrap();
        let info = PaneInfo { agent: "claude".into(), running: "claude".into(), transcript: path.display().to_string(), ..Default::default() };
        let screen = PaneScreen { rows: 1, lines: vec!["  ? for shortcuts".into()] };
        let mut w = Watch::new(&dir, 7, 3, info, &screen);
        let v = w.poll(&screen, 0, None, true);
        let agents = v.agents.unwrap();
        assert_eq!(agents.iter().map(|a| (a.id.as_str(), a.status.as_str(), a.started)).collect::<Vec<_>>(), [("a1", "running", 1_792_788_299_816)]);
        assert_eq!(v.items[0].agent.as_ref().map(|a| a.id.as_str()), Some("a1"));
        assert!(w.subagent("../x").is_err());
        let mut s = w.subagent("a1").unwrap();
        s.set_sub_info(w.agent_info("a1"));
        let sv = s.poll(&screen, 0, None, true);
        assert_eq!((sv.kind.as_str(), sv.status.as_str(), sv.title.as_str(), sv.typing, sv.prompt.is_none()), ("chat", "working", "Fix it", false, true));
        assert_eq!(sv.items.iter().map(|i| i.text.as_str()).collect::<Vec<_>>(), ["Task: fix it", "Read a.rs"]);
        assert_eq!(sv.activity, "Read a.rs");
        let _ = std::fs::remove_dir_all(&dir);
    }

    /// A pane running ssh through a wrapper: Warp sends the ssh's command line, the screen shows tmux's status line cut
    /// with no space (a name of 8) and nothing of the agent yet: it looks for the conversation in that session.
    #[test]
    fn ssh_wrapper_looks_in_tmux() {
        let info = PaneInfo { running: "fr fnviewer".into(), ssh: r#""C:\WINDOWS\System32\OpenSSH\ssh.exe" -t fr "~/bin/fr-session fnviewer""#.into(), ..Default::default() };
        let screen = PaneScreen { rows: 2, lines: vec!["loading".into(), "[fnviewer]0:claude*            \"host\" 23:10".into()] };
        let w = Watch::new(&std::env::temp_dir(), 1, 1, info.clone(), &screen);
        match &w.source {
            Source::Remote(r) => assert_eq!((r.host.as_str(), r.tmux.as_deref(), r.agent), ("fr", Some("fnviewer"), None)),
            _ => panic!("no remote source"),
        }
        assert!(w.note.starts_with("Looking for the conversation on fr"));
        // A plain ssh to a shell, no tmux: nothing is looked for.
        let plain = PaneInfo { running: "ssh fr".into(), ..Default::default() };
        let w2 = Watch::new(&std::env::temp_dir(), 1, 2, plain, &PaneScreen { rows: 1, lines: vec!["$ ls".into()] });
        assert!(matches!(w2.source, Source::None));
    }

    #[test]
    fn screen_only() {
        let info = PaneInfo { running: String::new(), cwd: "C:\\".into(), ..Default::default() };
        let mut s = PaneScreen { rows: 2, lines: vec!["old".into(), "PS C:\\> dir".into(), "PS C:\\> ".into()] };
        let mut w = Watch::new(&std::env::temp_dir(), 1, 1, info, &s);
        let v = w.poll(&s, 0, None, false);
        assert_eq!((v.kind.as_str(), v.status.as_str()), ("screen", "idle"));
        assert_eq!(v.screen.as_deref().unwrap().len(), 3);
        let v2 = w.poll(&s, v.rev, None, false);
        assert!(v2.screen.is_none(), "unchanged screen is not sent again");
        s.lines.push("more".into());
        let v3 = w.poll(&s, v2.rev, None, false);
        assert_eq!(v3.screen.unwrap().last().unwrap(), "more");
    }
}
