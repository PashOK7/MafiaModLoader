//! hello_rust: a native mod in Rust, through the `mml` crate (sdk/rust/mml).
//!
//! What hello_c does, and a little of the world: it counts frames and mission ticks and keeps the counts across its
//! own hot reloads; it draws a line at the top left (the mission, the player's place, health and money); a hotkey
//! from its settings writes where the player is in the game's console; and it answers commands over the bridge:
//! `hello_rust.status`, `hello_rust.say {"text"}`, `hello_rust.spawn {"car"}` (a car of the game's list made 6 m in
//! front of the player). Rebuild it while the game runs and `deploy`: the counts go on from where they were.
use std::cell::{Cell, RefCell};
use std::rc::Rc;

use mml::{Align, EventType, Font, Host};
use serde::{Deserialize, Serialize};

/// What survives a reload (state_put_json at the unload, state_get_json at the load).
#[derive(Default, Serialize, Deserialize)]
struct Counts {
    frames: u64,
    ticks: u64,
    loads: u32,
}

struct State {
    counts: RefCell<Counts>,
    presses: Cell<u32>,
}

#[derive(Serialize)]
struct Status {
    frames: u64,
    mission_ticks: u64,
    loads: u32,
    presses: u32,
    game: String,
    mission: String,
}

#[derive(Deserialize)]
struct Say {
    text: String,
}

#[derive(Deserialize, Default)]
struct Spawn {
    car: Option<String>,
}

fn place(host: Host) -> Option<[f32; 3]> {
    host.position(host.player()?)
}

fn load(host: Host) -> Result<(), String> {
    let mut counts: Counts = host.state_get_json("counts").unwrap_or_default();
    counts.loads += 1;
    let s = Rc::new(State { counts: RefCell::new(counts), presses: Cell::new(0) });

    // every frame (the menus and the pause too): count, and draw while a mission is open
    let st = s.clone();
    host.subscribe(EventType::FRAME, move |_| {
        st.counts.borrow_mut().frames += 1;
        if host.config_number("show", 1.0) == 0.0 || host.mission_name().is_empty() {
            return;
        }
        let mut line = format!("hello_rust: {}", host.mission_name());
        if let (Some(p), Some(me)) = (place(host), host.player()) {
            line += &format!("  at {:.0} {:.0} {:.0}", p[0], p[1], p[2]);
            if let Some((hp, _)) = host.health(me) {
                line += &format!("  health {hp:.0}");
            }
        }
        if let Some(m) = host.money() {
            line += &format!("  ${m}");
        }
        let w = host.text_width(Font::SMALL, 14.0, &line);
        host.draw_rect(8.0, 8.0, w + 12.0, 20.0, 0xA000_0000, true);
        host.draw_text(14.0, 10.0, 14.0, 0xFFFF_FFFF, Font::SMALL, Align::Left, &line);
    });
    // the mission's world moved
    let st = s.clone();
    host.subscribe(EventType::MISSION_TICK, move |_| st.counts.borrow_mut().ticks += 1);

    // a hotkey from the settings, bound again when they change
    let st = s.clone();
    let key = host
        .hotkey(&host.config_string("hotkey", "Ctrl+F9"), move || {
            st.presses.set(st.presses.get() + 1);
            let line = match place(host) {
                Some(p) => format!("Tommy is at {:.1}, {:.1}, {:.1}", p[0], p[1], p[2]),
                None => "no player here".to_owned(),
            };
            host.hud_console(&line, 0xFFFF_FF80);
        })
        .ok_or("the hotkey setting is not a key")?;
    host.subscribe(EventType::CONFIG_CHANGED, move |_| {
        if !key.rebind(&host.config_string("hotkey", "Ctrl+F9")) {
            mml::warn!(host, "hotkey {:?} not understood", host.config_string("hotkey", ""));
        }
    });

    // commands: hello_rust.status, .say, .spawn
    let st = s.clone();
    host.command_json("status", "frames and mission ticks seen, loads of this mod, hotkey presses", move |()| {
        let c = st.counts.borrow();
        Ok(Status {
            frames: c.frames,
            mission_ticks: c.ticks,
            loads: c.loads,
            presses: st.presses.get(),
            game: host.game_build(),
            mission: host.mission_name(),
        })
    });
    host.command_json("say", "a line in the log and the game's console (params: {\"text\": \"...\"})", move |a: Say| {
        mml::info!(host, "hello_rust says: {}", a.text);
        Ok(host.hud_console(&a.text, 0xFFFF_FFFF))
    });
    host.command_json("spawn", "a car 6 m in front of the player (params: {\"car\": name}; the settings' car)", move |a: Spawn| {
        let me = host.player().ok_or("no player: no mission")?;
        let m = host.frame_world(host.actor_frame(me).ok_or("the player has no model")?).ok_or("no matrix")?;
        let (fwd, at) = ([m[8], 0.0, m[10]], [m[12], m[13], m[14]]);
        let to = [at[0] + fwd[0] * 6.0, at[1] + 0.5, at[2] + fwd[2] * 6.0];
        let name = a.car.unwrap_or_else(|| host.config_string("car", "Bolt-Thrower"));
        let car = host.spawn_car(&name, to, Some([fwd[2], 0.0, -fwd[0]])).ok_or(format!("no car {name:?} (the log says why)"))?;
        Ok(format!("{:p}", car.as_ptr()))
    });

    let st = s.clone();
    host.on_unload(move || {
        host.state_put_json("counts", &*st.counts.borrow());
    });
    let c = s.counts.borrow();
    mml::info!(host, "hello from Rust (load {}, game {}, mod folder {})", c.loads, host.game_build(), host.dir());
    Ok(())
}

mml::entry!(load);
