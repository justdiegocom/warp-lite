//! Pairing ticket: everything the phone needs to reach this host once and prove it was shown the
//! code (the host's endpoint id, its relay and IP addresses, and a one-time secret).
//!
//! Text form: `WARP1` + base32 (RFC 4648, upper case, no padding) of
//! `[1] [endpoint id: 32] [secret: 16] [count: 1] count × ([kind: 1] [len: 1] [data: len])`,
//! kind 4 = IPv4 (4 bytes + port BE), 6 = IPv6 (16 + port), `r` = relay URL (UTF-8).
//! Upper-case base32 fits QR alphanumeric mode, which makes the code smaller. Unknown kinds are
//! skipped, so a newer host can add address kinds.

use std::net::{IpAddr, Ipv4Addr, Ipv6Addr, SocketAddr};

use data_encoding::BASE32_NOPAD;
use iroh::{EndpointAddr, PublicKey, RelayUrl, TransportAddr};

use crate::{Result, err};

const PREFIX: &str = "WARP1";
const FORMAT: u8 = 1;
/// IP addresses carried at most (a desktop can have many interfaces; the relay finds the rest).
const MAX_IPS: usize = 8;

#[derive(Debug, Clone, PartialEq)]
pub struct Ticket {
    pub addr: EndpointAddr,
    pub secret: [u8; 16],
}

impl Ticket {
    pub fn encode(&self) -> String {
        format!("{PREFIX}{}", BASE32_NOPAD.encode(&encode_addr(&self.addr, &self.secret)))
    }

    pub fn decode(s: &str) -> Result<Ticket> {
        let s: String = s.chars().filter(|c| !c.is_whitespace()).collect::<String>().to_ascii_uppercase();
        let body = s.strip_prefix(PREFIX).ok_or_else(|| err("not a Warp pairing code"))?;
        let bytes = BASE32_NOPAD.decode(body.as_bytes()).map_err(|_| err("damaged pairing code"))?;
        let (addr, secret) = decode_addr(&bytes)?;
        Ok(Ticket { addr, secret })
    }
}

/// The same binary form, used to remember a host's last known address (secret all zeros).
pub fn encode_addr(addr: &EndpointAddr, secret: &[u8; 16]) -> Vec<u8> {
    let mut b = vec![FORMAT];
    b.extend_from_slice(addr.id.as_bytes());
    b.extend_from_slice(secret);
    let mut items: Vec<(u8, Vec<u8>)> = Vec::new();
    for url in addr.relay_urls() {
        let u = url.to_string();
        if u.len() <= 255 {
            items.push((b'r', u.into_bytes()));
        }
    }
    // Public IPv4 first: the likeliest direct path for a phone on mobile data.
    let mut ips: Vec<&SocketAddr> = addr.ip_addrs().collect();
    ips.sort_by_key(|a| match a.ip() {
        IpAddr::V4(v4) if !v4.is_private() && !v4.is_loopback() && !v4.is_link_local() => 0,
        IpAddr::V4(_) => 1,
        IpAddr::V6(_) => 2,
    });
    for a in ips.into_iter().take(MAX_IPS) {
        let mut d = Vec::new();
        let kind = match a.ip() {
            IpAddr::V4(v4) => {
                d.extend_from_slice(&v4.octets());
                4
            }
            IpAddr::V6(v6) => {
                d.extend_from_slice(&v6.octets());
                6
            }
        };
        d.extend_from_slice(&a.port().to_be_bytes());
        items.push((kind, d));
    }
    b.push(items.len() as u8);
    for (kind, d) in items {
        b.push(kind);
        b.push(d.len() as u8);
        b.extend_from_slice(&d);
    }
    b
}

pub fn decode_addr(b: &[u8]) -> Result<(EndpointAddr, [u8; 16])> {
    let bad = || err("damaged pairing code");
    if b.first().is_some_and(|&v| v > FORMAT) {
        return Err(err("pairing code from a newer Warp: update the app"));
    }
    if b.len() < 1 + 32 + 16 + 1 || b[0] != FORMAT {
        return Err(bad());
    }
    let id = PublicKey::from_bytes(b[1..33].try_into().unwrap()).map_err(|_| bad())?;
    let secret: [u8; 16] = b[33..49].try_into().unwrap();
    let count = b[49] as usize;
    let mut addrs = Vec::new();
    let mut i = 50;
    for _ in 0..count {
        if i + 2 > b.len() {
            return Err(bad());
        }
        let (kind, len) = (b[i], b[i + 1] as usize);
        let d = b.get(i + 2..i + 2 + len).ok_or_else(bad)?;
        i += 2 + len;
        match (kind, len) {
            (4, 6) => {
                let ip = Ipv4Addr::new(d[0], d[1], d[2], d[3]);
                addrs.push(TransportAddr::Ip(SocketAddr::new(ip.into(), u16::from_be_bytes([d[4], d[5]]))));
            }
            (6, 18) => {
                let ip = Ipv6Addr::from(<[u8; 16]>::try_from(&d[..16]).unwrap());
                addrs.push(TransportAddr::Ip(SocketAddr::new(ip.into(), u16::from_be_bytes([d[16], d[17]]))));
            }
            (b'r', _) => {
                let url: RelayUrl = std::str::from_utf8(d).map_err(|_| bad())?.parse().map_err(|_| bad())?;
                addrs.push(TransportAddr::Relay(url));
            }
            _ => {}  // a kind this version doesn't know
        }
    }
    Ok((EndpointAddr::from_parts(id, addrs), secret))
}

#[cfg(test)]
mod tests {
    use super::*;
    use iroh::SecretKey;

    #[test]
    fn round_trip() {
        let id = SecretKey::generate().public();
        let addr = EndpointAddr::from_parts(
            id,
            [
                TransportAddr::Relay("https://euc1-1.relay.n0.iroh.link./".parse().unwrap()),
                TransportAddr::Ip("141.144.251.45:59874".parse().unwrap()),
                TransportAddr::Ip("192.168.0.103:59874".parse().unwrap()),
                TransportAddr::Ip("[2001:db8::1]:4433".parse().unwrap()),
            ],
        );
        let t = Ticket { addr: addr.clone(), secret: [7; 16] };
        let s = t.encode();
        assert!(s.starts_with("WARP1"));
        assert!(s.chars().all(|c| c.is_ascii_uppercase() || c.is_ascii_digit()), "QR alphanumeric: {s}");
        assert_eq!(Ticket::decode(&s).unwrap(), t);
        // Pasted with spaces, line breaks or in lower case.
        let messy = format!(" {}\n{} ", &s[..20].to_lowercase(), &s[20..]);
        assert_eq!(Ticket::decode(&messy).unwrap(), t);
        assert!(Ticket::decode("WARP1AAAA").is_err());
        assert!(Ticket::decode("hello").is_err());
    }
}
