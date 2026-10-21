//! A prof.sample report's stacks made functions (MCP's `profile`): each function's self time (the thread in it), its
//! calls out (the thread in a system or driver module it called itself: Direct3D, the driver) and its total (it
//! anywhere on the stack, once a sample however deep a recursion), in % of the samples.
//!
//! A stack holds stale return addresses too: a call that returned leaves its return address in its caller's unwritten
//! locals. So a return address is kept only where its call can be checked to lead to the frame below it: the host
//! gives each one's direct call's target (followed through jumps and hooks), and a target that is not the start of the
//! function below means a stale one (or a frame below that was stale: the frames between are taken out when the target
//! is an earlier kept frame's function). Calls through a vtable cannot be checked: they are kept. The frames the host
//! found by their frame pointers (the system's code, the driver's, the loader's: up to the first frame of the game's)
//! are certain.

use serde_json::{Value, json};
use std::collections::{HashMap, HashSet};

/// What is known of the call before a return address.
#[derive(Clone, Debug, PartialEq)]
pub enum Target {
    /// a call through a register or an object: not checked
    Unknown,
    /// a direct call: where it went (a function's start, "ls3df.dll+0x5ac20")
    Start(String),
    /// found by the frame pointers: certain
    Anchored,
}

/// The frames kept of one stack: `eip` (an index) at the bottom, then each (return address, its call's target) of
/// `pairs` that leads to a kept frame. `start(i)`: where address i's function starts, when known.
///
/// A direct call that leads to no kept frame is a stale return address, or a real one whose callee ended in a jump
/// to another function (a tail call: the game's compiler makes them): it is held, and kept only when a frame above
/// leads to it (the chain goes on through it); a stale one is not led to, or is taken out with what lies above the
/// frame a later one leads to.
pub fn kept_frames(eip: usize, pairs: &[(usize, Target)], start: &dyn Fn(usize) -> Option<String>) -> Vec<usize> {
    // (index, its function's start, checked: false for one held)
    let mut kept: Vec<(usize, Option<String>, bool)> = vec![(eip, start(eip), true)];
    for (r, target) in pairs {
        match target {
            Target::Unknown | Target::Anchored => kept.push((*r, start(*r), true)),
            Target::Start(t) => {
                if let Some(j) = kept.iter().rposition(|k| k.1.as_deref() == Some(t.as_str())) {
                    kept.truncate(j + 1);   // (what was kept above that frame was stale)
                    kept[j].2 = true;       // (one held: a tail call's caller, led to)
                    kept.push((*r, start(*r), true));
                } else if kept.last().is_some_and(|k| k.1.is_none() && k.2) {
                    kept.push((*r, start(*r), true));   // (the frame below is of no known function: not checked)
                } else {
                    kept.push((*r, start(*r), false));   // held: stale, or after a tail call
                }
            }
        }
    }
    kept.into_iter().filter(|k| k.2).map(|k| k.0).collect()
}

/// A crash report's calls (`calls`: [return address, its call's target: "Game.exe+0x..", "frame" when walked by the
/// frame pointers, null for an indirect call], top first) under the faulting address `at`: the frames kept, `at`
/// first, and how many were left out. `start`: where a frame's function starts ("game.exe+0x7ad00"), when known.
pub fn checked_calls(at: &str, calls: &[Value], start: &dyn Fn(&str) -> Option<String>) -> (Vec<String>, usize) {
    let mut texts = vec![at.to_string()];
    let mut pairs = vec![];
    for c in calls {
        let Some(r) = c.get(0).and_then(|x| x.as_str()) else { continue };
        texts.push(r.to_string());
        let t = match c.get(1) {
            Some(Value::String(s)) if s == "frame" => Target::Anchored,
            Some(Value::String(s)) => norm(s).map_or(Target::Unknown, Target::Start),
            _ => Target::Unknown,
        };
        pairs.push((texts.len() - 1, t));
    }
    let starts: Vec<Option<String>> = texts.iter().map(|t| start(t)).collect();
    let kept = kept_frames(0, &pairs, &|i| starts.get(i).cloned().flatten());
    let dropped = pairs.len() + 1 - kept.len();
    (kept.into_iter().map(|i| texts[i].clone()).collect(), dropped)
}

fn norm(t: &str) -> Option<String> {
    let (m, o) = t.split_once("+0x")?;
    Some(format!("{}+0x{:x}", m.to_lowercase(), u32::from_str_radix(o, 16).ok()?))
}

const COUNTERS: &str = "prof.counters' hooks [MafiaModLoader.asi]";

pub fn by_function(report: &Value, focus: Option<&str>) -> Value {
    let addrs: Vec<&str> = report["addresses"].as_array().map(|a| a.iter().map(|x| x.as_str().unwrap_or("?")).collect()).unwrap_or_default();
    let game = |t: &str| {
        let m = t.split("+0x").next().unwrap_or("").to_lowercase();
        m == "game.exe" || m == "ls3df.dll"
    };
    // each address: its function's name, whether it is code of the game's or ours, where its function starts
    let names: Vec<(String, bool)> = addrs.iter().map(|t| {
        let f = crate::syms::function_of(t);
        // the counters' hooks (d3dstats.cpp, while prof.counters is on): one name, and see-through for calls out
        if f.contains("'::Hk<") { return (COUNTERS.to_string(), true); }
        if f != *t || game(t) { (f, true) } else { (t.split("+0x").next().unwrap_or(t).to_string(), false) }
    }).collect();
    let starts: Vec<Option<String>> = addrs.iter().zip(&names).map(|(t, n)| if n.1 { crate::syms::start_of(t) } else { None }).collect();
    let norms: Vec<Option<String>> = addrs.iter().map(|t| norm(t)).collect();
    let start = |i: usize| starts.get(i).cloned().flatten();
    let total_n = report["samples"].as_f64().unwrap_or(0.0).max(1.0);
    let mut stats: HashMap<String, [f64; 3]> = HashMap::new();   // self, out, total
    let (mut found, mut kept_n) = (0.0, 0.0);
    for row in report["chains"].as_array().into_iter().flatten() {
        let Some(r) = row.as_array() else { continue };
        let (Some(c), Some(w)) = (r.first().and_then(|x| x.as_f64()), r.get(1).and_then(|x| x.as_u64())) else { continue };
        let w = w as usize;
        let Some((wname, wcode)) = names.get(w) else { continue };
        let pairs: Vec<(usize, Target)> = r[2..].chunks(2).filter_map(|p| {
            let a = p.first()?.as_u64()? as usize;
            let t = match p.get(1).and_then(|x| x.as_u64()) {
                Some(x) if x == u64::from(u32::MAX - 1) => Target::Anchored,
                Some(x) if x != u64::from(u32::MAX) => norms.get(x as usize).cloned().flatten().map_or(Target::Unknown, Target::Start),
                _ => Target::Unknown,
            };
            Some((a, t))
        }).collect();
        let kept = kept_frames(w, &pairs, &start);
        found += c * pairs.len() as f64;
        kept_n += c * (kept.len() - 1) as f64;
        stats.entry(wname.clone()).or_default()[0] += c;
        if !wcode {
            // the first frame of the game's or ours above the module the thread is in, past the counters' hooks
            if let Some(n) = kept.iter().skip(1).filter_map(|&i| names.get(i)).find(|n| n.0 != COUNTERS) {
                stats.entry(n.0.clone()).or_default()[1] += c;
            }
        }
        let set: HashSet<&String> = kept.iter().filter_map(|&i| names.get(i).map(|n| &n.0)).collect();
        for n in set { stats.entry(n.clone()).or_default()[2] += c; }
    }
    let pct = |x: f64| (x / total_n * 1000.0).round() / 10.0;
    let mut v: Vec<(String, [f64; 3])> = stats.into_iter().collect();
    let row = |(n, s): &(String, [f64; 3])| json!({"function": n, "self": pct(s[0]), "calls_out": pct(s[1]), "total": pct(s[2])});
    v.sort_by(|a, b| b.1[2].partial_cmp(&a.1[2]).unwrap_or(std::cmp::Ordering::Equal));
    let by_total: Vec<Value> = v.iter().take(60).map(row).collect();
    let focused: Option<Vec<Value>> = focus.filter(|f| !f.is_empty()).map(|f| {
        let f = f.to_lowercase();
        v.iter().filter(|(n, _)| n.to_lowercase().contains(&f)).take(200).map(row).collect()
    });
    v.sort_by(|a, b| (b.1[0] + b.1[1]).partial_cmp(&(a.1[0] + a.1[1])).unwrap_or(std::cmp::Ordering::Equal));
    let by_self: Vec<Value> = v.iter().take(40).map(row).collect();
    let mut out = json!({"by_total": by_total, "by_self": by_self,
           "stale_left_out_percent": if found > 0.0 { ((found - kept_n) / found * 1000.0).round() / 10.0 } else { 0.0 },
           "note": "% of the samples: self, the thread in the function; calls_out, in a system or driver module the \
                    function called itself (Direct3D, the driver: its draw calls' cost); total, the function anywhere on \
                    the stack, once a sample. A return address whose direct call leads elsewhere than the frame below \
                    is a stale one, left out (stale_left_out_percent of those found); one of an indirect call is kept"});
    if let Some(f) = focused { out["focus"] = json!(f); }
    out
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn a_stale_return_address_is_left_out_and_a_checked_chain_kept() {
        // the thread in F (starts at "f"); on its stack: a stale address (its call went to "x"), then G's call of F,
        // then an indirect call (kept), then H's call of G... behind the indirect one: H called "i" (the indirect one's
        // function), so the chain stands
        let starts = ["f", "s", "g", "i", "h"];
        let start = |i: usize| Some(starts[i].to_string());
        let pairs = vec![(1, Target::Start("x".into())), (2, Target::Start("f".into())), (3, Target::Unknown), (4, Target::Start("i".into()))];
        assert_eq!(kept_frames(0, &pairs, &start), vec![0, 2, 3, 4]);
    }

    #[test]
    fn frames_kept_above_a_matched_one_are_taken_out() {
        // in a driver (no start): an indirect caller kept, a stale indirect one kept, then a direct call to the first
        // one's function: the stale one goes
        let starts = [None, Some("a"), Some("s"), Some("b")];
        let start = |i: usize| starts[i].map(String::from);
        let pairs = vec![(1, Target::Unknown), (2, Target::Unknown), (3, Target::Start("a".into()))];
        assert_eq!(kept_frames(0, &pairs, &start), vec![0, 1, 3]);
    }

    #[test]
    fn a_tail_calls_caller_is_kept_once_a_frame_above_leads_to_it() {
        // the thread in B; A's caller C called A, which jumped to B (no frame of A's): C's return address leads to
        // no kept frame, held; D's call of C leads to it: both kept. A stale one held at the top is left out.
        let starts = ["b", "c", "s", "d", "t"];
        let start = |i: usize| Some(starts[i].to_string());
        let pairs = vec![(1, Target::Start("a".into())), (2, Target::Start("x".into())), (3, Target::Start("c".into())),
                         (4, Target::Start("y".into()))];
        assert_eq!(kept_frames(0, &pairs, &start), vec![0, 1, 3]);
    }

    #[test]
    fn a_crash_reports_calls_checked() {
        // harness_crash (0x1000) <- main (0x2000): a stale return address of main's earlier call of pump (0x3000) in
        // between, an indirect call above, then the thread's start walked by its frame pointers
        let start = |t: &str| match t.split("+0x").nth(1).and_then(|o| u32::from_str_radix(o, 16).ok()) {
            Some(o) if t.starts_with("h.exe") => Some(format!("h.exe+0x{:x}", o & !0xFFF)),
            _ => None,
        };
        let calls: Vec<Value> = serde_json::from_str(
            r#"[["h.exe+0x2040", "h.exe+0x3000"], ["h.exe+0x2080", "h.exe+0x1000"], ["h.exe+0x4010", null], ["kernel32.dll+0x10", "frame"]]"#).unwrap();
        let (kept, dropped) = checked_calls("h.exe+0x1008", &calls, &start);
        assert_eq!(kept, vec!["h.exe+0x1008", "h.exe+0x2080", "h.exe+0x4010", "kernel32.dll+0x10"]);
        assert_eq!(dropped, 1);
    }
}
