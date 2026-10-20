//! kit_rs's checks of the whole safe layer: `offline` with no game (the harness: refusals, round trips, what needs no
//! game), `live` in a running freeride (each area of the host and mml::game against the host's own answers). Each
//! check is a name and {ok, info}; tests/run_tests.py reads the offline ones, an agent the live ones in both labs.
use std::cell::Cell;
use std::rc::Rc;

use mml::game::Game;
use mml::serde_json::{json, Map, Value};
use mml::{sys, Host, Marker, Place, ShaderRule};

pub struct Checks(Map<String, Value>);
impl Checks {
    fn new() -> Checks {
        Checks(Map::new())
    }
    fn check(&mut self, name: &str, ok: bool, info: Value) {
        self.0.insert(name.to_owned(), json!({ "ok": ok, "info": info }));
    }
    fn done(self) -> Value {
        let failed: Vec<&String> = self.0.iter().filter(|(_, v)| v["ok"] != true).map(|(k, _)| k).collect();
        json!({ "failed": failed, "checks": self.0 })
    }
}

/// A table a service publishes (C layout)
#[repr(C)]
pub struct Math {
    pub add: extern "C" fn(i32, i32) -> i32,
}
extern "C" fn add(a: i32, b: i32) -> i32 {
    a + b
}
pub static MATH: Math = Math { add };

pub fn offline(host: Host) -> Value {
    let mut c = Checks::new();
    c.check("game: no build known with no game", Game::detect(host).is_none(), json!(null));
    c.check("game: the tables (1.2's missionPtr, a layout constant)", mml::game::EXE_12.missionPtr == 0x63788C && mml::game::kActorFrame == 0x68,
        json!(mml::game::EXE_10.missionPtr));
    let ps: &[u8] = mml::include_shader!("PsTiny");
    c.check("mml-build: the shader's bytecode (DXBC)", ps.len() > 100 && &ps[..4] == b"DXBC", json!(ps.len()));
    let ok = unsafe { host.service_publish("test.rust_math", 3, &MATH) };
    let found = unsafe { host.service_find::<Math>("test.rust_math", 2) };
    c.check("services: published and found (its version), called", ok && found.is_some_and(|(m, v)| v == 3 && (m.add)(2, 40) == 42), json!(null));
    c.check("services: a newer version than published: none", unsafe { host.service_find::<Math>("test.rust_math", 4) }.is_none(), json!(null));
    c.check("game_files: the harness's archive listed", host.game_files(None, |_, _| false) >= 1, json!(host.game_files(None, |_, _| false)));
    // (files_tests has left files_b's file over the archive's archived.txt: flags 0 read it, MML_FILE_GAME the game's)
    let (file, own) = (host.file_read("archived.txt", 0), host.file_read("archived.txt", sys::MML_FILE_GAME as i32));
    let s = |f: &Option<Vec<u8>>| f.as_ref().map(|f| String::from_utf8_lossy(f).into_owned());
    c.check("file_read: as the game reads it (a mod's file over the archive's), and the game's own",
        file.is_some() && file != own && own.as_deref() == Some(b"the archive's own".as_slice()), json!([s(&file), s(&own)]));
    c.check("file_read: no such file: None", host.file_read("no\\such.file", 0).is_none(), json!(null));
    c.check("render_device with no game: null", host.render_device(9).is_null(), json!(null));
    // a channel (the one a 64-bit half would open): a value written whole and read back, another size refused
    #[derive(Clone, Copy, PartialEq, Debug)]
    #[repr(C)]
    struct Body {
        pos: [f32; 3],
        n: u32,
    }
    let ch = host.channel_open("bodies", core::mem::size_of::<Body>() as u32);
    let before = ch.and_then(|ch| ch.read_value::<Body>());
    let body = Body { pos: [1.0, 2.5, -3.0], n: 7 };
    let wrote = ch.is_some_and(|ch| ch.write_value(&body));
    let back = ch.and_then(|ch| ch.read_value::<Body>());
    c.check("channel: nothing before the first write; a value written whole, read back as version 1",
        before.is_none() && wrote && back == Some((body, 1)), json!(format!("{back:?}")));
    c.check("channel: the same name with another size refused", host.channel_open("bodies", 4).is_none(), json!(null));
    c.check("gfx_shared_texture outside a pass: None", host.gfx_shared_texture("picture").is_none(), json!(null));
    // (a pass and a view are kept with the bridge off, as mml.h says: refused only with no function, a size over
    // 8192, eight views; they run once the bridge is on)
    let pass = host.gfx_pass(0, |_| ());
    if let Some(p) = pass {
        host.gfx_pass_remove(p);
    }
    c.check("gfx_pass registered and removed", pass.is_some(), json!(pass));
    let view = host.scene_view(64, 64, |_| true);
    if let Some(v) = view {
        host.scene_view_remove(v);
    }
    c.check("scene_view registered and removed", view.is_some(), json!(view));
    c.check("scene_view of 9000 pixels: refused", host.scene_view(9000, 64, |_| true).is_none(), json!(null));
    c.check("prop_create with no game: None", host.prop_create("2bbat1", None, [0.0; 3], None, 1.0).is_none(), json!(null));
    c.check("scene_lights with no game: none", host.scene_lights().is_empty(), json!(null));
    c.check("weather_get with no game: None", host.weather_get(sys::MML_WEATHER_ON).is_none(), json!(null));
    c.check("game_camera with no game: None", host.game_camera().is_none(), json!(null));
    c.check("model_open with no game: None", host.model_open("Tommy").is_none(), json!(null));
    c.check("picture_load with no game: None", host.picture_load("2sight.bmp", None).is_none(), json!(null));
    c.check("hud_marker_add with no game: None", host.hud_marker_add(&Marker { flags: sys::MML_MARKER_SCREEN, ..Default::default() }).is_none(), json!(null));
    c.check("menu_open with no menus: refused", !host.menu_open("kit_rs", &[mml::MenuControl::new(1, "wind", 0.0, 0.0, 300.0, 200.0, Some("x"))], |_| ()),
        json!(null));
    let set = [host.exhaust_hook(|_| false), host.particle_hook(|_| false), host.vehicle_hook(|_| false)];
    let unset = [host.exhaust_unhook(), host.particle_unhook(), host.vehicle_unhook()];
    c.check("hooks (exhaust, particles, vehicles) set and taken back with no game: no fault", true, json!({"set": set, "unset": unset}));
    c.check("shader_add with no device: an error saying why", host.shader_add(&ShaderRule { ps_code: Some(ps), ..Default::default() }).is_err(),
        json!(host.shader_add(&ShaderRule { ps_code: Some(ps), ..Default::default() }).err()));
    let item = host.menu_item("gamemenu", 108, "kit_rs (Rust)", |_| ());
    c.check("menu_item: an item asked for the pause menu, its text and place changed", item.as_ref().is_some_and(|i| i.set(103, "kit_rs again")),
        json!(item.map(|i| i.handle())));
    // (frames are measured Present to Present: the harness's stand-in Present ends each of its frames)
    c.check("frame_stats: filled, the harness's frames counted", host.frame_stats().is_some_and(|s| s.frames > 0 && s.avg_ms > 0.0),
            json!(host.frame_stats().map(|s| s.frames)));
    c.check("profile_saves with no game: none", host.profile_saves().is_empty(), json!(null));
    c.done()
}

pub struct Live {
    pub particles: Rc<Cell<u32>>,
    pub vehicles: Rc<Cell<u32>>,
    pub exhausts: Rc<Cell<u32>>,
    pub captured: Rc<Cell<i64>>,
}

pub fn live(host: Host, l: &Live) -> Value {
    let mut c = Checks::new();
    let Some(g) = Game::detect(host) else {
        c.check("game: a build known", false, json!(host.game_build()));
        return c.done();
    };
    c.check("game: the build detected is the host's", g.build() == host.game_build(), json!(g.build()));
    let me = host.player();
    c.check("game: the player by the game's layouts is the host's", me.is_some() && g.player() == me, json!(me.map(|a| format!("{:p}", a.as_ptr()))));
    let Some(me) = me else { return c.done() };
    c.check("game: the mission's name", g.mission_name() == host.mission_name(), json!(g.mission_name()));
    let (gp, hp) = (g.actor_world_pos(me), host.position(me));
    c.check("game: his place (his model's matrix) is the host's", gp.zip(hp).is_some_and(|(a, b)| mml::game::len(mml::game::sub(a, b)) < 0.01), json!([gp, hp]));
    let model = g.actor_frame(me);
    let kids = model.map(|m| unsafe { g.descendants(m) }.len()).unwrap_or(0);
    c.check("game: his model's frames (EnumFrames)", kids > 10, json!(kids));
    let wm = model.and_then(|m| unsafe { g.world_matrix(m) });
    c.check("game: the game's own world matrix is the frame's", wm.is_some() && wm == model.and_then(|m| g.frame_world(m)), json!(null));
    let skeleton = host.human_skeleton(me);
    let back1 = skeleton.as_ref().and_then(|s| s[sys::MML_BONE_BACK1 as usize].frame);
    // (a model's frames are named after it: "Tommy.back1")
    c.check("people: his skeleton (20 bones, back1's frame named so)", skeleton.as_ref().is_some_and(|s| s.len() == 20)
        && back1.is_some_and(|f| g.frame_name(f).to_ascii_lowercase().ends_with(".back1")), json!(back1.map(|f| g.frame_name(f))));
    let found = model.and_then(|m| unsafe { g.find_child(m, "back1") });
    c.check("game: FindChild finds back1", found.is_some() && found == back1, json!(null));
    let op = g.script_opcode("detector_inrange");
    c.check("game: a script command's number in this build", op == if g.is_10() { 28 } else { 29 } && g.script_command_name(op) == Some("detector_inrange"), json!(op));
    c.check("game: the text code page", [1250, 1251, 1252].contains(&g.text_code_page()), json!(g.text_code_page()));
    let car = g.player_car().or_else(|| g.last_car());
    if let Some(car) = car {
        let cr = g.car(car);
        let body = host.vehicle_body(car);
        c.check("game: the car's place (C_Vehicle) is its body's", body.zip(cr.position()).is_some_and(|(b, p)| mml::game::len(mml::game::sub(b.pos, p)) < 0.05),
            json!({"speed": cr.speed(), "gear": cr.gear(), "fuel": cr.fuel(), "wheels": cr.wheel_count(), "health": cr.engine_health()}));
        let state = unsafe { g.car_state_save(car) };
        c.check("game: the car's whole state saved, it fits its car", state.as_ref().is_some_and(|s| unsafe { g.car_state_fits(car, s) }), json!(state.map(|s| s.len())));
        c.check("world: the car's exhaust", host.car_exhaust(car).is_some(), json!(host.car_exhaust(car).map(|e| e.type_)));
    } else {
        c.check("game: a car (the freeride's taxi)", false, json!(null));
    }
    let at = hp.unwrap_or_default();
    c.check("world: actors near him", !host.world_near(at, 30.0, 0).is_empty(), json!(host.world_near(at, 30.0, 0).len()));
    let down = host.world_sweep([at[0], at[1] + 2.0, at[2]], [at[0], at[1] - 5.0, at[2]], 0.2, sys::MML_WORLD_STATIC, None);
    c.check("world: a sweep down meets the ground", down.is_some(), json!(down.map(|h| h.pos)));
    let shapes = host.world_shapes([at[0] - 10.0, at[1] - 5.0, at[2] - 10.0], [at[0] + 10.0, at[1] + 5.0, at[2] + 10.0], sys::MML_WORLD_ALL);
    c.check("world: collision shapes around him", !shapes.is_empty(), json!(shapes.len()));
    let lights = host.scene_lights();
    // (every light, the ones off and unnamed too; lights.list shows those on)
    let named: Vec<String> = lights.iter().map(|l| mml::c_text(&l.name)).filter(|n| !n.is_empty()).collect();
    c.check("world: the scene's lights, named", !named.is_empty(), json!({"all": lights.len(), "on": lights.iter().filter(|l| l.on != 0).count(),
        "first named": named.first()}));
    if let Some(f) = lights.first().and_then(Host::light_frame) {
        c.check("world: light_set keeping everything", host.light_set(f, None, f32::NAN, f32::NAN, f32::NAN, None), json!(null));
    }
    c.check("world: the sun", host.scene_sun().is_some(), json!(host.scene_sun().map(|s| s.0)));
    let on = host.weather_get(sys::MML_WEATHER_ON);
    c.check("world: the weather read, set back as it was", on.is_some_and(|v| host.weather_set(sys::MML_WEATHER_ON, v)), json!(on));
    let n = host.particle_def_count();
    // (its record 0 is empty in both builds)
    let pmodel = (0..n).find_map(|i| host.particle_def(i).map(|d| Host::particle_def_model(&d)).filter(|m| !m.is_empty()));
    c.check("world: particles.def's records", n > 0 && pmodel.is_some(), json!({"count": n, "first model": pmodel}));
    let prop = host.prop_create("2bbat1", None, [at[0] + 1.5, at[1] + 1.0, at[2]], None, 1.0);
    c.check("props: made, placed, hidden and shown", prop.is_some_and(|p| host.prop_place(p, Some([at[0] + 1.5, at[1] + 0.5, at[2]]), None, 0.0)
        && host.prop_show(p, false) && host.prop_show(p, true)), json!(null));
    if let Some(p) = prop {
        let phys = host.prop_physics(p, Some(mml::world::PropPhysics { mass: 2.0, velocity: [0.0; 3] }));
        c.check("props: physics, a push, its body", phys && host.prop_push(p, [0.0, 0.0, 1.0], None) && host.prop_body(p).is_some(), json!(phys));
        c.check("props: removed", host.prop_remove(p), json!(null));
    }
    let s = host.sound_play("menuopen.wav", Place::Flat, 0.2, 1.0, 0.0, 0.0, 0);
    c.check("sound: played, its state, stopped", s.is_some_and(|s| host.sound_state(s).is_some() && host.sound_stop(s)), json!(s));
    let pic = host.picture_load("2sight.bmp", Some("2sight+.bmp"));
    c.check("hud: a picture loaded, its size", pic.is_some_and(|p| host.picture_size(p).is_some()), json!(pic.and_then(|p| host.picture_size(p))));
    if let Some(p) = pic {
        host.picture_free(p);
    }
    let m = host.hud_marker_add(&Marker { pos: at, flags: sys::MML_MARKER_SCREEN, label: Some("kit_rs".into()), ..Default::default() });
    c.check("hud: a marker added, changed, removed", m.is_some_and(|m| host.hud_marker_set(m, &Marker { pos: at, flags: sys::MML_MARKER_SCREEN | sys::MML_MARKER_DISTANCE, ..Default::default() })
        && host.hud_marker_remove(m)), json!(m));
    let anim = host.human_anim_state(me);
    c.check("people: what he plays", anim.is_some(), json!(anim.map(|a| a.name)));
    c.check("people: his items", !host.items(me).is_empty(), json!(host.items(me).len()));
    let cam = host.game_camera();
    c.check("camera: the game's", cam.is_some(), json!(cam.map(|c| Host::game_camera_mode_name(&c))));
    c.check("camera: the frame's, a ray through the middle", host.render_camera().is_some() && host.camera_ray(400.0, 300.0).is_some(), json!(null));
    c.check("camera: an override that changes nothing, given back", host.camera_override(|_| false) && host.camera_release(), json!(null));
    c.check("data: the statistics, the profile's progress", host.stats(sys::MML_SAVE_SLOT as i32).is_some() && host.profile_progress().is_some(),
        json!(host.profile_progress().map(|p| p.chapter)));
    c.check("data: a save's state put and read back", host.save_put(sys::MML_SAVE_SLOT as i32, "kit_rs", b"42")
        && host.save_get(sys::MML_SAVE_SLOT as i32, "kit_rs").as_deref() == Some(b"42".as_slice()), json!(null));
    host.save_put(sys::MML_SAVE_SLOT as i32, "kit_rs", &[]);
    let model4 = host.model_open("Tommy");
    let nodes = model4.as_ref().map(|m| m.nodes()).unwrap_or_default();
    let mesh = model4.as_ref().and_then(|m| (0..nodes.len() as i32).find_map(|i| m.mesh(i, 0).map(|x| x.triangles().len())));
    c.check("data: Tommy.4ds read: nodes, a mesh, materials", !nodes.is_empty() && mesh.is_some_and(|t| t > 0)
        && model4.as_ref().is_some_and(|m| !m.materials().is_empty()), json!({"nodes": nodes.len(), "triangles": mesh}));
    let model_file = model.and_then(|m| host.model_file(m));
    c.check("data: his model's file", model_file.as_deref().is_some_and(|f| f.to_lowercase().contains("tommy")), json!(model_file));
    let mut seen = 0;
    let n = host.game_files(Some("models\\*.4ds"), |_, _| {
        seen += 1;
        seen >= 5
    });
    c.check("data: the game's models listed", n >= 5 && seen == 5, json!(n));
    let def = host.file_read("tables\\predmety.def", sys::MML_FILE_GAME as i32);
    c.check("data: a game's table read", def.as_ref().is_some_and(|d| d.len() > 100), json!(def.map(|d| d.len())));
    let pieces = host.scene_pieces(|p| p.iter().map(|x| x.triangles().len()).sum::<usize>());
    host.scene_focus(Some(([at[0] - 20.0, at[1] - 10.0, at[2] - 20.0], [at[0] + 20.0, at[1] + 10.0, at[2] + 20.0])));
    c.check("meshes: the scenery around him (a second call: its scan's)", true, json!(pieces));
    let car_meshes = car.and_then(|car| host.actor_meshes(car, |p| p.len()));
    c.check("meshes: the car's parts", car_meshes.is_some_and(|n| n > 0), json!(car_meshes));
    let cap = l.captured.clone();
    c.check("geometry: a capture asked (its count in a second call)", host.geom_capture("*", move |g| cap.set(g.triangles().len() as i64)), json!(l.captured.get()));
    let rule = ShaderRule { pattern: Some("kit_rs_none*.bmp"), source: Some("float4 Ps(float4 c : COLOR0) : COLOR { return c; }"), ps_entry: Some("Ps"), ..Default::default() };
    let sh = host.shader_add(&rule);
    c.check("shaders: a rule compiled by the host, removed", sh.as_ref().is_ok_and(|&h| host.shader_remove(h)), json!(sh.err()));
    let p = l.particles.clone();
    let v = l.vehicles.clone();
    let e = l.exhausts.clone();
    c.check("hooks: particles, vehicles, exhaust set (counted, nothing changed)", host.particle_hook(move |_| { p.set(p.get() + 1); false })
        && host.vehicle_hook(move |_| { v.set(v.get() + 1); false }) && host.exhaust_hook(move |_| { e.set(e.get() + 1); false }),
        json!({"particles": l.particles.get(), "vehicles": l.vehicles.get(), "exhausts": l.exhausts.get()}));
    c.check("control: a key kept and let go", host.key_capture(host.key_code("F12").unwrap_or(0), true) && host.key_capture(host.key_code("F12").unwrap_or(0), false), json!(null));
    c.check("music: the game's city music let play", host.music_game(true).is_some(), json!(null));
    c.check("frame: measured", host.frame_stats().is_some_and(|s| s.frames > 0), json!(host.frame_stats().map(|s| s.avg_ms)));
    c.done()
}
