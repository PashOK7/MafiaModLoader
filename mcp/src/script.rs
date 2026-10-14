//! The mission scripts' commands, for agents writing programs for the game's script machine (`script.run`):
//! each command's opcode and handler in both builds (knowledge/game.exe/script_commands.csv, read from the
//! binaries), and, where st0rm94's 1.0 reverse is cloned (re/ext/mafia-reverse-engineering-export: never kept in
//! the repository), the note its annotated decompile has on the command's case, read in place.

use serde_json::{Value, json};
use std::path::PathBuf;

fn root() -> Result<PathBuf, String> {
    crate::game::repo_root().ok_or_else(|| "cannot find the repository (set MML_REPO)".to_string())
}

/// The case notes of st0rm94's annotated 1.0 dispatcher, by command name (upper case): its `/* ... */` block.
fn case_notes(names: &[String]) -> std::collections::HashMap<String, String> {
    let mut out = std::collections::HashMap::new();
    let Ok(r) = root() else { return out };
    let path = r.join("re").join("ext").join("mafia-reverse-engineering-export").join("ghidra").join("Game-ghidra.exe.annotated.c");
    let Ok(bytes) = std::fs::read(&path) else { return out };
    let text = String::from_utf8_lossy(&bytes);
    let lines: Vec<&str> = text.lines().collect();
    let want: std::collections::HashSet<&str> = names.iter().map(|s| s.as_str()).collect();
    let mut i = 0;
    while i < lines.len() {
        let l = lines[i];
        // "  case 0x3a:                                     // HUMAN_ADDWEAPON"
        if l.starts_with("  case 0x") {
            if let Some(pos) = l.find("// ") {
                let name = l[pos + 3..].trim().to_string();
                if want.contains(name.as_str()) && i + 1 < lines.len() && lines[i + 1].trim_start().starts_with("/*") {
                    let mut note = Vec::new();
                    let mut j = i + 1;
                    while j < lines.len() {
                        let t = lines[j].trim();
                        let t = t.trim_start_matches("/*").trim_end_matches("*/").trim_start_matches('*').trim();
                        if !t.starts_with("---") { note.push(t.to_string()); }
                        if lines[j].trim_end().ends_with("*/") { break; }
                        j += 1;
                    }
                    let joined = note.join("\n").trim().to_string();
                    out.entry(name).or_insert(joined);
                    i = j;
                }
            }
        }
        i += 1;
    }
    out
}

/// The commands whose name contains `query` (case-insensitive; empty: all names only).
pub fn commands(query: &str, notes: bool) -> Result<Value, String> {
    let path = root()?.join("knowledge").join("game.exe").join("script_commands.csv");
    let mut rdr = csv::Reader::from_path(&path).map_err(|e| format!("{}: {e}", path.display()))?;
    let q = query.trim().to_uppercase();
    let mut rows = vec![];
    for rec in rdr.records() {
        let rec = rec.map_err(|e| e.to_string())?;
        let name = rec.get(1).unwrap_or("").to_string();
        if !q.is_empty() && !name.contains(&q) { continue; }
        let field = |i: usize| rec.get(i).unwrap_or("").to_string();
        let handled = |h: &str| !h.is_empty() && h != "default";
        rows.push(json!({
            "command": name.to_lowercase(),
            "1.2": {"opcode": field(0), "handler": field(2), "runs": handled(&field(2))},
            "1.0": {"opcode": field(3), "handler": field(4), "runs": handled(&field(4))},
        }));
    }
    if q.is_empty() {
        let names: Vec<Value> = rows.iter().map(|r| r["command"].clone()).collect();
        return Ok(json!({"count": names.len(), "commands": names,
                         "note": "give a query (\"human_\", \"car_\", \"frm_\", \"actor\") for opcodes, handlers and notes"}));
    }
    if notes && rows.len() <= 40 {
        let names: Vec<String> = rows.iter().map(|r| r["command"].as_str().unwrap_or("").to_uppercase()).collect();
        let found = case_notes(&names);
        for r in rows.iter_mut() {
            let n = r["command"].as_str().unwrap_or("").to_uppercase();
            if let Some(t) = found.get(&n) { r["note_1_0"] = json!(t); }
        }
    }
    Ok(json!({"count": rows.len(), "commands": rows,
              "source": "opcodes and handlers: knowledge/game.exe/script_commands.csv; note_1_0: st0rm94's annotated 1.0 decompile, read in place (re/ext), a lead: the 1.2 case may differ"}))
}
