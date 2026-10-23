//! Following one pane for a phone (protocol v2 `watch`): its screen, which Warp sends on every poll, and the
//! conversation of the Claude Code or Codex running in it, from its transcript:
//!
//! - on this computer: the file Warp names (Claude Code's session of that pane's process, or Codex's newest
//!   conversation in its folder), read from where it was left on each poll;
//! - over ssh (a pane running `ssh host`, also with tmux): with the user's own ssh setup, `ssh -T -n -o BatchMode=yes
//!   host` runs [`REMOTE_SCRIPT`], which finds the conversation (Claude Code's `sessions/<pid>.json` of a live process,
//!   in the tmux session the pane shows) and prints what is new in it since the last time, and ends. One such ssh
//!   when the pane's screen changes (at most every 1.5 s, and once more a moment later) and every 15 s otherwise. It
//!   only reads, and nothing of it stays on that host.
//!
//! Nothing runs while no phone watches a pane: a watch exists only between `watch` and `unwatch` (or the end of the
//! connection, or a minute without a poll).

use std::{
    io::{Read, Seek, SeekFrom},
    path::PathBuf,
    sync::atomic::{AtomicU64, Ordering},
    time::{Duration, Instant},
};

#[cfg(not(windows))]
use std::process::{Command, Stdio};

use data_encoding::BASE64;

use crate::{
    chat::{Agent, Chat},
    proto::PaneView,
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
/// its folder and, for an agent running on this computer, the path of its transcript.
#[derive(Debug, Clone, Default, PartialEq)]
pub struct PaneInfo {
    pub agent: String,
    pub running: String,
    pub cwd: String,
    pub transcript: String,
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
/// How long before a remote search that found nothing is tried again: an agent writes where its conversation is only
/// once it is past its first dialog.
const RETRY_REMOTE: Duration = Duration::from_secs(6);
/// A fetch over ssh: after the screen changed, at most this often; once more this long after that (the transcript
/// may be written a moment after the screen); otherwise this often; and an ssh that takes longer than the last is ended.
const FETCH_MIN: Duration = Duration::from_millis(1500);
const FETCH_TRAIL: Duration = Duration::from_millis(2500);
const FETCH_MAX: Duration = Duration::from_secs(15);
const FETCH_TIMEOUT: Duration = Duration::from_secs(30);

/// Runs on the ssh host (POSIX sh, any shell as login shell), reads and ends. $1: the tmux session the pane shows
/// ("-": none; a name ending in `*` is the start of one, as tmux's status line cut it), $2: claude, codex or auto,
/// $3: the transcript ("-": find it), $4: where the last fetch ended in it (bytes).
///
/// Finding it prints `@claude <path>`, `@codex <path>` or `@none`; then come the transcript's lines from $4 (from the
/// last 3 MB the first time; only the record types the phone shows, and only whole lines) and `@next <offset>`, where
/// the next fetch starts. It reads only transcripts (`.jsonl` under `.claude/projects` or `.codex/sessions`).
pub const REMOTE_SCRIPT: &str = r#"h=$1 a=$2 t=$3 o=$4
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
    for f in $(ls -t "${CODEX_HOME:-$HOME/.codex}"/sessions/*/*/*/rollout-*.jsonl 2>/dev/null | head -n 20); do
      head -c 4096 "$f" | grep -q '"source":"exec"' && continue
      t=$f k=codex
      break
    done
  fi
  [ -n "$t" ] || { echo @none; exit 0; }
  echo "@$k $t"
  o=-3000000
fi
case $t in */.claude/projects/*.jsonl) e='"type":"(user|assistant|ai-title)"' ;; */.codex/sessions/*.jsonl) e='"type":"(event_msg|response_item)"' ;; *) echo @none; exit 0 ;; esac
s=$(wc -c < "$t" 2>/dev/null | tr -d ' ')
[ -n "$s" ] || { echo "@next 0"; exit 0; }
if [ "$o" -lt 0 ]; then o=$((s + o)); [ "$o" -ge 0 ] || o=0; fi
[ "$o" -le "$s" ] || o=0
tail -c +$((o + 1)) "$t" | head -c $((s - o)) | LC_ALL=C awk -v o="$o" -v total=$((s - o)) -v e="$e" '
function out(l) { if (l ~ e) print l }
NR > 1 { out(prev) }
{ prev = $0; n += length($0) + 1 }
END { if (NR == 0) print "@next " o; else if (n == total) { out(prev); print "@next " (o + n) } else print "@next " (o + n - length(prev) - 1) }'
"#;

/// The ssh command a pane runs: the options to reuse (port, user, key, jump host, config file), the host and the
/// tmux session its remote command opens, if any. None for anything that isn't a plain `ssh host [command]`.
pub fn ssh_target(running: &str) -> Option<(Vec<String>, String, Option<String>)> {
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
    // tmux's session in the remote command: `tmux new -A -s name`, `tmux attach -t name`.
    let rest: Vec<&str> = args[i + 1..].iter().flat_map(|a| a.split_whitespace()).collect();
    let tmux = rest.iter().position(|w| w.ends_with("tmux")).and_then(|t| {
        rest[t..].windows(2).find(|w| w[0] == "-s" || w[0] == "-t").map(|w| w[1].to_string())
    });
    Some((keep, host, tmux.filter(|n| screen::valid_name(n))))
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
    /// The transcript found there: whose and where, and where the next fetch starts in it.
    found: Option<(Agent, String)>,
    offset: i64,
    /// The last search found nothing.
    none: bool,
    error: String,
    fetch: Option<Fetch>,
    last: Option<Instant>,
    /// The screen changed since the last fetch; one more fetch is due a moment after the last.
    dirty: bool,
    trail: bool,
}

/// One ssh running, and the files it writes to (in the sidecar's folder, read once it has ended, then deleted).
struct Fetch {
    proc: Proc,
    out: PathBuf,
    err: PathBuf,
    started: Instant,
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

impl Remote {
    fn new(dir: &std::path::Path, opts: Vec<String>, host: String, tmux: Option<String>, agent: Option<Agent>) -> Remote {
        Remote {
            dir: dir.to_path_buf(),
            opts,
            host,
            tmux,
            agent,
            found: None,
            offset: 0,
            none: false,
            error: String::new(),
            fetch: None,
            last: None,
            dirty: false,
            trail: false,
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
            Some(e) if self.found.is_none() => e >= RETRY_REMOTE,
            Some(e) => (self.dirty && e >= FETCH_MIN) || (self.trail && e >= FETCH_TRAIL) || e >= FETCH_MAX,
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
        // Everything in it is checked (names, the path) or fixed; the tmux session and the path in single quotes.
        let remote = format!(
            "sh -c \"$(printf %s {} | base64 -d)\" warp '{}' {} '{path}' {offset}",
            BASE64.encode(REMOTE_SCRIPT.as_bytes()),
            self.tmux.as_deref().unwrap_or("-"),
            self.agent.map(|a| a.name()).unwrap_or("auto")
        );
        let mut args: Vec<String> = ["-T", "-n", "-o", "BatchMode=yes", "-o", "ConnectTimeout=10", "-o", "LogLevel=ERROR"]
            .iter()
            .chain(&["-o", "ClearAllForwardings=yes"])
            .map(|a| a.to_string())
            .collect();
        args.extend(self.opts.iter().cloned());
        args.extend(["--".to_string(), self.host.clone(), remote]);
        let o = std::fs::File::create_new(&out)?;
        let e = match std::fs::File::create_new(&err) {
            Ok(e) => e,
            Err(x) => {
                let _ = std::fs::remove_file(&out);
                return Err(x);
            }
        };
        match spawn_ssh(&args, o, e) {
            Ok(proc) => Ok(Fetch { proc, out, err, started: Instant::now() }),
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
        for l in text.split(|b| *b == b'\n') {
            let l = l.strip_suffix(b"\r").unwrap_or(l);
            if l.first() == Some(&b'@') {
                any = true;
                let h = String::from_utf8_lossy(l);
                if let Some(n) = h.strip_prefix("@next ") {
                    self.offset = n.trim().parse().unwrap_or(self.offset);
                } else if h == "@none" {
                    self.none = true;
                } else if let Some((who, p)) = h.strip_prefix("@claude ").map(|p| (Agent::Claude, p)).or_else(|| h.strip_prefix("@codex ").map(|p| (Agent::Codex, p))) {
                    if safe_path(p) {
                        self.found = Some((who, p.to_string()));
                        self.none = false;
                    } else {
                        self.error = "the conversation's path has characters it can't be read with".into();
                    }
                }
            } else if !l.is_empty() && l.len() <= MAX_LINE && self.found.is_some() {
                lines.push(l.to_vec());
            }
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

/// On Windows, ssh (OpenSSH for Windows 9.5) writes all it receives only to a file nobody else has open, and only
/// when it has a console of its own: to a pipe it stops after the first write, and with no console, or a file read
/// while it writes, it stops after a few KB. So it gets a hidden console (CREATE_NEW_CONSOLE with SW_HIDE, as
/// `Start-Process -WindowStyle Hidden`: never a window on the screen), and what it wrote is read once it has ended.
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

enum Source {
    None,
    /// A transcript on this computer: where it was left, and the start of a line not finished yet.
    Local { path: PathBuf, offset: Option<u64>, carry: Vec<u8> },
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
    pub fn new(dir: &std::path::Path, window: u64, pane_id: u32, info: PaneInfo, screen: &PaneScreen) -> Watch {
        // Revisions start at the clock (ms): a watch made after a reconnection is always newer than what the phone
        // has, so its first answer starts the pane over.
        let start = std::time::SystemTime::now().duration_since(std::time::UNIX_EPOCH).map(|d| d.as_millis() as u64).unwrap_or(1);
        let mut w = Watch {
            window,
            pane_id,
            last_poll: Instant::now(),
            dir: dir.to_path_buf(),
            info: PaneInfo::default(),
            info_at: Instant::now(),
            agent: String::new(),
            source: Source::None,
            chat: None,
            rev: start,
            screen_hash: 0,
            screen_rev: 0,
            kind: "screen",
            kind_rev: start,
            note: String::new(),
        };
        w.set_info(info, screen);
        w
    }

    /// Time to ask Warp about the pane again.
    pub fn info_due(&self) -> bool {
        self.info_at.elapsed() >= INFO_EVERY
    }

    /// What Warp says about the pane now: the agent may have started or ended, or moved to another conversation.
    pub fn set_info(&mut self, info: PaneInfo, screen: &PaneScreen) {
        self.info_at = Instant::now();
        let ssh = ssh_target(&info.running);
        let agent = if !info.agent.is_empty() {
            info.agent.clone()
        } else if ssh.is_some() {
            screen::agent(screen.visible()).to_string()
        } else {
            String::new()
        };
        if info == self.info && agent == self.agent {
            return;
        }
        let kind = match agent.as_str() {
            "claude" => Some(Agent::Claude),
            "codex" => Some(Agent::Codex),
            _ => None,
        };
        let local = !info.transcript.is_empty() && kind.is_some();
        let keep = match &self.source {
            Source::Local { path, .. } => local && path.as_os_str() == info.transcript.as_str(),
            Source::Remote(_) => !local && ssh.is_some() && (kind.is_some() || self.chat.is_some()),
            Source::None => false,
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
            self.source = Source::Local { path: PathBuf::from(&self.info.transcript), offset: None, carry: Vec::new() };
            self.chat = Some(Chat::new(kind.unwrap()));
            self.note = "this computer".into();
        } else if let (Some((opts, host, tmux_cmd)), Some(_)) = (ssh, kind) {
            let tmux = screen::pick_tmux(screen::tmux_session(screen.visible()), tmux_cmd);
            self.note = format!("Looking for the conversation on {host}…");
            self.source = Source::Remote(Box::new(Remote::new(&self.dir, opts, host, tmux, kind)));
        }
    }

    /// New lines of the transcript, into the chat. `screen_changed`: a fetch over ssh may be due.
    fn read_transcript(&mut self, screen_changed: bool) {
        let mut lines: Vec<Vec<u8>> = Vec::new();
        match &mut self.source {
            Source::None => return,
            Source::Local { path, offset, carry } => {
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
            }
            Source::Remote(remote) => {
                lines = remote.step(screen_changed);
                if self.chat.is_none()
                    && let Some((who, _)) = &remote.found
                {
                    self.chat = Some(Chat::new(*who));
                }
                self.note = if remote.found.is_some() {
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
        if lines.is_empty() {
            return;
        }
        let Some(chat) = self.chat.as_mut() else { return };
        self.rev += 1;
        for l in &lines {
            chat.feed(l, self.rev);
        }
    }

    /// The pane for the phone: what changed since revision `since`, or the items before `before`.
    pub fn poll(&mut self, screen: &PaneScreen, since: u64, before: Option<u64>, typing: bool) -> PaneView {
        self.last_poll = Instant::now();
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
            ..Default::default()
        };
        if let Some(chat) = &self.chat {
            v.title = chat.title.clone();
            let (items, more) = match before {
                Some(b) => chat.before(b, 60, BUDGET),
                None if reset => chat.since(0, 80, BUDGET),
                None => chat.since(since, usize::MAX, BUDGET),
            };
            v.items = items;
            v.more = more;
        } else if before.is_none() && (reset || self.screen_rev > since) {
            let from = screen.lines.len().saturating_sub(MAX_ROWS);
            v.screen = Some(screen.lines[from..].iter().map(|l| l.chars().take(MAX_COLS).collect()).collect());
        }
        v
    }
}

#[cfg(test)]
mod tests {
    use std::io::Write;

    use super::*;

    #[test]
    fn ssh_commands() {
        assert_eq!(ssh_target("ssh fr"), Some((vec![], "fr".into(), None)));
        assert_eq!(
            ssh_target(r#"ssh -t fr "tmux -S /tmp/claude-1001/demo.sock new -A -s phone-demo""#),
            Some((vec![], "fr".into(), Some("phone-demo".into())))
        );
        assert_eq!(
            ssh_target("C:\\Windows\\System32\\OpenSSH\\ssh.exe -p 2222 -i C:\\k\\id -L 80:x:80 -A me@server tmux attach -t work"),
            Some((vec!["-p".into(), "2222".into(), "-i".into(), "C:\\k\\id".into()], "me@server".into(), Some("work".into())))
        );
        assert_eq!(ssh_target("ssh -oProxyCommand=evil host").map(|t| t.1), Some("host".into()));
        assert_eq!(ssh_target("sshfs host:/ x"), None);
        assert_eq!(ssh_target("ssh -p -oX host"), None);
        assert_eq!(ssh_target("ssh 'a;b'"), None);
        assert_eq!(ssh_target("claude"), None);
    }

    /// The script itself (where sh runs): only whole lines of the record types the phone shows, and the next fetch starts
    /// right after the last whole line, so a line still being written comes whole next time.
    #[cfg(unix)]
    #[test]
    fn remote_script_reads_whole_lines() {
        let dir = std::env::temp_dir().join(format!("warp-remote-script-{}", std::process::id()));
        let proj = dir.join(".claude/projects/x");
        std::fs::create_dir_all(&proj).unwrap();
        let t = proj.join("t.jsonl");
        let (a, b) = (r#"{"type":"user","message":{"content":"one"}}"#, r#"{"type":"attachment","x":1}"#);
        let c = r#"{"type":"assistant","message":{"content":[{"type":"text","text":"two"}]}}"#;
        std::fs::write(&t, format!("{a}\n{b}\n{}", &c[..20])).unwrap();
        let run = |off: usize| -> Vec<String> {
            let o = std::process::Command::new("sh")
                .args(["-c", REMOTE_SCRIPT, "warp", "-", "claude", t.to_str().unwrap(), &off.to_string()])
                .output()
                .unwrap();
            String::from_utf8_lossy(&o.stdout).lines().map(String::from).collect()
        };
        let first = a.len() + 1 + b.len() + 1;
        assert_eq!(run(0), [a.to_string(), format!("@next {first}")]);
        std::fs::OpenOptions::new().append(true).open(&t).unwrap().write_all(format!("{}\n", &c[20..]).as_bytes()).unwrap();
        let end = first + c.len() + 1;
        assert_eq!(run(first), [c.to_string(), format!("@next {end}")]);
        assert_eq!(run(end), [format!("@next {end}")]);
        // Not a transcript: nothing is read.
        let o = std::process::Command::new("sh").args(["-c", REMOTE_SCRIPT, "warp", "-", "claude", "/etc/passwd", "0"]).output().unwrap();
        assert_eq!(String::from_utf8_lossy(&o.stdout).trim(), "@none");
        assert!(safe_path("/home/u/.claude/projects/-tmp-x/9d09-21b2.jsonl") && !safe_path("/x/'a'.jsonl") && !safe_path("/x/../y.jsonl"));
        let _ = std::fs::remove_dir_all(&dir);
    }

    #[test]
    fn local_transcript() {
        let dir = std::env::temp_dir().join(format!("warp-remote-watch-{}", std::process::id()));
        let _ = std::fs::create_dir_all(&dir);
        let path = dir.join("t.jsonl");
        std::fs::write(&path, "{\"type\":\"user\",\"message\":{\"content\":\"hello\"}}\n{\"type\":\"assistant\",\"mess").unwrap();
        let info = PaneInfo { agent: "claude".into(), running: "claude".into(), cwd: "C:\\x".into(), transcript: path.display().to_string() };
        let screen = PaneScreen { rows: 3, lines: vec!["❯ hello".into(), String::new(), "  ? for shortcuts".into()] };
        let mut w = Watch::new(&dir, 7, 3, info, &screen);
        let v = w.poll(&screen, 0, None, false);
        assert_eq!((v.kind.as_str(), v.status.as_str(), v.reset, v.items.len()), ("chat", "finished", true, 1));
        assert!(v.screen.is_none());
        // The rest of the line arrives: only the new item comes, and the same rev means nothing new.
        let mut f = std::fs::OpenOptions::new().append(true).open(&path).unwrap();
        std::io::Write::write_all(&mut f, b"age\":{\"content\":[{\"type\":\"text\",\"text\":\"hi!\"}]}}\n").unwrap();
        let v2 = w.poll(&screen, v.rev, None, true);
        assert_eq!(v2.items.iter().map(|i| i.text.as_str()).collect::<Vec<_>>(), ["hi!"]);
        assert!(!v2.reset && v2.typing);
        let v3 = w.poll(&screen, v2.rev, None, true);
        assert!(v3.items.is_empty() && v3.rev == v2.rev);
        // After a reconnection the phone asks a new watch with its old revision: it starts over.
        let mut again = Watch::new(&dir, 7, 3, PaneInfo { transcript: path.display().to_string(), agent: "claude".into(), ..Default::default() }, &screen);
        let v4 = again.poll(&screen, v3.rev, None, true);
        assert!(v4.reset && v4.items.len() == 2, "{v4:?}");
        let _ = std::fs::remove_dir_all(&dir);
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
