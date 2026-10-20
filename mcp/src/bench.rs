//! `bench`: two configurations of the running game compared in one run, without the drifts of a run between them.
//!
//! The configurations take turns in blocks (ABBA, then BAAB: a drift through the run falls on both alike), each turn a
//! segment of frames labelled in the game (prof.mark: its first frames let pass while the switch settles) and read
//! back as each frame's record (prof.frames). A segment's figure is its median (a hitch does not move it); a block
//! gives one difference, B's segments less A's; the difference is the mean over the blocks, with a 95 % interval from
//! their spread (Student's t). The two segments of one configuration in a block tell the noise of the measure itself:
//! the same configuration measured twice. With no B the run compares A with itself: a difference inside its interval
//! says the measure is sound.

use serde_json::{Value, json};
use std::collections::BTreeMap;

/// One frame's figures by name: the times, and while counted the device's calls and the host's sites.
pub type Frame = BTreeMap<String, f64>;

/// The frames of a prof.frames reply whose label is `label`'s index, each made a Frame.
pub fn frames_of(reply: &Value, label: &str) -> Vec<Frame> {
    let Some(idx) = reply["labels"].as_array().and_then(|l| l.iter().position(|x| x.as_str() == Some(label))) else {
        return vec![];
    };
    let f = &reply["frames"];
    let labels = f["label"].as_array().cloned().unwrap_or_default();
    let col = |v: &Value, i: usize| v.as_array().and_then(|a| a.get(i)).and_then(|x| x.as_f64());
    let mut out = vec![];
    for (i, l) in labels.iter().enumerate() {
        if l.as_u64() != Some(idx as u64) { continue; }
        let flags = col(&f["flags"], i).unwrap_or(0.0) as u32;
        if flags & 1 != 0 { continue; }   // (settling)
        let mut fr = Frame::new();
        for name in ["wall_ms", "present_ms", "present_cpu_ms", "cpu_ms", "gpu_ms"] {
            if let Some(v) = col(&f[name], i) { fr.insert(name.into(), v); }
        }
        fr.insert("flags".into(), flags as f64);
        if let (Some(w), Some(p)) = (fr.get("wall_ms").copied(), fr.get("present_ms").copied()) {
            fr.insert("work_ms".into(), w - p);
            if let (Some(c), Some(pc)) = (fr.get("cpu_ms").copied(), fr.get("present_cpu_ms").copied()) {
                fr.insert("off_cpu_ms".into(), ((w - p) - (c - pc)).max(0.0));
            }
        }
        if let Some(h) = f["hot_ms"].as_object() {
            for (k, v) in h { if let Some(x) = col(v, i) { fr.insert(format!("hot.{k}"), x); } }
        }
        if let Some(d) = f["d3d"].as_object() {
            for (kind, v) in d {
                match kind.as_str() {
                    "switches" => { if let Some(x) = col(v, i) { fr.insert("d3d.switches".into(), x); } }
                    "primitives" | "driver_ms" | "driver_draws" => {
                        for (who, c) in v.as_object().into_iter().flatten() {
                            if let Some(x) = col(c, i) { fr.insert(format!("d3d.{kind}.{who}"), x); }
                        }
                    }
                    _ => {
                        for (who, c) in v.as_object().into_iter().flatten() {
                            if let Some(x) = col(&c["calls"], i) { fr.insert(format!("d3d.{kind}.{who}.calls"), x); }
                            if let Some(x) = col(&c["ms"], i) { fr.insert(format!("d3d.{kind}.{who}.ms"), x); }
                        }
                    }
                }
            }
        }
        out.push(fr);
    }
    out
}

/// The times compared by their medians (the rest by their means: counts, steady from frame to frame).
pub const MAIN: [&str; 7] = ["wall_ms", "work_ms", "cpu_ms", "off_cpu_ms", "present_ms", "present_cpu_ms", "gpu_ms"];

pub fn median(v: &mut [f64]) -> Option<f64> {
    if v.is_empty() { return None; }
    v.sort_by(|a, b| a.partial_cmp(b).unwrap_or(std::cmp::Ordering::Equal));
    let n = v.len();
    Some(if n % 2 == 1 { v[n / 2] } else { (v[n / 2 - 1] + v[n / 2]) / 2.0 })
}

/// A segment's figure for each name: the median of a time, the mean of the rest.
pub fn segment_stats(frames: &[Frame]) -> BTreeMap<String, f64> {
    let mut names: Vec<&String> = frames.iter().flat_map(|f| f.keys()).collect();
    names.sort();
    names.dedup();
    let mut out = BTreeMap::new();
    for n in names {
        if n == "flags" { continue; }
        let mut v: Vec<f64> = frames.iter().filter_map(|f| f.get(n).copied()).collect();
        let x = if MAIN.contains(&n.as_str()) { median(&mut v) } else if v.is_empty() { None } else { Some(v.iter().sum::<f64>() / v.len() as f64) };
        if let Some(x) = x { out.insert(n.clone(), x); }
    }
    out
}

/// Student's t at 97.5 % for `df` degrees of freedom.
pub fn t975(df: usize) -> f64 {
    const T: [f64; 30] = [12.706, 4.303, 3.182, 2.776, 2.571, 2.447, 2.365, 2.306, 2.262, 2.228, 2.201, 2.179, 2.160, 2.145,
                          2.131, 2.120, 2.110, 2.101, 2.093, 2.086, 2.080, 2.074, 2.069, 2.064, 2.060, 2.056, 2.052, 2.048,
                          2.045, 2.042];
    if df == 0 { f64::INFINITY } else if df <= 30 { T[df - 1] } else { 1.96 }
}

/// A segment measured: its configuration (0 A, 1 B), its block and its figures.
pub struct Segment {
    pub config: usize,
    pub block: usize,
    pub frames: usize,
    pub stats: BTreeMap<String, f64>,
}

fn round(x: f64) -> f64 { (x * 1000.0).round() / 1000.0 }

/// The comparison: each configuration's figures, B less A with its interval, the noise of the measure.
pub fn compare(segs: &[Segment], names: [&str; 2]) -> Value {
    let blocks = segs.iter().map(|s| s.block + 1).max().unwrap_or(0);
    let mut all: Vec<&String> = segs.iter().flat_map(|s| s.stats.keys()).collect();
    all.sort();
    all.dedup();
    let mean = |v: &[f64]| if v.is_empty() { None } else { Some(v.iter().sum::<f64>() / v.len() as f64) };
    let mut configs = [BTreeMap::<String, Value>::new(), BTreeMap::new()];
    let mut diffs = BTreeMap::<String, Value>::new();
    for name in all {
        // per block: each configuration's mean of its segments
        let mut d = vec![];
        let mut within = vec![];
        let mut per = [vec![], vec![]];
        for b in 0..blocks {
            let mut by = [vec![], vec![]];
            for s in segs.iter().filter(|s| s.block == b) {
                if let Some(x) = s.stats.get(name) { by[s.config].push(*x); }
            }
            for c in 0..2 {
                per[c].extend(by[c].iter().copied());
                if by[c].len() == 2 { within.push((by[c][0] - by[c][1]).powi(2) / 2.0); }
            }
            if let (Some(a), Some(bb)) = (mean(&by[0]), mean(&by[1])) { d.push(bb - a); }
        }
        let main = MAIN.contains(&name.as_str());
        for c in 0..2 {
            let mut v = per[c].clone();
            let x = if main { median(&mut v) } else { mean(&v) };
            if let Some(x) = x { configs[c].insert(name.clone(), json!(round(x))); }
        }
        let Some(m) = mean(&d) else { continue };
        let a = configs[0].get(name).and_then(|v| v.as_f64()).unwrap_or(0.0);
        if !main && m.abs() < 1e-9 && a.abs() < 1e-9 { continue; }
        let n = d.len();
        let sd = if n > 1 { (d.iter().map(|x| (x - m).powi(2)).sum::<f64>() / (n - 1) as f64).sqrt() } else { f64::NAN };
        let ci = t975(n.saturating_sub(1)) * sd / (n as f64).sqrt();
        let noise = mean(&within).map(f64::sqrt);
        let mut e = json!({"b_minus_a": round(m), "ci95": if ci.is_finite() { json!(round(ci)) } else { json!(null) },
                           "blocks": n, "significant": ci.is_finite() && m.abs() > ci});
        if a.abs() > 1e-9 { e["percent"] = json!((m / a * 1000.0).round() / 10.0); }
        if let Some(nz) = noise { e["noise"] = json!(round(nz)); }
        diffs.insert(name.clone(), e);
    }
    json!({
        "configs": {names[0]: configs[0], names[1]: configs[1]},
        "difference": diffs,
        "note": "a segment's times are its median, its counts their mean; b_minus_a: the mean over the blocks of B's \
                 segments less A's, ci95 its 95 % interval (significant: outside it); noise: the spread of one \
                 configuration measured twice in a block (the measure's own)"
    })
}

#[cfg(test)]
mod tests {
    use super::*;

    fn seg(config: usize, block: usize, wall: f64) -> Segment {
        Segment { config, block, frames: 100, stats: BTreeMap::from([("wall_ms".to_string(), wall)]) }
    }

    #[test]
    fn a_steady_difference_is_found_and_a_drift_cancels() {
        // B costs 1 ms more; the run drifts by 0.1 ms a segment (ABBA, BAAB cancel it)
        let order = [0, 1, 1, 0, 1, 0, 0, 1, 0, 1, 1, 0];
        let segs: Vec<Segment> = order.iter().enumerate().map(|(i, &c)| seg(c, i / 4, 5.0 + c as f64 + 0.1 * i as f64)).collect();
        let r = compare(&segs, ["A", "B"]);
        let d = &r["difference"]["wall_ms"];
        assert!((d["b_minus_a"].as_f64().unwrap() - 1.0).abs() < 1e-6, "{d}");
        assert_eq!(d["significant"], json!(true));
        assert_eq!(d["blocks"], json!(3));
    }

    #[test]
    fn the_same_configuration_twice_is_no_difference() {
        let walls = [5.0, 5.2, 4.9, 5.1, 5.05, 4.95, 5.1, 5.0];
        let segs: Vec<Segment> = walls.iter().enumerate().map(|(i, &w)| seg([0, 1, 1, 0][i % 4], i / 4, w)).collect();
        let r = compare(&segs, ["A", "A'"]);
        assert_eq!(r["difference"]["wall_ms"]["significant"], json!(false), "{r}");
    }

    #[test]
    fn frames_of_takes_the_label_without_its_settling_frames() {
        let reply = json!({"labels": ["", "x"], "frames": {
            "seq": [1, 2, 3], "label": [1, 1, 0], "flags": [1, 0, 0], "wall_ms": [9.0, 3.0, 4.0], "present_ms": [1.0, 1.0, 1.0],
            "present_cpu_ms": [0.5, 0.5, 0.5], "cpu_ms": [2.0, 1.5, 2.0], "gpu_ms": [null, 0.7, null],
            "d3d": {"draw_indexed": {"game": {"calls": [400, 401, 402], "ms": [0.2, 0.2, 0.2]}}, "switches": [0, 3, 0]}}});
        let f = frames_of(&reply, "x");
        assert_eq!(f.len(), 1);
        assert_eq!(f[0]["work_ms"], 2.0);
        assert_eq!(f[0]["off_cpu_ms"], 1.0);   // 2 ms outside Present, 1 ms of it running
        assert_eq!(f[0]["d3d.draw_indexed.game.calls"], 401.0);
        assert_eq!(f[0]["d3d.switches"], 3.0);
        assert_eq!(f[0]["gpu_ms"], 0.7);
    }
}

// ---- the run's conditions: what else ran on the machine meanwhile
use windows_sys::Win32::Foundation::{CloseHandle, FILETIME, INVALID_HANDLE_VALUE};
use windows_sys::Win32::System::Diagnostics::ToolHelp::{
    CreateToolhelp32Snapshot, PROCESSENTRY32W, Process32FirstW, Process32NextW, TH32CS_SNAPPROCESS,
};
use windows_sys::Win32::System::Threading::{GetProcessTimes, GetSystemTimes, OpenProcess, PROCESS_QUERY_LIMITED_INFORMATION};

fn ft(f: &FILETIME) -> u64 { (u64::from(f.dwHighDateTime) << 32) | u64::from(f.dwLowDateTime) }

/// The machine's busy time and the game's own, in 100 ns units, at a moment.
pub struct CpuMark { busy: u64, total: u64, game: u64 }

pub fn cpu_mark(pid: Option<u32>) -> CpuMark {
    let z = FILETIME { dwLowDateTime: 0, dwHighDateTime: 0 };
    let (mut idle, mut kernel, mut user) = (z, z, z);
    let mut game = 0;
    unsafe {
        GetSystemTimes(&mut idle, &mut kernel, &mut user);
        if let Some(pid) = pid {
            let h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, 0, pid);
            if !h.is_null() {
                let (mut c, mut e, mut k, mut u) = (z, z, z, z);
                if GetProcessTimes(h, &mut c, &mut e, &mut k, &mut u) != 0 { game = ft(&k) + ft(&u); }
                CloseHandle(h);
            }
        }
    }
    // (the kernel's time holds the idle time)
    let total = ft(&kernel) + ft(&user);
    CpuMark { busy: total - ft(&idle), total, game }
}

/// Between two marks: how busy the machine was, the game's share and everything else's, in % of all its cores.
pub fn cpu_between(a: &CpuMark, b: &CpuMark) -> Value {
    let total = b.total.saturating_sub(a.total).max(1) as f64;
    let busy = b.busy.saturating_sub(a.busy) as f64;
    let game = b.game.saturating_sub(a.game) as f64;
    let pct = |x: f64| (x / total * 1000.0).round() / 10.0;
    json!({"machine_busy_percent": pct(busy), "game_percent": pct(game), "others_percent": pct((busy - game).max(0.0)),
           "cores": std::thread::available_parallelism().map(|n| n.get()).unwrap_or(1)})
}

/// Every Game.exe running on the machine (the labs' and a player's own): their pids.
pub fn game_processes() -> Vec<u32> {
    let mut out = vec![];
    unsafe {
        let snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
        if snap == INVALID_HANDLE_VALUE { return out; }
        let mut e: PROCESSENTRY32W = std::mem::zeroed();
        e.dwSize = std::mem::size_of::<PROCESSENTRY32W>() as u32;
        let mut ok = Process32FirstW(snap, &mut e) != 0;
        while ok {
            let len = e.szExeFile.iter().position(|&c| c == 0).unwrap_or(e.szExeFile.len());
            if String::from_utf16_lossy(&e.szExeFile[..len]).eq_ignore_ascii_case("Game.exe") { out.push(e.th32ProcessID); }
            ok = Process32NextW(snap, &mut e) != 0;
        }
        CloseHandle(snap);
    }
    out
}
