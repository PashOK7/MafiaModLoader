//! The game itself: its builds' addresses, its layouts, and the calls into it (the Rust form of the C++ SDK's
//! `mml/game/game_build.h`, `game.h`, `racing.h`, `script_commands.h`, `mathx.h`).
//!
//! [`tables`] is made from those headers by `tools/gen_rust_game.py`: the same names (`missionPtr`, `vehSpeed`,
//! `kActorFrame`), the same values, one source. [`Game::detect`] tells the builds apart by their code, as
//! `game::Detect` does; the helpers below are game.h's, member for member. Reads go through the host (`Host::read`:
//! they never fault, a wrong address is `None`); what calls the game's own code is `unsafe`: its arguments must be
//! the game's live objects, as in C++ (a fault there is the mod's, unloaded at the next frame).
//!
//! ```ignore
//! let g = mml::game::Game::detect(host).ok_or("not a build of the game whose addresses are known")?;
//! if let Some(car) = g.player_car() {
//!     mml::info!(host, "{:.1} km/h in gear {}", g.car(car).speed_kmh(), g.car(car).gear());
//! }
//! ```
pub mod tables;
pub use tables::*;

use crate::{Actor, Frame, Host};
use core::ffi::{c_char, c_void};

/// The builds of the game the mod runs in, found by [`Game::detect`]; Copy it freely.
#[derive(Clone, Copy)]
pub struct Game {
    pub host: Host,
    /// Game.exe's addresses (game_build.h ExeBuild)
    pub exe: &'static Exe,
    /// LS3DF.dll's RVAs (game_build.h Ls3dBuild): add [`Game::ls3d_base`]
    pub ls3d: &'static Ls3d,
    pub ls3d_base: usize,
    /// the race of mission 6 (racing.h)
    pub racing: &'static Racing,
}

fn exe_is(host: Host, e: &Exe) -> bool {
    SIGS.iter().filter_map(|s| s.exe.map(|f| (f(e), &s.bytes))).all(|(at, b)| {
        let mut got = [0u8; 16];
        at != 0 && host.read_bytes(at, &mut got) && &got == b
    })
}
fn ls3d_is(host: Host, l: &Ls3d, base: usize) -> bool {
    SIGS.iter().filter_map(|s| s.ls3d.map(|f| (f(l), &s.bytes))).all(|(rva, b)| {
        let mut got = [0u8; 16];
        rva != 0 && host.read_bytes(base + rva, &mut got) && &got == b
    })
}

impl Game {
    /// The builds of Game.exe and LS3DF.dll, by their code (game_build.h game::Detect). None: not a build whose
    /// addresses are known (no game: the offline tests' stand-in).
    pub fn detect(host: Host) -> Option<Game> {
        let base = host.module_base("LS3DF.dll")?;
        let (exe, racing) = [(&EXE_12, &RACING_12), (&EXE_10, &RACING_10)].into_iter().find(|(e, _)| exe_is(host, e))?;
        let ls3d = [&LS3D_12, &LS3D_10].into_iter().find(|l| ls3d_is(host, l, base))?;
        Some(Game { host, exe, ls3d, ls3d_base: base, racing })
    }
    /// "1.2" or "1.0" (Game.exe's)
    pub fn build(&self) -> &'static str {
        self.exe.name
    }
    pub fn is_10(&self) -> bool {
        self.exe.name == "1.0"
    }
    /// An LS3DF.dll RVA of [`Game::ls3d`] as an address
    pub fn ls3d_at(&self, rva: usize) -> usize {
        self.ls3d_base + rva
    }

    // ---- memory: reads that never fault
    /// A value at `base + off` (None: not readable)
    pub fn at<T: crate::Pod>(&self, base: usize, off: i32) -> Option<T> {
        self.host.read::<T>(base.wrapping_add(off as isize as usize))
    }
    /// A pointer at `base + off`, None when null or not readable
    pub fn ptr(&self, base: usize, off: i32) -> Option<usize> {
        self.at::<usize>(base, off).filter(|&p| p != 0)
    }
    fn v3(&self, base: usize, off: i32) -> Option<[f32; 3]> {
        self.at::<[f32; 3]>(base, off)
    }

    // ---- the mission and its objects
    /// C_mission
    pub fn mission(&self) -> Option<usize> {
        self.ptr(self.exe.missionPtr, 0)
    }
    /// g_collision
    pub fn collision(&self) -> usize {
        self.exe.collision
    }
    /// C_game (the mission's)
    pub fn game(&self) -> Option<usize> {
        self.ptr(self.mission()?, kMissionGame)
    }
    /// the player's C_human
    pub fn player(&self) -> Option<Actor> {
        Actor::from_ptr(self.ptr(self.game()?, kGamePlayer)? as *mut c_void)
    }
    /// Every actor the game knows: the mission's scene actors (C_mission +0x38) and the temporary ones the traffic
    /// adds (C_game +0x124)
    pub fn list_actors(&self) -> Vec<Actor> {
        let mut out = Vec::new();
        let mut take = |vec: usize| {
            let (Some(b), Some(e)) = (self.at::<usize>(vec, 0), self.at::<usize>(vec, 4)) else { return };
            if b == 0 || e < b || (e - b) / 4 > 20000 {
                return;
            }
            for p in (b..e).step_by(4) {
                if let Some(a) = self.ptr(p, 0) {
                    out.extend(Actor::from_ptr(a as *mut c_void));
                }
            }
        };
        if let Some(m) = self.mission() {
            take(m + 0x38);
            if let Some(g) = self.game() {
                take(g + 0x124);
            }
        }
        out
    }
    /// An actor's type (C_actor +0x10: 2 a person, 4 a car, 0x16 an airplane...)
    pub fn actor_type(&self, actor: Actor) -> Option<i32> {
        self.at(actor.as_ptr() as usize, kActorType)
    }
    /// An actor's model frame
    pub fn actor_frame(&self, actor: Actor) -> Option<Frame> {
        Frame::from_ptr(self.ptr(actor.as_ptr() as usize, kActorFrame)? as *mut c_void)
    }
    /// The open mission's folder ("freeitaly", "00menu"); "" none
    pub fn mission_name(&self) -> String {
        let name = self.mission().and_then(|m| if self.exe.missionName != 0 { self.ptr(m, self.exe.missionName as i32) } else { None });
        name.map(|p| self.c_string(p, 260)).unwrap_or_default()
    }
    /// A NUL-ended text of the game's at `p` (at most `max` bytes), in its own code page turned into UTF-8
    pub fn c_string(&self, p: usize, max: usize) -> String {
        let mut bytes = Vec::new();
        let mut chunk = [0u8; 64];
        while bytes.len() < max {
            let n = chunk.len().min(max - bytes.len());
            if !self.host.read_bytes(p + bytes.len(), &mut chunk[..n]) {
                // the end of a readable page: byte by byte up to it
                let mut one = [0u8; 1];
                if !self.host.read_bytes(p + bytes.len(), &mut one) || one[0] == 0 {
                    break;
                }
                bytes.push(one[0]);
                continue;
            }
            if let Some(z) = chunk[..n].iter().position(|&c| c == 0) {
                bytes.extend_from_slice(&chunk[..z]);
                break;
            }
            bytes.extend_from_slice(&chunk[..n]);
        }
        self.text_utf8(&bytes)
    }
    /// The code page of the game's language (1250, 1251 or 1252: game.h TextCodePage)
    pub fn text_code_page(&self) -> u32 {
        if self.exe.menuLang == 0 {
            return 1252;
        }
        let lang: u16 = self.at(self.exe.menuLang, 0).unwrap_or(0);
        let ll = [((lang & 0xFF) as u8) | 0x20, ((lang >> 8) as u8) | 0x20];
        match &ll {
            b"cz" | b"cs" | b"pl" | b"hu" | b"sk" => 1250,
            b"ru" => 1251,
            _ => 1252,
        }
    }
    /// A game text (its language's code page) as UTF-8 (game.h TextUtf8)
    pub fn text_utf8(&self, text: &[u8]) -> String {
        if text.is_empty() {
            return String::new();
        }
        let cp = self.text_code_page();
        // SAFETY: kernel32's conversion, both lengths given
        unsafe {
            let n = MultiByteToWideChar(cp, 0, text.as_ptr() as *const c_char, text.len() as i32, core::ptr::null_mut(), 0);
            if n <= 0 {
                return String::from_utf8_lossy(text).into_owned();
            }
            let mut w = vec![0u16; n as usize];
            MultiByteToWideChar(cp, 0, text.as_ptr() as *const c_char, text.len() as i32, w.as_mut_ptr(), n);
            String::from_utf16_lossy(&w)
        }
    }

    // ---- frames
    /// A frame's world matrix as LS3D keeps it (+0x10, row vectors), read
    pub fn frame_world(&self, frame: Frame) -> Option<[f32; 16]> {
        self.at(frame.as_ptr() as usize, kFrameWorld)
    }
    /// A frame's world matrix by the game's own function (made valid first, as game.h WorldMatrix)
    pub unsafe fn world_matrix(&self, frame: Frame) -> Option<[f32; 16]> {
        let f: unsafe extern "fastcall" fn(*mut c_void) -> *const f32 = core::mem::transmute(self.exe.frameWorldMatrix);
        let m = f(frame.as_ptr());
        if m.is_null() { None } else { self.host.read(m as usize) }
    }
    /// A child frame by name (I3D_frame::FindChildFrame, vtable +0x38)
    pub unsafe fn find_child(&self, frame: Frame, name: &str) -> Option<Frame> {
        let n = crate::cstring(name);
        let vt = self.ptr(frame.as_ptr() as usize, 0)?;
        let f: unsafe extern "stdcall" fn(*mut c_void, *const c_char, u32) -> *mut c_void =
            core::mem::transmute(self.ptr(vt, kVtFindChild)?);
        Frame::from_ptr(f(frame.as_ptr(), n.as_ptr(), 0xffff))
    }
    /// The direct children of a frame (I3D_frame::EnumFrames, vtable +0x34; flags 0xffff every type)
    pub unsafe fn children(&self, root: Frame, flags: u32) -> Vec<Frame> {
        unsafe extern "stdcall" fn collect(frame: *mut c_void, ctx: *mut c_void) -> i32 {
            (*(ctx as *mut Vec<Frame>)).extend(Frame::from_ptr(frame));
            1
        }
        let mut out = Vec::new();
        let Some(vt) = self.ptr(root.as_ptr() as usize, 0) else { return out };
        let Some(at) = self.ptr(vt, kVtEnumFrames) else { return out };
        type Enum = unsafe extern "stdcall" fn(*mut c_void, unsafe extern "stdcall" fn(*mut c_void, *mut c_void) -> i32, *mut c_void, u32, *const c_char);
        let f: Enum = core::mem::transmute(at);
        f(root.as_ptr(), collect, &mut out as *mut Vec<Frame> as *mut c_void, flags, core::ptr::null());
        out
    }
    /// The whole subtree, depth-first, parents before children
    pub unsafe fn descendants(&self, root: Frame) -> Vec<Frame> {
        fn walk(g: &Game, f: Frame, out: &mut Vec<Frame>, depth: u32) {
            if depth > 32 {
                return;
            }
            for k in unsafe { g.children(f, 0xffff) } {
                out.push(k);
                walk(g, k, out, depth + 1);
            }
        }
        let mut out = Vec::new();
        walk(self, root, &mut out, 0);
        out
    }
    /// A frame's name (+0x100)
    pub fn frame_name(&self, frame: Frame) -> String {
        self.ptr(frame.as_ptr() as usize, kFrameName).map(|p| self.c_string(p, 256)).unwrap_or_default()
    }
    /// A frame's parent (+0x120)
    pub fn frame_parent(&self, frame: Frame) -> Option<Frame> {
        Frame::from_ptr(self.ptr(frame.as_ptr() as usize, kFrameParent)? as *mut c_void)
    }
    /// A frame's local rotation written (w x y z) as game.h SetLocalRotation does: the game builds its matrices
    /// again when it next draws it
    pub fn set_local_rotation(&self, frame: Frame, q: [f32; 4]) -> bool {
        let f = frame.as_ptr() as usize;
        let Some(flags) = self.at::<u32>(f, kFrameFlags) else { return false };
        let flags = (flags & !(kFlagScaled | kFlagLocalBuilt | kFlagWorldValid | kFlagBoundValid)) | kFlagRotValid | kFlagConstructed;
        self.host.write(f + kFrameRot as usize, q) && self.host.write(f + kFrameFlags as usize, flags)
    }
    /// A frame's local position written (game.h SetLocalPosition)
    pub fn set_local_position(&self, frame: Frame, p: [f32; 3]) -> bool {
        let f = frame.as_ptr() as usize;
        let Some(flags) = self.at::<u32>(f, kFrameFlags) else { return false };
        let flags = flags & !(kFlagLocalBuilt | kFlagWorldValid | kFlagBoundValid);
        self.host.write(f + kFrameLocal as usize + 12 * 4, p) && self.host.write(f + kFrameFlags as usize, flags)
    }

    // ---- positions
    /// An actor's place in the world: its model frame's (its own +0x24 is in its model's parent's space)
    pub fn actor_world_pos(&self, actor: Actor) -> Option<[f32; 3]> {
        if let Some(m) = self.actor_frame(actor).and_then(|f| self.frame_world(f)) {
            return Some([m[12], m[13], m[14]]);
        }
        self.v3(actor.as_ptr() as usize, kActorPos)
    }
    /// The world matrix of a model's parent (the space an actor's position is kept in: the world in the street, a
    /// tram's frame in a tram)
    pub fn parent_world(&self, model: Frame) -> Option<[f32; 16]> {
        self.frame_world(self.frame_parent(model)?)
    }
    /// A world point into a model's parent's space (game.h ToParentPoint)
    pub fn to_parent_point(&self, model: Frame, world: [f32; 3]) -> [f32; 3] {
        match self.parent_world(model) {
            Some(pw) => apply(&inverse(&raw3(&pw)), sub(world, [pw[12], pw[13], pw[14]])),
            None => world,
        }
    }
    /// A world direction into it (game.h ToParentDir)
    pub fn to_parent_dir(&self, model: Frame, world: [f32; 3]) -> [f32; 3] {
        match self.parent_world(model) {
            Some(pw) => {
                let r = rot3(&pw);
                [dot(world, r[0]), dot(world, r[1]), dot(world, r[2])]
            }
            None => world,
        }
    }
    /// A direction of its space into the world's (game.h FromParentDir)
    pub fn from_parent_dir(&self, model: Frame, local: [f32; 3]) -> [f32; 3] {
        match self.parent_world(model) {
            Some(pw) => apply(&rot3(&pw), local),
            None => local,
        }
    }
    /// The static world's ray (Exe testLine with game.h's flags): the point hit, its normal, the primitive
    pub unsafe fn test_line(&self, from: [f32; 3], to: [f32; 3], flags: u32) -> Option<([f32; 3], [f32; 3], usize)> {
        type F = unsafe extern "thiscall" fn(usize, *const [f32; 3], *const [f32; 3], *mut [f32; 3], *mut [f32; 3], u32) -> usize;
        let f: F = core::mem::transmute(self.exe.testLine);
        let (delta, mut h, mut n) = (sub(to, from), [0f32; 3], [0f32; 3]);
        let r = f(self.collision(), &from, &delta, &mut h, &mut n, flags);
        if r == 0 { None } else { Some((h, n, r)) }
    }

    // ---- cars
    /// The car the player sits in now (None on foot, or in a tram)
    pub fn player_car(&self) -> Option<Actor> {
        let v = self.ptr(self.player()?.as_ptr() as usize, kHumanVehicle)?;
        if self.at::<i32>(v, kActorType)? == kActorTypeCar { Actor::from_ptr(v as *mut c_void) } else { None }
    }
    /// The car the player drives or drove last (the speedometer's)
    pub fn last_car(&self) -> Option<Actor> {
        if self.exe.hudCar == 0 {
            return None;
        }
        Actor::from_ptr(self.ptr(self.exe.hud, self.exe.hudCar as i32)? as *mut c_void)
    }
    /// A car's fields (any actor of type 4)
    pub fn car(&self, car: Actor) -> Car {
        Car { g: *self, car: car.as_ptr() as usize }
    }
    /// A car repaired as the scripts' car_repair does (C_car::RepairPosition)
    pub unsafe fn car_repair(&self, car: Actor) {
        if self.exe.carRepair != 0 {
            let f: unsafe extern "thiscall" fn(*mut c_void, bool) = core::mem::transmute(self.exe.carRepair);
            f(car.as_ptr(), true);
        }
    }
    /// A car's whole state by the game's own save code (game.h CarStateSave: its place, speeds, engine, dents...)
    pub unsafe fn car_state_save(&self, car: Actor) -> Option<Vec<u8>> {
        if self.exe.vehStateSize == 0 || self.exe.vehStateSave == 0 {
            return None;
        }
        let veh = car.as_ptr() as usize + self.exe.vehicle as usize;
        let size: unsafe extern "thiscall" fn(usize) -> i32 = core::mem::transmute(self.exe.vehStateSize);
        let n = size(veh) as usize;
        if !(8..=16 << 20).contains(&n) {
            return None;
        }
        let mut out = vec![0u8; n];
        let save: unsafe extern "thiscall" fn(usize, *mut u8) -> u8 = core::mem::transmute(self.exe.vehStateSave);
        if save(veh, out.as_mut_ptr()) != 0 { Some(out) } else { None }
    }
    /// Whether a state was made for a car built as this one (game.h CarStateFits)
    pub unsafe fn car_state_fits(&self, car: Actor, state: &[u8]) -> bool {
        state.len() >= 368 && self.car_state_save(car).is_some_and(|now| now.len() >= 368 && now[352..368] == state[352..368])
    }
    /// A state put back into a car built as the one it came from (game.h CarStateLoad: repaired first)
    pub unsafe fn car_state_load(&self, car: Actor, state: &[u8]) -> bool {
        let word = |at: usize| u32::from_le_bytes([state[at], state[at + 1], state[at + 2], state[at + 3]]);
        if state.len() < 8 || word(0) != 9 || word(4) as usize != state.len() || !self.car_state_fits(car, state) {
            return false;
        }
        self.car_repair(car);
        let load: unsafe extern "thiscall" fn(usize, *const u8) -> u8 = core::mem::transmute(self.exe.vehStateLoad);
        load(car.as_ptr() as usize + self.exe.vehicle as usize, state.as_ptr()) != 0
    }

    // ---- the race of mission 6 (racing.h)
    /// C_racing (None outside the race)
    pub fn racing_global(&self) -> Option<usize> {
        self.ptr(self.racing.global, 0)
    }

    // ---- the scripts' commands (script_commands.h)
    /// A command's number in this build (-1: not in it)
    pub fn script_opcode(&self, name: &str) -> i32 {
        SCRIPT_COMMANDS
            .iter()
            .find(|c| c.0.eq_ignore_ascii_case(name))
            .map_or(-1, |c| if self.is_10() { c.2 } else { c.1 } as i32)
    }
    /// The name of a number in this build
    pub fn script_command_name(&self, opcode: i32) -> Option<&'static str> {
        SCRIPT_COMMANDS.iter().find(|c| (if self.is_10() { c.2 } else { c.1 }) as i32 == opcode).map(|c| c.0)
    }
}

/// A car's fields (game.h game::Car): its rigid body, a C_Vehicle at `car + Exe::vehicle`, read for the build
/// running. Positions are metres, the world's; +y up. None: not readable.
#[derive(Clone, Copy)]
pub struct Car {
    pub g: Game,
    pub car: usize,
}
impl Car {
    fn v<T: crate::Pod>(&self, off: i32) -> Option<T> {
        self.g.at(self.car + self.g.exe.vehicle as usize, off)
    }
    pub fn actor(&self) -> Option<Actor> {
        Actor::from_ptr(self.car as *mut c_void)
    }
    /// its C_Vehicle (what the game's Tick, Engine, Move take)
    pub fn vehicle(&self) -> usize {
        self.car + self.g.exe.vehicle as usize
    }
    pub fn position(&self) -> Option<[f32; 3]> {
        self.v(self.g.exe.vehPos)
    }
    /// m/s, the world's axes
    pub fn velocity(&self) -> Option<[f32; 3]> {
        self.v(self.g.exe.vehVelocity)
    }
    /// m/s (the speedometer shows it in mph)
    pub fn speed(&self) -> f32 {
        self.v(self.g.exe.vehSpeed).unwrap_or(0.0)
    }
    pub fn speed_kmh(&self) -> f32 {
        self.speed() * 3.6
    }
    /// the wheels now: radians, left negative
    pub fn steer_angle(&self) -> f32 {
        self.v(self.g.exe.vehSteer).unwrap_or(0.0)
    }
    pub fn steer_wanted(&self) -> f32 {
        self.v(self.g.exe.vehSteerWanted).unwrap_or(0.0)
    }
    pub fn steer_max(&self) -> f32 {
        self.v(self.g.exe.vehSteerMax).unwrap_or(0.0)
    }
    /// -1 left .. 1 right
    pub fn steering(&self) -> f32 {
        let m = self.steer_max();
        if m > 1e-4 { self.steer_angle() / m } else { 0.0 }
    }
    /// -1 reverse, 0 neutral, 1...
    pub fn gear(&self) -> i32 {
        self.v(self.g.exe.vehGear).unwrap_or(0)
    }
    pub fn gear_max(&self) -> i32 {
        self.v(self.g.exe.vehGearMax).unwrap_or(0)
    }
    pub fn engine_on(&self) -> bool {
        self.v::<u8>(self.g.exe.vehEngineOn).unwrap_or(0) != 0
    }
    /// as C_Vehicle::GetRotations
    pub fn rpm(&self) -> f32 {
        if self.engine_on() { self.v::<f32>(self.g.exe.vehRps).unwrap_or(0.0) * 60.0 } else { 0.0 }
    }
    /// litres
    pub fn fuel(&self) -> f32 {
        self.v(self.g.exe.vehFuel).unwrap_or(0.0)
    }
    pub fn fuel_tank(&self) -> f32 {
        self.v(self.g.exe.vehFuelTank).unwrap_or(0.0)
    }
    /// 0..1
    pub fn engine_health(&self) -> f32 {
        let m: f32 = self.v(self.g.exe.vehEngineHealthMax).unwrap_or(0.0);
        if m > 1e-4 { self.v::<f32>(self.g.exe.vehEngineHealth).unwrap_or(0.0) / m } else { 0.0 }
    }
    pub fn damage(&self) -> f32 {
        1.0 - self.engine_health()
    }
    /// its model frame
    pub fn frame(&self) -> Option<Frame> {
        Frame::from_ptr(self.g.ptr(self.car, kActorFrame)? as *mut c_void)
    }
    /// rad/s
    pub fn angular_velocity(&self) -> Option<[f32; 3]> {
        self.v(self.g.exe.vehTurn)
    }
    /// the body's axes (unit vectors in the world): forward, right, up
    pub fn axes(&self) -> Option<[[f32; 3]; 3]> {
        self.v(self.g.exe.vehAxes)
    }
    /// kVehAwake, kVehCalm, kVehAirborne
    pub fn flags(&self) -> u32 {
        self.v(self.g.exe.vehicleFlags).unwrap_or(0)
    }
    pub fn awake(&self) -> bool {
        self.flags() & kVehAwake != 0
    }
    pub fn calm(&self) -> bool {
        self.flags() & kVehCalm != 0
    }
    /// s: the last step Engine made
    pub fn step_time(&self) -> f32 {
        self.v(self.g.exe.vehicleStepTime).unwrap_or(0.0)
    }
    pub fn substep_max(&self) -> f32 {
        self.v(self.g.exe.vehSubstepMax).unwrap_or(0.0)
    }
    pub fn wheel_count(&self) -> i32 {
        self.v(self.g.exe.vehWheelCount).unwrap_or(0)
    }
    /// an S_wheel (the kWheel* fields)
    pub fn wheel(&self, i: i32) -> Option<usize> {
        if i < 0 || i >= self.wheel_count() {
            return None;
        }
        let w: usize = self.v(self.g.exe.vehWheels)?;
        self.g.ptr(w, i * 4)
    }
}

// ---- mathx.h: row vectors, p_world = p_local * M, a rotation's rows its axes

pub fn dot(a: [f32; 3], b: [f32; 3]) -> f32 {
    a[0] * b[0] + a[1] * b[1] + a[2] * b[2]
}
pub fn cross(a: [f32; 3], b: [f32; 3]) -> [f32; 3] {
    [a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2], a[0] * b[1] - a[1] * b[0]]
}
pub fn sub(a: [f32; 3], b: [f32; 3]) -> [f32; 3] {
    [a[0] - b[0], a[1] - b[1], a[2] - b[2]]
}
pub fn len(a: [f32; 3]) -> f32 {
    dot(a, a).sqrt()
}
pub fn norm(a: [f32; 3], fallback: [f32; 3]) -> [f32; 3] {
    let l = len(a);
    if l > 1e-6 { [a[0] / l, a[1] / l, a[2] / l] } else { fallback }
}
/// v * M (M's rows)
pub fn apply(m: &[[f32; 3]; 3], v: [f32; 3]) -> [f32; 3] {
    [0, 1, 2].map(|i| m[0][i] * v[0] + m[1][i] * v[1] + m[2][i] * v[2])
}
/// a 4x4's 3x3 as it is (with its scale)
pub fn raw3(m: &[f32; 16]) -> [[f32; 3]; 3] {
    [[m[0], m[1], m[2]], [m[4], m[5], m[6]], [m[8], m[9], m[10]]]
}
/// a 4x4's rotation rows, scale removed (mathx.h M3::FromWorld)
pub fn rot3(m: &[f32; 16]) -> [[f32; 3]; 3] {
    let r = raw3(m);
    [norm(r[0], [1.0, 0.0, 0.0]), norm(r[1], [0.0, 1.0, 0.0]), norm(r[2], [0.0, 0.0, 1.0])]
}
/// mathx.h M3::Inverse
pub fn inverse(r: &[[f32; 3]; 3]) -> [[f32; 3]; 3] {
    let (c0, c1, c2) = (cross(r[1], r[2]), cross(r[2], r[0]), cross(r[0], r[1]));
    let det = dot(r[0], c0);
    let inv = if det.abs() > 1e-12 { 1.0 / det } else { 0.0 };
    let a = [c0, c1, c2].map(|c| c.map(|x| x * inv));
    [[a[0][0], a[1][0], a[2][0]], [a[0][1], a[1][1], a[2][1]], [a[0][2], a[1][2], a[2][2]]]
}
/// A unit quaternion (w x y z) as an engine 4x4, for every quaternion (mathx.h QuatMatrix: LS3D's own drops turns
/// under 0.00049 rad)
pub fn quat_matrix(q: [f32; 4]) -> [f32; 16] {
    let [w, x, y, z] = q;
    let (x2, y2, z2) = (x + x, y + y, z + z);
    let (xx, xy, xz, yy, yz, zz) = (x2 * x, y2 * x, z2 * x, y2 * y, z2 * y, z2 * z);
    let (wx, wy, wz) = (x2 * w, y2 * w, z2 * w);
    [
        1.0 - (zz + yy), xy - wz, wy + xz, 0.0,
        wz + xy, 1.0 - (zz + xx), yz - wx, 0.0,
        xz - wy, wx + yz, 1.0 - (yy + xx), 0.0,
        0.0, 0.0, 0.0, 1.0,
    ]
}
/// The quaternion of a rotation (rows its axes), unit length (mathx.h QuatOf)
pub fn quat_of(m: &[[f32; 3]; 3]) -> [f32; 4] {
    let ([m00, m01, m02], [m10, m11, m12], [m20, m21, m22]) = (m[0], m[1], m[2]);
    let tr = m00 + m11 + m22;
    let q = if tr > 0.0 {
        let s = (tr + 1.0).sqrt() * 2.0;
        [0.25 * s, (m21 - m12) / s, (m02 - m20) / s, (m10 - m01) / s]
    } else if m00 > m11 && m00 > m22 {
        let s = (1.0 + m00 - m11 - m22).sqrt() * 2.0;
        [(m21 - m12) / s, 0.25 * s, (m01 + m10) / s, (m02 + m20) / s]
    } else if m11 > m22 {
        let s = (1.0 + m11 - m00 - m22).sqrt() * 2.0;
        [(m02 - m20) / s, (m01 + m10) / s, 0.25 * s, (m12 + m21) / s]
    } else {
        let s = (1.0 + m22 - m00 - m11).sqrt() * 2.0;
        [(m10 - m01) / s, (m02 + m20) / s, (m12 + m21) / s, 0.25 * s]
    };
    let n = (q[0] * q[0] + q[1] * q[1] + q[2] * q[2] + q[3] * q[3]).sqrt();
    if n > 1e-9 { q.map(|c| c / n) } else { [1.0, 0.0, 0.0, 0.0] }
}

#[link(name = "kernel32")]
extern "system" {
    fn MultiByteToWideChar(code_page: u32, flags: u32, s: *const c_char, len: i32, wide: *mut u16, wide_len: i32) -> i32;
}
