//! The game's folder and process: the config, bridge.json, launching and closing (by the exact pid), the log on disk,
//! deploying the build.

use serde_json::Value;
use std::os::windows::process::CommandExt;
use std::path::{Path, PathBuf};
use windows_sys::Win32::Foundation::{CloseHandle, HWND, LPARAM, STILL_ACTIVE};
use windows_sys::Win32::System::Threading::{GetExitCodeProcess, OpenProcess, PROCESS_QUERY_LIMITED_INFORMATION, PROCESS_TERMINATE, TerminateProcess};
use windows_sys::Win32::UI::WindowsAndMessaging::{EnumWindows, GetWindowThreadProcessId, IsWindowVisible, PostMessageW, WM_CLOSE};

/// The repository's root: MML_REPO, else the first folder above this executable (or the current one) holding
/// mcp\Cargo.toml.
pub fn repo_root() -> Option<PathBuf> {
    if let Ok(r) = std::env::var("MML_REPO") { return Some(PathBuf::from(r)); }
    let starts = [std::env::current_exe().ok(), std::env::current_dir().ok()];
    for s in starts.into_iter().flatten() {
        for dir in s.ancestors() {
            if dir.join("mcp").join("Cargo.toml").exists() && dir.join("CMakeLists.txt").exists() {
                return Some(dir.to_path_buf());
            }
        }
    }
    None
}

pub fn game_dir() -> Result<PathBuf, String> {
    if let Ok(d) = std::env::var("MML_GAME_DIR") { return Ok(PathBuf::from(d)); }
    let root = repo_root().ok_or("cannot find the repository (set MML_REPO or MML_GAME_DIR)")?;
    let cfg = root.join("mml.config.json");
    let text = std::fs::read_to_string(&cfg)
        .map_err(|_| format!("no {}: copy mml.config.example.json there and set game_dir (or set MML_GAME_DIR)", cfg.display()))?;
    let v: Value = serde_json::from_str(&text).map_err(|e| format!("{}: {e}", cfg.display()))?;
    v["game_dir"].as_str().map(PathBuf::from).ok_or_else(|| format!("{}: no game_dir", cfg.display()))
}

pub fn bridge_info() -> Option<Value> {
    let p = game_dir().ok()?.join("mml").join("bridge.json");
    serde_json::from_str(&std::fs::read_to_string(p).ok()?).ok()
}

pub fn bridge_pid() -> Option<u32> {
    bridge_info()?.get("pid")?.as_u64().map(|p| p as u32)
}

pub fn alive(pid: u32) -> bool {
    unsafe {
        let h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, 0, pid);
        if h.is_null() { return false; }
        let mut code = 0u32;
        let ok = GetExitCodeProcess(h, &mut code) != 0;
        CloseHandle(h);
        ok && code == STILL_ACTIVE as u32
    }
}

pub fn launch() -> Result<u32, String> {
    let dir = game_dir()?;
    let exe = dir.join("Game.exe");
    if !exe.exists() { return Err(format!("no {}", exe.display())); }
    const DETACHED_PROCESS: u32 = 0x8;
    const CREATE_NEW_PROCESS_GROUP: u32 = 0x200;
    let child = std::process::Command::new(&exe)
        .current_dir(&dir)
        .creation_flags(DETACHED_PROCESS | CREATE_NEW_PROCESS_GROUP)
        .spawn()
        .map_err(|e| format!("cannot start {}: {e}", exe.display()))?;
    Ok(child.id())
}

unsafe extern "system" fn close_windows(hwnd: HWND, lparam: LPARAM) -> i32 {
    let mut pid = 0u32;
    unsafe {
        GetWindowThreadProcessId(hwnd, &mut pid);
        if pid == lparam as u32 && IsWindowVisible(hwnd) != 0 {
            PostMessageW(hwnd, WM_CLOSE, 0, 0);
        }
    }
    1
}

pub async fn close(pid: u32, force: bool) -> Result<String, String> {
    if !force {
        unsafe { EnumWindows(Some(close_windows), pid as LPARAM); }
        for _ in 0..40 {
            tokio::time::sleep(std::time::Duration::from_millis(250)).await;
            if !alive(pid) { return Ok(format!("pid {pid} closed")); }
        }
    }
    unsafe {
        let h = OpenProcess(PROCESS_TERMINATE, 0, pid);
        if h.is_null() { return Err(format!("cannot open pid {pid} to end it")); }
        let ok = TerminateProcess(h, 1) != 0;
        CloseHandle(h);
        if !ok { return Err(format!("TerminateProcess failed for pid {pid}")); }
    }
    Ok(format!("pid {pid} ended{}", if force { "" } else { " (it did not close in 10 s)" }))
}

pub fn log_file_tail(previous: bool, lines: usize) -> Result<String, String> {
    let p = game_dir()?.join("mml").join("logs").join(if previous { "mml.prev.log" } else { "mml.log" });
    let text = std::fs::read_to_string(&p).map_err(|e| format!("{}: {e}", p.display()))?;
    let all: Vec<&str> = text.lines().collect();
    Ok(all[all.len().saturating_sub(lines)..].join("\n"))
}

pub struct Deployed {
    pub copied: Vec<String>,
    pub skipped: Vec<String>,
    pub mods: Vec<String>,
}

fn copy_dir(from: &Path, to: &Path, copied: &mut Vec<String>) -> Result<(), String> {
    std::fs::create_dir_all(to).map_err(|e| format!("{}: {e}", to.display()))?;
    for e in std::fs::read_dir(from).map_err(|e| format!("{}: {e}", from.display()))? {
        let e = e.map_err(|e| e.to_string())?;
        let src = e.path();
        if src.is_dir() { continue; }
        let ext = src.extension().and_then(|x| x.to_str()).unwrap_or("").to_ascii_lowercase();
        if ["ilk", "exp", "lib"].contains(&ext.as_str()) { continue; }
        let dst = to.join(e.file_name());
        std::fs::copy(&src, &dst).map_err(|err| format!("{} -> {}: {err}", src.display(), dst.display()))?;
        copied.push(dst.display().to_string());
    }
    Ok(())
}

/// build/x86/out -> the game's folder: every mod's folder (or the ones named), and the loader when the game is not
/// running.
pub fn deploy(only: Option<&[String]>, running: bool) -> Result<Deployed, String> {
    let root = repo_root().ok_or("cannot find the repository")?;
    let out = root.join("build").join("x86").join("out");
    let game = game_dir()?;
    let mut d = Deployed { copied: vec![], skipped: vec![], mods: vec![] };
    let asi = out.join("MafiaModLoader.asi");
    if asi.exists() {
        if running {
            d.skipped.push("MafiaModLoader.asi (the game is running: game_close, then deploy again)".into());
        } else {
            for f in ["MafiaModLoader.asi", "MafiaModLoader.pdb"] {
                let src = out.join(f);
                if src.exists() {
                    std::fs::copy(&src, game.join(f)).map_err(|e| format!("{f}: {e}"))?;
                    d.copied.push(game.join(f).display().to_string());
                }
            }
        }
    }
    let mods = out.join("mml").join("mods");
    for e in std::fs::read_dir(&mods).map_err(|e| format!("{}: {e} (build first)", mods.display()))? {
        let e = e.map_err(|e| e.to_string())?;
        let id = e.file_name().to_string_lossy().to_string();
        if !e.path().is_dir() || only.is_some_and(|o| !o.contains(&id)) { continue; }
        copy_dir(&e.path(), &game.join("mml").join("mods").join(&id), &mut d.copied)?;
        d.mods.push(id);
    }
    Ok(d)
}
