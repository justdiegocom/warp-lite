//! Host and client on loopback (no relays, nothing leaves this machine): pairing with a one-time,
//! expiring code, unknown devices turned away, sessions, ping, reconnecting without a code and
//! revoking a device.

use std::{
    path::PathBuf,
    sync::{Arc, Mutex},
    time::Duration,
};

use warp_remote::{
    Result,
    client::{Client, RemoteError},
    host::{Event, Host, HostConfig, Net, SessionSource},
    proto::Session,
    store,
    ticket::Ticket,
};

struct Fixed(Vec<Session>);

impl SessionSource for Fixed {
    async fn sessions(&self) -> Result<Vec<Session>> {
        Ok(self.0.clone())
    }
}

fn tmp(name: &str) -> PathBuf {
    let d = std::env::temp_dir().join(format!("warp-remote-test-{}-{name}", std::process::id()));
    let _ = std::fs::remove_dir_all(&d);
    d
}

fn code(e: &warp_remote::Error) -> String {
    e.downcast_ref::<RemoteError>().map(|r| r.code.clone()).unwrap_or_else(|| format!("other: {e}"))
}

fn panes() -> Vec<Session> {
    vec![
        Session {
            window: 1,
            tab: 0,
            pane: 0,
            tab_name: "warp-lite".into(),
            title: "Fix the updater".into(),
            cwd: "C:\\src\\warp-lite".into(),
            busy: true,
            running: "claude".into(),
            agent: "claude".into(),
            pane_id: 1,
            ..Default::default()
        },
        Session { window: 2, tab: 1, pane: 1, tab_name: "logs".into(), title: "pwsh".into(), cwd: "C:\\".into(), ..Default::default() },
    ]
}

#[tokio::test]
async fn pairing_sessions_and_revoking() {
    let hdir = tmp("host");
    let events = Arc::new(Mutex::new(Vec::new()));
    let ev = events.clone();
    let cfg = HostConfig { dir: hdir.clone(), name: "TEST-HOST".into(), net: Net::Loopback };
    let host = Host::bind(cfg, Fixed(panes()), move |e| ev.lock().unwrap().push(e)).await.unwrap();
    {
        let host = host.clone();
        tokio::spawn(async move { host.run().await });
    }

    // No pairing code active: an unknown device is turned away.
    let mut stranger = Client::open(&tmp("stranger"), "stranger", Net::Loopback).await.unwrap();
    let fake = Ticket { addr: host.endpoint().addr(), secret: [0; 16] }.encode();
    let e = stranger.pair(&fake).await.unwrap_err();
    assert_eq!(code(&e), "not_paired", "{e}");

    // A code is active: a wrong secret is refused.
    let t = host.start_pairing(Duration::from_secs(60)).await.unwrap();
    let wrong = Ticket { secret: [1; 16], ..t.clone() }.encode();
    let e = stranger.pair(&wrong).await.unwrap_err();
    assert_eq!(code(&e), "bad_secret", "{e}");

    // The right one pairs: the device is in the allow-list with its name.
    let pdir = tmp("phone");
    let mut phone = Client::open(&pdir, "Test phone", Net::Loopback).await.unwrap();
    let rec = phone.pair(&t.encode()).await.unwrap();
    assert_eq!(rec.name, "TEST-HOST");
    let devs = store::devices(&hdir);
    assert_eq!(devs.len(), 1);
    assert_eq!((devs[0].id, devs[0].name.as_str()), (phone.id(), "Test phone"));
    assert_eq!(phone.sessions().await.unwrap(), panes());
    assert!(phone.ping().await.unwrap() < Duration::from_secs(2));
    assert_eq!(phone.path().kind, "direct");
    assert_eq!(host.phones(), 1);

    // The code was one-time: another device can't use it.
    let mut other = Client::open(&tmp("other"), "other", Net::Loopback).await.unwrap();
    let e = other.pair(&t.encode()).await.unwrap_err();
    assert!(["not_paired", "ticket_expired"].contains(&code(&e).as_str()), "{e}");

    // Later, from the remembered host and no code.
    phone.close().await;
    let mut phone = Client::open(&pdir, "Test phone", Net::Loopback).await.unwrap();
    assert_eq!(phone.sessions().await.unwrap().len(), 2);

    // Revoked in Warp: disconnected now and turned away next time.
    store::remove_device(&hdir, &phone.id()).unwrap();
    host.reload();
    tokio::time::sleep(Duration::from_millis(300)).await;
    let e = phone.sessions().await.unwrap_err();
    assert!(["revoked", "not_paired"].contains(&code(&e).as_str()), "{e}");
    let e = phone.sessions().await.unwrap_err();
    assert_eq!(code(&e), "not_paired", "{e}");

    // An expired code.
    let t = host.start_pairing(Duration::from_millis(300)).await.unwrap();
    tokio::time::sleep(Duration::from_millis(600)).await;
    let e = phone.pair(&t.encode()).await.unwrap_err();
    assert!(["not_paired", "ticket_expired"].contains(&code(&e).as_str()), "{e}");

    let ev = events.lock().unwrap().clone();
    assert!(ev.contains(&Event::Paired { name: "Test phone".into() }), "{ev:?}");
    assert!(ev.contains(&Event::Phones(1)), "{ev:?}");
    assert!(ev.contains(&Event::PairingEnded), "{ev:?}");
    phone.close().await;
    other.close().await;
    stranger.close().await;
    host.close().await;
}
