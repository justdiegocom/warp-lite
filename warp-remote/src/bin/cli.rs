//! warp-remote-cli: a client like the phone app, for tests and scripts.
//!
//!   warp-remote-cli [--state <dir>] [--name <device name>] [--local | --relay-only] [--json] <command>
//!     pair <ticket>     pair with the Warp that shows that code (and remember it)
//!     sessions          its tabs and panes
//!     ping [n]          n round trips (default 5)
//!     path [seconds]    which network path the connection uses, once a second (default 10)
//!     forget            forget the paired Warp
//!
//! `--relay-only` connects only through n0's relay (as a phone behind a strict NAT would);
//! `--local` stays on loopback. The state directory (default ~/.warp-remote-cli) keeps this device's key and the paired Warp.

use std::{path::PathBuf, time::Duration};

use warp_remote::{Result, client::Client, err, host::Net};

struct Opts {
    state: PathBuf,
    name: String,
    net: Net,
    json: bool,
    cmd: Vec<String>,
}

fn opts() -> Result<Opts> {
    let home = std::env::var_os("APPDATA").or_else(|| std::env::var_os("HOME")).unwrap_or_else(|| ".".into());
    let mut o = Opts {
        state: PathBuf::from(home).join(".warp-remote-cli"),
        name: "warp-remote-cli".into(),
        net: Net::Internet,
        json: false,
        cmd: Vec::new(),
    };
    let mut it = std::env::args().skip(1);
    while let Some(a) = it.next() {
        match a.as_str() {
            "--state" => o.state = it.next().ok_or_else(|| err("--state needs a folder"))?.into(),
            "--name" => o.name = it.next().ok_or_else(|| err("--name needs a value"))?,
            "--local" => o.net = Net::Loopback,
            "--relay-only" => o.net = Net::RelayOnly,
            "--json" => o.json = true,
            _ => o.cmd.push(a),
        }
    }
    Ok(o)
}

fn main() {
    let o = match opts() {
        Ok(o) => o,
        Err(e) => {
            eprintln!("{e}");
            std::process::exit(2);
        }
    };
    let rt = tokio::runtime::Builder::new_current_thread().enable_all().build().expect("tokio");
    if let Err(e) = rt.block_on(run(o)) {
        eprintln!("error: {e}");
        std::process::exit(1);
    }
}

async fn run(o: Opts) -> Result<()> {
    let mut c = Client::open(&o.state, &o.name, o.net).await?;
    let cmd: Vec<&str> = o.cmd.iter().map(|s| s.as_str()).collect();
    let r = match cmd.as_slice() {
        ["pair", ticket] => {
            let host = c.pair(ticket).await?;
            if o.json {
                println!("{}", serde_json::json!({"paired": host.name, "id": host.addr.id.to_string(), "path": c.path()}));
            } else {
                println!("paired with {} ({})", host.name, host.addr.id.fmt_short());
                println!("path {}", fmt_path(&c));
            }
            Ok(())
        }
        ["sessions"] => {
            let list = c.sessions().await?;
            if o.json {
                println!("{}", serde_json::json!({"host": c.host_name(), "path": c.path(), "sessions": list}));
            } else {
                println!("{} — {} panes, path {}", c.host_name().unwrap_or_default(), list.len(), fmt_path(&c));
                for s in &list {
                    let busy = if s.busy { "*" } else { " " };
                    println!("{busy} w{:x} t{} p{}  {:<24}  {:<28}  {}", s.window, s.tab, s.pane, s.tab_name, s.title, s.cwd);
                }
            }
            Ok(())
        }
        ["ping", rest @ ..] => {
            let n: u32 = rest.first().map(|s| s.parse()).transpose()?.unwrap_or(5);
            let mut times = Vec::new();
            for i in 0..n {
                let t = c.ping().await?;
                times.push(t.as_secs_f64() * 1000.0);
                if !o.json {
                    println!("ping {i}: {:.1} ms  path {}", t.as_secs_f64() * 1000.0, fmt_path(&c));
                }
                tokio::time::sleep(Duration::from_millis(300)).await;
            }
            times.sort_by(|a, b| a.partial_cmp(b).unwrap());
            let median = times.get(times.len() / 2).copied().unwrap_or(0.0);
            if o.json {
                println!("{}", serde_json::json!({"pings": n, "median_ms": median, "min_ms": times.first(), "path": c.path()}));
            } else {
                println!("median {median:.1} ms");
            }
            Ok(())
        }
        ["path", rest @ ..] => {
            let secs: u64 = rest.first().map(|s| s.parse()).transpose()?.unwrap_or(10);
            c.connect().await?;
            for i in 0..secs {
                let p = c.path();
                if o.json {
                    println!("{}", serde_json::json!({"t": i, "path": p}));
                } else {
                    println!("{i:>3}s  {}", fmt_path(&c));
                }
                tokio::time::sleep(Duration::from_secs(1)).await;
            }
            Ok(())
        }
        ["forget"] => c.forget(),
        _ => Err(err("usage: warp-remote-cli [--state dir] [--name n] [--local | --relay-only] [--json] pair <ticket> | sessions | ping [n] | path [s] | forget")),
    };
    c.close().await;
    r
}

fn fmt_path(c: &Client) -> String {
    let p = c.path();
    format!("{} {} ({:.1} ms)", p.kind, p.addr, p.rtt_ms)
}
