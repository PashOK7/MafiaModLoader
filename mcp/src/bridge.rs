//! The bridge client: one connection to the loader of each game (by its folder: the call's `game`, else the
//! default), made (and the token read) on first use, made again when that game restarts. One request at a time to
//! a game (its bridge answers in order); games side by side are asked at the same time.

use serde_json::{Value, json};
use std::collections::HashMap;
use std::path::PathBuf;
use std::sync::Arc;
use tokio::io::{AsyncBufReadExt, AsyncWriteExt, BufReader};
use tokio::net::TcpStream;
use tokio::sync::Mutex;

struct Conn {
    reader: BufReader<tokio::net::tcp::OwnedReadHalf>,
    writer: tokio::net::tcp::OwnedWriteHalf,
    next: u64,
    pid: u64,
}

type Slot = Arc<Mutex<Option<Conn>>>;

pub struct Bridge {
    conns: std::sync::Mutex<HashMap<PathBuf, Slot>>,
}

impl Bridge {
    pub fn new() -> Self {
        Self { conns: std::sync::Mutex::new(HashMap::new()) }
    }

    /// The connection of the game this call is for.
    fn slot(&self) -> Result<Slot, String> {
        let dir = crate::game::game_dir()?;
        let mut map = self.conns.lock().map_err(|_| "the bridge's table is poisoned")?;
        Ok(map.entry(dir).or_default().clone())
    }

    pub async fn reset(&self) {
        if let Ok(slot) = self.slot() { *slot.lock().await = None; }
    }

    async fn connect() -> Result<Conn, String> {
        let info = crate::game::bridge_info().ok_or("the game is not running with the loader (no mml\\bridge.json)")?;
        let port = info["port"].as_u64().unwrap_or(47100);
        let pid = info["pid"].as_u64().unwrap_or(0);
        if pid != 0 && !crate::game::alive(pid as u32) {
            return Err(format!("the game is not running (its last pid {pid} is gone)"));
        }
        let stream = TcpStream::connect(("127.0.0.1", port as u16)).await.map_err(|e| format!("cannot connect to the bridge on port {port}: {e}"))?;
        let (r, w) = stream.into_split();
        let mut c = Conn { reader: BufReader::new(r), writer: w, next: 0, pid };
        let token = info["token"].as_str().unwrap_or_default().to_string();
        Self::exchange(&mut c, "hello", json!({"token": token}), 3000).await?;
        Ok(c)
    }

    async fn exchange(c: &mut Conn, method: &str, params: Value, timeout_ms: u64) -> Result<Value, String> {
        c.next += 1;
        let id = c.next;
        let line = json!({"id": id, "method": method, "params": params}).to_string() + "\n";
        c.writer.write_all(line.as_bytes()).await.map_err(|e| format!("bridge write: {e}"))?;
        let mut reply = String::new();
        let read = tokio::time::timeout(std::time::Duration::from_millis(timeout_ms), c.reader.read_line(&mut reply)).await;
        match read {
            Err(_) => return Err(format!("no answer in {timeout_ms} ms")),
            Ok(Err(e)) => return Err(format!("bridge read: {e}")),
            Ok(Ok(0)) => return Err("the bridge closed the connection (the game exited?)".into()),
            Ok(Ok(_)) => {}
        }
        let v: Value = serde_json::from_str(&reply).map_err(|e| format!("bad reply: {e}"))?;
        if let Some(err) = v.get("error") {
            return Err(err["message"].as_str().unwrap_or("error").to_string());
        }
        Ok(v.get("result").cloned().unwrap_or(Value::Null))
    }

    /// A request; a broken or stale connection (the game restarted) is made again once.
    pub async fn call(&self, method: &str, params: Value, timeout_ms: u64) -> Result<Value, String> {
        let slot = self.slot()?;
        let mut guard = slot.lock().await;
        for attempt in 0..2 {
            let current_pid = crate::game::bridge_info().and_then(|i| i["pid"].as_u64()).unwrap_or(0);
            if guard.as_ref().map_or(true, |c| c.pid != current_pid) {
                *guard = Some(Self::connect().await?);
            }
            let c = guard.as_mut().unwrap();
            match Self::exchange(c, method, params.clone(), timeout_ms).await {
                Ok(v) => return Ok(v),
                Err(e) if attempt == 0 && (e.starts_with("bridge") || e.contains("closed")) => {
                    *guard = None;
                    continue;
                }
                Err(e) => {
                    if e.starts_with("no answer") { *guard = None; }   // the reply may still come: start afresh
                    return Err(e);
                }
            }
        }
        Err("the bridge could not be reached".into())
    }
}
