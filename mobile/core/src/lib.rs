//! The phone's core: warp-remote's client (shared with `warp-remote-cli`) behind a small synchronous C
//! interface, `include/warp_remote_core.h`. Nothing async crosses it: each call blocks until done, and the
//! app makes them from a background queue. iroh keeps running on the core's own threads between calls
//! (path upgrades, keep-alives).
//!
//! Requests (JSON, field `op`) and their answers (`{"ok":true,...}` or `{"ok":false,"code":...,"error":...}`):
//! - `status`: `paired`, `host`, `host_id`, `device_id`, `connected`, `path`
//! - `pair` (`ticket`): pairs with the computer showing that code; `host`, `path`
//! - `connect`: connects to the paired computer; `host`, `path`
//! - `sessions`: `host`, `sessions` (see warp-remote's protocol), `path`, `version` (the protocol both speak)
//! - `ping`: `rtt_ms`, `path`
//! - `disconnect`
//! - `forget`: at once, also while another call waits for that computer (a connect to one that is gone waits up to
//!   30 s): that call, and any made before the forget still waiting for its turn, ends with code `cancelled`. The
//!   device keeps its own key.
//! - `remember` (`ticket`, `name`): remembers the computer of that code without reaching it (tests: one that is gone)
//! - `watch` (`window`, `pane_id`, `since`, optional `before`, `agent`): `pane` (warp-remote's `PaneView`: a chat or the
//!   screen's text, the question it shows, whether this phone may type; with `agent`, that subagent's conversation)
//! - `unwatch` (`window`, `pane_id`, optional `agent`)
//! - `input` (`window`, `pane_id`, optional `text`, `keys`, `option` + `prompt`): types into the pane
//!
//! `path` is `{"kind":"direct"|"relay"|"none","addr":...,"rtt_ms":...}`.

use std::{
    ffi::{CStr, CString, c_char},
    panic::{AssertUnwindSafe, catch_unwind},
    path::{Path, PathBuf},
    sync::{
        Mutex,
        atomic::{AtomicBool, Ordering},
    },
};

use serde_json::{Value, json};
use tokio::sync::watch;
use warp_remote::{
    client::{Client, RemoteError},
    host::Net,
    store::{self, HostRecord},
    ticket::Ticket,
};

pub struct WrCore {
    rt: tokio::runtime::Runtime,
    client: Mutex<Client>,
    dir: PathBuf,
    /// How many times the computer was forgotten: a call made before the last `forget` gives up at once.
    forgets: watch::Sender<u64>,
    /// A `forget` whose cleanup (the connection, the host saved again by a connect on its way out) is still to do: the
    /// first to hold the client does it, before anything else.
    forget_pending: AtomicBool,
}

fn error(e: &warp_remote::Error) -> Value {
    match e.downcast_ref::<RemoteError>() {
        Some(r) => json!({"ok": false, "code": r.code, "error": r.message}),
        None => json!({"ok": false, "code": "error", "error": e.to_string()}),
    }
}

impl WrCore {
    pub fn open(dir: &Path, name: &str) -> Result<WrCore, warp_remote::Error> {
        WrCore::open_with(dir, name, Net::Internet)
    }

    pub fn open_with(dir: &Path, name: &str, net: Net) -> Result<WrCore, warp_remote::Error> {
        let rt = tokio::runtime::Builder::new_multi_thread().worker_threads(2).enable_all().build()?;
        let client = rt.block_on(Client::open(dir, name, net))?;
        Ok(WrCore {
            rt,
            client: Mutex::new(client),
            dir: dir.to_path_buf(),
            forgets: watch::channel(0).0,
            forget_pending: AtomicBool::new(false),
        })
    }

    pub fn call(&self, req: &Value) -> Value {
        let op = req.get("op").and_then(Value::as_str).unwrap_or("");
        if op == "forget" {
            return self.forget();
        }
        let epoch = *self.forgets.borrow();
        let mut forgotten = self.forgets.subscribe();
        let mut c = self.lock();
        let r = self.rt.block_on(async {
            tokio::select! {
                biased;
                _ = forgotten.wait_for(|&n| n != epoch) => None,
                r = run(&mut c, &self.dir, op, req) => Some(r),
            }
        });
        match r {
            Some(Ok(mut v)) => {
                v["ok"] = json!(true);
                v
            }
            Some(Err(e)) => error(&e),
            None => {
                c.disconnect();  // a request cut halfway would leave its answer on the stream
                json!({"ok": false, "code": "cancelled", "error": "the computer was forgotten"})
            }
        }
    }

    /// Forgets the paired computer without waiting for the client: a call still waiting for that computer (or for its
    /// turn) is cancelled, and the host is gone from disk at once, so a call that gets the client first finds none.
    fn forget(&self) -> Value {
        self.forget_pending.store(true, Ordering::SeqCst);
        self.forgets.send_modify(|n| *n += 1);
        let r = store::forget_host(&self.dir);
        drop(self.lock());
        match r {
            Ok(()) => json!({"ok": true}),
            Err(e) => error(&e),
        }
    }

    /// The client, once a pending forget is done with it.
    fn lock(&self) -> std::sync::MutexGuard<'_, Client> {
        let mut c = self.client.lock().unwrap();
        if self.forget_pending.swap(false, Ordering::SeqCst) {
            c.disconnect();
            let _ = store::forget_host(&self.dir);
        }
        c
    }
}

async fn run(c: &mut Client, dir: &Path, op: &str, req: &Value) -> Result<Value, warp_remote::Error> {
    Ok(match op {
        "status" => {
            let host = c.host();
            json!({"paired": host.is_some(), "host": host.as_ref().map(|h| h.name.clone()),
                   "host_id": host.map(|h| h.addr.id.to_string()), "device_id": c.id().to_string(),
                   "connected": c.connected(), "path": c.path()})
        }
        "pair" => {
            let ticket = req.get("ticket").and_then(Value::as_str).unwrap_or("");
            let host = c.pair(ticket).await?;
            json!({"host": host.name, "path": c.path()})
        }
        "connect" => {
            c.connect().await?;
            json!({"host": c.host_name(), "path": c.path()})
        }
        "sessions" => {
            let sessions = c.sessions().await?;
            let version = c.version().await?;
            json!({"host": c.host_name(), "sessions": sessions, "path": c.path(), "version": version})
        }
        "ping" => {
            let rtt = c.ping().await?;
            json!({"rtt_ms": rtt.as_secs_f64() * 1000.0, "path": c.path()})
        }
        "disconnect" => {
            c.disconnect();
            json!({})
        }
        "remember" => {
            let t = Ticket::decode(req.get("ticket").and_then(Value::as_str).unwrap_or(""))?;
            let name = req.get("name").and_then(Value::as_str).unwrap_or("").to_string();
            c.disconnect();
            store::save_host(dir, &HostRecord { addr: t.addr, name: name.clone() })?;
            json!({"host": name})
        }
        "watch" => {
            let (w, p) = pane(req);
            let since = req.get("since").and_then(Value::as_u64).unwrap_or(0);
            let before = req.get("before").and_then(Value::as_u64);
            let agent = req.get("agent").and_then(Value::as_str).unwrap_or("");
            json!({"pane": c.watch(w, p, since, before, agent).await?})
        }
        "unwatch" => {
            let (w, p) = pane(req);
            c.unwatch(w, p, req.get("agent").and_then(Value::as_str).unwrap_or("")).await?;
            json!({})
        }
        "input" => {
            let (w, p) = pane(req);
            let text = req.get("text").and_then(Value::as_str).unwrap_or("");
            let keys: Vec<&str> = req.get("keys").and_then(Value::as_array).into_iter().flatten().filter_map(Value::as_str).collect();
            let option = req.get("option").and_then(Value::as_u64).map(|o| o as u32);
            let prompt = req.get("prompt").and_then(Value::as_str).unwrap_or("");
            c.input(w, p, text, &keys, option, prompt).await?;
            json!({})
        }
        _ => return Err(warp_remote::err(format!("unknown op {op:?}"))),
    })
}

/// A pane in a request: its window and pane id.
fn pane(req: &Value) -> (u64, u32) {
    let n = |k: &str| req.get(k).and_then(Value::as_u64).unwrap_or(0);
    (n("window"), n("pane_id") as u32)
}

fn to_c(v: &Value) -> *mut c_char {
    CString::new(v.to_string()).unwrap_or_default().into_raw()
}

/// # Safety
/// Both arguments are NUL-terminated UTF-8 strings (or null).
#[unsafe(no_mangle)]
pub unsafe extern "C" fn wr_core_open(state_dir: *const c_char, device_name: *const c_char) -> *mut WrCore {
    let s = |p: *const c_char| if p.is_null() { String::new() } else { unsafe { CStr::from_ptr(p) }.to_string_lossy().into_owned() };
    let (dir, name) = (s(state_dir), s(device_name));
    match catch_unwind(|| WrCore::open(Path::new(&dir), &name)) {
        Ok(Ok(core)) => Box::into_raw(Box::new(core)),
        _ => std::ptr::null_mut(),
    }
}

/// # Safety
/// `core` comes from `wr_core_open`; `request_json` is a NUL-terminated string. Free the answer with `wr_string_free`.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn wr_core_call(core: *mut WrCore, request_json: *const c_char) -> *mut c_char {
    if core.is_null() || request_json.is_null() {
        return to_c(&json!({"ok": false, "code": "error", "error": "no core"}));
    }
    let core = unsafe { &*core };
    let text = unsafe { CStr::from_ptr(request_json) }.to_string_lossy().into_owned();
    let answer = catch_unwind(AssertUnwindSafe(|| match serde_json::from_str::<Value>(&text) {
        Ok(req) => core.call(&req),
        Err(e) => json!({"ok": false, "code": "error", "error": e.to_string()}),
    }));
    to_c(&answer.unwrap_or_else(|_| json!({"ok": false, "code": "error", "error": "internal error"})))
}

/// # Safety
/// `s` comes from `wr_core_call` (or is null).
#[unsafe(no_mangle)]
pub unsafe extern "C" fn wr_string_free(s: *mut c_char) {
    if !s.is_null() {
        drop(unsafe { CString::from_raw(s) });
    }
}

/// # Safety
/// `core` comes from `wr_core_open` (or is null) and is not used afterwards.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn wr_core_close(core: *mut WrCore) {
    if core.is_null() {
        return;
    }
    let core = unsafe { Box::from_raw(core) };
    let WrCore { rt, client, .. } = *core;
    if let Ok(c) = client.into_inner() {
        rt.block_on(c.close());
    }
    rt.shutdown_background();
}

#[cfg(test)]
mod tests {
    use std::{
        net::UdpSocket,
        sync::Arc,
        thread,
        time::{Duration, Instant},
    };

    use super::*;

    /// A computer that is gone (its address is a UDP socket that never answers, so a connect waits its full 30 s):
    /// Forget answers at once, cancels the connect in flight and the call waiting behind it, and nothing brings the
    /// computer back.
    #[test]
    fn forget_cancels_a_connect_in_flight() {
        let dir = std::env::temp_dir().join(format!("wr-core-forget-{}", std::process::id()));
        let _ = std::fs::remove_dir_all(&dir);
        let core = Arc::new(WrCore::open_with(&dir, "test phone", Net::Loopback).unwrap());
        let silent = UdpSocket::bind("127.0.0.1:0").unwrap();
        // Its pairing code: format 1, endpoint id, secret, one IPv4 address (see warp-remote's ticket.rs).
        let id = store::load_or_create_key(&dir.join("gone")).unwrap().public();
        let mut b = vec![1];
        b.extend_from_slice(id.as_bytes());
        b.extend_from_slice(&[0; 16]);
        b.extend_from_slice(&[1, 4, 6, 127, 0, 0, 1]);
        b.extend_from_slice(&silent.local_addr().unwrap().port().to_be_bytes());
        let (addr, secret) = warp_remote::ticket::decode_addr(&b).unwrap();
        let ticket = Ticket { addr, secret }.encode();
        let ok = |a: &Value| a["ok"] == json!(true);
        assert!(ok(&core.call(&json!({"op": "remember", "ticket": ticket, "name": "Gone"}))));
        let s = core.call(&json!({"op": "status"}));
        assert_eq!((&s["paired"], &s["host"]), (&json!(true), &json!("Gone")), "{s}");

        // The app's refresh dials it, and its next call waits for its turn.
        let t0 = Instant::now();
        let dial = thread::spawn({
            let core = core.clone();
            move || core.call(&json!({"op": "sessions"}))
        });
        let queued = thread::spawn({
            let core = core.clone();
            move || {
                thread::sleep(Duration::from_millis(100));
                core.call(&json!({"op": "ping"}))
            }
        });
        thread::sleep(Duration::from_millis(600));
        assert!(!dial.is_finished(), "the connect should still wait for the computer");

        let t = Instant::now();
        assert!(ok(&core.call(&json!({"op": "forget"}))));
        assert!(t.elapsed() < Duration::from_secs(2), "forget took {:?}", t.elapsed());
        let a = dial.join().unwrap();
        assert_eq!(a["code"], json!("cancelled"), "{a}");
        let q = queued.join().unwrap();
        assert_eq!(q["code"], json!("cancelled"), "{q}");
        assert!(t0.elapsed() < Duration::from_secs(5), "took {:?}", t0.elapsed());

        assert_eq!(core.call(&json!({"op": "status"}))["paired"], json!(false));
        assert!(!dir.join("host.txt").exists());
        assert!(dir.join("key").exists(), "the device keeps its own key");
        // Afterwards: nothing to connect to, and forgetting again is fine.
        assert_eq!(core.call(&json!({"op": "connect"}))["code"], json!("not_paired"));
        assert!(ok(&core.call(&json!({"op": "forget"}))));
        drop(core);
        let _ = std::fs::remove_dir_all(&dir);
    }
}
