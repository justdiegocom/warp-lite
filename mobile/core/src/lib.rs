//! The phone's core: warp-remote's client (shared with `warp-remote-cli`) behind a small synchronous C
//! interface, `include/warp_remote_core.h`. Nothing async crosses it: each call blocks until done, and the
//! app makes them from a background queue. iroh keeps running on the core's own threads between calls
//! (path upgrades, keep-alives).
//!
//! Requests (JSON, field `op`) and their answers (`{"ok":true,...}` or `{"ok":false,"code":...,"error":...}`):
//! - `status`: `paired`, `host`, `host_id`, `device_id`, `connected`, `path`
//! - `pair` (`ticket`): pairs with the computer showing that code; `host`, `path`
//! - `connect`: connects to the paired computer; `host`, `path`
//! - `sessions`: `host`, `sessions` (see warp-remote's protocol), `path`
//! - `ping`: `rtt_ms`, `path`
//! - `disconnect`, `forget`
//!
//! `path` is `{"kind":"direct"|"relay"|"none","addr":...,"rtt_ms":...}`.

use std::{
    ffi::{CStr, CString, c_char},
    panic::{AssertUnwindSafe, catch_unwind},
    path::Path,
    sync::Mutex,
};

use serde_json::{Value, json};
use warp_remote::{
    client::{Client, RemoteError},
    host::Net,
};

pub struct WrCore {
    rt: tokio::runtime::Runtime,
    client: Mutex<Client>,
}

fn error(e: &warp_remote::Error) -> Value {
    match e.downcast_ref::<RemoteError>() {
        Some(r) => json!({"ok": false, "code": r.code, "error": r.message}),
        None => json!({"ok": false, "code": "error", "error": e.to_string()}),
    }
}

impl WrCore {
    pub fn open(dir: &Path, name: &str) -> Result<WrCore, warp_remote::Error> {
        let rt = tokio::runtime::Builder::new_multi_thread().worker_threads(2).enable_all().build()?;
        let client = rt.block_on(Client::open(dir, name, Net::Internet))?;
        Ok(WrCore { rt, client: Mutex::new(client) })
    }

    pub fn call(&self, req: &Value) -> Value {
        let mut c = self.client.lock().unwrap();
        let op = req.get("op").and_then(Value::as_str).unwrap_or("");
        let r: Result<Value, warp_remote::Error> = self.rt.block_on(async {
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
                    json!({"host": c.host_name(), "sessions": sessions, "path": c.path()})
                }
                "ping" => {
                    let rtt = c.ping().await?;
                    json!({"rtt_ms": rtt.as_secs_f64() * 1000.0, "path": c.path()})
                }
                "disconnect" => {
                    c.disconnect();
                    json!({})
                }
                "forget" => {
                    c.forget()?;
                    json!({})
                }
                _ => return Err(warp_remote::err(format!("unknown op {op:?}"))),
            })
        });
        match r {
            Ok(mut v) => {
                v["ok"] = json!(true);
                v
            }
            Err(e) => error(&e),
        }
    }
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
    let WrCore { rt, client } = *core;
    if let Ok(c) = client.into_inner() {
        rt.block_on(c.close());
    }
    rt.shutdown_background();
}
