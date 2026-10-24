//! Protocol v2 on loopback: watching a pane (its text, then only what changed, and the question it shows) and
//! typing into it, which a device may only do once allowed; an option is only chosen while that question is shown.

use std::{
    path::PathBuf,
    sync::{Arc, Mutex},
    time::Duration,
};

use warp_remote::{
    Result,
    client::{Client, RemoteError},
    err,
    host::{Host, HostConfig, Net, SessionSource},
    proto::Session,
    store,
    watch::{PaneInfo, PaneInput, PaneScreen},
};

#[derive(Default)]
struct Pane {
    lines: Vec<String>,
    typed: Vec<PaneInput>,
}

#[derive(Clone, Default)]
struct Fake(Arc<Mutex<Pane>>);

impl SessionSource for Fake {
    async fn sessions(&self) -> Result<Vec<Session>> {
        Ok(vec![Session { window: 9, tab: 0, pane: 0, pane_id: 4, title: "claude".into(), busy: true, agent: "claude".into(), ..Default::default() }])
    }

    async fn screen(&self, window: u64, pane_id: u32) -> Result<PaneScreen> {
        if (window, pane_id) != (9, 4) {
            return Err(err("no such pane"));
        }
        let p = self.0.lock().unwrap();
        Ok(PaneScreen { rows: p.lines.len(), lines: p.lines.clone() })
    }

    async fn info(&self, _window: u64, _pane_id: u32) -> Result<PaneInfo> {
        Ok(PaneInfo { agent: "claude".into(), running: "claude".into(), cwd: "C:\\src".into(), ..Default::default() })
    }

    async fn input(&self, _window: u64, _pane_id: u32, _device: &str, what: &PaneInput) -> Result<()> {
        self.0.lock().unwrap().typed.push(what.clone());
        Ok(())
    }
}

fn tmp(name: &str) -> PathBuf {
    let d = std::env::temp_dir().join(format!("warp-remote-panes-{}-{name}", std::process::id()));
    let _ = std::fs::remove_dir_all(&d);
    d
}

fn code(e: &warp_remote::Error) -> String {
    e.downcast_ref::<RemoteError>().map(|r| r.code.clone()).unwrap_or_else(|| format!("other: {e}"))
}

const PROMPT: &str = " Do you want to proceed?\n ❯ 1. Yes\n   2. Yes, and don't ask again\n   3. No\n\n Esc to cancel · Tab to amend";

#[tokio::test]
async fn watch_and_type() {
    let hdir = tmp("host");
    let fake = Fake::default();
    fake.0.lock().unwrap().lines = vec!["❯ hi".into(), "".into(), "  ? for shortcuts".into()];
    let cfg = HostConfig { dir: hdir.clone(), name: "TEST-HOST".into(), net: Net::Loopback };
    let host = Host::bind(cfg, fake.clone(), |_| {}).await.unwrap();
    {
        let host = host.clone();
        tokio::spawn(async move { host.run().await });
    }
    let t = host.start_pairing(Duration::from_secs(60)).await.unwrap();
    let mut phone = Client::open(&tmp("phone"), "Test phone", Net::Loopback).await.unwrap();
    phone.pair(&t.encode()).await.unwrap();
    assert_eq!(phone.version().await.unwrap(), 3);
    // The list says what the agent there is doing, from its screen.
    let s = phone.sessions().await.unwrap();
    assert_eq!((s[0].pane_id, s[0].status.as_str()), (4, "finished"));

    // The screen as text (no transcript here), then only when it changes.
    let v = phone.watch(9, 4, 0, None, "").await.unwrap();
    assert_eq!((v.kind.as_str(), v.status.as_str(), v.reset, v.typing), ("screen", "finished", true, false));
    assert_eq!(v.screen.as_ref().unwrap().len(), 3);
    let v2 = phone.watch(9, 4, v.rev, None, "").await.unwrap();
    assert!(v2.screen.is_none() && !v2.reset && v2.prompt.is_none());
    let e = phone.watch(9, 5, 0, None, "").await.unwrap_err();
    assert_eq!(code(&e), "no_pane", "{e}");
    // A subagent: only of a pane whose conversation is open (this one shows its screen).
    let e = phone.watch(9, 4, 0, None, "a6d5e25a0889e3ee4").await.unwrap_err();
    assert_eq!(code(&e), "no_pane", "{e}");

    // Typing is off until the device is allowed in Warp.
    let e = phone.input(9, 4, "hello", &["enter"], None, "").await.unwrap_err();
    assert_eq!(code(&e), "typing_off", "{e}");
    assert!(fake.0.lock().unwrap().typed.is_empty());
    store::set_typing(&hdir, &phone.id(), true).unwrap();
    phone.input(9, 4, "two\nlines", &["enter"], None, "").await.unwrap();
    assert_eq!(fake.0.lock().unwrap().typed, [PaneInput::Text("two lines".into()), PaneInput::Key("enter".into())]);
    let e = phone.input(9, 4, "", &["rm -rf"], None, "").await.unwrap_err();
    assert_eq!(code(&e), "bad_request", "{e}");

    // A question appears: its options come with the pane, and choosing one moves to it and presses Enter.
    fake.0.lock().unwrap().lines = PROMPT.lines().map(String::from).collect();
    assert_eq!(phone.sessions().await.unwrap()[0].status, "waiting");
    let v3 = phone.watch(9, 4, v2.rev, None, "").await.unwrap();
    let p = v3.prompt.expect("prompt");
    assert_eq!((p.question.as_str(), p.options.len(), p.selected, v3.status.as_str()), ("Do you want to proceed?", 3, 0, "waiting"));
    assert!(v3.typing);
    fake.0.lock().unwrap().typed.clear();
    let e = phone.input(9, 4, "", &[], Some(2), "0000000000000000").await.unwrap_err();
    assert_eq!(code(&e), "prompt_gone", "{e}");
    phone.input(9, 4, "", &[], Some(2), &p.id).await.unwrap();
    let keys: Vec<PaneInput> = ["down", "down", "enter"].iter().map(|k| PaneInput::Key(k.to_string())).collect();
    assert_eq!(fake.0.lock().unwrap().typed, keys);

    // Removed in Warp: no more typing even if still listed.
    store::remove_device(&hdir, &phone.id()).unwrap();
    phone.unwatch(9, 4, "").await.ok();
    phone.close().await;
    host.close().await;
}
