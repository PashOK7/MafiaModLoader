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
