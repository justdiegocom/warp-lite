//! warp-remote: lets a paired phone reach the Warp running on this computer.
//!
//! Warp is always the host and the phone always the client. They talk over iroh (QUIC with hole
//! punching, falling back to n0's public relays), so it works away from home and behind CG-NAT
//! with nothing to configure and no server of our own.
//!
//! - [`ticket`]: the pairing ticket shown as a QR code (endpoint address + one-time secret).
//! - [`proto`]: the protocol (v2), length-prefixed JSON frames on one bidirectional stream.
//! - [`store`]: secret keys, the paired-device allow-list (and who may type) and the remembered host.
//! - [`host`]: the side that runs next to Warp (`warp-remote.exe`).
//! - [`client`]: the side the phone and `warp-remote-cli` use.
//! - [`watch`], [`chat`], [`screen`]: a pane for the phone: its Claude Code or Codex conversation, or its text, and
//!   the question it asks.

pub mod chat;
pub mod client;
pub mod host;
pub mod proto;
pub mod screen;
pub mod store;
pub mod ticket;
pub mod watch;
#[cfg(windows)]
pub mod win;

/// QUIC application protocol. A new incompatible protocol gets a new ALPN.
pub const ALPN: &[u8] = b"warp-remote/1";

/// How long a pairing ticket is valid.
pub const PAIRING_TTL: std::time::Duration = std::time::Duration::from_secs(120);

pub type Error = Box<dyn std::error::Error + Send + Sync>;
pub type Result<T> = std::result::Result<T, Error>;

/// An error with a message (for `?` on plain strings).
pub fn err(msg: impl Into<String>) -> Error {
    msg.into().into()
}
