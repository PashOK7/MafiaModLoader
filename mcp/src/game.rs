//! The game's folder and process: the config, bridge.json, launching and closing (by the exact pid), the log on disk,
//! deploying the build.

use serde_json::Value;
use std::os::windows::ffi::OsStrExt;
use std::path::{Path, PathBuf};
use windows_sys::Win32::Foundation::{CloseHandle, GetLastError, HWND, LPARAM, STILL_ACTIVE};
use windows_sys::Win32::System::Threading::{
    CREATE_NEW_PROCESS_GROUP, CreateProcessW, DETACHED_PROCESS, GetExitCodeProcess, OpenProcess, PROCESS_INFORMATION,
    PROCESS_QUERY_LIMITED_INFORMATION, PROCESS_TERMINATE, STARTF_USESHOWWINDOW, STARTUPINFOW, TerminateProcess,
};
use windows_sys::Win32::UI::WindowsAndMessaging::{
    EnumWindows, GetForegroundWindow, GetWindowThreadProcessId, IsWindowVisible, PostMessageW, SW_SHOWNOACTIVATE, WM_CLOSE,
};

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

tokio::task_local! {
    /// The game a tool call is for (its `game` argument: main.rs sets it around the call).
    static GAME: PathBuf;
}

/// Runs `f` for the game in `dir` (None: the default game).
pub async fn with_game<F: std::future::Future>(dir: Option<PathBuf>, f: F) -> F::Output {
    match dir {
        Some(d) => GAME.scope(d, f).await,
        None => f.await,
    }
}

fn config() -> Result<Value, String> {
    let root = repo_root().ok_or("cannot find the repository (set MML_REPO or MML_GAME_DIR)")?;
    let cfg = root.join("mml.config.json");
    let text = std::fs::read_to_string(&cfg)
        .map_err(|_| format!("no {}: copy mml.config.example.json there and set game_dir (or set MML_GAME_DIR)", cfg.display()))?;
    serde_json::from_str(&text).map_err(|e| format!("{}: {e}", cfg.display()))
}

/// The game's folder: the call's (`game`), else MML_GAME_DIR, else mml.config.json's game_dir.
pub fn game_dir() -> Result<PathBuf, String> {
    if let Ok(d) = GAME.try_with(|d| d.clone()) { return Ok(d); }
    if let Ok(d) = std::env::var("MML_GAME_DIR") { return Ok(PathBuf::from(d)); }
    config()?["game_dir"].as_str().map(PathBuf::from).ok_or_else(|| "mml.config.json: no game_dir".to_string())
}

/// The labs of mml.config.json (`labs`: a build -> its folder), in their order.
pub fn labs() -> Vec<(String, PathBuf)> {
    let Ok(cfg) = config() else { return vec![] };
    cfg["labs"].as_object().map(|o| {
        o.iter().filter_map(|(k, v)| v.as_str().map(|p| (k.clone(), PathBuf::from(p)))).collect()
    }).unwrap_or_default()
}

/// A tool's `game`: a lab's build ("1.2", "1.0": mml.config.json's `labs`) or a game's folder (one with Game.exe).
pub fn resolve(game: &str) -> Result<PathBuf, String> {
    let labs = labs();
    if let Some((_, dir)) = labs.iter().find(|(b, _)| b == game) { return Ok(dir.clone()); }
    let dir = PathBuf::from(game);
    if dir.join("Game.exe").exists() { return Ok(dir); }
    let known: Vec<String> = labs.iter().map(|(b, d)| format!("\"{b}\" ({})", d.display())).collect();
    Err(format!("game \"{game}\": not a lab ({}) nor a folder with Game.exe", if known.is_empty() { "none in mml.config.json".into() } else { known.join(", ") }))
}

pub fn bridge_info() -> Option<Value> {
    let p = game_dir().ok()?.join("mml").join("bridge.json");
    serde_json::from_str(&std::fs::read_to_string(p).ok()?).ok()
}

pub fn bridge_pid() -> Option<u32> {
    bridge_info()?.get("pid")?.as_u64().map(|p| p as u32)
}

/// The pid of the game running with the loader in `dir` (its bridge.json's, alive), if any.
pub fn running_pid(dir: &Path) -> Option<u32> {
    let text = std::fs::read_to_string(dir.join("mml").join("bridge.json")).ok()?;
    let v: Value = serde_json::from_str(&text).ok()?;
    let pid = v.get("pid")?.as_u64()? as u32;
    alive(pid).then_some(pid)
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

/// Starts Game.exe in its folder, detached. `background`: its window shown without being activated (the startup
/// information's SW_SHOWNOACTIVATE, which the game's first ShowWindow takes; the loader keeps the game from
/// activating it afterwards: start.json's "background").
pub fn launch(background: bool) -> Result<u32, String> {
    let dir = game_dir()?;
    let exe = dir.join("Game.exe");
    if !exe.exists() { return Err(format!("no {}", exe.display())); }
    let wide = |s: &std::ffi::OsStr| s.encode_wide().chain(Some(0)).collect::<Vec<u16>>();
    let app = wide(exe.as_os_str());
    let mut line = wide(std::ffi::OsStr::new(&format!("\"{}\"", exe.display())));
    let cwd = wide(dir.as_os_str());
    unsafe {
        let mut si: STARTUPINFOW = std::mem::zeroed();
        si.cb = std::mem::size_of::<STARTUPINFOW>() as u32;
        if background {
            si.dwFlags = STARTF_USESHOWWINDOW;
            si.wShowWindow = SW_SHOWNOACTIVATE as u16;
        }
        let mut pi: PROCESS_INFORMATION = std::mem::zeroed();
        let ok = CreateProcessW(app.as_ptr(), line.as_mut_ptr(), std::ptr::null(), std::ptr::null(), 0,
                                DETACHED_PROCESS | CREATE_NEW_PROCESS_GROUP, std::ptr::null(), cwd.as_ptr(), &si, &mut pi);
        if ok == 0 { return Err(format!("cannot start {}: error {}", exe.display(), GetLastError())); }
        CloseHandle(pi.hThread);
        CloseHandle(pi.hProcess);
        Ok(pi.dwProcessId)
    }
}

/// The process whose window is the one in front (0: none).
pub fn foreground_pid() -> u32 {
    let mut pid = 0u32;
    unsafe {
        let w = GetForegroundWindow();
        if !w.is_null() { GetWindowThreadProcessId(w, &mut pid); }
    }
    pid
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

/// The game's build as the loader last told it ("game: Mafia 1.0" in its log; "1.2" when not told): the knowledge
/// base names addresses for 1.2 only, so 1.0's are named through their twins.
pub fn current_build() -> String {
    for prev in [false, true] {
        let Ok(dir) = game_dir() else { break };
        let p = dir.join("mml").join("logs").join(if prev { "mml.prev.log" } else { "mml.log" });
        if let Ok(text) = std::fs::read_to_string(&p) {
            if let Some(line) = text.lines().rev().find(|l| l.contains("game: Mafia ")) {
                return line.rsplit("game: Mafia ").next().unwrap_or("1.2").trim().to_string();
            }
        }
    }
    "1.2".into()
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

/// One file put in place. A file the game may hold open (a mod's game file: a sound it streams) cannot be renamed
/// over, so the copy goes beside it (`<name>.mmltmp`, which the loader never takes for a game file), the old one is
/// deleted (the loader opens the mods' files sharing deletion: the game reads the old one until it closes it) and
/// the copy renamed. A file with the same size and time as its source is left alone (nothing changes in the game).
fn put_file(src: &Path, dst: &Path) -> Result<bool, String> {
    let err = |e: std::io::Error| format!("{} -> {}: {e}", src.display(), dst.display());
    let from = std::fs::metadata(src).map_err(err)?;
    if let Ok(to) = std::fs::metadata(dst) {
        if to.len() == from.len() && to.modified().ok() == from.modified().ok() { return Ok(false); }
    }
    let mut tmp = dst.as_os_str().to_owned();
    tmp.push(".mmltmp");
    let tmp = std::path::PathBuf::from(tmp);
    std::fs::copy(src, &tmp).map_err(err)?;
    if dst.exists() {
        if let Err(e) = std::fs::remove_file(dst) {
            let _ = std::fs::remove_file(&tmp);
            return Err(err(e));
        }
    }
    std::fs::rename(&tmp, dst).map_err(err)?;
    Ok(true)
}

/// A mod's folder copied with its subfolders (its `files\`: the game files it ships); the build's leftovers skipped.
fn copy_dir(from: &Path, to: &Path, copied: &mut Vec<String>) -> Result<(), String> {
    std::fs::create_dir_all(to).map_err(|e| format!("{}: {e}", to.display()))?;
    for e in std::fs::read_dir(from).map_err(|e| format!("{}: {e}", from.display()))? {
        let e = e.map_err(|e| e.to_string())?;
        let src = e.path();
        let dst = to.join(e.file_name());
        if src.is_dir() {
            copy_dir(&src, &dst, copied)?;
            continue;
        }
        let ext = src.extension().and_then(|x| x.to_str()).unwrap_or("").to_ascii_lowercase();
        if ["ilk", "exp", "lib", "mmltmp"].contains(&ext.as_str()) { continue; }
        if put_file(&src, &dst)? { copied.push(dst.display().to_string()); }
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
