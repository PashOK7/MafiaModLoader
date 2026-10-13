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
    let mut names = vec![];
    if let Ok(mut rd) = csv::Reader::from_path(d.join("names.csv")) {
        for r in rd.records().flatten() {
            let Some(addr) = hex(&r[0]) else { continue };
            names.push(Name { addr, kind: r[1].into(), name: r[2].into(), source: r[3].into(), note: r[4].into() });
        }
    }
    names.sort_by_key(|n| n.addr);
    Ok(Module { name, base, source, funcs, names, callers, callees })
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
    Ok(json!({ "notes": notes, "functions": functions }))
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
            let (m, a) = if c.get(2).is_some() {
                (kb.modules.iter().find(|m| m.name == "ls3df.dll"), 0x10000000 + raw)
            } else {
                (kb.modules.iter().find(|m| m.name == "game.exe"), raw)
            };
            let note = m.and_then(|m| {
                m.containing(a).and_then(|f| m.best_name(f).map(|n| format!(" ({}+0x{:X})", n, a - f.addr)))
            });
            format!("{}{}", &c[0], note.unwrap_or_default())
        }).into_owned()
    }).collect::<Vec<_>>().join("\n")
}

/// "Game.exe+0x7AD00" and "LS3DF.dll+0x5AC2E" in a text get the function's name after them, when known.
pub fn annotate(text: &str) -> String {
    let Ok(kb) = get() else { return text.to_string() };
    static RE: OnceLock<Regex> = OnceLock::new();
    let re = RE.get_or_init(|| Regex::new(r"(?i)\b(game\.exe|ls3df\.dll)\+0x([0-9a-f]+)").unwrap());
    re.replace_all(text, |c: &regex::Captures| {
        let module = c[1].to_lowercase();
        let off = u32::from_str_radix(&c[2], 16).unwrap_or(0);
        let m = kb.modules.iter().find(|m| m.name == module);
        let note = m.and_then(|m| {
            let a = m.base + off;
            m.containing(a).and_then(|f| m.best_name(f).map(|n| format!(" ({}+0x{:X})", n, a - f.addr)))
        });
        format!("{}{}", &c[0], note.unwrap_or_default())
    })
    .into_owned()
}
