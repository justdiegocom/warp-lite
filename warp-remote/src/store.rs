//! Files kept in a data directory. Plain text, so Warp (C++) reads and rewrites the allow-list too.
//!
//! Host (one directory per Warp: `%APPDATA%\warp-lite\remote`, or next to a test's session):
//! - `key`: this host's secret key (hex). Its public half is the id phones remember.
//! - `devices.txt`: the allow-list, one paired device per line: `<endpoint id hex>\t<unix seconds>\t<name>`.
//!   Warp's «Paired devices» menu removes lines from it to revoke a device.
//! - `typing.txt`: the paired devices that may type into the terminals, one endpoint id per line. Only Warp writes
//!   it (Paired devices → the device → Allow typing); none may type until it is chosen there.
//! - `remote.log`: what happened (start, pairing, connections, errors).
//!
//! Client (the phone, `warp-remote-cli`):
//! - `key`: the device's secret key; its public half is what the host allows.
//! - `host.txt`: the paired host, `<endpoint id hex>\t<name>\t<last known address, base32>`.
//!
//! The key, the allow-list and the host are written readable by this user only ([`write_secret`]):
//! on Unix (the phone, the CLI) the directory is 0700 and the files 0600; on Windows the directory
//! is under the user's own %APPDATA%, whose ACL already keeps other users out.

use std::{
    fs,
    io::Write,
    path::{Path, PathBuf},
    time::{SystemTime, UNIX_EPOCH},
};

use data_encoding::{BASE32_NOPAD, HEXLOWER};
use iroh::{EndpointAddr, PublicKey, SecretKey};

use crate::{Result, err, ticket};

pub fn now_secs() -> u64 {
    SystemTime::now().duration_since(UNIX_EPOCH).map(|d| d.as_secs()).unwrap_or(0)
}

/// Writes through a temporary file and a rename (a reader never sees half a file), readable by this
/// user only: the temporary file is always a fresh one, never a stale one someone else could have opened.
pub fn write_secret(path: &Path, data: &[u8]) -> Result<()> {
    let tmp = path.with_extension("tmp");
    let _ = fs::remove_file(&tmp);
    {
        let mut o = fs::OpenOptions::new();
        o.write(true).create_new(true);
        #[cfg(unix)]
        {
            use std::os::unix::fs::OpenOptionsExt;
            o.mode(0o600);
        }
        let mut f = o.open(&tmp)?;
        f.write_all(data)?;
        f.sync_all()?;
    }
    fs::rename(&tmp, path)?;
    Ok(())
}

/// Creates the data directory, private to this user on Unix.
pub fn create_private_dir(dir: &Path) -> Result<()> {
    fs::create_dir_all(dir)?;
    #[cfg(unix)]
    {
        use std::os::unix::fs::PermissionsExt;
        fs::set_permissions(dir, fs::Permissions::from_mode(0o700))?;
    }
    Ok(())
}

/// The secret key in `dir/key`, created the first time.
pub fn load_or_create_key(dir: &Path) -> Result<SecretKey> {
    create_private_dir(dir)?;
    let path = dir.join("key");
    if let Ok(text) = fs::read_to_string(&path) {
        return text.trim().parse::<SecretKey>().map_err(|e| err(format!("{}: {e}", path.display())));
    }
    let key = SecretKey::generate();
    write_secret(&path, HEXLOWER.encode(&key.to_bytes()).as_bytes())?;
    Ok(key)
}

#[derive(Debug, Clone, PartialEq)]
pub struct Device {
    pub id: PublicKey,
    pub paired: u64,
    pub name: String,
}

fn devices_path(dir: &Path) -> PathBuf {
    dir.join("devices.txt")
}

/// Paired devices (a missing or unreadable file: none).
pub fn devices(dir: &Path) -> Vec<Device> {
    let text = fs::read_to_string(devices_path(dir)).unwrap_or_default();
    text.lines()
        .filter_map(|l| {
            let mut f = l.trim_end_matches('\r').splitn(3, '\t');
            let id = f.next()?.trim().parse().ok()?;
            let paired = f.next().and_then(|s| s.parse().ok()).unwrap_or(0);
            let name = f.next().unwrap_or("").to_string();
            Some(Device { id, paired, name })
        })
        .collect()
}

fn save_devices(dir: &Path, list: &[Device]) -> Result<()> {
    let mut out = String::new();
    for d in list {
        let name: String = d.name.chars().map(|c| if c.is_control() { ' ' } else { c }).collect();
        out += &format!("{}\t{}\t{}\n", d.id, d.paired, name);
    }
    create_private_dir(dir)?;
    write_secret(&devices_path(dir), out.as_bytes())
}

/// Adds a device (or renames it, if it was already there).
pub fn add_device(dir: &Path, id: PublicKey, name: &str) -> Result<()> {
    let mut list = devices(dir);
    list.retain(|d| d.id != id);
    list.push(Device { id, paired: now_secs(), name: name.chars().take(64).collect() });
    save_devices(dir, &list)
}

pub fn remove_device(dir: &Path, id: &PublicKey) -> Result<bool> {
    let mut list = devices(dir);
    let n = list.len();
    list.retain(|d| &d.id != id);
    if list.len() == n {
        return Ok(false);
    }
    save_devices(dir, &list)?;
    Ok(true)
}

/// May that device type into the terminals? Paired and in `typing.txt` (read each time: Warp changes it).
pub fn typing_allowed(dir: &Path, id: &PublicKey) -> bool {
    let text = fs::read_to_string(dir.join("typing.txt")).unwrap_or_default();
    let listed = text.lines().any(|l| l.trim().parse::<PublicKey>().is_ok_and(|k| &k == id));
    listed && devices(dir).iter().any(|d| &d.id == id)
}

/// Lets that device type, or not (what Warp's menu does; for tests).
pub fn set_typing(dir: &Path, id: &PublicKey, on: bool) -> Result<()> {
    let text = fs::read_to_string(dir.join("typing.txt")).unwrap_or_default();
    let mut ids: Vec<String> = text.lines().map(|l| l.trim().to_string()).filter(|l| !l.is_empty() && *l != id.to_string()).collect();
    if on {
        ids.push(id.to_string());
    }
    create_private_dir(dir)?;
    write_secret(&dir.join("typing.txt"), (ids.join("\n") + "\n").as_bytes())
}

/// The host a client is paired with.
#[derive(Debug, Clone, PartialEq)]
pub struct HostRecord {
    pub addr: EndpointAddr,
    pub name: String,
}

pub fn load_host(dir: &Path) -> Option<HostRecord> {
    let text = fs::read_to_string(dir.join("host.txt")).ok()?;
    let mut f = text.trim().splitn(3, '\t');
    let id: PublicKey = f.next()?.parse().ok()?;
    let name = f.next().unwrap_or("").to_string();
    let addr = f
        .next()
        .and_then(|s| BASE32_NOPAD.decode(s.as_bytes()).ok())
        .and_then(|b| ticket::decode_addr(&b).ok())
        .map(|(a, _)| a)
        .filter(|a| a.id == id)
        .unwrap_or_else(|| EndpointAddr::new(id));
    Some(HostRecord { addr, name })
}

pub fn save_host(dir: &Path, host: &HostRecord) -> Result<()> {
    create_private_dir(dir)?;
    let addr = BASE32_NOPAD.encode(&ticket::encode_addr(&host.addr, &[0; 16]));
    let name: String = host.name.chars().filter(|c| !c.is_control()).collect();
    write_secret(&dir.join("host.txt"), format!("{}\t{}\t{}\n", host.addr.id, name, addr).as_bytes())
}

pub fn forget_host(dir: &Path) -> Result<()> {
    match fs::remove_file(dir.join("host.txt")) {
        Err(e) if e.kind() != std::io::ErrorKind::NotFound => Err(e.into()),
        _ => Ok(()),
    }
}

/// Appends a line to `dir/remote.log` (started over when it passes 256 KB).
pub fn log(dir: &Path, line: &str) {
    let path = dir.join("remote.log");
    if fs::metadata(&path).map(|m| m.len() > 256 * 1024).unwrap_or(false) {
        let _ = fs::remove_file(&path);
    }
    if let Ok(mut f) = fs::OpenOptions::new().create(true).append(true).open(&path) {
        let _ = writeln!(f, "{} {line}", now_secs());
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn allow_list_and_host() {
        let dir = std::env::temp_dir().join(format!("warp-remote-store-{}", std::process::id()));
        let _ = fs::remove_dir_all(&dir);
        let key = load_or_create_key(&dir).unwrap();
        assert_eq!(load_or_create_key(&dir).unwrap().public(), key.public(), "the key is kept");
        let a = SecretKey::generate().public();
        let b = SecretKey::generate().public();
        assert!(devices(&dir).is_empty());
        add_device(&dir, a, "iPhone\tde Diego").unwrap();
        add_device(&dir, b, "iPad").unwrap();
        add_device(&dir, a, "iPhone").unwrap();
        let d = devices(&dir);
        assert_eq!(d.len(), 2);
        assert_eq!((d[0].id, d[0].name.as_str()), (b, "iPad"));
        assert_eq!((d[1].id, d[1].name.as_str()), (a, "iPhone"));
        assert!(!typing_allowed(&dir, &b), "typing is off until allowed");
        set_typing(&dir, &b, true).unwrap();
        assert!(typing_allowed(&dir, &b) && !typing_allowed(&dir, &a));
        assert!(remove_device(&dir, &b).unwrap());
        assert!(!remove_device(&dir, &b).unwrap());
        assert!(!typing_allowed(&dir, &b), "a removed device can't type even if still listed");
        set_typing(&dir, &b, false).unwrap();
        assert_eq!(devices(&dir).len(), 1);

        let host = HostRecord {
            addr: EndpointAddr::new(key.public()).with_ip_addr("10.0.0.1:1234".parse().unwrap()),
            name: "DESKTOP".into(),
        };
        save_host(&dir, &host).unwrap();
        assert_eq!(load_host(&dir).unwrap(), host);
        forget_host(&dir).unwrap();
        assert!(load_host(&dir).is_none());
        let _ = fs::remove_dir_all(&dir);
    }

    #[cfg(unix)]
    #[test]
    fn secrets_are_private() {
        use std::os::unix::fs::PermissionsExt;
        let dir = std::env::temp_dir().join(format!("warp-remote-perm-{}", std::process::id()));
        let _ = fs::remove_dir_all(&dir);
        load_or_create_key(&dir).unwrap();
        add_device(&dir, SecretKey::generate().public(), "x").unwrap();
        save_host(&dir, &HostRecord { addr: EndpointAddr::new(SecretKey::generate().public()), name: "h".into() }).unwrap();
        let mode = |p: &Path| fs::metadata(p).unwrap().permissions().mode() & 0o777;
        assert_eq!(mode(&dir), 0o700);
        for f in ["key", "devices.txt", "host.txt"] {
            assert_eq!(mode(&dir.join(f)), 0o600, "{f}");
        }
        // A stale temporary file (e.g. left by someone else with wider permissions) is not reused.
        let tmp = dir.join("key.tmp");
        fs::write(&tmp, b"old").unwrap();
        fs::set_permissions(&tmp, fs::Permissions::from_mode(0o644)).unwrap();
        write_secret(&dir.join("key2"), b"new").unwrap();
        write_secret(&dir.join("key"), b"x").unwrap();
        assert_eq!(mode(&dir.join("key")), 0o600);
        let _ = fs::remove_dir_all(&dir);
    }
}
