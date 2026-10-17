//! Test mod: the Rust mod kit (sdk/rust/mml) with no game (tests/run_tests.py). `kit_rs.status` tells what it saw:
//! its loads (kept across reloads as bytes), frames (as JSON), calls posted from another thread, its settings.
//! `kit_rs.add {"a", "b"}` takes typed params, `kit_rs.fail` replies an error, `kit_rs.panic` panics (logged, the
//! mod faulted and unloaded as a C mod's fault is), `kit_rs.post` posts from a thread of its own, `kit_rs.timer`
//! adds a timer and removes it (its closure freed), and asks for one of 0 ms (refused). At its unload a value it
//! held logs its drop.
mod checks;

use std::cell::Cell;
use std::rc::Rc;
use std::sync::atomic::{AtomicU32, Ordering};

use mml::{EventType, Host};
use serde::{Deserialize, Serialize};

static POSTED: AtomicU32 = AtomicU32::new(0);

/// Held by a call posted at the unload, which never runs: its drop is logged when the unload frees it
struct PostedAtUnload;
impl Drop for PostedAtUnload {
    fn drop(&mut self) {
        mml::info!(mml::host(), "kit_rs: a call posted at the unload dropped unrun");
    }
}

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

    host.command("offline", "the safe layer's checks with no game (run_tests.py)", move |_| Ok(checks::offline(host).to_string()));
    let live = checks::Live {
        particles: Rc::new(Cell::new(0)),
        vehicles: Rc::new(Cell::new(0)),
        exhausts: Rc::new(Cell::new(0)),
        captured: Rc::new(Cell::new(-1)),
    };
    host.command("live", "the safe layer's checks in a running freeride (twice: the second sees the first's scans)", move |_| {
        Ok(checks::live(host, &live).to_string())
    });

    // a scene view and a pass that sees it (the bridge on: a mod with "gfx": true started with the game)
    let seen = Rc::new(Cell::new((0u32, 0u32, 0u32)));   // (passes run, views seen in the last, its frames drawn)
    let view = Rc::new(Cell::new(0));
    let (sn, vw) = (seen.clone(), view.clone());
    host.command("view", "a scene view (the game camera's, 256x128) and a pass reading it; again: what the pass saw", move |_| {
        if vw.get() == 0 {
            let v = host.scene_view(256, 128, |cam| {
                cam.world[13] += 10.0; // ten metres above the game camera
                true
            });
            let s = sn.clone();
            let p = host.gfx_pass(500, move |f| {
                let me = f.views().iter().find(|v| v.width == 256 && v.height == 128);
                let (n, _, _) = s.get();
                s.set((n + 1, f.views().len() as u32, me.map_or(0, |v| v.frames)));
            });
            vw.set(v.unwrap_or(-1));
            return Ok(format!("{{\"view\":{},\"pass\":{}}}", v.unwrap_or(0), p.unwrap_or(0)));
        }
        let (n, views, drawn) = sn.get();
        Ok(format!("{{\"view\":{},\"passes\":{n},\"views\":{views},\"drawn\":{drawn}}}", vw.get()))
    });
    host.command("page", "a page of the game's controls over the pause menu (its title, a text, a check box, Back)", move |_| {
        use mml::MenuControl as C;
        let mut check = C::new(3, "chck", 30.0, 90.0, 240.0, 25.0, Some("A check box from Rust"));
        check.flags = 0;
        let controls = [
            C::new(1, "wind", 250.0, 150.0, 300.0, 220.0, Some("kit_rs: a page")),
            C::new(2, "stxt", 30.0, 50.0, 240.0, 25.0, Some("Made by a mod in Rust")),
            check,
            C::new(4, "butt", 75.0, 160.0, 150.0, 30.0, Some("Back")),
        ];
        let ok = host.menu_open("page", &controls, move |id| {
            if id == 4 {
                host.menu_close();
            }
        });
        Ok(ok.to_string())
    });

    let f = frames.clone();
    // the game's own Direct3D 9 device in RENDER_WORLD (typed by the feature "windows"): its viewport read and a
    // magenta square cleared into the world's picture, under the HUD
    let d9 = Rc::new(Cell::new((false, 0u32, 0u32, 0u32))); // (on, frames, viewport width, height)
    let d = d9.clone();
    host.subscribe(EventType::RENDER_WORLD, move |ev| {
        let (on, n, _, _) = d.get();
        if !on || !ev.world() {
            return;
        }
        let Some(dev) = host.d3d9_device() else { return };
        let mut vp = mml::windows::Win32::Graphics::Direct3D9::D3DVIEWPORT9::default();
        unsafe {
            if dev.GetViewport(&mut vp).is_err() {
                return;
            }
            let r = mml::windows::Win32::Graphics::Direct3D9::D3DRECT { x1: 20, y1: 140, x2: 120, y2: 240 };
            let _ = dev.Clear(1, &r, mml::windows::Win32::Graphics::Direct3D9::D3DCLEAR_TARGET as u32, 0xFFFF00FF, 1.0, 0);
        }
        d.set((true, n + 1, vp.Width, vp.Height));
    });
    let d = d9.clone();
    host.command("d3d9", "the game's Direct3D 9 device in RENDER_WORLD: {\"on\": true} draws a square; what it saw", move |args| {
        let (on, n, w, h) = d.get();
        let on = if args.contains("true") { true } else if args.contains("false") { false } else { on };
        d.set((on, n, w, h));
        Ok(format!("{{\"on\":{on},\"frames\":{n},\"viewport\":[{w},{h}],\"device8\":{}}}", !host.render_device(8).is_null()))
    });

    // an item of the pause menu, moved by MenuItem::set: under Options (108) first, then under Load Game (103)
    let clicks = Rc::new(Cell::new(0u32));
    let item = Rc::new(std::cell::RefCell::new(None::<mml::MenuItem>));
    let (cl, it) = (clicks.clone(), item.clone());
    host.command("item", "a pause menu item ({\"move\": true}: under Load Game, renamed); its clicks", move |args| {
        if it.borrow().is_none() {
            let c = cl.clone();
            *it.borrow_mut() = host.menu_item("gamemenu", 108, "kit_rs: under Options", move |_| c.set(c.get() + 1));
        }
        let moved = args.contains("true") && it.borrow().as_ref().is_some_and(|i| i.set(103, "kit_rs: moved"));
        Ok(format!("{{\"handle\":{},\"moved\":{moved},\"clicks\":{}}}", it.borrow().as_ref().map_or(0, |i| i.handle()), cl.get()))
    });

    let poster = host.poster();
    host.on_unload(move || {
        host.state_put("loads", &loads.to_le_bytes());
        host.state_put_json("frames", &f.get());
        let held = PostedAtUnload;
        poster.post(move |h| mml::error!(h, "kit_rs: a call posted at the unload ran ({})", core::mem::size_of_val(&held)));
    });
    mml::info!(host, "kit_rs loaded (load {loads})");
    Ok(())
}

mml::entry!(load);
