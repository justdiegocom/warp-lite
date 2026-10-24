//! The sidecar's side of talking to Warp (Windows only).
//!
//! Every window of a Warp lives in one process, which has a hidden message-only window (class `WarpLiteHost`) with
//! the window property `warp-lite-instance` set to that Warp's instance key: that is the one endpoint. The sidecar
//! has a hidden message-only window of its own with the same property; Warp only answers a requester that carries it.
//!
//! - sidecar → Warp: the registered message `warp-lite-list-panes` with the sidecar's HWND, answered with
//!   `WM_COPYDATA` (`kCopyPaneList`): one line per pane of every window (see [`parse_pane_list`]).
//! - Warp → sidecar: `WM_COPYDATA` `kCopyRemote` with a command (`pair <hwnd>`, `reload`, `quit`),
//!   and `warp-lite-remote-state` (sent) answers how many phones are connected.
//! - sidecar → Warp: `warp-lite-remote` posted to it (wParam: phones connected; lParam: 0 state, 1 a phone was
//!   paired, 2 the pairing code ended), and the ticket as `WM_COPYDATA` `kCopyRemoteTicket` to the HWND of `pair`
//!   (`ticket\t<text>\t<seconds>` or `error\t<message>`).
//! - sidecar → Warp, about one pane (by its id, whichever window it is in now: a pane moved to another window keeps
//!   its id), `WM_COPYDATA` `kCopyRemotePane`, which Warp only takes from this sidecar's window: `screen <id>
//!   <scrollback rows>` and `info <id>` (answered with `WM_COPYDATA` `kCopyRemoteAnswer` before it returns:
//!   `screen\t<rows>\t<cols>\n<line>\n…`, `info\t<agent>\t<running>\t<cwd>\t<transcript>`), and `input <id> <device id>
//!   text\t<text>` / `key\t<name>` (TRUE if typed; Warp checks the device may type too).

use std::{
    ptr::null,
    sync::{
        Mutex, OnceLock,
        atomic::{AtomicUsize, Ordering},
    },
    time::Duration,
};

use tokio::sync::{mpsc::UnboundedSender, oneshot};
use windows_sys::Win32::{
    Foundation::{CloseHandle, HANDLE, HWND, LPARAM, LRESULT, WAIT_ABANDONED, WAIT_OBJECT_0, WPARAM},
    System::{
        DataExchange::COPYDATASTRUCT,
        JobObjects::{
            AssignProcessToJobObject, CreateJobObjectW, JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE, JOBOBJECT_EXTENDED_LIMIT_INFORMATION,
            JobObjectExtendedLimitInformation, SetInformationJobObject,
        },
        LibraryLoader::GetModuleHandleW,
        Threading::{CreateMutexW, GetCurrentProcess, INFINITE, OpenProcess, PROCESS_SYNCHRONIZE, WaitForSingleObject},
    },
    UI::WindowsAndMessaging::{
        CreateWindowExW, DefWindowProcW, DestroyWindow, DispatchMessageW, FindWindowExW, GetMessageW,
        GetPropW, GetWindowThreadProcessId, HWND_MESSAGE, MSG, PostMessageW, RegisterClassW, RegisterWindowMessageW,
        RemovePropW, SMTO_ABORTIFHUNG, SMTO_NORMAL, SendMessageTimeoutW, SetPropW, WM_APP, WM_CLOSE, WM_COPYDATA,
        WNDCLASSW,
    },
};

use crate::{
    Result, err,
    proto::Session,
    watch::{PaneInfo, PaneInput, PaneScreen},
};

/// Same values as in Warp's main.cpp.
const COPY_PANE_LIST: usize = 0x574c5300;
const COPY_REMOTE: usize = 0x57524d00;
const COPY_REMOTE_TICKET: usize = 0x57525400;
const COPY_REMOTE_PANE: usize = 0x57525000;
const COPY_REMOTE_ANSWER: usize = 0x57524100;
const WM_APP_LIST: u32 = WM_APP + 1;
const WM_APP_STOP: u32 = WM_APP + 2;
const WM_APP_PANE: u32 = WM_APP + 3;
/// Scrollback rows sent with a pane's screen.
const SCROLLBACK_ROWS: usize = 200;

/// A request about a pane, waiting for the window thread: the window, the text, and where its answer goes
/// (Ok(Some(answer)), Ok(None) when the window just said yes).
type PaneRequest = (isize, String, oneshot::Sender<Result<Option<String>>>);
pub const EVENT_STATE: isize = 0;
pub const EVENT_PAIRED: isize = 1;
pub const EVENT_PAIRING_ENDED: isize = 2;

/// What Warp (or this process) asks the sidecar to do.
#[derive(Debug, Clone, PartialEq)]
pub enum Cmd {
    /// New pairing code, sent to that window.
    Pair(isize),
    /// The allow-list changed.
    Reload,
    Quit,
    /// The pairing code ended (expired or used): quit if nothing is left to do.
    Idle,
}

fn wide(s: &str) -> Vec<u16> {
    s.encode_utf16().chain(std::iter::once(0)).collect()
}

struct Shared {
    key: isize,
    msg_list: u32,
    msg_state: u32,
    msg_remote: u32,
    tx: UnboundedSender<Cmd>,
    hwnd: AtomicUsize,
    phones: AtomicUsize,
    /// (window, its pane lines) while a list is being collected.
    collected: Mutex<Vec<(isize, String)>>,
    waiting: Mutex<Vec<oneshot::Sender<Vec<Session>>>>,
    pane_requests: Mutex<Vec<PaneRequest>>,
    /// The answer of the window asked now (kCopyRemoteAnswer, sent while SendMessageTimeout waits).
    answer: Mutex<Option<String>>,
}

static SHARED: OnceLock<Shared> = OnceLock::new();

/// The sidecar's message window and what goes through it.
pub struct WarpLink;

impl WarpLink {
    /// Creates the hidden window on its own thread. `key`: Warp's instance key.
    pub fn start(key: isize, tx: UnboundedSender<Cmd>) -> Result<WarpLink> {
        let reg = |s: &str| unsafe { RegisterWindowMessageW(wide(s).as_ptr()) };
        let shared = Shared {
            key,
            msg_list: reg("warp-lite-list-panes"),
            msg_state: reg("warp-lite-remote-state"),
            msg_remote: reg("warp-lite-remote"),
            tx,
            hwnd: AtomicUsize::new(0),
            phones: AtomicUsize::new(0),
            collected: Mutex::new(Vec::new()),
            waiting: Mutex::new(Vec::new()),
            pane_requests: Mutex::new(Vec::new()),
            answer: Mutex::new(None),
        };
        SHARED.set(shared).map_err(|_| err("started twice"))?;
        let (ready_tx, ready_rx) = std::sync::mpsc::channel();
        std::thread::spawn(move || unsafe { window_thread(ready_tx) });
        let hwnd = ready_rx.recv_timeout(Duration::from_secs(5)).map_err(|_| err("no message window"))?;
        if hwnd == 0 {
            return Err(err("could not create the message window"));
        }
        Ok(WarpLink)
    }

    /// Every pane of every window of this Warp.
    pub async fn sessions(&self) -> Result<Vec<Session>> {
        let s = SHARED.get().unwrap();
        let (tx, rx) = oneshot::channel();
        s.waiting.lock().unwrap().push(tx);
        unsafe { PostMessageW(s.hwnd.load(Ordering::Relaxed) as HWND, WM_APP_LIST, 0, 0) };
        tokio::time::timeout(Duration::from_secs(10), rx).await.map_err(|_| err("Warp didn't answer"))?.map_err(|_| err("Warp didn't answer"))
    }

    /// Asks a window of this Warp about one of its panes (see the module's doc).
    async fn pane(&self, window: u64, text: String) -> Result<Option<String>> {
        let s = SHARED.get().unwrap();
        let (tx, rx) = oneshot::channel();
        s.pane_requests.lock().unwrap().push((window as isize, text, tx));
        unsafe { PostMessageW(s.hwnd.load(Ordering::Relaxed) as HWND, WM_APP_PANE, 0, 0) };
        tokio::time::timeout(Duration::from_secs(10), rx).await.map_err(|_| err("Warp didn't answer"))?.map_err(|_| err("Warp didn't answer"))?
    }

    /// The text of a pane: its last scrollback rows, then its screen.
    pub async fn screen(&self, window: u64, pane_id: u32) -> Result<PaneScreen> {
        let a = self.pane(window, format!("screen {pane_id} {SCROLLBACK_ROWS}")).await?.ok_or_else(|| err("no answer"))?;
        parse_screen(&a).ok_or_else(|| err("bad answer"))
    }

    pub async fn info(&self, window: u64, pane_id: u32) -> Result<PaneInfo> {
        let a = self.pane(window, format!("info {pane_id}")).await?.ok_or_else(|| err("no answer"))?;
        let f: Vec<&str> = a.split('\t').collect();
        if f.len() < 5 || f[0] != "info" {
            return Err(err("bad answer"));
        }
        Ok(PaneInfo { agent: f[1].into(), running: f[2].into(), cwd: f[3].into(), transcript: f[4].into() })
    }

    pub async fn input(&self, window: u64, pane_id: u32, device: &str, what: &PaneInput) -> Result<()> {
        let body = match what {
            PaneInput::Text(t) => format!("text\t{t}"),
            PaneInput::Key(k) => format!("key\t{k}"),
        };
        self.pane(window, format!("input {pane_id} {device} {body}")).await.map(|_| ())
    }

    /// Withdraws the message window, first thing when quitting: Warp stops seeing this sidecar at once,
    /// so a Warp starting meanwhile (an update restart) launches a new one, which waits for this one to end.
    pub fn stop(&self) {
        let s = SHARED.get().unwrap();
        let mut r = 0usize;
        unsafe { SendMessageTimeoutW(s.hwnd.load(Ordering::Relaxed) as HWND, WM_APP_STOP, 0, 0, SMTO_NORMAL, 3000, &mut r) };
    }

    pub fn set_phones(&self, n: usize) {
        SHARED.get().unwrap().phones.store(n, Ordering::Relaxed);
    }

    /// Tells this Warp (title-bar indicator, pairing card).
    pub fn notify(&self, phones: usize, event: isize) {
        let s = SHARED.get().unwrap();
        for h in warp_hosts(s.key) {
            unsafe { PostMessageW(h as HWND, s.msg_remote, phones, event) };
        }
    }

    /// The pairing code (or why there is none) for the window that asked. Blocks up to 2 s.
    pub fn send_ticket(&self, to: isize, text: &str) {
        let s = SHARED.get().unwrap();
        let data: Vec<u16> = text.encode_utf16().collect();
        let cd = COPYDATASTRUCT {
            dwData: COPY_REMOTE_TICKET,
            cbData: (data.len() * 2) as u32,
            lpData: data.as_ptr() as *mut _,
        };
        let mut r = 0usize;
        unsafe {
            SendMessageTimeoutW(
                to as HWND,
                WM_COPYDATA,
                s.hwnd.load(Ordering::Relaxed),
                &cd as *const _ as LPARAM,
                SMTO_ABORTIFHUNG | SMTO_NORMAL,
                2000,
                &mut r,
            )
        };
    }
}

/// This Warp's hidden message windows (class WarpLiteHost with its instance key): one, while it runs (two for a moment
/// while an update restart hands over).
fn warp_hosts(key: isize) -> Vec<isize> {
    let class = wide("WarpLiteHost");
    let prop = wide("warp-lite-instance");
    let mut out = Vec::new();
    let mut h: HWND = std::ptr::null_mut();
    loop {
        h = unsafe { FindWindowExW(HWND_MESSAGE, h, class.as_ptr(), null()) };
        if h.is_null() {
            break;
        }
        if unsafe { GetPropW(h, prop.as_ptr()) } as isize == key {
            out.push(h as isize);
        }
    }
    out
}

unsafe fn window_thread(ready: std::sync::mpsc::Sender<isize>) {
    let s = SHARED.get().unwrap();
    let class = wide("WarpRemote");
    let inst = unsafe { GetModuleHandleW(null()) };
    let mut wc: WNDCLASSW = unsafe { std::mem::zeroed() };
    wc.lpfnWndProc = Some(wndproc);
    wc.hInstance = inst;
    wc.lpszClassName = class.as_ptr();
    unsafe { RegisterClassW(&wc) };
    let hwnd = unsafe {
        CreateWindowExW(0, class.as_ptr(), null(), 0, 0, 0, 0, 0, HWND_MESSAGE, std::ptr::null_mut(), inst, null())
    };
    if !hwnd.is_null() {
        // Warp's windows only answer a requester with their instance key, and find this one by it.
        unsafe { SetPropW(hwnd, wide("warp-lite-instance").as_ptr(), s.key as HANDLE) };
        s.hwnd.store(hwnd as usize, Ordering::Relaxed);
    }
    let _ = ready.send(hwnd as isize);
    if hwnd.is_null() {
        return;
    }
    let mut msg: MSG = unsafe { std::mem::zeroed() };
    while unsafe { GetMessageW(&mut msg, std::ptr::null_mut(), 0, 0) } > 0 {
        unsafe { DispatchMessageW(&msg) };
    }
}

unsafe extern "system" fn wndproc(hwnd: HWND, msg: u32, wp: WPARAM, lp: LPARAM) -> LRESULT {
    let Some(s) = SHARED.get() else { return unsafe { DefWindowProcW(hwnd, msg, wp, lp) } };
    if msg == WM_APP_STOP {
        unsafe {
            RemovePropW(hwnd, wide("warp-lite-instance").as_ptr());
            DestroyWindow(hwnd);
        }
        return 0;
    }
    if msg == WM_APP_LIST {
        // Warp answers with WM_COPYDATA while SendMessageTimeout waits (handled re-entrantly below),
        // so nothing may stay locked across the call.
        s.collected.lock().unwrap().clear();
        for h in warp_hosts(s.key) {
            let mut r = 0usize;
            unsafe { SendMessageTimeoutW(h as HWND, s.msg_list, hwnd as WPARAM, 0, SMTO_ABORTIFHUNG | SMTO_NORMAL, 1000, &mut r) };
        }
        let got = std::mem::take(&mut *s.collected.lock().unwrap());
        let sessions: Vec<Session> = got.iter().flat_map(|(w, text)| parse_pane_list(*w as u64, text)).collect();
        for tx in s.waiting.lock().unwrap().drain(..) {
            let _ = tx.send(sessions.clone());
        }
        return 0;
    }
    if msg == WM_APP_PANE {
        // To this Warp, which finds the pane by its id (the phone's window is only where it was listed), one at a time;
        // its answer arrives re-entrantly.
        let todo = std::mem::take(&mut *s.pane_requests.lock().unwrap());
        let host = warp_hosts(s.key).first().copied();
        for (_window, text, tx) in todo {
            let Some(host) = host else {
                let _ = tx.send(Err(err("no such window")));
                continue;
            };
            *s.answer.lock().unwrap() = None;
            let data: Vec<u16> = text.encode_utf16().collect();
            let cd = COPYDATASTRUCT { dwData: COPY_REMOTE_PANE, cbData: (data.len() * 2) as u32, lpData: data.as_ptr() as *mut _ };
            let mut r = 0usize;
            let sent = unsafe {
                SendMessageTimeoutW(host as HWND, WM_COPYDATA, hwnd as WPARAM, &cd as *const _ as LPARAM, SMTO_ABORTIFHUNG | SMTO_NORMAL, 2000, &mut r)
            };
            let answer = s.answer.lock().unwrap().take();
            let _ = tx.send(if sent == 0 || r == 0 { Err(err("no such pane")) } else { Ok(answer) });
        }
        return 0;
    }
    if msg == WM_COPYDATA {
        let cd = unsafe { &*(lp as *const COPYDATASTRUCT) };
        let text = if cd.lpData.is_null() {
            String::new()
        } else {
            let words = unsafe { std::slice::from_raw_parts(cd.lpData as *const u16, cd.cbData as usize / 2) };
            String::from_utf16_lossy(words)
        };
        if cd.dwData == COPY_PANE_LIST {
            s.collected.lock().unwrap().push((wp as isize, text));
            return 1;
        }
        if cd.dwData == COPY_REMOTE_ANSWER {
            *s.answer.lock().unwrap() = Some(text);
            return 1;
        }
        if cd.dwData == COPY_REMOTE {
            let mut it = text.split_whitespace();
            let cmd = match it.next() {
                Some("pair") => it.next().and_then(|h| isize::from_str_radix(h, 16).ok()).map(Cmd::Pair),
                Some("reload") => Some(Cmd::Reload),
                Some("quit") => Some(Cmd::Quit),
                _ => None,
            };
            return match cmd {
                Some(c) => {
                    let _ = s.tx.send(c);
                    1
                }
                None => 0,
            };
        }
        return 0;
    }
    if msg == s.msg_state {
        return s.phones.load(Ordering::Relaxed) as LRESULT;
    }
    if msg == WM_CLOSE {
        let _ = s.tx.send(Cmd::Quit);
        return 0;
    }
    unsafe { DefWindowProcW(hwnd, msg, wp, lp) }
}

/// A pane's text: `screen\t<rows>\t<cols>` then one line per row.
fn parse_screen(a: &str) -> Option<PaneScreen> {
    let (head, body) = a.split_once('\n').unwrap_or((a, ""));
    let f: Vec<&str> = head.split('\t').collect();
    if f.first() != Some(&"screen") {
        return None;
    }
    let rows = f.get(1)?.parse().ok()?;
    Some(PaneScreen { rows, lines: body.split('\n').map(String::from).collect() })
}

/// Everything this process starts (the ssh that follows a transcript on another host) ends with it, even if it is
/// killed: it puts itself in a job object that kills what is left in it when its last handle closes.
pub fn end_children_with_me() {
    unsafe {
        let job = CreateJobObjectW(null(), null());
        if job.is_null() {
            return;
        }
        let mut info: JOBOBJECT_EXTENDED_LIMIT_INFORMATION = std::mem::zeroed();
        info.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
        let size = std::mem::size_of::<JOBOBJECT_EXTENDED_LIMIT_INFORMATION>() as u32;
        if SetInformationJobObject(job, JobObjectExtendedLimitInformation, &info as *const _ as *const _, size) == 0
            || AssignProcessToJobObject(job, GetCurrentProcess()) == 0
        {
            CloseHandle(job);
        }
        // The handle stays open (this process's) until it ends.
    }
}

/// Warp's pane list: one line per pane, fields separated by tabs: tab, pane, title, folder, what is
/// running, the tab's name, the agent (claude / codex), the pane's id and its window (`window` when a line has none).
pub fn parse_pane_list(window: u64, text: &str) -> Vec<Session> {
    text.lines()
        .filter_map(|l| {
            let f: Vec<&str> = l.split('\t').collect();
            if f.len() < 5 {
                return None;
            }
            Some(Session {
                window: f.get(8).and_then(|s| s.parse().ok()).unwrap_or(window),
                tab: f[0].parse().ok()?,
                pane: f[1].parse().ok()?,
                title: f[2].to_string(),
                cwd: f[3].to_string(),
                busy: !f[4].is_empty(),
                running: f[4].to_string(),
                tab_name: f.get(5).map(|s| s.to_string()).unwrap_or_else(|| f[2].to_string()),
                agent: f.get(6).map(|s| s.to_string()).unwrap_or_default(),
                pane_id: f.get(7).and_then(|s| s.parse().ok()).unwrap_or(0),
            })
        })
        .collect()
}

/// One sidecar per Warp: waits up to 10 s for an earlier one that is still exiting (an update
/// restart); false if another one keeps running.
pub fn single_instance(key: isize) -> bool {
    let name = wide(&format!("Local\\warp-lite-{:08x}-remote", key as u32));
    let m = unsafe { CreateMutexW(null(), 0, name.as_ptr()) };
    if m.is_null() {
        return true;
    }
    let r = unsafe { WaitForSingleObject(m, 10_000) };
    r == WAIT_OBJECT_0 || r == WAIT_ABANDONED  // the handle stays open (owned) until this process ends
}

/// Lives as long as this Warp runs: waits for the process that started it, then for the one that took over (an
/// update restart), and sends `Quit` when none is left. No polling.
pub fn follow_warp(pid: u32, key: isize, tx: UnboundedSender<Cmd>) {
    std::thread::spawn(move || {
        let mut pid = pid;
        loop {
            let h = unsafe { OpenProcess(PROCESS_SYNCHRONIZE, 0, pid) };
            if !h.is_null() {
                unsafe {
                    WaitForSingleObject(h, INFINITE);
                    CloseHandle(h);
                }
            }
            let next = warp_hosts(key).into_iter().map(|w| {
                let mut p = 0u32;
                unsafe { GetWindowThreadProcessId(w as HWND, &mut p) };
                p
            });
            let next: Vec<u32> = next.filter(|&p| p != 0).collect();
            match next.iter().find(|&&p| p != pid) {
                Some(&p) => pid = p,
                None if next.is_empty() => {
                    let _ = tx.send(Cmd::Quit);
                    return;
                }
                None => std::thread::sleep(Duration::from_millis(300)),  // its hidden window is still closing
            }
        }
    });
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn pane_list() {
        let s = parse_pane_list(7, "0\t0\tclaude chat\tC:\\src\tclaude\twork\tclaude\t12\t99\n0\t1\tpwsh\tC:\\\t\n");
        assert_eq!(s.len(), 2);
        assert_eq!((s[0].tab_name.as_str(), s[0].busy, s[0].agent.as_str(), s[0].pane_id, s[0].window), ("work", true, "claude", 12, 99));
        assert_eq!((s[1].pane, s[1].busy, s[1].tab_name.as_str(), s[1].pane_id, s[1].window), (1, false, "pwsh", 0, 7));
        let sc = parse_screen("screen\t2\t80\nold\nPS C:\\> dir\nPS C:\\> ").unwrap();
        assert_eq!((sc.rows, sc.lines.len(), sc.visible()[0].as_str()), (2, 3, "PS C:\\> dir"));
        assert!(parse_screen("info\tx").is_none());
    }
}
