//! The game knowledge base (knowledge/): every known function of Game.exe and LS3DF.dll (1.2) with its name, class,
//! signature, topics, strings and calls; searched, looked up by address or name, used to name the addresses in crash
//! reports and `where`. The decompiled code itself is read from the reverse folder (`reverse_dir` in
//! mml.config.json, a MafiaReverse checkout) and never kept in the repository.

use regex::Regex;
use serde_json::{Value, json};
use std::collections::HashMap;
use std::path::PathBuf;
use std::sync::OnceLock;

pub struct Func {
    pub addr: u32,
    pub name: String,
    pub class: String,
    pub signature: String,
    pub topics: String,
    pub strings: String,
    pub src: (usize, usize),
}

/// A name found by our own work (knowledge/<module>/names.csv): a function, or a site or datum.
pub struct Name {
    pub addr: u32,
    pub kind: String,
    pub name: String,
    pub source: String,
    pub note: String,
}

pub struct Module {
    pub name: &'static str,      // "game.exe", "ls3df.dll"
    pub base: u32,               // the decompile's image base
    pub source: &'static str,    // the decompile in the reverse folder
    pub funcs: Vec<Func>,        // by address
    pub names: Vec<Name>,        // by address
    /// 1.0's own names (names_10.csv: what agents named in the 1.0 Ghidra project), by 1.0 address
    pub names10: Vec<Name>,
    /// 1.0's functions paired with their 1.2 twins (knowledge/<module>/twins_10.csv), (1.0, 1.2) by 1.0 address
    pub twins10: Vec<(u32, u32)>,
    /// Every function's (start, size) as the Ghidra projects have them (knowledge/<module>/bounds.csv, bounds_10.csv:
    /// `ghidra_cli.py bounds`), by start: an address is named only by the function that holds it
    pub bounds: Vec<(u32, u32)>,
    pub bounds10: Vec<(u32, u32)>,
    pub callers: HashMap<u32, Vec<u32>>,
    pub callees: HashMap<u32, Vec<u32>>,
}

pub struct Kb {
    pub modules: Vec<Module>,
}

static KB: OnceLock<Result<Kb, String>> = OnceLock::new();

fn hex(s: &str) -> Option<u32> {
    u32::from_str_radix(s.trim().trim_start_matches("0x").trim_start_matches("0X"), 16).ok()
}

fn load_module(dir: &PathBuf, name: &'static str, base: u32, source: &'static str) -> Result<Module, String> {
    let d = dir.join(name);
    let mut funcs = vec![];
    let mut rd = csv::Reader::from_path(d.join("functions.csv")).map_err(|e| format!("{name}/functions.csv: {e}"))?;
    for r in rd.records() {
        let r = r.map_err(|e| e.to_string())?;
        let Some(addr) = hex(&r[0]) else { continue };
        funcs.push(Func {
            addr,
            name: r[1].to_string(),
            class: r[2].to_string(),
            signature: r[3].to_string(),
            topics: r[4].to_string(),
            strings: r[5].to_string(),
            src: (r[6].parse().unwrap_or(0), r[7].parse().unwrap_or(0)),
        });
    }
    funcs.sort_by_key(|f| f.addr);
    // the strings each function uses: read from this machine's own game files (tools/kb_strings.py), never committed
    if let Ok(mut rd) = csv::Reader::from_path(d.join("strings.local.csv")) {
        for r in rd.records().flatten() {
            let Some(addr) = hex(&r[0]) else { continue };
            if let Ok(i) = funcs.binary_search_by_key(&addr, |f| f.addr) {
                funcs[i].strings = r[1].to_string();
            }
        }
    }
    let mut callers: HashMap<u32, Vec<u32>> = HashMap::new();
    let mut callees: HashMap<u32, Vec<u32>> = HashMap::new();
    if let Ok(mut rd) = csv::Reader::from_path(d.join("calls.csv")) {
        for r in rd.records().flatten() {
            if let (Some(a), Some(b)) = (hex(&r[0]), hex(&r[1])) {
                callees.entry(a).or_default().push(b);
                callers.entry(b).or_default().push(a);
            }
        }
    }
    // ours (names.csv), the Mafia demo's LS3D.dll's own names carried here (tools/kb_demo.py: made on this machine, the
    // demo's licence lets nothing of it be copied), then other projects' (tools/kb_external.py, each checked against
    // the files): external.csv the ones the repository carries, external.local.csv the ones made from their clones
    let mut names = vec![];
    for file in ["names.csv", "demo.local.csv", "external.csv", "external.local.csv"] {
        let Ok(mut rd) = csv::Reader::from_path(d.join(file)) else { continue };
        for r in rd.records().flatten() {
            let Some(addr) = hex(&r[0]) else { continue };
            names.push(Name { addr, kind: r[1].into(), name: r[2].into(), source: r[3].into(), note: r[4].into() });
        }
    }
    names.sort_by_key(|n| n.addr);
    // 1.0's own: the demo's (demo_10.local.csv), st0rm94's at his addresses (external_10.csv), ours (names_10.csv)
    // over them at one address
    let mut by_addr = std::collections::BTreeMap::new();
    for file in ["demo_10.local.csv", "external_10.csv", "names_10.csv"] {
        let Ok(mut rd) = csv::Reader::from_path(d.join(file)) else { continue };
        for r in rd.records().flatten() {
            let Some(addr) = hex(&r[0]) else { continue };
            by_addr.insert(addr, Name { addr, kind: r[1].into(), name: r[2].into(), source: r[3].into(), note: r[4].into() });
        }
    }
    let names10: Vec<Name> = by_addr.into_values().collect();
    let mut twins10 = vec![];
    if let Ok(mut rd) = csv::Reader::from_path(d.join("twins_10.csv")) {
        for r in rd.records().flatten() {
            if let (Some(a12), Some(a10)) = (hex(&r[0]), hex(&r[1])) { twins10.push((a10, a12)); }
        }
    }
    twins10.sort();
    let bounds_of = |file: &str| {
        let mut v = vec![];
        if let Ok(mut rd) = csv::Reader::from_path(d.join(file)) {
            for r in rd.records().flatten() {
                if let (Some(s), Some(n)) = (hex(&r[0]), hex(&r[1])) { v.push((s, n)); }
            }
        }
        v.sort();
        v
    };
    let (bounds, bounds10) = (bounds_of("bounds.csv"), bounds_of("bounds_10.csv"));
    Ok(Module { name, base, source, funcs, names, names10, twins10, bounds, bounds10, callers, callees })
}

pub fn get() -> Result<&'static Kb, String> {
    KB.get_or_init(|| {
        let root = crate::game::repo_root().ok_or("cannot find the repository")?;
        let dir = root.join("knowledge");
        Ok(Kb {
            modules: vec![
                load_module(&dir, "game.exe", 0x400000, "Game.c")?,
                load_module(&dir, "ls3df.dll", 0x10000000, "LS3DF_dll.c")?,
            ],
        })
    })
    .as_ref()
    .map_err(|e| e.clone())
}

impl Module {
    /// The start of the function holding `addr` by `bounds` (its start and size), when one does.
    pub fn bounded(bounds: &[(u32, u32)], addr: u32) -> Option<u32> {
        let i = bounds.partition_point(|b| b.0 <= addr);
        let (s, n) = *bounds.get(i.checked_sub(1)?)?;
        (addr < s + n.max(1)).then_some(s)
    }
    /// The function holding an address of this module's image (its decompile's base).
    pub fn containing(&self, addr: u32) -> Option<&Func> {
        let i = self.funcs.partition_point(|f| f.addr <= addr);
        if i == 0 { return None; }
        let f = &self.funcs[i - 1];
        // without sizes: a function is taken to end where the next one starts; past the last one, data or another
        // module (the image's code ends within 64 KiB of its last function's start)
        if i == self.funcs.len() && addr - f.addr > 0x10000 { return None; }
        Some(f)
    }
    pub fn at(&self, addr: u32) -> Option<&Func> {
        self.funcs.binary_search_by_key(&addr, |f| f.addr).ok().map(|i| &self.funcs[i])
    }
    /// Our names for the function at an address (names.csv, kind "function").
    fn aliases(&self, addr: u32) -> impl Iterator<Item = &Name> {
        self.names.iter().filter(move |n| n.addr == addr && n.kind == "function")
    }
    /// The best name of a function: the decompile's, else ours.
    fn best_name(&self, f: &Func) -> Option<String> {
        if !f.name.is_empty() { return Some(f.name.clone()); }
        self.aliases(f.addr).next().map(|n| n.name.clone())
    }
    fn label(&self, addr: u32) -> String {
        match self.at(addr).and_then(|f| self.best_name(f)) {
            Some(n) => format!("{addr:08X} {n}"),
            None => format!("{addr:08X}"),
        }
    }
}

fn name_json(m: &Module, n: &Name) -> Value {
    let inside = m.containing(n.addr).filter(|f| f.addr != n.addr).map(|f| m.label(f.addr));
    json!({ "module": m.name, "address": format!("{:08X}", n.addr), "kind": n.kind, "name": n.name,
            "source": n.source, "note": n.note, "in": inside })
}

fn func_json(m: &Module, f: &Func) -> Value {
    json!({
        "module": m.name, "address": format!("{:08X}", f.addr), "name": f.name, "class": f.class,
        "signature": f.signature, "topics": f.topics, "strings": f.strings,
    })
}

/// The notes' lines (knowledge/notes/*.md) holding the text: "file:line: text".
fn search_notes(q: &str, limit: usize) -> Vec<String> {
    let Some(dir) = crate::game::repo_root().map(|r| r.join("knowledge").join("notes")) else { return vec![] };
    let mut files: Vec<_> = std::fs::read_dir(dir).into_iter().flatten().flatten().map(|e| e.path())
        .filter(|p| p.extension().is_some_and(|x| x == "md")).collect();
    files.sort();
    let mut out = vec![];
    for p in files {
        let Ok(text) = std::fs::read_to_string(&p) else { continue };
        let name = p.file_name().unwrap_or_default().to_string_lossy().into_owned();
        for (i, line) in text.lines().enumerate() {
            if line.to_lowercase().contains(q) {
                out.push(format!("notes/{name}:{}: {}", i + 1, line.trim()));
                if out.len() >= limit { return out; }
            }
        }
    }
    out
}

pub fn search(query: &str, module: Option<&str>, limit: usize) -> Result<Value, String> {
    let q = query.to_lowercase();
    let notes = search_notes(&q, limit);
    let functions = search_functions(&q, module, limit)?;
    let functions_10 = search_10(&q, module, limit)?;
    Ok(json!({ "notes": notes, "functions": functions, "functions_10": functions_10 }))
}

/// 1.0's own names (st0rm94's at his addresses, ours from the 1.0 Ghidra project) matching a name, a note or an
/// address of the 1.0 image; ghidra_read with build "1.0" reads the code there.
fn search_10(q: &str, module: Option<&str>, limit: usize) -> Result<Value, String> {
    let kb = get()?;
    let mut out = vec![];
    for m in &kb.modules {
        if module.is_some_and(|x| !x.eq_ignore_ascii_case(m.name)) { continue; }
        for n in &m.names10 {
            if n.name.to_lowercase().contains(q) || n.note.to_lowercase().contains(q) || format!("{:08x}", n.addr).contains(q) {
                out.push(json!({ "module": m.name, "address_10": format!("{:08X}", n.addr), "kind": n.kind, "name": n.name,
                                 "source": n.source, "note": n.note }));
                if out.len() >= limit { return Ok(json!(out)); }
            }
        }
    }
    Ok(json!(out))
}

fn search_functions(q: &str, module: Option<&str>, limit: usize) -> Result<Value, String> {
    let kb = get()?;
    let q = q.to_string();
    let mut out = vec![];
    for m in &kb.modules {
        if module.is_some_and(|x| !x.eq_ignore_ascii_case(m.name)) { continue; }
        // our own names first: they are the findings checked in the running game
        for n in &m.names {
            if n.name.to_lowercase().contains(&q) || n.note.to_lowercase().contains(&q) || format!("{:08x}", n.addr).contains(&q) {
                out.push(name_json(m, n));
                if out.len() >= limit { return Ok(json!(out)); }
            }
        }
        for f in &m.funcs {
            let hay = [&f.name, &f.class, &f.signature, &f.strings, &f.topics];
            if hay.iter().any(|h| h.to_lowercase().contains(&q)) || format!("{:08x}", f.addr).contains(&q) {
                out.push(func_json(m, f));
                if out.len() >= limit { return Ok(json!(out)); }
            }
        }
    }
    Ok(json!(out))
}

fn reverse_dir() -> Option<PathBuf> {
    if let Ok(d) = std::env::var("MML_REVERSE_DIR") { return Some(PathBuf::from(d)); }
    let root = crate::game::repo_root()?;
    let v: Value = serde_json::from_str(&std::fs::read_to_string(root.join("mml.config.json")).ok()?).ok()?;
    v["reverse_dir"].as_str().map(PathBuf::from)
}

static SOURCES: OnceLock<std::sync::Mutex<HashMap<&'static str, std::sync::Arc<Vec<String>>>>> = OnceLock::new();

fn source_lines(file: &'static str) -> Result<std::sync::Arc<Vec<String>>, String> {
    let cache = SOURCES.get_or_init(|| std::sync::Mutex::new(HashMap::new()));
    if let Some(v) = cache.lock().unwrap().get(file) { return Ok(v.clone()); }
    let dir = reverse_dir().ok_or("no reverse_dir in mml.config.json (a MafiaReverse folder): the code is not available, the facts are")?;
    let path = dir.join(file);
    let bytes = std::fs::read(&path).map_err(|e| format!("{}: {e}", path.display()))?;
    let text = String::from_utf8_lossy(&bytes);
    let lines = std::sync::Arc::new(text.lines().map(String::from).collect::<Vec<_>>());
    cache.lock().unwrap().insert(file, lines.clone());
    Ok(lines)
}

/// A function by address ("5409D0", "0x5409D0", "game+0x1409D0"), or by name ("C_mission::Open"): its facts, callers,
/// callees, and its decompiled code from the reverse folder.
pub fn function(key: &str, with_code: bool, max_lines: usize) -> Result<Value, String> {
    let kb = get()?;
    let k = key.trim();
    let mut found: Option<(&Module, &Func)> = None;
    let addr = if let Some(rest) = k.strip_prefix("game+").or_else(|| k.strip_prefix("exe+")) {
        hex(rest).map(|o| 0x400000 + o)
    } else if let Some(rest) = k.strip_prefix("ls3d+") {
        hex(rest).map(|o| 0x10000000 + o)
    } else {
        hex(k)
    };
    if let Some(a) = addr {
        for m in &kb.modules {
            if let Some(f) = m.at(a).or_else(|| m.containing(a)) { found = Some((m, f)); break; }
        }
    }
    if found.is_none() {
        'outer: for m in &kb.modules {
            for f in &m.funcs {
                if f.name.eq_ignore_ascii_case(k) { found = Some((m, f)); break 'outer; }
            }
            for n in &m.names {
                if n.name.eq_ignore_ascii_case(k) {
                    if let Some(f) = m.at(n.addr).or_else(|| m.containing(n.addr)) { found = Some((m, f)); break 'outer; }
                }
            }
        }
    }
    let (m, f) = found.ok_or_else(|| format!("no function '{k}' in the knowledge base (kb_search)"))?;
    let mut v = func_json(m, f);
    let end = m.funcs.iter().find(|g| g.addr > f.addr).map_or(u32::MAX, |g| g.addr);
    let ours: Vec<Value> = m.names.iter().filter(|n| n.addr >= f.addr && n.addr < end).map(|n| name_json(m, n)).collect();
    if !ours.is_empty() { v["our_names"] = json!(ours); }
    v["callers"] = json!(m.callers.get(&f.addr).map(|c| c.iter().map(|&a| m.label(a)).collect::<Vec<_>>()).unwrap_or_default());
    v["callees"] = json!(m.callees.get(&f.addr).map(|c| c.iter().map(|&a| m.label(a)).collect::<Vec<_>>()).unwrap_or_default());
    if with_code && f.src.0 > 0 {
        match source_lines(m.source) {
            Ok(lines) => {
                let (a, b) = (f.src.0.saturating_sub(1), f.src.1.min(lines.len()));
                let take = (b - a).min(max_lines);
                let mut code = lines[a..a + take].join("\n");
                if take < b - a { code += &format!("\n... ({} more lines)", b - a - take); }
                v["code"] = json!(code);
            }
            Err(e) => v["code_unavailable"] = json!(e),
        }
    }
    Ok(v)
}

/// A name for an address of the running build's image. The knowledge base is 1.2's: in 1.0 an address takes the 1.0 name
/// agents gave its function (names_10.csv, the 1.0 Ghidra project's; st0rm94's, the demo's), else it is named
/// through its function's 1.2 twin (<module>/twins_10.csv) and marked so, an unpaired function's not at all (a 1.2
/// name on a 1.0 address would be a wrong name).
fn name_for(kb: &Kb, module: &str, addr: u32) -> Option<String> {
    let m = kb.modules.iter().find(|m| m.name == module)?;
    if crate::game::current_build() != "1.0" {
        return m.containing(addr).and_then(|f| m.best_name(f).map(|n| format!(" ({}+0x{:X})", n, addr - f.addr)));
    }
    name_10(m, &m.twins10, addr)
}

/// A 1.0 address's name: 1.0's own (names_10.csv) of the function starting nearest below when it is nearer than the
/// start of the nearest twin (or an exact label of its own), else the 1.2 twin's name with the offset kept inside it.
fn name_10(m: &Module, twins10: &[(u32, u32)], addr: u32) -> Option<String> {
    // with the 1.0 project's functions known: the name of the function holding the address, else none (a name below
    // it belongs to another function)
    if !m.bounds10.is_empty() {
        if let Some(n) = m.names10.iter().find(|n| n.addr == addr && n.kind != "function") {
            return Some(format!(" ({}+0x0, 1.0)", n.name));   // (a label of ours exactly there)
        }
        let start = Module::bounded(&m.bounds10, addr)?;
        if let Some(n) = m.names10.iter().find(|n| n.addr == start && n.kind == "function") {
            return Some(format!(" ({}+0x{:X}, 1.0)", n.name, addr - start));
        }
        let t = twins10.binary_search_by_key(&start, |t| t.0).ok().map(|i| twins10[i])?;
        let f = m.at(t.1)?;
        return m.best_name(f).map(|n| format!(" ({}+0x{:X}, 1.0; 1.2 twin {:08X})", n, addr - start, t.1));
    }
    let own = {
        let i = m.names10.partition_point(|n| n.addr <= addr);
        (i > 0).then(|| &m.names10[i - 1]).filter(|n| n.addr == addr || (n.kind == "function" && addr - n.addr < 0x10000))
    };
    let twin = {
        let i = twins10.partition_point(|t| t.0 <= addr);
        (i > 0).then(|| twins10[i - 1])
    };
    if let Some(n) = own {
        if twin.is_none_or(|(a10, _)| n.addr >= a10) {
            return Some(format!(" ({}+0x{:X}, 1.0)", n.name, addr - n.addr));
        }
    }
    let (a10, a12) = twin?;
    let f = m.at(a12)?;
    // the offset must stay inside the 1.2 twin (taken to end where the next 1.2 function starts)
    let next = m.funcs.iter().find(|g| g.addr > a12).map(|g| g.addr).unwrap_or(a12 + 0x10000);
    if addr - a10 >= next - a12 { return None; }
    m.best_name(f).map(|n| format!(" ({}+0x{:X}, 1.0; 1.2 twin {:08X})", n, addr - a10, a12))
}

/// tap's output: the addresses of code it prints (Game.exe absolute "5409D0", LS3DF "ls3d+5AC2E") after "on ",
/// "from " and in the callers' line get the function's name.
pub fn annotate_tap(text: &str) -> String {
    let Ok(kb) = get() else { return text.to_string() };
    static RE: OnceLock<Regex> = OnceLock::new();
    let re = RE.get_or_init(|| Regex::new(r"(on |from |callers: |, )(ls3d\+)?([0-9A-F]{5,8})\b").unwrap());
    text.lines().map(|line| {
        let callers = line.trim_start().starts_with("callers:");
        re.replace_all(line, |c: &regex::Captures| {
            let lead = &c[1];
            if (lead == ", " || lead == "callers: ") && !callers { return c[0].to_string(); }
            let raw = u32::from_str_radix(&c[3], 16).unwrap_or(0);
            let note = if c.get(2).is_some() { name_for(kb, "ls3df.dll", 0x10000000 + raw) } else { name_for(kb, "game.exe", raw) };
            format!("{}{}", &c[0], note.unwrap_or_default())
        }).into_owned()
    }).collect::<Vec<_>>().join("\n")
}

/// Where the function holding `off` in a game module starts (an offset in the module, the running build's): 1.2's
/// from its functions; 1.0's the nearest start below of its own named functions and its twins'.
pub fn function_start(module: &str, off: u32) -> Option<u32> {
    let kb = get().ok()?;
    let m = kb.modules.iter().find(|m| m.name == module)?;
    let addr = m.base + off;
    let start = if crate::game::current_build() != "1.0" {
        if m.bounds.is_empty() { m.containing(addr)?.addr } else { Module::bounded(&m.bounds, addr)? }
    } else if !m.bounds10.is_empty() {
        Module::bounded(&m.bounds10, addr)?
    } else {
        let own = { let i = m.names10.partition_point(|n| n.addr <= addr); m.names10[..i].iter().rev().find(|n| n.kind == "function").map(|n| n.addr) };
        let twin = { let i = m.twins10.partition_point(|t| t.0 <= addr); (i > 0).then(|| m.twins10[i - 1].0) };
        own.into_iter().chain(twin).max()?
    };
    (addr - start < 0x10000).then(|| start - m.base)
}

/// The function holding `off` in a game module ("game.exe", "ls3df.dll"), by its name alone (the build's: 1.0's
/// through its own names or its twin's), for adding samples up by function.
pub fn function_key(module: &str, off: u32) -> Option<String> {
    let kb = get().ok()?;
    let base = kb.modules.iter().find(|m| m.name == module)?.base;
    match name_for(kb, module, base + off) {
        Some(s) => {
            let s = s.trim().trim_start_matches('(').trim_end_matches(')');
            Some(s.split("+0x").next()?.to_string())
        }
        None => function_start(module, off).map(|s| format!("FUN_{:08X}", base + s)),
    }
}

/// "Game.exe+0x7AD00" and "LS3DF.dll+0x5AC2E" in a text get the function's name after them, when known.
pub fn annotate(text: &str) -> String {
    let Ok(kb) = get() else { return text.to_string() };
    static RE: OnceLock<Regex> = OnceLock::new();
    let re = RE.get_or_init(|| Regex::new(r"(?i)\b(game\.exe|ls3df\.dll)\+0x([0-9a-f]+)").unwrap());
    re.replace_all(text, |c: &regex::Captures| {
        let module = c[1].to_lowercase();
        let off = u32::from_str_radix(&c[2], 16).unwrap_or(0);
        let base = kb.modules.iter().find(|m| m.name == module).map(|m| m.base).unwrap_or(0);
        format!("{}{}", &c[0], name_for(kb, &module, base + off).unwrap_or_default())
    })
    .into_owned()
}

#[cfg(test)]
mod tests {
    use super::*;

    fn name(addr: u32, kind: &str, n: &str) -> Name {
        Name { addr, kind: kind.into(), name: n.into(), source: "mml".into(), note: String::new() }
    }
    fn func(addr: u32, n: &str) -> Func {
        Func { addr, name: n.into(), class: String::new(), signature: String::new(), topics: String::new(), strings: String::new(), src: (0, 0) }
    }

    #[test]
    fn a_1_0_address_takes_its_own_name_when_nearer_than_the_twin() {
        let m = Module {
            name: "game.exe", base: 0x400000, source: "", funcs: vec![func(0x500000, "C_x::A"), func(0x500100, "C_x::B")],
            names: vec![], names10: vec![name(0x600050, "function", "C_y::Own"), name(0x600400, "label", "aSite")],
            twins10: vec![], bounds: vec![], bounds10: vec![], callers: HashMap::new(), callees: HashMap::new(),
        };
        let twins = [(0x600000, 0x500000), (0x600300, 0x500100)];
        // inside the twin, before the own name: the twin's
        assert_eq!(name_10(&m, &twins, 0x600010).as_deref(), Some(" (C_x::A+0x10, 1.0; 1.2 twin 00500000)"));
        // past the own name's start, the twin's start further below: the own name
        assert_eq!(name_10(&m, &twins, 0x600060).as_deref(), Some(" (C_y::Own+0x10, 1.0)"));
        // a twin starting after the own name: the twin's again
        assert_eq!(name_10(&m, &twins, 0x600310).as_deref(), Some(" (C_x::B+0x10, 1.0; 1.2 twin 00500100)"));
        // an own label exactly: itself, a label is no function past its address
        assert_eq!(name_10(&m, &twins, 0x600400).as_deref(), Some(" (aSite+0x0, 1.0)"));
        // LS3DF (no twins), no own name: none
        assert_eq!(name_10(&m, &[], 0x400000), None);
    }

    #[test]
    fn with_the_bounds_a_1_0_address_is_named_only_by_the_function_holding_it() {
        let m = Module {
            name: "game.exe", base: 0x400000, source: "", funcs: vec![func(0x500000, "C_x::A")],
            names: vec![], names10: vec![name(0x600050, "function", "C_y::Own")],
            twins10: vec![], bounds: vec![], bounds10: vec![(0x600000, 0x40), (0x600050, 0x20), (0x600100, 0x80)],
            callers: HashMap::new(), callees: HashMap::new(),
        };
        let twins = [(0x600000, 0x500000)];
        assert_eq!(name_10(&m, &twins, 0x600010).as_deref(), Some(" (C_x::A+0x10, 1.0; 1.2 twin 00500000)"));
        assert_eq!(name_10(&m, &twins, 0x600060).as_deref(), Some(" (C_y::Own+0x10, 1.0)"));
        // a function of no name past the own name (the old rule gave it C_y::Own+0xC0): none
        assert_eq!(name_10(&m, &twins, 0x600110), None);
        // between functions: none
        assert_eq!(name_10(&m, &twins, 0x600045), None);
    }
}
