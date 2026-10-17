//! The game's structures by name (knowledge/structs/, made by tools/kb_structs.py): each class's base, size, vtable
//! and actor kind in both builds, and its fields with each build's offset, type and source. `inspect` reads an object
//! of the running game through them: its class told by its vtable, every field named and typed, pointers followed.

use crate::bridge::Bridge;
use serde_json::{Value, json};
use std::collections::HashMap;
use std::future::Future;
use std::pin::Pin;
use std::sync::OnceLock;

pub struct Field {
    pub off: [Option<u32>; 2], // 1.2, 1.0
    pub name: String,
    pub ty: String,
    pub source: String,
    pub note: String,
}

pub struct Struct {
    pub name: String,
    pub base: Option<String>,
    pub size: [Option<u32>; 2],
    pub vtable: [Option<u32>; 2],
    pub kind: Option<u32>,
    pub source: String,
    pub note: String,
    pub fields: Vec<Field>,
}

pub struct Structs {
    pub list: Vec<Struct>,
    by_name: HashMap<String, usize>,
}

static STRUCTS: OnceLock<Result<Structs, String>> = OnceLock::new();

fn hex(s: &str) -> Option<u32> {
    let s = s.trim();
    if s.is_empty() { return None; }
    u32::from_str_radix(s.trim_start_matches("0x").trim_start_matches("0X"), 16).ok()
}

pub const BUILDS: [&str; 2] = ["1.2", "1.0"];

pub fn build_index(build: &str) -> Option<usize> {
    BUILDS.iter().position(|b| *b == build)
}

pub fn get() -> Result<&'static Structs, String> {
    STRUCTS
        .get_or_init(|| {
            let dir = crate::game::repo_root().ok_or("cannot find the repository")?.join("knowledge").join("structs");
            let mut list = vec![];
            let mut rd = csv::Reader::from_path(dir.join("structs.csv")).map_err(|e| format!("structs/structs.csv: {e}"))?;
            for r in rd.records() {
                let r = r.map_err(|e| e.to_string())?;
                list.push(Struct {
                    name: r[0].to_string(),
                    base: Some(r[1].to_string()).filter(|b| !b.is_empty()),
                    size: [hex(&r[2]), hex(&r[3])],
                    vtable: [hex(&r[4]), hex(&r[5])],
                    kind: hex(&r[6]),
                    source: r[7].to_string(),
                    note: r[8].to_string(),
                    fields: vec![],
                });
            }
            let mut by_name: HashMap<String, usize> = list.iter().enumerate().map(|(i, s)| (s.name.to_lowercase(), i)).collect();
            let mut rd = csv::Reader::from_path(dir.join("fields.csv")).map_err(|e| format!("structs/fields.csv: {e}"))?;
            for r in rd.records() {
                let r = r.map_err(|e| e.to_string())?;
                let key = r[0].to_lowercase();
                let i = match by_name.get(&key) {
                    Some(i) => *i,
                    None => {
                        list.push(Struct { name: r[0].to_string(), base: None, size: [None; 2], vtable: [None; 2], kind: None,
                                           source: String::new(), note: String::new(), fields: vec![] });
                        by_name.insert(key, list.len() - 1);
                        list.len() - 1
                    }
                };
                list[i].fields.push(Field { off: [hex(&r[1]), hex(&r[2])], name: r[3].to_string(), ty: r[4].to_string(),
                                            source: r[5].to_string(), note: r[6].to_string() });
            }
            Ok(Structs { list, by_name })
        })
        .as_ref()
        .map_err(|e| e.clone())
}

#[derive(Clone, Copy, PartialEq)]
pub enum Prim { U8, I8, U16, I16, U32, I32, F32, F64, Bool }

pub enum Ty {
    Prim(Prim),
    Ptr(Option<String>),
    Str,
    Chars,
    Embed(String),
}

/// "f32[3]" -> (F32, 3); "ptr:I3D_frame" -> (Ptr(I3D_frame), 1); "chars[32]" -> (Chars, 32 bytes).
pub fn parse_ty(t: &str) -> (Ty, u32) {
    let (base, n) = match t.find('[') {
        Some(i) => (&t[..i], t[i + 1..].trim_end_matches(']').parse().unwrap_or(1)),
        None => (t, 1),
    };
    let ty = match base {
        "u8" => Ty::Prim(Prim::U8), "i8" => Ty::Prim(Prim::I8), "u16" => Ty::Prim(Prim::U16), "i16" => Ty::Prim(Prim::I16),
        "u32" => Ty::Prim(Prim::U32), "i32" => Ty::Prim(Prim::I32), "f32" => Ty::Prim(Prim::F32), "f64" => Ty::Prim(Prim::F64),
        "bool" => Ty::Prim(Prim::Bool), "ptr" => Ty::Ptr(None), "str" => Ty::Str, "chars" => Ty::Chars,
        b if b.starts_with("ptr:") => Ty::Ptr(Some(b[4..].to_string())),
        b => Ty::Embed(b.to_string()),
    };
    (ty, n)
}

fn prim_size(p: Prim) -> u32 {
    match p { Prim::U8 | Prim::I8 | Prim::Bool => 1, Prim::U16 | Prim::I16 => 2, Prim::F64 => 8, _ => 4 }
}

impl Structs {
    pub fn find(&self, name: &str) -> Option<&Struct> {
        self.by_name.get(&name.to_lowercase()).map(|i| &self.list[*i])
    }

    /// The struct and its bases, the first base first.
    pub fn chain<'a>(&'a self, s: &'a Struct) -> Vec<&'a Struct> {
        let mut out = vec![s];
        while let Some(b) = out.last().and_then(|s| s.base.as_deref()).and_then(|b| self.find(b)) {
            if out.iter().any(|x| x.name == b.name) { break; }
            out.push(b);
        }
        out.reverse();
        out
    }

    pub fn by_vtable(&self, vt: u32, b: usize) -> Option<&Struct> {
        self.list.iter().find(|s| s.vtable[b] == Some(vt))
    }

    /// One element's bytes (an embedded struct's: its size, else where its last field ends).
    pub fn elem_size(&self, ty: &Ty, b: usize) -> Option<u32> {
        match ty {
            Ty::Prim(p) => Some(prim_size(*p)),
            Ty::Ptr(_) | Ty::Str => Some(4),
            Ty::Chars => Some(1),
            Ty::Embed(n) => self.find(n).and_then(|s| self.size_of(s, b)),
        }
    }

    pub fn size_of(&self, s: &Struct, b: usize) -> Option<u32> {
        if let Some(n) = s.size[b] { return Some(n); }
        let end = self.placed(s, b).iter().filter_map(|(o, f, _)| {
            let (ty, n) = parse_ty(&f.ty);
            let e = if matches!(ty, Ty::Embed(ref m) if m == &s.name) { None } else { self.elem_size(&ty, b) };
            Some(o + e.unwrap_or(4) * n)
        }).max();
        end
    }

    /// The fields of a struct and of its bases that have an offset in build b: (offset, field, the struct it is of).
    pub fn placed<'a>(&'a self, s: &'a Struct, b: usize) -> Vec<(u32, &'a Field, &'a str)> {
        let mut out: Vec<(u32, &Field, &str)> = vec![];
        for c in self.chain(s) {
            for f in &c.fields {
                if let Some(o) = f.off[b] { out.push((o, f, c.name.as_str())); }
            }
        }
        out.sort_by_key(|(o, _, _)| *o);
        out
    }

    /// The fields known only in the other build (st0rm94's 1.0 fields not carried to 1.2).
    pub fn unplaced(&self, s: &Struct, b: usize) -> usize {
        self.chain(s).iter().flat_map(|c| c.fields.iter()).filter(|f| f.off[b].is_none()).count()
    }
}

/// kb_search's part: the structs and fields whose name holds the text.
pub fn search(q: &str, limit: usize) -> Value {
    let Ok(st) = get() else { return json!([]) };
    let mut out = vec![];
    for s in &st.list {
        if s.name.to_lowercase().contains(q) {
            out.push(format!("struct {}{}{}", s.name, s.base.as_ref().map(|b| format!(" : {b}")).unwrap_or_default(),
                             s.vtable[0].map(|v| format!(", vtable 0x{v:X}")).unwrap_or_default()));
        }
        for f in &s.fields {
            if out.len() >= limit { break; }
            if f.name.to_lowercase().contains(q) {
                out.push(format!("{}.{} {} {} ({})", s.name, f.name, offsets(&f.off), f.ty, f.source));
            }
        }
        if out.len() >= limit { break; }
    }
    json!(out)
}

fn offsets(off: &[Option<u32>; 2]) -> String {
    let o = |v: Option<u32>| v.map(|v| format!("+0x{v:X}")).unwrap_or_else(|| "-".into());
    if off[0] == off[1] { o(off[0]) } else { format!("{} (1.0 {})", o(off[0]), o(off[1])) }
}

/// The layout of a struct with no game: each field with both builds' offsets, type, source and note.
pub fn layout(name: &str, filter: &[String]) -> Result<Value, String> {
    let st = get()?;
    let s = st.find(name).ok_or_else(|| format!("no struct \"{name}\" (kb_search finds them by name)"))?;
    let mut lines = vec![];
    let mut all: Vec<(u32, &Field, &str)> = vec![];
    for c in st.chain(s) {
        for f in &c.fields {
            all.push((f.off[1].or(f.off[0]).unwrap_or(0), f, c.name.as_str()));
        }
    }
    all.sort_by_key(|(o, _, _)| *o);
    for (_, f, owner) in all {
        if !keep(filter, &f.name) { continue; }
        let of = if owner != s.name { format!("{owner}.") } else { String::new() };
        let note = if f.note.is_empty() { String::new() } else { format!(": {}", f.note) };
        lines.push(format!("{} {of}{} {} [{}]{note}", offsets(&f.off), f.name, f.ty, f.source));
    }
    Ok(json!({
        "struct": s.name, "chain": st.chain(s).iter().map(|c| c.name.clone()).collect::<Vec<_>>(),
        "size": [s.size[0].map(|v| format!("0x{v:X}")), s.size[1].map(|v| format!("0x{v:X}"))],
        "vtable": [s.vtable[0].map(|v| format!("0x{v:X}")), s.vtable[1].map(|v| format!("0x{v:X}"))],
        "kind": s.kind, "source": s.source, "note": s.note, "fields": lines,
    }))
}

fn keep(filter: &[String], name: &str) -> bool {
    filter.is_empty() || filter.iter().any(|f| name.to_lowercase().contains(&f.to_lowercase()))
}

// ---- the running game
pub struct Inspector<'a> {
    pub bridge: &'a Bridge,
    pub b: usize,
    pub st: &'static Structs,
}

pub struct Opts {
    pub filter: Vec<String>,
    pub depth: u32,
    pub gaps: bool,
}

fn u32_at(bytes: &[u8], o: usize) -> Option<u32> {
    bytes.get(o..o + 4).map(|b| u32::from_le_bytes([b[0], b[1], b[2], b[3]]))
}

impl<'a> Inspector<'a> {
    async fn call(&self, method: &str, params: Value) -> Result<Value, String> {
        self.bridge.call(method, params, 5000).await
    }

    pub async fn read(&self, addr: u32, size: u32) -> Result<Vec<u8>, String> {
        let v = self.call("mem.read", json!({"addr": format!("0x{addr:X}"), "size": size})).await?;
        let h = v["hex"].as_str().ok_or("mem.read: no hex")?;
        (0..h.len() / 2).map(|i| u8::from_str_radix(&h[2 * i..2 * i + 2], 16).map_err(|e| e.to_string())).collect()
    }

    async fn dword(&self, addr: u32) -> Option<u32> {
        self.read(addr, 4).await.ok().and_then(|b| u32_at(&b, 0))
    }

    async fn text(&self, addr: u32) -> Option<String> {
        let mut out = vec![];
        for chunk in 0..4 {
            let b = self.read(addr + chunk * 64, 64).await.ok()?;
            match b.iter().position(|c| *c == 0) {
                Some(n) => { out.extend_from_slice(&b[..n]); break; }
                None => out.extend_from_slice(&b),
            }
        }
        Some(String::from_utf8_lossy(&out).into_owned())
    }

    /// The struct an object is, by its vtable; how it was told.
    pub async fn detect(&self, addr: u32) -> Option<(&'static Struct, String)> {
        let vt = self.dword(addr).await?;
        self.st.by_vtable(vt, self.b).map(|s| (s, format!("its vtable 0x{vt:X}")))
    }

    /// `at`: an address ("0x0A1B2C30", "Game.exe+0x2F9464"), or a name ("player", "car", "mission", "game") followed
    /// by fields (".inventory", ".vehicle", ".camera", "belt[2]"): an embedded one is its place, a pointer what it
    /// points at.
    pub async fn resolve(&self, at: &str) -> Result<(u32, Option<&'static Struct>, String), String> {
        let mut parts = at.split('.');
        let root = parts.next().unwrap_or_default();
        let (mut addr, mut s, mut how): (u32, Option<&'static Struct>, String) = match root {
            "player" | "mission" | "game" => {
                let g = self.call("game", json!({})).await.map_err(|e| format!("the core mod's `game`: {e}"))?;
                let field = if root == "player" { "player" } else { "mission" };
                let a = g[field].as_str().and_then(hex).filter(|a| *a != 0).ok_or(format!("no {field} now"))?;
                if root == "game" {
                    let m = self.st.find("C_mission").ok_or("no C_mission in the structs")?;
                    let o = m.fields.iter().find(|f| f.name == "game").and_then(|f| f.off[self.b]).ok_or("C_mission.game not known")?;
                    let p = self.dword(a + o).await.filter(|p| *p != 0).ok_or("no game object")?;
                    (p, self.st.find("C_game"), "the mission's game".into())
                } else {
                    (a, None, format!("the core mod's {field}"))
                }
            }
            "car" => {
                let c = self.call("car", json!({})).await.map_err(|e| format!("the core mod's `car`: {e}"))?;
                let a = c["car"].as_str().and_then(hex).ok_or("no car: the player has not driven one")?;
                (a, None, "the player's car (driven now, else last)".into())
            }
            _ => {
                let v = self.call("addr.describe", json!({"addr": at})).await?;
                let a = v["abs"].as_u64().ok_or_else(|| format!("not an address: {at}"))? as u32;
                return Ok((a, None, "the address".into()));
            }
        };
        for part in parts {
            if s.is_none() {
                s = self.detect(addr).await.map(|(x, _)| x);
            }
            let cur = s.ok_or_else(|| format!("{at}: the object at 0x{addr:X} is not a known struct before \"{part}\""))?;
            let (name, index) = match part.find('[') {
                Some(i) => (&part[..i], part[i + 1..].trim_end_matches(']').parse::<u32>().map_err(|_| format!("{part}: an index"))?),
                None => (part, 0),
            };
            let placed = self.st.placed(cur, self.b);
            let (o, f, _) = placed.iter().find(|(_, f, _)| f.name == name)
                .ok_or_else(|| format!("{} has no field \"{name}\" in {}", cur.name, BUILDS[self.b]))?;
            let (ty, _) = parse_ty(&f.ty);
            let es = self.st.elem_size(&ty, self.b).unwrap_or(4);
            let place = addr + o + index * es;
            match ty {
                Ty::Embed(n) => { addr = place; s = self.st.find(&n); }
                Ty::Ptr(t) => {
                    addr = self.dword(place).await.ok_or_else(|| format!("cannot read {name}"))?;
                    if addr == 0 { return Err(format!("{at}: {name} is null")); }
                    s = t.and_then(|t| self.st.find(&t));
                }
                _ => { addr = place; s = None; }
            }
            how = format!("{at}");
        }
        Ok((addr, s, how))
    }

    /// An object's fields with their values.
    pub fn inspect<'b>(&'b self, addr: u32, s: &'static Struct, opts: &'b Opts) -> Pin<Box<dyn Future<Output = Result<Value, String>> + Send + 'b>>
    where 'a: 'b {
        Box::pin(async move {
            let st = self.st;
            let b = self.b;
            let placed = st.placed(s, b);
            let size = st.size_of(s, b).unwrap_or(0x100).clamp(4, 0x10000);
            let bytes = match self.read(addr, size).await {
                Ok(x) => x,
                Err(e) => return Err(format!("{} at 0x{addr:X}: {e}", s.name)),
            };
            let mut lines = vec![];
            let mut follow = serde_json::Map::new();
            let mut covered = vec![false; bytes.len()];
            for (o, f, owner) in &placed {
                let (ty, n) = parse_ty(&f.ty);
                let es = st.elem_size(&ty, b);
                if let Some(es) = es {
                    for k in *o as usize..((*o + es * n) as usize).min(covered.len()) { covered[k] = true; }
                }
                if !keep(&opts.filter, &f.name) && !matches!(ty, Ty::Embed(_)) { continue; }
                let tag = if f.source == "st0rm94" { " (st0rm94)" } else { "" };
                let _ = owner;
                self.field_lines(&bytes, *o, &f.name, &f.ty, tag, opts, 0, &mut lines, &mut follow).await;
            }
            if opts.gaps {
                let mut o = 0;
                while o + 4 <= covered.len() {
                    if !covered[o..o + 4].iter().any(|c| *c) {
                        let v = u32_at(&bytes, o).unwrap_or(0);
                        if v != 0 {
                            lines.push(format!("+0x{o:03X} ? = 0x{v:08X} ({} / {})", v as i32, f32::from_bits(v)));
                        }
                    }
                    o += 4;
                }
                lines.sort();
            }
            let mut out = json!({
                "at": format!("0x{addr:08X}"), "struct": s.name, "build": BUILDS[b],
                "chain": st.chain(s).iter().map(|c| c.name.clone()).collect::<Vec<_>>(),
                "size": format!("0x{size:X}"), "fields": lines,
            });
            let un = st.unplaced(s, b);
            if un > 0 {
                out["not_placed"] = json!(format!("{un} field(s) known only in the other build (inspect without `at` lists them)"));
            }
            if !follow.is_empty() { out["follow"] = Value::Object(follow); }
            Ok(out)
        })
    }

    #[allow(clippy::too_many_arguments)]
    fn field_lines<'b>(&'b self, bytes: &'b [u8], o: u32, name: &'b str, tys: &'b str, tag: &'b str, opts: &'b Opts, level: u32,
                       lines: &'b mut Vec<String>, follow: &'b mut serde_json::Map<String, Value>)
                       -> Pin<Box<dyn Future<Output = ()> + Send + 'b>> {
        Box::pin(async move {
            let st = self.st;
            let (ty, n) = parse_ty(tys);
            let es = st.elem_size(&ty, self.b);
            let at = o as usize;
            match &ty {
                Ty::Embed(sn) => {
                    let Some(es) = es else {
                        lines.push(format!("+0x{o:03X} {name} {tys} (its size is not known){tag}"));
                        return;
                    };
                    let Some(sub) = st.find(sn) else { return };
                    if level > 3 { return; }
                    for k in 0..n.min(16) {
                        let base = o + k * es;
                        let prefix = if n > 1 { format!("{name}[{k}]") } else { name.to_string() };
                        let span = bytes.get(base as usize..(base + es) as usize);
                        if n > 1 && span.is_some_and(|b| b.iter().all(|x| *x == 0)) {
                            if keep(&opts.filter, &prefix) || keep(&opts.filter, name) || opts.filter.is_empty() {
                                lines.push(format!("+0x{base:03X} {prefix} {sn} = (all zero){tag}"));
                            }
                            continue;
                        }
                        for (so, f, _) in st.placed(sub, self.b) {
                            if !keep(&opts.filter, &f.name) && !keep(&opts.filter, &prefix) && !matches!(parse_ty(&f.ty).0, Ty::Embed(_)) { continue; }
                            let sub_name = format!("{prefix}.{}", f.name);
                            let sub_tag = if f.source == "st0rm94" { " (st0rm94)" } else { tag };
                            self.field_lines(bytes, base + so, &sub_name, &f.ty, sub_tag, opts, level + 1, lines, follow).await;
                        }
                    }
                }
                Ty::Chars => {
                    let end = (at + n as usize).min(bytes.len());
                    let raw = &bytes[at.min(end)..end];
                    let s = String::from_utf8_lossy(&raw[..raw.iter().position(|c| *c == 0).unwrap_or(raw.len())]).into_owned();
                    lines.push(format!("+0x{o:03X} {name} {tys} = {s:?}{tag}"));
                }
                Ty::Prim(p) => {
                    let es = prim_size(*p) as usize;
                    let vals: Vec<String> = (0..n as usize).map(|k| prim(bytes, at + k * es, *p)).collect();
                    let v = if n == 1 { vals.join("") } else { format!("[{}]", vals.join(", ")) };
                    lines.push(format!("+0x{o:03X} {name} {tys} = {v}{tag}"));
                }
                Ty::Str => {
                    let p = u32_at(bytes, at).unwrap_or(0);
                    let s = if p == 0 { "null".into() } else { self.text(p).await.map(|t| format!("{t:?}")).unwrap_or_else(|| format!("0x{p:08X} (unreadable)")) };
                    lines.push(format!("+0x{o:03X} {name} {tys} = {s}{tag}"));
                }
                Ty::Ptr(t) => {
                    let mut vals = vec![];
                    for k in 0..n.min(16) as usize {
                        let p = u32_at(bytes, at + 4 * k).unwrap_or(0);
                        if p == 0 { vals.push("null".to_string()); continue; }
                        let (desc, target) = self.describe(p, t.as_deref()).await;
                        vals.push(format!("0x{p:08X}{desc}"));
                        if opts.depth > 0 && level == 0 && n == 1 {
                            if let Some(ts) = target {
                                let sub = Opts { filter: vec![], depth: opts.depth - 1, gaps: false };
                                let r = self.inspect(p, ts, &sub).await;
                                follow.insert(name.to_string(), r.unwrap_or_else(|e| json!({"error": e})));
                            }
                        }
                    }
                    let v = if n == 1 { vals.join("") } else { format!("[{}]", vals.join(", ")) };
                    lines.push(format!("+0x{o:03X} {name} {tys} = {v}{tag}"));
                }
            }
        })
    }

    /// What a pointer points at: a known struct (by its vtable, else as typed), a frame's name, a place in a module.
    async fn describe(&self, p: u32, typed: Option<&str>) -> (String, Option<&'static Struct>) {
        if let Some(s) = self.st.by_vtable(p, self.b) {
            return (format!(" ({}'s vtable)", s.name), None);
        }
        if let Some((s, _)) = self.detect(p).await {
            return (format!(" -> {}", s.name), Some(s));
        }
        if let Some(t) = typed {
            let s = self.st.find(t);
            if t == "I3D_frame" {
                if let Some(o) = s.and_then(|s| s.fields.iter().find(|f| f.name == "name")).and_then(|f| f.off[self.b]) {
                    if let Some(np) = self.dword(p + o).await.filter(|x| *x != 0) {
                        if let Some(n) = self.text(np).await { return (format!(" -> I3D_frame {n:?}"), s); }
                    }
                }
            }
            return (format!(" -> {t}?"), s);
        }
        if let Ok(v) = self.call("addr.describe", json!({"addr": format!("0x{p:X}")})).await {
            if let Some(w) = v["where"].as_str().filter(|w| w.contains('+')) { return (format!(" ({w})"), None); }
        }
        (String::new(), None)
    }
}

fn prim(bytes: &[u8], at: usize, p: Prim) -> String {
    let b = |n: usize| bytes.get(at..at + n);
    match p {
        Prim::U8 => b(1).map(|x| x[0].to_string()),
        Prim::I8 => b(1).map(|x| (x[0] as i8).to_string()),
        Prim::Bool => b(1).map(|x| (x[0] != 0).to_string()),
        Prim::U16 => b(2).map(|x| u16::from_le_bytes([x[0], x[1]]).to_string()),
        Prim::I16 => b(2).map(|x| i16::from_le_bytes([x[0], x[1]]).to_string()),
        Prim::U32 => b(4).map(|x| { let v = u32::from_le_bytes([x[0], x[1], x[2], x[3]]); if v > 0xFFFF { format!("0x{v:X}") } else { v.to_string() } }),
        Prim::I32 => b(4).map(|x| i32::from_le_bytes([x[0], x[1], x[2], x[3]]).to_string()),
        Prim::F32 => b(4).map(|x| f32::from_le_bytes([x[0], x[1], x[2], x[3]]).to_string()),
        Prim::F64 => b(8).map(|x| f64::from_le_bytes([x[0], x[1], x[2], x[3], x[4], x[5], x[6], x[7]]).to_string()),
    }
    .unwrap_or_else(|| "?".into())
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn types() {
        assert!(matches!(parse_ty("f32[3]"), (Ty::Prim(Prim::F32), 3)));
        assert!(matches!(parse_ty("ptr:I3D_frame"), (Ty::Ptr(Some(ref n)), 1) if n == "I3D_frame"));
        assert!(matches!(parse_ty("chars[32]"), (Ty::Chars, 32)));
        assert!(matches!(parse_ty("S_GameItem[5]"), (Ty::Embed(ref n), 5) if n == "S_GameItem"));
        assert_eq!(prim(&[0, 0, 0x48, 0x43], 0, Prim::F32), "200");
        assert_eq!(prim(&[0xFF], 0, Prim::I8), "-1");
    }

    #[test]
    fn the_tables() {
        let st = get().expect("knowledge/structs");
        let h = st.find("c_human").expect("C_human");
        let chain: Vec<_> = st.chain(h).iter().map(|s| s.name.as_str()).collect();
        assert_eq!(chain, ["C_actor", "C_human"]);
        // the SDK's fields, in both builds
        let p = st.placed(h, 0);
        assert!(p.iter().any(|(o, f, _)| *o == 0x644 && f.name == "health"));
        assert!(p.iter().any(|(o, f, owner)| *o == 0x68 && f.name == "frame" && *owner == "C_actor"));
        // the inventory is embedded, its items' size known
        assert_eq!(st.elem_size(&parse_ty("G_Inventory").0, 1), Some(0xE0));
        // C_Vehicle in 1.0 is 1.2's + 0x7C past its start (game_build.h)
        let v = st.find("C_Vehicle").expect("C_Vehicle");
        let speed = v.fields.iter().find(|f| f.name == "speed").expect("speed");
        assert_eq!(speed.off, [Some(0x59C), Some(0x618)]);
        assert!(st.by_vtable(st.find("C_player").unwrap().vtable[0].unwrap(), 0).is_some_and(|s| s.name == "C_player"));
    }
}
