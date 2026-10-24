//! The client: what the phone app (through `mobile/core`) and `warp-remote-cli` use.
//!
//! The client keeps its own secret key (its id is what the host allows) and remembers the host it
//! paired with, so later it reconnects with no ticket: the remembered address, or just the host's
//! id through n0's address lookup when the host's addresses changed.

use std::{
    fmt,
    path::{Path, PathBuf},
    time::{Duration, Instant},
};

use data_encoding::HEXLOWER;
use iroh::{
    Endpoint, EndpointAddr, TransportAddr,
    endpoint::{Connection, ConnectionError, RecvStream, SendStream},
};
use serde::Serialize;
use tokio::time::timeout;

use crate::{
    ALPN, Result,
    host::{Net, endpoint_builder},
    proto::{self, PaneView, Request, Response, Session, code},
    store::{self, HostRecord},
    ticket::Ticket,
};

/// An error the host reported (or a close reason), with its [`code`].
#[derive(Debug, Clone)]
pub struct RemoteError {
    pub code: String,
    pub message: String,
}

impl fmt::Display for RemoteError {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        write!(f, "{}: {}", self.code, self.message)
    }
}

impl std::error::Error for RemoteError {}

fn remote(code: &str, message: impl Into<String>) -> crate::Error {
    Box::new(RemoteError { code: code.into(), message: message.into() })
}

/// The network path a connection uses right now.
#[derive(Debug, Clone, Serialize, PartialEq)]
pub struct PathInfo {
    /// "direct" (UDP between the two), "relay" (through n0's relay) or "none".
    pub kind: String,
    pub addr: String,
    pub rtt_ms: f64,
}

struct Live {
    conn: Connection,
    send: SendStream,
    recv: RecvStream,
    host: String,
    next_id: u64,
    /// The protocol version both sides speak (the lower of the two).
    version: u32,
}

pub struct Client {
    ep: Endpoint,
    dir: PathBuf,
    name: String,
    live: Option<Live>,
}

const CONNECT_TIMEOUT: Duration = Duration::from_secs(30);
const REQUEST_TIMEOUT: Duration = Duration::from_secs(20);

impl Client {
    /// `dir`: where this device keeps its key and its host. `name`: this device's name, shown in Warp.
    pub async fn open(dir: &Path, name: &str, net: Net) -> Result<Client> {
        let key = store::load_or_create_key(dir)?;
        let ep = endpoint_builder(net)?.secret_key(key).bind().await?;
        Ok(Client { ep, dir: dir.to_path_buf(), name: name.into(), live: None })
    }

    pub fn id(&self) -> iroh::PublicKey {
        self.ep.id()
    }

    /// The host this device is paired with.
    pub fn host(&self) -> Option<HostRecord> {
        store::load_host(&self.dir)
    }

    pub fn connected(&self) -> bool {
        self.live.as_ref().is_some_and(|l| l.conn.close_reason().is_none())
    }

    /// Pairs with the host of that ticket (QR code text) and stays connected.
    pub async fn pair(&mut self, ticket: &str) -> Result<HostRecord> {
        let t = Ticket::decode(ticket)?;
        self.disconnect();
        let (conn, mut send, mut recv, host, paired, version) = self.dial(t.addr.clone()).await?;
        if !paired {
            proto::write_frame(&mut send, &Request::Pair { secret: HEXLOWER.encode(&t.secret) }).await?;
            match read(&conn, &mut recv).await? {
                Response::Paired => {}
                Response::Error { code, message } => return Err(remote(&code, message)),
                other => return Err(remote(code::BAD_REQUEST, format!("unexpected answer {other:?}"))),
            }
        }
        let record = HostRecord { addr: t.addr, name: host.clone() };
        store::save_host(&self.dir, &record)?;
        self.live = Some(Live { conn, send, recv, host, next_id: 1, version });
        Ok(record)
    }

    /// Connects to the paired host (nothing to do if already connected).
    pub async fn connect(&mut self) -> Result<()> {
        if let Some(l) = self.live.as_ref() {
            match l.conn.close_reason() {
                None => return Ok(()),
                // Removed in Warp (or forgotten there) while connected: say so instead of trying again.
                Some(ConnectionError::ApplicationClosed(ac))
                    if [code::REVOKED, code::NOT_PAIRED].iter().any(|c| ac.reason.as_ref() == c.as_bytes()) =>
                {
                    let e = explain(&l.conn, remote(code::UNAVAILABLE, "closed"));
                    self.live = None;
                    return Err(e);
                }
                Some(_) => self.live = None,
            }
        }
        let host = self.host().ok_or_else(|| remote(code::NOT_PAIRED, "no paired computer"))?;
        let (conn, send, recv, name, paired, version) = self.dial(host.addr.clone()).await?;
        if !paired {
            conn.close(proto::CLOSE_BYE.into(), b"");
            return Err(remote(code::NOT_PAIRED, "this computer doesn't know this device any more: pair again"));
        }
        if name != host.name {
            let _ = store::save_host(&self.dir, &HostRecord { addr: host.addr, name: name.clone() });
        }
        self.live = Some(Live { conn, send, recv, host: name, next_id: 1, version });
        Ok(())
    }

    /// The protocol version of the connection (connecting first): 2 or more lets the phone watch panes and type.
    pub async fn version(&mut self) -> Result<u32> {
        self.connect().await?;
        Ok(self.live.as_ref().map(|l| l.version).unwrap_or(1))
    }

    #[allow(clippy::type_complexity)]
    async fn dial(&self, addr: EndpointAddr) -> Result<(Connection, SendStream, RecvStream, String, bool, u32)> {
        let conn = timeout(CONNECT_TIMEOUT, self.ep.connect(addr, ALPN))
            .await
            .map_err(|_| remote(code::UNAVAILABLE, "the computer didn't answer (is Warp open?)"))??;
        let (mut send, mut recv) = conn.open_bi().await.map_err(|e| explain(&conn, e.into()))?;
        let hello = Request::Hello { version: proto::VERSION, name: self.name.clone() };
        proto::write_frame(&mut send, &hello).await.map_err(|e| explain(&conn, e))?;
        match read(&conn, &mut recv).await? {
            Response::Hello { host, paired, version } => Ok((conn, send, recv, host, paired, version)),
            Response::Error { code, message } => Err(remote(&code, message)),
            other => Err(remote(code::BAD_REQUEST, format!("unexpected answer {other:?}"))),
        }
    }

    /// One request and its answer, connecting first if needed.
    pub async fn request(&mut self, req: &Request) -> Result<Response> {
        self.connect().await?;
        let live = self.live.as_mut().unwrap();
        let r = async {
            proto::write_frame(&mut live.send, req).await?;
            read(&live.conn, &mut live.recv).await
        };
        let r = match timeout(REQUEST_TIMEOUT, r).await {
            Ok(r) => r,
            Err(_) => Err(remote(code::UNAVAILABLE, "no answer")),
        };
        if r.is_err() {
            self.disconnect();
        }
        r
    }

    pub async fn sessions(&mut self) -> Result<Vec<Session>> {
        match self.request(&Request::Sessions).await? {
            Response::Sessions { sessions } => Ok(sessions),
            Response::Error { code, message } => Err(remote(&code, message)),
            other => Err(remote(code::BAD_REQUEST, format!("unexpected answer {other:?}"))),
        }
    }

    /// Round trip of a ping through the protocol.
    pub async fn ping(&mut self) -> Result<Duration> {
        self.connect().await?;
        let id = {
            let live = self.live.as_mut().unwrap();
            live.next_id += 1;
            live.next_id
        };
        let t0 = Instant::now();
        match self.request(&Request::Ping { id }).await? {
            Response::Pong { id: got } if got == id => Ok(t0.elapsed()),
            other => Err(remote(code::BAD_REQUEST, format!("unexpected answer {other:?}"))),
        }
    }

    /// v2: what that pane shows; subscribes to it (see [`Request::Watch`]). v3: `agent`, that subagent of it instead.
    pub async fn watch(&mut self, window: u64, pane_id: u32, since: u64, before: Option<u64>, agent: &str) -> Result<PaneView> {
        self.need_v2().await?;
        if !agent.is_empty() && self.version().await? < 3 {
            return Err(remote(code::UNSUPPORTED_VERSION, "update Warp on the computer to see its subagents"));
        }
        match self.request(&Request::Watch { window, pane_id, since, before, agent: agent.into() }).await? {
            Response::Pane(v) => Ok(*v),
            Response::Error { code, message } => Err(remote(&code, message)),
            other => Err(remote(code::BAD_REQUEST, format!("unexpected answer {other:?}"))),
        }
    }

    pub async fn unwatch(&mut self, window: u64, pane_id: u32, agent: &str) -> Result<()> {
        self.need_v2().await?;
        if !agent.is_empty() && self.version().await? < 3 {
            return Ok(());
        }
        self.expect_ok(&Request::Unwatch { window, pane_id, agent: agent.into() }).await
    }

    /// v2: types into that pane: `text` (one line), then `keys`; or `option` of the question with id `prompt`.
    pub async fn input(&mut self, window: u64, pane_id: u32, text: &str, keys: &[&str], option: Option<u32>, prompt: &str) -> Result<()> {
        self.need_v2().await?;
        let req = Request::Input {
            window,
            pane_id,
            text: text.into(),
            keys: keys.iter().map(|k| k.to_string()).collect(),
            option,
            prompt: prompt.into(),
        };
        self.expect_ok(&req).await
    }

    async fn need_v2(&mut self) -> Result<()> {
        if self.version().await? < 2 {
            return Err(remote(code::UNSUPPORTED_VERSION, "update Warp on the computer to see and type into terminals"));
        }
        Ok(())
    }

    async fn expect_ok(&mut self, req: &Request) -> Result<()> {
        match self.request(req).await? {
            Response::Ok => Ok(()),
            Response::Error { code, message } => Err(remote(&code, message)),
            other => Err(remote(code::BAD_REQUEST, format!("unexpected answer {other:?}"))),
        }
    }

    pub fn host_name(&self) -> Option<String> {
        self.live.as_ref().map(|l| l.host.clone()).or_else(|| self.host().map(|h| h.name))
    }

    /// The path the connection uses now: direct UDP or through a relay.
    pub fn path(&self) -> PathInfo {
        let none = PathInfo { kind: "none".into(), addr: String::new(), rtt_ms: 0.0 };
        let Some(live) = self.live.as_ref() else { return none };
        let paths = live.conn.paths();
        let Some(p) = paths.iter().find(|p| p.is_selected()).or_else(|| paths.iter().next()) else { return none };
        let (kind, addr) = match p.remote_addr() {
            TransportAddr::Ip(a) => ("direct", a.to_string()),
            TransportAddr::Relay(u) => ("relay", u.to_string()),
            other => ("other", format!("{other:?}")),
        };
        PathInfo { kind: kind.into(), addr, rtt_ms: p.rtt().as_secs_f64() * 1000.0 }
    }

    pub fn disconnect(&mut self) {
        if let Some(l) = self.live.take() {
            l.conn.close(proto::CLOSE_BYE.into(), b"bye");
        }
    }

    /// Forgets the paired host (the host keeps this device until it is revoked there).
    pub fn forget(&mut self) -> Result<()> {
        self.disconnect();
        store::forget_host(&self.dir)
    }

    pub async fn close(mut self) {
        self.disconnect();
        let _ = timeout(Duration::from_secs(2), self.ep.close()).await;
    }
}

async fn read(conn: &Connection, recv: &mut RecvStream) -> Result<Response> {
    match proto::read_frame::<Response>(recv).await {
        Ok(Some(r)) => Ok(r),
        Ok(None) => Err(explain(conn, remote(code::UNAVAILABLE, "the computer closed the connection"))),
        Err(e) => Err(explain(conn, e)),
    }
}

/// When the host closed the connection on purpose, its reason (not paired, revoked) says more.
fn explain(conn: &Connection, e: crate::Error) -> crate::Error {
    match conn.close_reason() {
        Some(ConnectionError::ApplicationClosed(ac)) => {
            let reason = String::from_utf8_lossy(&ac.reason).to_string();
            let msg = match reason.as_str() {
                code::NOT_PAIRED => "this computer doesn't know this device: pair again from Warp (Pair phone…)",
                code::REVOKED => "this device was removed in Warp: pair again",
                _ => "the computer closed the connection",
            };
            remote(if reason.is_empty() { code::UNAVAILABLE } else { &reason }, msg)
        }
        Some(ConnectionError::TimedOut) => remote(code::UNAVAILABLE, "connection lost"),
        _ => e,
    }
}
