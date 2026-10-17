//! The crash dumps the loader writes (<game>\mml\crash): a list, and one opened: the exception, where, the registers,
//! the faulting address's memory region (freed, decommitted, its protection) and the code addresses on the crashing
//! thread's stack (a scan: return addresses and leftovers).

use minidump::{Minidump, MinidumpException, MinidumpMemoryInfoList, MinidumpMemoryList, MinidumpMiscInfo, MinidumpModuleList,
               MinidumpSystemInfo, MinidumpThreadList, Module};
use serde_json::{Value, json};

fn crash_dir() -> Result<std::path::PathBuf, String> {
    Ok(crate::game::game_dir()?.join("mml").join("crash"))
}

pub fn list() -> Result<Value, String> {
    let dir = crash_dir()?;
    let mut v: Vec<(std::time::SystemTime, Value)> = vec![];
    if let Ok(rd) = std::fs::read_dir(&dir) {
        for e in rd.flatten() {
            let p = e.path();
            if p.extension().and_then(|x| x.to_str()) != Some("dmp") { continue; }
            let meta = e.metadata().map_err(|e| e.to_string())?;
            let t = meta.modified().unwrap_or(std::time::UNIX_EPOCH);
            let secs = t.duration_since(std::time::UNIX_EPOCH).map(|d| d.as_secs()).unwrap_or(0);
            v.push((t, json!({"file": e.file_name().to_string_lossy(), "bytes": meta.len(), "modified_unix": secs})));
        }
    }
    v.sort_by(|a, b| b.0.cmp(&a.0));
    Ok(json!({"dir": dir.display().to_string(), "dumps": v.into_iter().map(|x| x.1).collect::<Vec<_>>()}))
}

fn where_(mods: &MinidumpModuleList, a: u64) -> String {
    match mods.module_at_address(a) {
        Some(m) => {
            let name = m.code_file();
            let short = name.rsplit(['\\', '/']).next().unwrap_or(&name).to_string();
            format!("{short}+0x{:X}", a - m.base_address())
        }
        None => format!("0x{a:08X}"),
    }
}

pub fn inspect(file: Option<String>) -> Result<Value, String> {
    let dir = crash_dir()?;
    let name = match file {
        Some(f) => f,
        None => list()?["dumps"][0]["file"].as_str().ok_or("no dumps")?.to_string(),
    };
    let path = dir.join(&name);
    let dump = Minidump::read_path(&path).map_err(|e| format!("{}: {e}", path.display()))?;
    let sys: MinidumpSystemInfo = dump.get_stream().map_err(|e| format!("system info: {e}"))?;
    let misc: Option<MinidumpMiscInfo> = dump.get_stream().ok();
    let mods: MinidumpModuleList = dump.get_stream().map_err(|e| format!("modules: {e}"))?;
    let exc: MinidumpException = dump.get_stream().map_err(|e| format!("no exception stream: {e}"))?;
    let rec = &exc.raw.exception_record;
    let code = rec.exception_code;
    let at = rec.exception_address as u32 as u64;
    let mut out = json!({
        "file": name,
        "exception": format!("0x{code:08X}"),
        "at": where_(&mods, at),
        "thread": exc.get_crashing_thread_id(),
    });
    if code == 0xC0000005 && rec.number_parameters >= 2 {
        let kind = match rec.exception_information[0] { 0 => "read", 1 => "write", 8 => "execute", _ => "?" };
        let addr = rec.exception_information[1] as u32 as u64;
        out["access"] = json!({"kind": kind, "address": format!("0x{addr:08X}"), "where": where_(&mods, addr)});
        if let Ok(infos) = dump.get_stream::<MinidumpMemoryInfoList>() {
            if let Some(i) = infos.memory_info_at_address(addr) {
                out["access"]["region"] = json!({
                    "base": format!("0x{:08X}", i.raw.base_address),
                    "size": i.raw.region_size,
                    "state": format!("{:?}", i.state),
                    "protection": format!("{:?}", i.protection),
                    "type": format!("{:?}", i.ty),
                    "allocation_base": format!("0x{:08X}", i.raw.allocation_base),
                });
            } else {
                out["access"]["region"] = json!("not in the dump's memory map (a free address)");
            }
        }
    }
    if let Some(ctx) = exc.context(&sys, misc.as_ref()) {
        let mut regs = serde_json::Map::new();
        for r in ["eip", "esp", "ebp", "eax", "ebx", "ecx", "edx", "esi", "edi"] {
            if let Some(v) = ctx.get_register(r) {
                regs.insert(r.into(), json!(format!("0x{v:08X}")));
            }
        }
        out["registers"] = Value::Object(regs);
        // the crashing thread's stack, scanned for code addresses
        if let (Some(esp), Ok(mem)) = (ctx.get_register("esp"), dump.get_stream::<MinidumpMemoryList>()) {
            if let Some(region) = mem.memory_at_address(esp) {
                let bytes = region.bytes;
                let start = (esp - region.base_address) as usize;
                let mut found = vec![];
                let mut off = start;
                while off + 4 <= bytes.len() && found.len() < 48 {
                    let v = u32::from_le_bytes([bytes[off], bytes[off + 1], bytes[off + 2], bytes[off + 3]]) as u64;
                    if mods.module_at_address(v).is_some() {
                        found.push(format!("[esp+{:04X}] {}", off - start, where_(&mods, v)));
                    }
                    off += 4;
                }
                out["stack_scan"] = json!(found);
            }
        }
    }
    if let Ok(threads) = dump.get_stream::<MinidumpThreadList>() {
        out["threads"] = json!(threads.threads.len());
    }
    Ok(out)
}

#[derive(PartialEq)]
enum Owner { Game, System, Ours, Mod, Other }

/// Whose code a stack frame ("Game.exe+0x...", "mod_x.dll+0x... (mod x)") is.
fn owner(frame: &str) -> Owner {
    if frame.contains("(mod ") { return Owner::Mod; }
    let m = frame.split('+').next().unwrap_or("").to_lowercase();
    if m == "mafiamodloader.asi" { return Owner::Ours; }
    if ["game.exe", "ls3df.dll", "rw_data.dll", "ijoy.dll", "binkw32.dll", "mml_harness.exe"].contains(&m.as_str()) { return Owner::Game; }
    let system = ["ntdll.dll", "kernel32.dll", "kernelbase.dll", "user32.dll", "gdi32.dll", "win32u.dll", "msvcrt.dll",
                  "ucrtbase.dll", "dinput8.dll", "dinput.dll", "dsound.dll", "winmm.dll", "dxwrapper.dll", "dbghelp.dll"];
    if system.contains(&m.as_str()) || m.starts_with("d3d") || m.starts_with("msvcp") || m.starts_with("vcruntime")
        || m.starts_with("nv") || m.starts_with("amd") || m.starts_with("ig") { return Owner::System; }
    if m.starts_with("0x") { return Owner::System; }
    Owner::Other
}

fn exception_name(code: &str) -> &'static str {
    match code {
        "C0000005" => "an access violation",
        "C00000FD" => "a stack overflow",
        "C0000094" => "an integer division by zero",
        "C000001D" => "an illegal instruction",
        "C0000096" => "a privileged instruction",
        "80000003" => "a breakpoint",
        "E06D7363" => "a C++ exception nobody caught",
        "C0000409" => "a stack buffer overrun (fast fail)",
        "C0000374" => "a corrupted heap",
        _ => "an exception",
    }
}

/// The log's lines up to the one telling of this dump (the run that crashed: mml.log, or mml.prev.log once the game
/// was started again).
fn log_before(dump: &str, n: usize) -> Option<Vec<String>> {
    let dir = crate::game::game_dir().ok()?.join("mml").join("logs");
    for f in ["mml.log", "mml.prev.log"] {
        let Ok(text) = std::fs::read_to_string(dir.join(f)) else { continue };
        let lines: Vec<&str> = text.lines().collect();
        if let Some(i) = lines.iter().position(|l| l.contains(dump)) {
            return Some(lines[i.saturating_sub(n)..=i].iter().map(|s| s.to_string()).collect());
        }
    }
    None
}

/// Everything about a crash in one answer: the newest dump (or `file`) opened, the report the loader wrote beside it
/// (the call stack with the mods named, the log's last lines, the taps' last calls, the events, the mods, the
/// mission), the log from the file when there is no report, and a verdict: what, where, through whose code.
pub fn triage(file: Option<String>) -> Result<Value, String> {
    let dir = crash_dir()?;
    let name = match file {
        Some(f) => f,
        None => {
            // the newest dump, or the newest report when its dump could not be written
            let mut newest: Option<(std::time::SystemTime, String)> = None;
            for e in std::fs::read_dir(&dir).map_err(|_| format!("no crash folder ({}): no crash yet", dir.display()))?.flatten() {
                let n = e.file_name().to_string_lossy().into_owned();
                let stem = n.trim_end_matches(".dmp").trim_end_matches(".json").to_string();
                if !(n.ends_with(".dmp") || n.ends_with(".json")) { continue; }
                let t = e.metadata().and_then(|m| m.modified()).unwrap_or(std::time::UNIX_EPOCH);
                if newest.as_ref().is_none_or(|(nt, _)| t > *nt) { newest = Some((t, stem + ".dmp")); }
            }
            newest.ok_or("no crash dumps")?.1
        }
    };
    let stem = name.trim_end_matches(".dmp").to_string();
    let report: Option<Value> = std::fs::read_to_string(dir.join(format!("{stem}.json"))).ok().and_then(|t| serde_json::from_str(&t).ok());
    let dump = inspect(Some(format!("{stem}.dmp")));
    let mut out = json!({"dump": format!("{stem}.dmp")});
    let r = report.clone().unwrap_or(Value::Null);
    let d = dump.as_ref().ok().cloned().unwrap_or(Value::Null);
    if let Err(e) = &dump { out["dump_error"] = json!(e); }
    let code = r["exception"].as_str().map(String::from)
        .or_else(|| d["exception"].as_str().map(|s| s.trim_start_matches("0x").to_string())).unwrap_or_default();
    let at = r["at"].as_str().or(d["at"].as_str()).unwrap_or("?").to_string();
    let access = if r["access"].is_object() { r["access"].clone() } else { d["access"].clone() };
    let stack: Vec<String> = match r["stack"].as_array() {
        Some(s) => s.iter().filter_map(|x| x.as_str().map(String::from)).collect(),
        None => d["stack_scan"].as_array().map(|s| s.iter().filter_map(|x| x.as_str().map(|t| t.split("] ").last().unwrap_or(t).to_string())).collect()).unwrap_or_default(),
    };
    // the verdict
    let mut v = format!("{} ({code}) at {at}", exception_name(&code));
    if let (Some(k), Some(a)) = (access["kind"].as_str(), access["address"].as_str()) {
        let addr = u32::from_str_radix(a.trim_start_matches("0x"), 16).ok();
        v += &format!(", a {k} of {a}");
        if addr.is_some_and(|x| x < 0x10000) { v += " (a null pointer, or one near it)"; }
    }
    // the nearest code that is neither the game's nor the system's: a mod, the loader, another ASI (the loader's own
    // frame hook is under every frame's code, so it only counts when nothing nearer is)
    let nearest = stack.iter().find(|s| matches!(owner(s), Owner::Ours | Owner::Mod | Owner::Other));
    v += &match nearest {
        Some(s) => {
            let whose = match owner(s) {
                Owner::Mod => "a mod of the loader",
                Owner::Ours => "the loader itself",
                _ => "not the loader's: another ASI or DLL",
            };
            if stack.first() == Some(s) { format!("; in {s} ({whose})") }
            else { format!("; called from {s} ({whose}), the game's own code running from there") }
        }
        None => "; only the game's and the system's code on the stack".into(),
    };
    if r["main_thread"].as_bool() == Some(false) { v += "; on another thread than the game's"; }
    out["verdict"] = json!(v);
    out["stack"] = json!(stack);
    if !access.is_null() { out["access"] = access; }
    if let Some(regs) = d.get("registers") { out["registers"] = regs.clone(); }
    if report.is_some() {
        out["game"] = json!({"build": r["build"], "frames": r["frames"], "mission": r["mission"], "thread": r["thread"],
                             "main_thread": r["main_thread"]});
        out["mods"] = json!(r["mods"].as_array().map(|m| m.iter().map(|x| format!("{}{}{}", x["id"].as_str().unwrap_or("?"),
            if x["loaded"].as_bool() == Some(true) { format!(" (generation {})", x["generation"]) } else { " (not loaded)".into() },
            if x["faulted"].as_bool() == Some(true) { ", faulted" } else { "" })).collect::<Vec<_>>()).unwrap_or_default());
        out["log"] = r["log"].clone();
        out["taps"] = json!(r["taps"].as_array().map(|t| t.iter().filter_map(|x| x.as_str()).map(crate::kb::annotate_tap).collect::<Vec<_>>()).unwrap_or_default());
        out["events"] = json!(r["events"].as_array().map(|e| e.iter().map(|x| format!("{} {} {}", x["t"], x["type"].as_str().unwrap_or("?"), x["data"])).collect::<Vec<_>>()).unwrap_or_default());
    } else {
        out["report"] = json!("none beside the dump (a loader before 2026-10-17, or the report did not finish): the stack is a scan, the log read from the file");
        if let Some(l) = log_before(&stem, 40) { out["log"] = json!(l); }
    }
    if let Some(n) = d.get("threads") { out["threads"] = n.clone(); }
    Ok(out)
}
