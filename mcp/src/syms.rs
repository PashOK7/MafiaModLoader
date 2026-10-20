//! Names of code addresses: the game's from the knowledge base (kb.rs), the loader's and the mods' from their PDBs
//! (the build's, deployed beside them: MafiaModLoader.pdb in the game's folder, <id>.pdb in mml\mods\<id>\).

use pdb::FallibleIterator;
use std::collections::HashMap;
use std::path::{Path, PathBuf};
use std::sync::{Arc, Mutex, OnceLock};
use std::time::SystemTime;

/// A PDB's functions: (start RVA, length, name), by start.
type Procs = Arc<Vec<(u32, u32, String)>>;

fn read_pdb(path: &Path) -> Result<Vec<(u32, u32, String)>, pdb::Error> {
    let mut pdb = pdb::PDB::open(std::fs::File::open(path)?)?;
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
    Ok(out)
}

/// The functions of the PDB at `path`, read once while the file is unchanged.
fn procs(path: &Path) -> Option<Procs> {
    static CACHE: OnceLock<Mutex<HashMap<PathBuf, (SystemTime, Procs)>>> = OnceLock::new();
    let modified = std::fs::metadata(path).and_then(|m| m.modified()).ok()?;
    let mut cache = CACHE.get_or_init(|| Mutex::new(HashMap::new())).lock().ok()?;
    if let Some((t, p)) = cache.get(path) {
        if *t == modified { return Some(p.clone()); }
    }
    let p: Procs = Arc::new(read_pdb(path).ok()?);
    cache.insert(path.to_path_buf(), (modified, p.clone()));
    Some(p)
}

/// Where a module of ours keeps its PDB: the loader's in the game's folder, a mod's in its folder of mml\mods.
fn pdb_of(module: &str) -> Option<PathBuf> {
    let dir = crate::game::game_dir().ok()?;
    let mut stem = Path::new(module).file_stem()?.to_string_lossy().to_string();
    if module.eq_ignore_ascii_case("MafiaModLoader.asi") { return Some(dir.join("MafiaModLoader.pdb")); }
    // a mod as the loader runs it: mml\live\<id>_<pid>_<generation>\mod_<id>_<pid>_<generation>.dll
    if let Some(rest) = stem.strip_prefix("mod_") {
        let parts: Vec<&str> = rest.rsplitn(3, '_').collect();
        if parts.len() == 3 && parts[0].parse::<u32>().is_ok() && parts[1].parse::<u32>().is_ok() { stem = parts[2].to_string(); }
    }
    let mods = dir.join("mml").join("mods");
    for e in std::fs::read_dir(&mods).ok()?.flatten() {
        let p = e.path().join(format!("{stem}.pdb"));
        if p.exists() { return Some(p); }
    }
    None
}

/// The function of ours at `module`+`rva` by its PDB.
pub fn ours(module: &str, rva: u32) -> Option<String> {
    let p = procs(&pdb_of(module)?)?;
    let i = p.partition_point(|x| x.0 <= rva);
    let (start, len, name) = p.get(i.checked_sub(1)?)?;
    (rva < start + len.max(&1)).then(|| name.clone())
}

/// Where the function holding an address of ours starts (an RVA), by its PDB.
pub fn ours_start(module: &str, rva: u32) -> Option<u32> {
    let p = procs(&pdb_of(module)?)?;
    let i = p.partition_point(|x| x.0 <= rva);
    let (start, len, _) = p.get(i.checked_sub(1)?)?;
    (rva < start + (*len).max(1)).then_some(*start)
}

/// "LS3DF.dll+0x5AC2E": where its function starts, "ls3df.dll+0x5AC20" (lower case), when known.
pub fn start_of(text: &str) -> Option<String> {
    let (module, off) = text.split_once("+0x")?;
    let off = u32::from_str_radix(off, 16).ok()?;
    let lower = module.to_lowercase();
    let start = if lower == "game.exe" || lower == "ls3df.dll" { crate::kb::function_start(&lower, off) } else { ours_start(module, off) }?;
    Some(format!("{lower}+0x{start:x}"))
}

/// "LS3DF.dll+0x5AC2E", "MafiaModLoader.asi+0x3D29": the function there, by name (the game's through the knowledge
/// base, ours by the PDBs), else the text itself.
pub fn function_of(text: &str) -> String {
    let Some((module, off)) = text.split_once("+0x") else { return text.to_string() };
    let Ok(off) = u32::from_str_radix(off, 16) else { return text.to_string() };
    let lower = module.to_lowercase();
    let name = if lower == "game.exe" || lower == "ls3df.dll" { crate::kb::function_key(&lower, off) } else { ours(module, off) };
    match name {
        Some(n) if lower == "game.exe" || lower == "ls3df.dll" => n,
        Some(n) => format!("{n} [{module}]"),
        None => text.to_string(),
    }
}
