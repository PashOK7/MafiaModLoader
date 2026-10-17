//! The game's two builds in Ghidra (tools/ghidra_cli.py): one resident server a build, each holding its project open
//! (re/ghidra/), asked with one JSON line {"op", "args"} and answering {"ok", "text"}. A server not running is
//! started here (the PyGhidra venv's python, a hidden process, its log beside the projects) and asked once it
//! listens. tools/ghidra_cli.py's CLI talks to the same servers.
use serde_json::{Value, json};
use std::path::PathBuf;
use std::time::{Duration, Instant};
use tokio::io::{AsyncReadExt, AsyncWriteExt};

/// The build's project name and its server's port (tools/ghidra_cli.py BUILDS).
fn build_of(build: &str) -> Result<(&'static str, u16), String> {
    match build {
        "1.2" => Ok(("mafia12", 47031)),
        "1.0" => Ok(("mafia10", 47032)),
        b => Err(format!("build: \"1.2\" or \"1.0\" (not \"{b}\")")),
    }
}

fn root() -> Result<PathBuf, String> {
    crate::game::repo_root().ok_or_else(|| "the repository's root is not found (MML_REPO)".to_string())
}

async fn connect(port: u16) -> Option<tokio::net::TcpStream> {
    tokio::time::timeout(Duration::from_secs(2), tokio::net::TcpStream::connect(("127.0.0.1", port))).await.ok()?.ok()
}

/// The build's server, started when it is not listening.
async fn ensure(build: &str) -> Result<u16, String> {
    let (name, port) = build_of(build)?;
    if connect(port).await.is_some() {
        return Ok(port);
    }
    let root = root()?;
    let py = root.join("third_party").join("_deps").join("pyghidra").join("Scripts").join("python.exe");
    let ghidra = root.join("third_party").join("_deps").join("ghidra").join("ghidraRun.bat");
    if !py.exists() || !ghidra.exists() {
        return Err("Ghidra is not installed: python tools/deps.py ghidra (then python tools/ghidra_cli.py setup)".into());
    }
    let projects = root.join("re").join("ghidra");
    if !projects.join(format!("{name}.rep")).exists() {
        return Err(format!("the {build} project is not made: python tools/ghidra_cli.py setup --build {build}"));
    }
    let log = std::fs::File::create(projects.join(format!("server_{name}.log"))).map_err(|e| e.to_string())?;
    let mut cmd = std::process::Command::new(&py);
    cmd.arg(root.join("tools").join("ghidra_cli.py")).args(["--serve", build]).current_dir(&root)
        .stdin(std::process::Stdio::null()).stdout(log.try_clone().map_err(|e| e.to_string())?).stderr(log);
    #[cfg(windows)]
    {
        use std::os::windows::process::CommandExt;
        cmd.creation_flags(0x0000_0200 | 0x0800_0000); // CREATE_NEW_PROCESS_GROUP, CREATE_NO_WINDOW
    }
    cmd.spawn().map_err(|e| format!("the {build} server could not be started: {e}"))?;
    let begun = Instant::now();
    while connect(port).await.is_none() {
        if begun.elapsed() > Duration::from_secs(300) {
            return Err(format!("the {build} server did not start in 300 s: see re/ghidra/server_{name}.log"));
        }
        tokio::time::sleep(Duration::from_millis(500)).await;
    }
    Ok(port)
}

/// One request: the op's text, or its error.
pub async fn ask(build: &str, op: &str, args: Value) -> Result<String, String> {
    let port = ensure(build).await?;
    let mut s = connect(port).await.ok_or("the Ghidra server stopped answering")?;
    let line = json!({"op": op, "args": args}).to_string() + "\n";
    s.write_all(line.as_bytes()).await.map_err(|e| e.to_string())?;
    let mut buf = Vec::new();
    tokio::time::timeout(Duration::from_secs(900), s.read_to_end(&mut buf))
        .await
        .map_err(|_| format!("{op}: no answer in 900 s"))?
        .map_err(|e| e.to_string())?;
    let v: Value = serde_json::from_slice(&buf).map_err(|e| format!("the Ghidra server's answer: {e}"))?;
    let text = v["text"].as_str().unwrap_or_default().to_string();
    if v["ok"].as_bool() == Some(true) { Ok(text) } else { Err(text) }
}

/// The ops each tool may ask (tools/ghidra_server.py OPS_*).
pub const READ: &[&str] = &["decompile", "asm", "function", "callers", "callees", "xrefs", "vtable", "bytes", "twin", "status"];
pub const FIND: &[&str] = &["imm", "string", "find", "names", "functions"];
pub const WRITE: &[&str] = &["rename", "comment", "proto", "make_function", "unfunction", "forget", "sync"];

pub fn check(op: &str, allowed: &[&str], tool: &str) -> Result<(), String> {
    if allowed.contains(&op) { Ok(()) } else { Err(format!("{tool}: op is one of {}", allowed.join(", "))) }
}
