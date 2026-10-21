//! Names of code addresses: the game's from the knowledge base (kb.rs), the loader's and the mods' from their PDBs
//! (the build's, deployed beside them: MafiaModLoader.pdb in the game's folder, <id>.pdb in mml\mods\<id>\; any other
//! module's <name>.pdb in the game's folder, as the stand-in game's is).

use pdb::FallibleIterator;
use std::collections::HashMap;
use std::path::{Path, PathBuf};
use std::sync::{Arc, Mutex, OnceLock};
use std::time::SystemTime;

/// A PDB read: its functions (start RVA, length, name), by start, and its GUID (lower case, hyphenated: what an
/// image's CodeView record names, a dump's module's debug identifier).
struct Pdb {
    procs: Vec<(u32, u32, String)>,
    guid: String,
}

fn read_pdb(path: &Path) -> Result<Pdb, pdb::Error> {
    let mut pdb = pdb::PDB::open(std::fs::File::open(path)?)?;
    let guid = pdb.pdb_information()?.guid.to_string().to_lowercase();
    let map = pdb.address_map()?;
    let dbi = pdb.debug_information()?;
    let mut modules = dbi.modules()?;
    let mut out = vec![];
    while let Some(module) = modules.next()? {
        let Some(info) = pdb.module_info(&module)? else { continue };
        let mut symbols = info.symbols()?;
        while let Some(sym) = symbols.next()? {
            if let Ok(pdb::SymbolData::Procedure(p)) = sym.parse() {
                if let Some(rva) = p.offset.to_rva(&map) {
                    out.push((rva.0, p.len, p.name.to_string().into_owned()));
                }
            }
        }
    }
    out.sort_by_key(|x| x.0);
    Ok(Pdb { procs: out, guid })
}

/// The PDB at `path`, read once while the file is unchanged.
fn read_cached(path: &Path) -> Option<Arc<Pdb>> {
    static CACHE: OnceLock<Mutex<HashMap<PathBuf, (SystemTime, Arc<Pdb>)>>> = OnceLock::new();
    let modified = std::fs::metadata(path).and_then(|m| m.modified()).ok()?;
    let mut cache = CACHE.get_or_init(|| Mutex::new(HashMap::new())).lock().ok()?;
    if let Some((t, p)) = cache.get(path) {
        if *t == modified { return Some(p.clone()); }
    }
    let p = Arc::new(read_pdb(path).ok()?);
    cache.insert(path.to_path_buf(), (modified, p.clone()));
    Some(p)
}

/// A mod as the loader runs it (mml\live\<id>_<pid>_<generation>\mod_<id>_<pid>_<generation>.dll): its id.
pub fn mod_id(module: &str) -> Option<String> {
    let stem = Path::new(module).file_stem()?.to_string_lossy().to_string();
    let rest = stem.strip_prefix("mod_")?;
    let parts: Vec<&str> = rest.rsplitn(3, '_').collect();
    (parts.len() == 3 && parts[0].parse::<u32>().is_ok() && parts[1].parse::<u32>().is_ok()).then(|| parts[2].to_string())
}

/// Where a module keeps its PDB: the loader's in the game's folder, a mod's in its folder of mml\mods, another's
/// beside the game (the stand-in game's).
fn pdb_of(module: &str) -> Option<PathBuf> {
    let dir = crate::game::game_dir().ok()?;
    if module.eq_ignore_ascii_case("MafiaModLoader.asi") { return Some(dir.join("MafiaModLoader.pdb")); }
    let stem = match mod_id(module) {
        Some(id) => id,
        None => {
            let stem = Path::new(module).file_stem()?.to_string_lossy().to_string();
            let beside = dir.join(format!("{stem}.pdb"));
            if beside.exists() { return Some(beside); }
            stem
        }
    };
    let mods = dir.join("mml").join("mods");
    for e in std::fs::read_dir(&mods).ok()?.flatten() {
        let p = e.path().join(format!("{stem}.pdb"));
        if p.exists() { return Some(p); }
    }
    None
}

fn pdb_for(module: &str) -> Option<Arc<Pdb>> { read_cached(&pdb_of(module)?) }

/// The GUID of the PDB found for a module (to be matched with the image's: a PDB built later names wrongly).
pub fn pdb_guid(module: &str) -> Option<String> { pdb_for(module).map(|p| p.guid.clone()) }

/// The function of ours holding `module`+`rva` by its PDB: (its start, its name).
fn proc_at(module: &str, rva: u32) -> Option<(u32, String)> {
    let p = pdb_for(module)?;
    let i = p.procs.partition_point(|x| x.0 <= rva);
    let (start, len, name) = p.procs.get(i.checked_sub(1)?)?;
    (rva < start + (*len).max(1)).then(|| (*start, name.clone()))
}

/// The function of ours at `module`+`rva` by its PDB.
pub fn ours(module: &str, rva: u32) -> Option<String> { proc_at(module, rva).map(|p| p.1) }

/// Where the function holding an address of ours starts (an RVA), by its PDB.
pub fn ours_start(module: &str, rva: u32) -> Option<u32> { proc_at(module, rva).map(|p| p.0) }

fn is_game(lower: &str) -> bool { lower == "game.exe" || lower == "ls3df.dll" }

/// "LS3DF.dll+0x5AC2E": where its function starts, "ls3df.dll+0x5AC20" (lower case), when known.
pub fn start_of(text: &str) -> Option<String> {
    let (module, off) = text.split_once("+0x")?;
    let off = u32::from_str_radix(off, 16).ok()?;
    let lower = module.to_lowercase();
    let start = if is_game(&lower) { crate::kb::function_start(&lower, off) } else { ours_start(module, off) }?;
    Some(format!("{lower}+0x{start:x}"))
}

/// "LS3DF.dll+0x5AC2E", "MafiaModLoader.asi+0x3D29": the function there, by name (the game's through the knowledge
/// base, ours by the PDBs), else the text itself.
pub fn function_of(text: &str) -> String {
    let Some((module, off)) = text.split_once("+0x") else { return text.to_string() };
    let Ok(off) = u32::from_str_radix(off, 16) else { return text.to_string() };
    let lower = module.to_lowercase();
    let name = if is_game(&lower) { crate::kb::function_key(&lower, off) } else { ours(module, off) };
    match name {
        Some(n) if is_game(&lower) => n,
        Some(n) => format!("{n} [{module}]"),
        None => text.to_string(),
    }
}

/// A stack's frame, "Game.exe+0x7AD00", named after it: the game's by the knowledge base (" (C_game::Tick+0x10)",
/// a function of no name by its bounds: " (FUN_10012340+0x1C, 1.0)"), ours by their PDBs (" (host::Pump+0x2E)"), a
/// mod's with its id (" (mod hook_b: `anonymous namespace'::Detour+0x12)"); another module's as it is. `pdb_ok`
/// says whether a module's PDB may name it (its GUID is the image's).
pub fn describe(text: &str, pdb_ok: &dyn Fn(&str) -> bool) -> String {
    let Some((module, off_text)) = text.split_once("+0x") else { return text.to_string() };
    let Ok(off) = u32::from_str_radix(off_text, 16) else { return text.to_string() };
    let lower = module.to_lowercase();
    if is_game(&lower) { return format!("{text}{}", crate::kb::describe(&lower, off).unwrap_or_default()); }
    let named = if pdb_ok(module) { proc_at(module, off).map(|(s, n)| format!("{n}+0x{:X}", off - s)) } else { None };
    match (mod_id(module), named) {
        (Some(id), Some(n)) => format!("{text} (mod {id}: {n})"),
        (Some(id), None) => format!("{text} (mod {id})"),
        (None, Some(n)) => format!("{text} ({n})"),
        (None, None) => text.to_string(),
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn a_mod_is_known_by_its_live_name() {
        assert_eq!(mod_id("mod_hook_b_1234_2.dll").as_deref(), Some("hook_b"));
        assert_eq!(mod_id("mod_flow_smoke_77_1.dll").as_deref(), Some("flow_smoke"));
        assert_eq!(mod_id("MafiaModLoader.asi"), None);
        assert_eq!(mod_id("mod_x.dll"), None);
    }
}
