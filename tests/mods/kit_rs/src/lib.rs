//! Test mod: the Rust mod kit (sdk/rust/mml) with no game (tests/run_tests.py). `kit_rs.status` tells what it saw:
//! its loads (kept across reloads as bytes), frames (as JSON), calls posted from another thread, its settings.
//! `kit_rs.add {"a", "b"}` takes typed params, `kit_rs.fail` replies an error, `kit_rs.panic` panics (logged, the
//! mod faulted and unloaded as a C mod's fault is), `kit_rs.post` posts from a thread of its own, `kit_rs.timer`
//! adds a timer and removes it (its closure freed), and asks for one of 0 ms (refused). At its unload a value it
//! held logs its drop.
use std::cell::Cell;
use std::rc::Rc;
use std::sync::atomic::{AtomicU32, Ordering};

use mml::{EventType, Host};
use serde::{Deserialize, Serialize};

static POSTED: AtomicU32 = AtomicU32::new(0);

struct Dropped(Host);
impl Drop for Dropped {
    fn drop(&mut self) {
        mml::info!(self.0, "kit_rs: its closures dropped");
    }
}

#[derive(Deserialize)]
struct Add {
    a: i64,
    b: i64,
}

#[derive(Serialize)]
struct Status {
    loads: u32,
    frames: u64,
    posted: u32,
    name: String,
    speed: f64,
    missing: f64,
    build: String,
    id: String,
    dir_ends: bool,
    mission: String,
}

fn load(host: Host) -> Result<(), String> {
    let loads = host.state_get("loads").map_or(0, |b| u32::from_le_bytes(b.try_into().unwrap_or([0; 4]))) + 1;
    let frames = Rc::new(Cell::new(host.state_get_json::<u64>("frames").unwrap_or(0)));
    let guard = Rc::new(Dropped(host));

    let f = frames.clone();
    host.subscribe(EventType::FRAME, move |ev| f.set(f.get() + (ev.dt() >= 0.0) as u64));
    let f = frames.clone();
    host.command_json("status", "what it saw", move |()| {
        let _ = &guard;
        Ok(Status {
            loads,
            frames: f.get(),
            posted: POSTED.load(Ordering::SeqCst),
            name: host.config_string("name", "?"),
            speed: host.config_number("speed", -1.0),
            missing: host.config_number("nothing.here", -7.0),
            build: host.game_build(),
            id: host.id(),
            dir_ends: host.dir().ends_with("\\kit_rs\\"),
            mission: host.mission_name(),
        })
    });
    host.command_json("add", "a + b (params: {a, b})", |p: Add| Ok(p.a + p.b));
    host.command("fail", "an error reply", |_| Err("refused on purpose".to_owned()));
    host.command("panic", "panics", |_| -> Result<String, String> { panic!("kit_rs panics on purpose") });
    let poster = host.poster();
    host.command("post", "a call posted from another thread", move |_| {
        std::thread::spawn(move || poster.post(|_| {
            POSTED.fetch_add(1, Ordering::SeqCst);
        }))
        .join()
        .map_err(|_| "the thread panicked".to_owned())?;
        Ok("true".to_owned())
    });
    host.command("timer", "a timer added and removed; one of 0 ms refused", move |_| {
        let id = host.timer_once(10, |_| ()).unwrap_or(0);
        let removed = id != 0 && host.interact_remove(id);
        let zero = host.timer_once(0, |_| ()).is_none();
        Ok(format!("{{\"id\":{id},\"removed\":{removed},\"zero_refused\":{zero}}}"))
    });

    let f = frames.clone();
    host.on_unload(move || {
        host.state_put("loads", &loads.to_le_bytes());
        host.state_put_json("frames", &f.get());
    });
    mml::info!(host, "kit_rs loaded (load {loads})");
    Ok(())
}

mml::entry!(load);
