//! depth_pulse_rs: the graphics bridge from a mod in Rust (the C++ example depth_pulse, in Rust).
//!
//! A scanner's wave runs out from the player over the world's surfaces every few seconds, fading into the game's
//! fog. The pass is a pixel shader of the mod's (pulse.hlsl, compiled at build time by mml-build: build.rs) drawn
//! with the bridge's full-screen triangle into the effects target; it reads the frame's linear depth
//! (frame.hlsli's ViewRay). The Direct3D 11 objects are the `windows` crate's (the mml feature "windows": the frame's
//! device and context come typed and borrowed), made on the bridge's device at the first pass and again when the
//! device is made anew (GfxFrame::generation); dropped at the unload. Where the player is comes from the game's own
//! objects through mml::game (the player, his car, a model's world matrix: the same layouts as the C++ game.h).
//!
//! "gfx": true in mod.json, and the mod on when the game starts (the bridge asks the game for its device then).
//! Commands: depth_pulse_rs.now (a wave from where the player is), depth_pulse_rs.status. Settings: config.json.
use std::cell::{Cell, RefCell};
use std::rc::Rc;
use std::time::Instant;

use mml::game::Game;
use mml::windows::Win32::Graphics::Direct3D11::{
    ID3D11Buffer, ID3D11Device, ID3D11PixelShader, D3D11_BIND_CONSTANT_BUFFER, D3D11_BUFFER_DESC, D3D11_USAGE_DEFAULT,
};
use mml::{EventType, GfxFrame, Host, MissionState};
use serde::Serialize;

const PS_PULSE: &[u8] = mml::include_shader!("PsPulse");

struct Settings {
    color: [f32; 3],
    strength: f32,
    period: f32,
    speed: f32,
    radius: f32,
    width: f32,
    cell: f32,
}

/// The mod's Direct3D 11 objects, made on one device generation
struct Gpu {
    generation: u32,
    ps: ID3D11PixelShader,
    cb: ID3D11Buffer,
}

#[derive(Default)]
struct Wave {
    center: [f32; 3],
    start: Option<Instant>,
}

struct State {
    game: Game,
    settings: RefCell<Settings>,
    gpu: RefCell<Option<Gpu>>,
    wave: RefCell<Wave>,
    frames: Cell<u32>,
    radius: Cell<f32>,
}

fn read_settings(host: Host) -> Settings {
    let c = host.config_string("color", "#FFA040");
    let v = u32::from_str_radix(c.trim_start_matches('#'), 16).unwrap_or(0xFFA040);
    let n = |key: &str, def: f64| host.config_number(key, def) as f32;
    Settings {
        color: [((v >> 16) & 255) as f32 / 255.0, ((v >> 8) & 255) as f32 / 255.0, (v & 255) as f32 / 255.0],
        strength: n("strength", 1.2),
        period: n("period", 4.0),
        speed: n("speed", 25.0),
        radius: n("radius", 80.0),
        width: n("width", 1.2),
        cell: n("cell", 2.0),
    }
}

impl State {
    /// where the player is (his car's place when he drives), in a running mission that is not the main menu's
    fn player_pos(&self) -> Option<[f32; 3]> {
        let host = self.game.host;
        let state = host.mission_state();
        if (state != MissionState::RUNNING && state != MissionState::PAUSED) || host.mission_name() == "00menu" {
            return None;
        }
        let at = self.game.player_car().or_else(|| self.game.player())?;
        self.game.actor_world_pos(at)
    }
    fn start_wave(&self) -> bool {
        match self.player_pos() {
            Some(p) => {
                *self.wave.borrow_mut() = Wave { center: p, start: Some(Instant::now()) };
                true
            }
            None => false,
        }
    }
    fn make(&self, device: &ID3D11Device, generation: u32) -> Option<Gpu> {
        let desc = D3D11_BUFFER_DESC {
            ByteWidth: 48,
            Usage: D3D11_USAGE_DEFAULT,
            BindFlags: D3D11_BIND_CONSTANT_BUFFER.0 as u32,
            ..Default::default()
        };
        let (mut ps, mut cb) = (None, None);
        unsafe {
            device.CreatePixelShader(PS_PULSE, None, Some(&mut ps)).ok()?;
            device.CreateBuffer(&desc, None, Some(&mut cb)).ok()?;
        }
        Some(Gpu { generation, ps: ps?, cb: cb? })
    }
    fn pass(&self, f: &GfxFrame) {
        let (Some(device), Some(ctx)) = (f.d3d_device(), f.d3d_context()) else { return };
        if self.gpu.borrow().as_ref().map(|g| g.generation) != Some(f.generation()) {
            let made = self.make(device, f.generation());
            *self.gpu.borrow_mut() = made;
        }
        let gpu = self.gpu.borrow();
        let Some(gpu) = gpu.as_ref() else { return };
        let s = self.settings.borrow();
        let due = self.wave.borrow().start.is_none_or(|t| t.elapsed().as_secs_f32() > s.period);
        if due && !self.start_wave() {
            return;
        }
        let w = self.wave.borrow();
        let r = w.start.map_or(0.0, |t| t.elapsed().as_secs_f32()) * s.speed;
        self.radius.set(r);
        if r > s.radius {
            return; // faded out: the next wave
        }
        let cb: [f32; 12] = [
            w.center[0], w.center[1], w.center[2], r, s.color[0], s.color[1], s.color[2], s.strength, s.width, s.radius, s.cell, 0.0,
        ];
        unsafe {
            ctx.UpdateSubresource(&gpu.cb, 0, None, cb.as_ptr() as *const _, 0, 0);
            ctx.PSSetConstantBuffers(1, Some(&[Some(gpu.cb.clone())]));
            ctx.PSSetShader(&gpu.ps, None);
            ctx.Draw(3, 0); // the bridge's full-screen triangle (its vertex shader is bound)
        }
        self.frames.set(self.frames.get() + 1);
    }
}

#[derive(Serialize)]
struct Status {
    pass: i32,
    frames: u32,
    radius: f32,
    center: [f32; 3],
    generation: Option<u32>,
    build: &'static str,
}

fn load(host: Host) -> Result<(), String> {
    let game = Game::detect(host).ok_or("not a build of the game whose addresses are known")?;
    let s = Rc::new(State {
        game,
        settings: RefCell::new(read_settings(host)),
        gpu: RefCell::new(None),
        wave: RefCell::new(Wave::default()),
        frames: Cell::new(0),
        radius: Cell::new(0.0),
    });

    let st = s.clone();
    let pass = host.gfx_pass(100, move |f| st.pass(f)).ok_or("no graphics pass (the log says why)")?;
    host.gfx_pass_views(pass, true); // the wave in mirrors and reflections too

    let st = s.clone();
    let key = host.hotkey(&host.config_string("hotkey", "Shift+F11"), move || {
        st.start_wave();
    });
    let st = s.clone();
    host.subscribe(EventType::CONFIG_CHANGED, move |_| {
        *st.settings.borrow_mut() = read_settings(host);
        if let Some(k) = key {
            k.rebind(&host.config_string("hotkey", "Shift+F11"));
        }
    });

    let st = s.clone();
    host.command("now", "a wave from where the player is now", move |_| {
        Ok(if st.start_wave() { r#"{"started":true}"#.into() } else { r#"{"started":false,"why":"no player in a running mission"}"#.into() })
    });
    let st = s.clone();
    host.command_json("status", "the pass, frames drawn, the wave", move |()| {
        Ok(Status {
            pass,
            frames: st.frames.get(),
            radius: st.radius.get(),
            center: st.wave.borrow().center,
            generation: st.gpu.borrow().as_ref().map(|g| g.generation),
            build: st.game.build(),
        })
    });
    Ok(())
}

mml::entry!(load);
