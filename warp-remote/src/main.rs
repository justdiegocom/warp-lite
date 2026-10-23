//! warp-remote.exe: Warp's sidecar. Warp starts one per Warp (its windows share it) when a phone is
//! paired or pairing is asked for; it ends when the last window of that Warp closes.
//!
//!   warp-remote --dir <data dir> --instance <key> --pid <warp pid> [--pair <hwnd>]
//!
//! For tests (any OS): `--mock <sessions.json>` lists those sessions instead of Warp's,
//! `--print-ticket` prints a pairing code on stdout, `--local` stays on loopback with no relays and
//! `--ttl <s>` changes how long a code lasts.

#![cfg_attr(windows, windows_subsystem = "windows")]

#[cfg(windows)]
use std::sync::Arc;
use std::{path::PathBuf, time::Duration};

use tokio::sync::mpsc;
use warp_remote::{
    PAIRING_TTL, Result, err,
    host::{Event, Host, HostConfig, Net, SessionSource, computer_name},
    proto::Session,
    store,
};

#[derive(Debug, Clone, PartialEq)]
#[cfg_attr(not(windows), allow(dead_code))]  // Reload and Quit come from Warp's windows
enum Cmd {
    Pair(isize),
    PrintTicket,
    Reload,
    Quit,
    Idle,
}

#[derive(Default)]
struct Args {
    dir: Option<PathBuf>,
    name: Option<String>,
    instance: Option<isize>,
    pid: u32,
    pair: Option<isize>,
    mock: Option<PathBuf>,
    print_ticket: bool,
    local: bool,
    ttl: Option<u64>,
}

fn parse_args() -> Result<Args> {
    let mut a = Args::default();
    let mut it = std::env::args_os().skip(1);
    while let Some(arg) = it.next() {
        let arg = arg.to_string_lossy().to_string();
        let mut val = || it.next().map(|v| v.to_string_lossy().to_string()).ok_or_else(|| err(format!("{arg} needs a value")));
        match arg.as_str() {
            "--dir" => a.dir = Some(PathBuf::from(val()?)),
            "--name" => a.name = Some(val()?),
            "--instance" => a.instance = Some(isize::from_str_radix(&val()?, 16)?),
            "--pid" => a.pid = val()?.parse()?,
            "--pair" => a.pair = Some(isize::from_str_radix(&val()?, 16)?),
            "--mock" => a.mock = Some(PathBuf::from(val()?)),
            "--ttl" => a.ttl = Some(val()?.parse()?),
            "--print-ticket" => a.print_ticket = true,
            "--local" => a.local = true,
            _ => return Err(err(format!("unknown argument {arg}"))),
        }
    }
    Ok(a)
}

/// Sessions from a JSON file (read on every request, so a test can change it).
struct MockSource(PathBuf);

impl SessionSource for MockSource {
    async fn sessions(&self) -> Result<Vec<Session>> {
        Ok(serde_json::from_slice(&std::fs::read(&self.0)?)?)
    }
}

#[cfg(windows)]
struct WarpSource(Arc<warp_remote::win::WarpLink>);

#[cfg(windows)]
impl SessionSource for WarpSource {
    async fn sessions(&self) -> Result<Vec<Session>> {
        self.0.sessions().await
    }
}

enum Source {
    Mock(MockSource),
    #[cfg(windows)]
    Warp(WarpSource),
}

impl SessionSource for Source {
    async fn sessions(&self) -> Result<Vec<Session>> {
        match self {
            Source::Mock(m) => m.sessions().await,
            #[cfg(windows)]
            Source::Warp(w) => w.sessions().await,
        }
    }
}

fn main() {
    let args = match parse_args() {
        Ok(a) => a,
        Err(e) => {
            eprintln!("warp-remote: {e}");
            std::process::exit(2);
        }
    };
    let Some(dir) = args.dir.clone() else {
        eprintln!("warp-remote: --dir is required");
        std::process::exit(2);
    };
    let rt = tokio::runtime::Builder::new_current_thread().enable_all().build().expect("tokio");
    if let Err(e) = rt.block_on(run(args, dir.clone())) {
        store::log(&dir, &format!("error: {e}"));
        eprintln!("warp-remote: {e}");
        std::process::exit(1);
    }
}

async fn run(a: Args, dir: PathBuf) -> Result<()> {
    std::fs::create_dir_all(&dir)?;
    #[cfg(windows)]
    if let Some(key) = a.instance
        && !warp_remote::win::single_instance(key)
    {
        store::log(&dir, "another warp-remote is running for this Warp");
        return Ok(());
    }
    let (tx, mut rx) = mpsc::unbounded_channel::<Cmd>();

    #[cfg(windows)]
    let link = match a.instance {
        Some(key) => {
            let (wtx, mut wrx) = mpsc::unbounded_channel::<warp_remote::win::Cmd>();
            let tx2 = tx.clone();
            tokio::spawn(async move {
                use warp_remote::win::Cmd as W;
                while let Some(c) = wrx.recv().await {
                    let _ = tx2.send(match c {
                        W::Pair(h) => Cmd::Pair(h),
                        W::Reload => Cmd::Reload,
                        W::Quit => Cmd::Quit,
                        W::Idle => Cmd::Idle,
                    });
                }
            });
            let link = Arc::new(warp_remote::win::WarpLink::start(key, wtx.clone())?);
            if a.pid != 0 {
                warp_remote::win::follow_warp(a.pid, key, wtx);
            }
            Some(link)
        }
        None => None,
    };

    let source = match &a.mock {
        Some(path) => Source::Mock(MockSource(path.clone())),
        #[cfg(windows)]
        None if link.is_some() => Source::Warp(WarpSource(link.clone().unwrap())),
        None => return Err(err("--instance (Warp) or --mock is required")),
    };

    let on_event = {
        let tx = tx.clone();
        let print = a.print_ticket;
        #[cfg(windows)]
        let link = link.clone();
        move |ev: Event| {
            if print {
                println!("event {ev:?}");
            }
            #[cfg(windows)]
            if let Some(link) = &link {
                use warp_remote::win::{EVENT_PAIRED, EVENT_PAIRING_ENDED, EVENT_STATE};
                match &ev {
                    Event::Phones(n) => {
                        link.set_phones(*n);
                        link.notify(*n, EVENT_STATE);
                    }
                    Event::Paired { .. } => link.notify(usize::MAX, EVENT_PAIRED),
                    Event::PairingEnded => link.notify(usize::MAX, EVENT_PAIRING_ENDED),
                }
            }
            if matches!(ev, Event::PairingEnded | Event::Phones(0)) {
                let _ = tx.send(Cmd::Idle);
            }
        }
    };
    let cfg = HostConfig { dir: dir.clone(), name: a.name.clone().unwrap_or_else(computer_name), net: if a.local { Net::Loopback } else { Net::Internet } };
    let host = Host::bind(cfg, source, on_event).await?;
    let ttl = Duration::from_secs(a.ttl.unwrap_or(PAIRING_TTL.as_secs()));
    {
        let host = host.clone();
        tokio::spawn(async move { host.run().await });
    }
    if let Some(h) = a.pair {
        let _ = tx.send(Cmd::Pair(h));
    }
    if a.print_ticket {
        let _ = tx.send(Cmd::PrintTicket);
    }
    let idle = |host: &Host<Source>| {
        store::devices(&dir).is_empty() && host.phones() == 0 && !host.pairing_active() && a.mock.is_none()
    };
    if a.pair.is_none() && !a.print_ticket && idle(&host) {
        store::log(&dir, "nothing paired: not needed");
        host.close().await;
        return Ok(());
    }
    while let Some(cmd) = rx.recv().await {
        match cmd {
            Cmd::Pair(_to) => {
                let host = host.clone();
                #[cfg(windows)]
                let link = link.clone();
                tokio::spawn(async move {
                    let text = match host.start_pairing(ttl).await {
                        Ok(t) => format!("ticket\t{}\t{}", t.encode(), ttl.as_secs()),
                        Err(e) => format!("error\t{e}"),
                    };
                    #[cfg(windows)]
                    if let Some(link) = link {
                        let _ = tokio::task::spawn_blocking(move || link.send_ticket(_to, &text)).await;
                    }
                    #[cfg(not(windows))]
                    let _ = text;
                });
            }
            Cmd::PrintTicket => {
                let t = host.start_pairing(ttl).await?;
                println!("ticket {}", t.encode());
                println!("id {}", host.endpoint().id());
            }
            Cmd::Reload => {
                host.reload();
                if idle(&host) {
                    break;
                }
            }
            Cmd::Idle => {
                if idle(&host) {
                    store::log(&dir, "nothing paired any more");
                    break;
                }
            }
            Cmd::Quit => break,
        }
    }
    #[cfg(windows)]
    if let Some(link) = &link {
        link.stop();
    }
    host.close().await;
    store::log(&dir, "stopped");
    Ok(())
}
