//! The host: runs next to Warp and lets paired phones in.
//!
//! Unknown devices are turned away as soon as their connection is up, unless a pairing ticket is
//! active; then they get one try at proving they saw it (the secret is one-time, expires after
//! [`crate::PAIRING_TTL`] and is dropped after 5 wrong guesses).

use std::{
    collections::HashMap,
    future::Future,
    path::PathBuf,
    sync::{
        Arc, Mutex,
        atomic::{AtomicUsize, Ordering},
    },
    time::{Duration, Instant},
};

use data_encoding::HEXLOWER_PERMISSIVE;
use iroh::{
    Endpoint, PublicKey, RelayMode,
    endpoint::{Connection, Incoming, RecvStream, SendStream, presets},
};
use tokio::time::timeout;

use crate::{
    ALPN, Result, err,
    proto::{self, KEYS, MAX_TEXT, Request, Response, Session, code},
    screen, store,
    ticket::Ticket,
    watch::{PaneInfo, PaneInput, PaneScreen, Watch},
};

/// Where the session list comes from: Warp's windows on Windows, a fixed list in tests. Panes (v2) are optional.
pub trait SessionSource: Send + Sync + 'static {
    fn sessions(&self) -> impl Future<Output = Result<Vec<Session>>> + Send;

    /// The text of that pane (by window and pane id).
    fn screen(&self, _window: u64, _pane_id: u32) -> impl Future<Output = Result<PaneScreen>> + Send {
        async { Err(err("no panes here")) }
    }

    /// Only what that pane shows now, no scrollback (for the list: whether an agent there works or asks something).
    fn visible(&self, window: u64, pane_id: u32) -> impl Future<Output = Result<PaneScreen>> + Send {
        self.screen(window, pane_id)
    }

    /// What runs in that pane.
    fn info(&self, _window: u64, _pane_id: u32) -> impl Future<Output = Result<PaneInfo>> + Send {
        async { Err(err("no panes here")) }
    }

    /// Types into that pane, for the paired device `device` (its endpoint id, hex).
    fn input(&self, _window: u64, _pane_id: u32, _device: &str, _what: &PaneInput) -> impl Future<Output = Result<()>> + Send {
        async { Err(err("no panes here")) }
    }
}

/// A phone that stopped asking about a pane for this long stops watching it.
const WATCH_IDLE: Duration = Duration::from_secs(60);
/// Panes (and subagents) one connection may watch at once.
const MAX_WATCHES: usize = 8;
/// Panes whose screen the session list looks at, at most (busy ones, for their agent's status).
const MAX_LOOKS: usize = 24;
/// Between pasted text and the keys after it, and between keys that move through a menu: a program reads each one
/// separately (an Enter right behind a paste could be taken as part of it).
const AFTER_TEXT: Duration = Duration::from_millis(150);
const BETWEEN_KEYS: Duration = Duration::from_millis(90);

/// What the host tells its owner (Warp's windows, or the log).
#[derive(Debug, Clone, PartialEq)]
pub enum Event {
    /// Phones connected right now.
    Phones(usize),
    /// A phone was just paired.
    Paired { name: String },
    /// The pairing ticket expired or was used up without a phone pairing.
    PairingEnded,
}

pub struct HostConfig {
    /// Data directory (key, allow-list, log).
    pub dir: PathBuf,
    /// Shown to phones (the computer's name).
    pub name: String,
    pub net: Net,
}

struct Pairing {
    secret: [u8; 16],
    expires: Instant,
    tries: u32,
}

#[derive(Default)]
struct State {
    pairing: Option<Pairing>,
    /// Connections that said hello, by device.
    live: HashMap<PublicKey, Vec<Connection>>,
}

struct Inner<S> {
    ep: Endpoint,
    dir: PathBuf,
    name: String,
    source: S,
    state: Mutex<State>,
    phones: AtomicUsize,
    net: Net,
    on_event: Box<dyn Fn(Event) + Send + Sync>,
}

pub struct Host<S> {
    inner: Arc<Inner<S>>,
}

impl<S> Clone for Host<S> {
    fn clone(&self) -> Self {
        Host { inner: self.inner.clone() }
    }
}

/// Which network an endpoint uses.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum Net {
    /// Direct UDP when hole punching works, n0's relays otherwise, n0's address lookup.
    Internet,
    /// Tests: only through a relay (what a phone behind a strict NAT gets).
    RelayOnly,
    /// Tests: loopback only, no relays, no address lookup (nothing leaves the machine).
    Loopback,
}

pub fn endpoint_builder(net: Net) -> Result<iroh::endpoint::Builder> {
    Ok(match net {
        Net::Internet => Endpoint::builder(presets::N0),
        Net::RelayOnly => Endpoint::builder(presets::N0).clear_ip_transports(),
        Net::Loopback => {
            Endpoint::builder(presets::Minimal).relay_mode(RelayMode::Disabled).clear_ip_transports().bind_addr("127.0.0.1:0")?
        }
    })
}

impl<S: SessionSource> Host<S> {
    pub async fn bind(cfg: HostConfig, source: S, on_event: impl Fn(Event) + Send + Sync + 'static) -> Result<Self> {
        let key = store::load_or_create_key(&cfg.dir)?;
        let ep = endpoint_builder(cfg.net)?.secret_key(key).alpns(vec![ALPN.to_vec()]).bind().await?;
        store::log(&cfg.dir, &format!("started as {}", ep.id()));
        Ok(Host {
            inner: Arc::new(Inner {
                ep,
                dir: cfg.dir,
                name: cfg.name,
                source,
                state: Mutex::new(State::default()),
                phones: AtomicUsize::new(0),
                net: cfg.net,
                on_event: Box::new(on_event),
            }),
        })
    }

    pub fn endpoint(&self) -> &Endpoint {
        &self.inner.ep
    }

    /// Phones connected right now.
    pub fn phones(&self) -> usize {
        self.inner.phones.load(Ordering::Relaxed)
    }

    pub fn pairing_active(&self) -> bool {
        let st = self.inner.state.lock().unwrap();
        st.pairing.as_ref().is_some_and(|p| Instant::now() < p.expires)
    }

    /// A new pairing ticket (any earlier one stops working). Waits a few seconds for a relay, so
    /// the ticket also works from outside this network.
    pub async fn start_pairing(&self, ttl: Duration) -> Result<Ticket> {
        let mut secret = [0u8; 16];
        getrandom::fill(&mut secret).map_err(|e| err(format!("random: {e}")))?;
        {
            let mut st = self.inner.state.lock().unwrap();
            st.pairing = Some(Pairing { secret, expires: Instant::now() + ttl, tries: 0 });
        }
        if self.inner.net != Net::Loopback {
            let _ = timeout(Duration::from_secs(8), self.inner.ep.online()).await;
        }
        let me = self.clone();
        tokio::spawn(async move {
            tokio::time::sleep(ttl).await;
            let ended = {
                let mut st = me.inner.state.lock().unwrap();
                let same = st.pairing.as_ref().is_some_and(|p| p.secret == secret);
                if same {
                    st.pairing = None;
                }
                same
            };
            if ended {
                store::log(&me.inner.dir, "pairing code expired");
                (me.inner.on_event)(Event::PairingEnded);
            }
        });
        store::log(&self.inner.dir, "pairing code issued");
        Ok(Ticket { addr: self.inner.ep.addr(), secret })
    }

    pub fn stop_pairing(&self) {
        self.inner.state.lock().unwrap().pairing = None;
    }

    /// The allow-list changed (a device revoked in Warp): devices no longer in it are disconnected.
    pub fn reload(&self) {
        let allowed: Vec<PublicKey> = store::devices(&self.inner.dir).into_iter().map(|d| d.id).collect();
        let st = self.inner.state.lock().unwrap();
        for (id, conns) in st.live.iter() {
            if !allowed.contains(id) {
                store::log(&self.inner.dir, &format!("revoked {}", id.fmt_short()));
                for c in conns {
                    c.close(proto::CLOSE_REVOKED.into(), code::REVOKED.as_bytes());
                }
            }
        }
    }

    /// Accepts connections until the endpoint is closed.
    pub async fn run(&self) {
        while let Some(incoming) = self.inner.ep.accept().await {
            let me = self.clone();
            tokio::spawn(async move {
                if let Err(e) = me.serve(incoming).await {
                    store::log(&me.inner.dir, &format!("connection: {e}"));
                }
            });
        }
    }

    pub async fn close(&self) {
        let _ = timeout(Duration::from_secs(2), self.inner.ep.close()).await;
    }

    fn check_secret(&self, hex: &str) -> std::result::Result<(), &'static str> {
        let mut st = self.inner.state.lock().unwrap();
        let Some(p) = st.pairing.as_mut() else { return Err(code::TICKET_EXPIRED) };
        if Instant::now() >= p.expires {
            st.pairing = None;
            return Err(code::TICKET_EXPIRED);
        }
        let got = HEXLOWER_PERMISSIVE.decode(hex.as_bytes()).unwrap_or_default();
        let diff = got.iter().zip(p.secret.iter()).fold(0u8, |a, (x, y)| a | (x ^ y));
        if got.len() == 16 && diff == 0 {
            st.pairing = None;  // one-time
            return Ok(());
        }
        p.tries += 1;
        if p.tries >= 5 {
            st.pairing = None;
        }
        Err(code::BAD_SECRET)
    }

    fn set_live(&self, id: PublicKey, conn: &Connection, on: bool) {
        let n = {
            let mut st = self.inner.state.lock().unwrap();
            let list = st.live.entry(id).or_default();
            if on {
                list.push(conn.clone());
            } else {
                list.retain(|c| c.stable_id() != conn.stable_id());
                if list.is_empty() {
                    st.live.remove(&id);
                }
            }
            st.live.len()
        };
        if self.inner.phones.swap(n, Ordering::Relaxed) != n {
            (self.inner.on_event)(Event::Phones(n));
        }
    }

    async fn serve(&self, incoming: Incoming) -> Result<()> {
        let conn = incoming.await?;
        let id = conn.remote_id();
        let short = id.fmt_short().to_string();
        let allowed = store::devices(&self.inner.dir).iter().any(|d| d.id == id);
        if !allowed && !self.pairing_active() {
            store::log(&self.inner.dir, &format!("turned away {short}: not paired"));
            conn.close(proto::CLOSE_NOT_PAIRED.into(), code::NOT_PAIRED.as_bytes());
            return Ok(());
        }
        let (mut send, mut recv) = timeout(Duration::from_secs(15), conn.accept_bi()).await.map_err(|_| err("no stream"))??;
        let Ok(Ok(Some(Request::Hello { version, name }))) =
            timeout(Duration::from_secs(15), proto::read_frame::<Request>(&mut recv)).await
        else {
            conn.close(proto::CLOSE_PROTOCOL.into(), code::BAD_REQUEST.as_bytes());
            return Err(err(format!("{short}: no hello")));
        };
        if version < 1 {
            let msg = Response::Error { code: code::UNSUPPORTED_VERSION.into(), message: format!("version {version}") };
            proto::write_frame(&mut send, &msg).await?;
            finish_and_close(&conn, &mut send, proto::CLOSE_PROTOCOL, code::UNSUPPORTED_VERSION).await;
            return Ok(());
        }
        let hello = Response::Hello { version: version.min(proto::VERSION), host: self.inner.name.clone(), paired: allowed };
        proto::write_frame(&mut send, &hello).await?;
        if !allowed {
            let req = timeout(Duration::from_secs(30), proto::read_frame::<Request>(&mut recv)).await;
            let Ok(Ok(Some(Request::Pair { secret }))) = req else {
                conn.close(proto::CLOSE_NOT_PAIRED.into(), code::NOT_PAIRED.as_bytes());
                return Ok(());
            };
            if let Err(c) = self.check_secret(&secret) {
                store::log(&self.inner.dir, &format!("pairing refused for {short}: {c}"));
                let msg = Response::Error { code: c.into(), message: "this pairing code is not valid any more".into() };
                proto::write_frame(&mut send, &msg).await?;
                finish_and_close(&conn, &mut send, proto::CLOSE_NOT_PAIRED, c).await;
                return Ok(());
            }
            store::add_device(&self.inner.dir, id, &name)?;
            store::log(&self.inner.dir, &format!("paired {short} ({name})"));
            proto::write_frame(&mut send, &Response::Paired).await?;
            (self.inner.on_event)(Event::Paired { name: name.clone() });
        }
        store::log(&self.inner.dir, &format!("connected {short} ({name})"));
        self.set_live(id, &conn, true);
        let r = self.requests(id, &mut send, &mut recv).await;
        self.set_live(id, &conn, false);
        store::log(&self.inner.dir, &format!("disconnected {short}"));
        if r.is_err() && conn.close_reason().is_some() {
            return Ok(());  // the phone went away (closed the app, lost the network)
        }
        r
    }

    async fn requests(&self, device: PublicKey, send: &mut SendStream, recv: &mut RecvStream) -> Result<()> {
        // The panes (and subagents: the pane's key with the subagent's id) this connection watches: they end with it
        // (and so does any ssh following a transcript).
        let mut watches: HashMap<(u64, u32, String), Watch> = HashMap::new();
        let mut typed = false;
        // Read as plain JSON first: a request type from a newer app gets an error, not a disconnect.
        while let Some(v) = proto::read_frame::<serde_json::Value>(recv).await? {
            let resp = match serde_json::from_value::<Request>(v) {
                Ok(Request::Sessions) => match self.inner.source.sessions().await {
                    Ok(mut sessions) => {
                        self.look_at(&mut sessions).await;
                        Response::Sessions { sessions }
                    }
                    Err(e) => Response::Error { code: code::UNAVAILABLE.into(), message: e.to_string() },
                },
                Ok(Request::Ping { id }) => Response::Pong { id },
                Ok(Request::Hello { .. } | Request::Pair { .. }) => {
                    Response::Error { code: code::BAD_REQUEST.into(), message: "already connected".into() }
                }
                Ok(Request::Watch { window, pane_id, since, before, agent }) if !agent.is_empty() => {
                    self.watch_subagent(&mut watches, window, pane_id, agent, since, before)
                }
                Ok(Request::Watch { window, pane_id, since, before, .. }) => {
                    self.watch(&device, &mut watches, window, pane_id, since, before).await
                }
                Ok(Request::Unwatch { window, pane_id, agent }) => {
                    watches.remove(&(window, pane_id, agent));
                    Response::Ok
                }
                Ok(Request::Input { window, pane_id, text, keys, option, prompt }) => {
                    let r = self.input(&device, window, pane_id, text, keys, option, &prompt).await;
                    if r == Response::Ok && !typed {
                        typed = true;  // what was typed is never logged
                        store::log(&self.inner.dir, &format!("{} typed into a pane", device.fmt_short()));
                    }
                    r
                }
                Err(e) => Response::Error { code: code::BAD_REQUEST.into(), message: e.to_string() },
            };
            proto::write_frame(send, &resp).await?;
        }
        Ok(())
    }

    /// An agent's pane in the list: its status (working, waiting for an answer, finished) from its screen, and the
    /// agent itself for a pane running ssh, which Warp can't tell (only busy panes, and only while the list is asked).
    async fn look_at(&self, sessions: &mut [Session]) {
        for s in sessions.iter_mut().filter(|s| s.busy && s.pane_id != 0).take(MAX_LOOKS) {
            let Ok(screen) = self.inner.source.visible(s.window, s.pane_id).await else { continue };
            let rows = screen.visible();
            if s.agent.is_empty() {
                s.agent = screen::agent(rows).to_string();
            }
            if !s.agent.is_empty() {
                let prompt = screen::prompt(rows).is_some();
                s.status = screen::status(rows, &s.agent, prompt, true).to_string();
            }
        }
    }

    /// A subagent of a pane's Claude Code (v3): its conversation from its own file, read-only. The pane's watch must
    /// be open (the phone opens the subagent from it); it is kept alive meanwhile.
    fn watch_subagent(
        &self,
        watches: &mut HashMap<(u64, u32, String), Watch>,
        window: u64,
        pane_id: u32,
        agent: String,
        since: u64,
        before: Option<u64>,
    ) -> Response {
        watches.retain(|_, w| w.last_poll.elapsed() < WATCH_IDLE);
        let parent = (window, pane_id, String::new());
        let key = (window, pane_id, agent.clone());
        let Some(p) = watches.get_mut(&parent) else {
            watches.remove(&key);
            return Response::Error { code: code::NO_PANE.into(), message: "open that terminal first".into() };
        };
        p.last_poll = Instant::now();
        let info = p.agent_info(&agent);
        if !watches.contains_key(&key) {
            if watches.len() >= MAX_WATCHES {
                return Response::Error { code: code::BAD_REQUEST.into(), message: "watching too many panes".into() };
            }
            match watches[&parent].subagent(&agent) {
                Ok(w) => {
                    watches.insert(key.clone(), w);
                }
                Err(e) => return Response::Error { code: code::NO_PANE.into(), message: e },
            }
        }
        let w = watches.get_mut(&key).unwrap();
        w.set_sub_info(info);
        Response::Pane(Box::new(w.poll(&PaneScreen::default(), since, before, false)))
    }

    async fn watch(
        &self,
        device: &PublicKey,
        watches: &mut HashMap<(u64, u32, String), Watch>,
        window: u64,
        pane_id: u32,
        since: u64,
        before: Option<u64>,
    ) -> Response {
        let src = &self.inner.source;
        watches.retain(|_, w| w.last_poll.elapsed() < WATCH_IDLE);
        let key = (window, pane_id, String::new());
        if !watches.contains_key(&key) && watches.len() >= MAX_WATCHES {
            return Response::Error { code: code::BAD_REQUEST.into(), message: "watching too many panes".into() };
        }
        let screen = match src.screen(window, pane_id).await {
            Ok(s) => s,
            Err(e) => {
                watches.remove(&key);
                return Response::Error { code: code::NO_PANE.into(), message: e.to_string() };
            }
        };
        let w = match watches.entry(key) {
            std::collections::hash_map::Entry::Occupied(o) => o.into_mut(),
            std::collections::hash_map::Entry::Vacant(v) => {
                let info = src.info(window, pane_id).await.unwrap_or_default();
                v.insert(Watch::new(&self.inner.dir, window, pane_id, info, &screen))
            }
        };
        if w.info_due()
            && let Ok(info) = src.info(window, pane_id).await
        {
            w.set_info(info, &screen);
        }
        let typing = store::typing_allowed(&self.inner.dir, device);
        Response::Pane(Box::new(w.poll(&screen, since, before, typing)))
    }

    /// Typing from a phone: only a device allowed to type (checked again here on every request), only the keys of
    /// [`KEYS`], text as one line; an option of a question only while that same question is on the screen.
    #[allow(clippy::too_many_arguments)]
    async fn input(
        &self,
        device: &PublicKey,
        window: u64,
        pane_id: u32,
        text: String,
        mut keys: Vec<String>,
        option: Option<u32>,
        prompt: &str,
    ) -> Response {
        let fail = |c: &str, m: &str| Response::Error { code: c.into(), message: m.into() };
        if !store::typing_allowed(&self.inner.dir, device) {
            return fail(code::TYPING_OFF, "Typing from this device is off. Allow it in Warp: title-bar menu → Paired devices → this device → Allow typing.");
        }
        if text.chars().count() > MAX_TEXT || keys.len() > 32 || keys.iter().any(|k| !KEYS.contains(&k.as_str())) {
            return fail(code::BAD_REQUEST, "too much text, or an unknown key");
        }
        let src = &self.inner.source;
        if let Some(n) = option {
            let screen = match src.screen(window, pane_id).await {
                Ok(s) => s,
                Err(e) => return fail(code::NO_PANE, &e.to_string()),
            };
            let Some(p) = screen::prompt(screen.visible()).filter(|p| p.id == prompt) else {
                return fail(code::PROMPT_GONE, "That question isn't on the screen any more.");
            };
            if n as usize >= p.options.len() {
                return fail(code::BAD_REQUEST, "no such option");
            }
            keys = screen::keys_for(p.selected, n).into_iter().map(String::from).collect();
        }
        // One line: newlines and other control characters become spaces (only an explicit Enter submits).
        let text: String = text.chars().map(|c| if c.is_control() { ' ' } else { c }).collect();
        let dev = device.to_string();
        if !text.trim().is_empty() {
            if let Err(e) = src.input(window, pane_id, &dev, &PaneInput::Text(text)).await {
                return fail(code::NO_PANE, &e.to_string());
            }
            if !keys.is_empty() {
                tokio::time::sleep(AFTER_TEXT).await;
            }
        }
        for (i, k) in keys.into_iter().enumerate() {
            if i > 0 {
                tokio::time::sleep(BETWEEN_KEYS).await;
            }
            if let Err(e) = src.input(window, pane_id, &dev, &PaneInput::Key(k)).await {
                return fail(code::NO_PANE, &e.to_string());
            }
        }
        Response::Ok
    }
}

/// Lets the last frame reach the phone before closing.
async fn finish_and_close(conn: &Connection, send: &mut SendStream, close: u32, reason: &str) {
    let _ = send.finish();
    let _ = timeout(Duration::from_secs(3), send.stopped()).await;
    conn.close(close.into(), reason.as_bytes());
}

/// The computer's name, shown to phones.
pub fn computer_name() -> String {
    std::env::var("COMPUTERNAME").or_else(|_| std::env::var("HOSTNAME")).unwrap_or_else(|_| "Warp".into())
}
