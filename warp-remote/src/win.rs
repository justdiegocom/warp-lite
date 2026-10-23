//! The sidecar's side of talking to Warp's windows (Windows only).
//!
//! Warp's windows already list their panes to each other for the terminal switcher (Ctrl+Shift+K):
//! a window sends the registered message `warp-lite-list-panes` with its own HWND, and each window
//! of the same Warp answers with `WM_COPYDATA` (`kCopyPaneList`), one line per pane. Windows only
//! answer a requester that carries the window property `warp-lite-instance` with their own
//! instance key, so the sidecar has a hidden message-only window with that property and asks the
//! same way. Nothing new is needed in Warp for the list.
//!
//! The other direction, also through that window:
//! - Warp → sidecar: `WM_COPYDATA` `kCopyRemote` with a command (`pair <hwnd>`, `reload`, `quit`),
//!   and `warp-lite-remote-state` (sent) answers how many phones are connected.
//! - sidecar → Warp: `warp-lite-remote` posted to every window of that Warp (wParam: phones
//!   connected; lParam: 0 state, 1 a phone was paired, 2 the pairing code ended), and the ticket as
//!   `WM_COPYDATA` `kCopyRemoteTicket` (`ticket\t<text>\t<seconds>` or `error\t<message>`).

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
        LibraryLoader::GetModuleHandleW,
        Threading::{CreateMutexW, INFINITE, OpenProcess, PROCESS_SYNCHRONIZE, WaitForSingleObject},
    },
    UI::WindowsAndMessaging::{
        CreateWindowExW, DefWindowProcW, DestroyWindow, DispatchMessageW, EnumWindows, GetClassNameW, GetMessageW,
        GetPropW, GetWindowThreadProcessId, HWND_MESSAGE, MSG, PostMessageW, RegisterClassW, RegisterWindowMessageW,
        RemovePropW, SMTO_ABORTIFHUNG, SMTO_NORMAL, SendMessageTimeoutW, SetPropW, WM_APP, WM_CLOSE, WM_COPYDATA,
        WNDCLASSW,
    },
};

use crate::{Result, err, proto::Session};

/// Same values as in Warp's main.cpp.
const COPY_PANE_LIST: usize = 0x574c5300;
const COPY_REMOTE: usize = 0x57524d00;
const COPY_REMOTE_TICKET: usize = 0x57525400;
const WM_APP_LIST: u32 = WM_APP + 1;
const WM_APP_STOP: u32 = WM_APP + 2;
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

    /// Tells every window of this Warp (title-bar indicator, pairing card).
    pub fn notify(&self, phones: usize, event: isize) {
        let s = SHARED.get().unwrap();
        for h in warp_windows(s.key) {
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

/// The windows of this Warp (class WarpLite with its instance key), front to back.
fn warp_windows(key: isize) -> Vec<isize> {
    struct Ctx {
        key: isize,
        out: Vec<isize>,
    }
    unsafe extern "system" fn each(h: HWND, lp: LPARAM) -> i32 {
        let c = unsafe { &mut *(lp as *mut Ctx) };
        let mut cls = [0u16; 32];
        let n = unsafe { GetClassNameW(h, cls.as_mut_ptr(), 32) } as usize;
        let prop = wide("warp-lite-instance");
        if String::from_utf16_lossy(&cls[..n]) == "WarpLite" && unsafe { GetPropW(h, prop.as_ptr()) } as isize == c.key {
            c.out.push(h as isize);
        }
        1
    }
    let mut ctx = Ctx { key, out: Vec::new() };
    unsafe { EnumWindows(Some(each), &mut ctx as *mut Ctx as LPARAM) };
    ctx.out
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
        // Each window answers with WM_COPYDATA while SendMessageTimeout waits (handled re-entrantly below),
        // so nothing may stay locked across the call.
        s.collected.lock().unwrap().clear();
        for h in warp_windows(s.key) {
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

/// Warp's pane list: one line per pane, fields separated by tabs: tab, pane, title, folder, what is
/// running, and (newer Warps) the tab's name and the agent (claude / codex).
pub fn parse_pane_list(window: u64, text: &str) -> Vec<Session> {
    text.lines()
        .filter_map(|l| {
            let f: Vec<&str> = l.split('\t').collect();
            if f.len() < 5 {
                return None;
            }
            Some(Session {
                window,
                tab: f[0].parse().ok()?,
                pane: f[1].parse().ok()?,
                title: f[2].to_string(),
                cwd: f[3].to_string(),
                busy: !f[4].is_empty(),
                running: f[4].to_string(),
                tab_name: f.get(5).map(|s| s.to_string()).unwrap_or_else(|| f[2].to_string()),
                agent: f.get(6).map(|s| s.to_string()).unwrap_or_default(),
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

/// Lives as long as a window of this Warp is open: waits for the process that started it, then for
/// another window's process, and sends `Quit` when none is left. No polling.
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
            let next = warp_windows(key).into_iter().map(|w| {
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
                None => std::thread::sleep(Duration::from_millis(300)),  // its window is still closing
            }
        }
    });
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn pane_list() {
        let s = parse_pane_list(7, "0\t0\tclaude chat\tC:\\src\tclaude\twork\tclaude\n0\t1\tpwsh\tC:\\\t\n");
        assert_eq!(s.len(), 2);
        assert_eq!((s[0].tab_name.as_str(), s[0].busy, s[0].agent.as_str()), ("work", true, "claude"));
        assert_eq!((s[1].pane, s[1].busy, s[1].tab_name.as_str()), (1, false, "pwsh"));
    }
}
