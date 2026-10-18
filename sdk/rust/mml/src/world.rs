//! The world: the game's collision asked (sweeps, shapes, the water), zones for everyone, props (models that are not
//! actors, their animations and physics), the scene's lights and sun, the game's effects (the car exhaust,
//! particles.def, the particles as they move, the weather) (docs/MODDING.md "Questions to the world", "Props", "The
//! scene's lights", "The game's effects").
use super::{guard, keep, release, slot};
use crate::{cstring, sys, Actor, Frame, Hit, Host};
use core::ffi::{c_int, c_void};
use core::ptr;

pub use sys::{
    MmlExhaust as Exhaust, MmlLight as Light, MmlParticle as Particle, MmlParticleDef as ParticleDef, MmlPropAnim as PropAnim,
    MmlPropPhysics as PropPhysics, MmlShape as Shape, MmlWater as Water, MmlWebNode as WebNode,
};

unsafe extern "C-unwind" fn on_exhaust<F: Fn(&Exhaust) -> bool>(e: *const sys::MmlExhaust, user: *mut c_void) -> c_int {
    guard("an exhaust hook", || (*(user as *const F))(&*e) as c_int)
}
unsafe extern "C-unwind" fn on_particle<F: Fn(&mut Particle) -> bool>(p: *mut sys::MmlParticle, user: *mut c_void) -> c_int {
    guard("a particle hook", || (*(user as *const F))(&mut *p) as c_int)
}

fn opt<T>(o: &Option<T>) -> *const T {
    o.as_ref().map_or(ptr::null(), |v| v as *const T)
}

impl Host {
    // ---- the pedestrians' web and the surfaces
    /// The nodes of the mission's pedestrians' web (check.bin) within `radius` of `at`, nearest first (their first
    /// sys::MML_WEB_LINKS links each)
    pub fn web_nodes(&self, at: [f32; 3], radius: f32) -> Vec<WebNode> {
        let get = f!(self, web_nodes);
        let mut v = vec![WebNode::default(); 64];
        let mut n = unsafe { get(self.module, at.as_ptr(), radius, v.as_mut_ptr(), v.len() as c_int) }.max(0) as usize;
        if n > v.len() {
            v.resize(n, WebNode::default());
            n = (unsafe { get(self.module, at.as_ptr(), radius, v.as_mut_ptr(), v.len() as c_int) }.max(0) as usize).min(v.len());
        }
        v.truncate(n);
        v
    }
    /// A surface's name in the game's table (tables\materials.def; a static face's material: Shape's flags >> 24)
    pub fn material_name(&self, material: i32) -> Option<String> {
        let p = unsafe { f!(self, material_name)(material) };
        if p.is_null() { None } else { Some(unsafe { core::ffi::CStr::from_ptr(p) }.to_string_lossy().into_owned()) }
    }

    // ---- questions to the world
    /// A sphere of `radius` moved from `from` to `to`: where it stops (hit.pos its centre), what it touched
    pub fn world_sweep(&self, from: [f32; 3], to: [f32; 3], radius: f32, what: u32, ignore: Option<Actor>) -> Option<Hit> {
        let mut hit = Hit { size: core::mem::size_of::<Hit>() as u32, ..Default::default() };
        let ig = ignore.map_or(ptr::null_mut(), Actor::as_ptr);
        let r = unsafe { f!(self, world_sweep)(self.module, from.as_ptr(), to.as_ptr(), radius, what, ig, &mut hit) };
        if r != 0 { Some(hit) } else { None }
    }
    /// The collision shapes in a box (the world's axes), as the game keeps them in its grids
    pub fn world_shapes(&self, lo: [f32; 3], hi: [f32; 3], what: u32) -> Vec<Shape> {
        let get = f!(self, world_shapes);
        let mut v = vec![Shape::default(); 256];
        let mut n = unsafe { get(self.module, lo.as_ptr(), hi.as_ptr(), what, v.as_mut_ptr(), v.len() as c_int) }.max(0) as usize;
        if n > v.len() {
            v.resize(n, Shape::default());
            n = (unsafe { get(self.module, lo.as_ptr(), hi.as_ptr(), what, v.as_mut_ptr(), v.len() as c_int) }.max(0) as usize).min(v.len());
        }
        v.truncate(n);
        v
    }
    /// The game's water under a point (31 the river and the sea, 44 shallow)
    pub fn world_water(&self, at: [f32; 3]) -> Option<Water> {
        let mut w = Water { size: core::mem::size_of::<Water>() as u32, ..Default::default() };
        if unsafe { f!(self, world_water)(self.module, at.as_ptr(), &mut w) } != 0 { Some(w) } else { None }
    }
    /// Who a zone watches: sys::MML_ZONE_PLAYER, _PEOPLE, _CARS, _ACTOR (that actor alone)
    pub fn zone_watch(&self, zone: i32, who: u32, actor: Option<Actor>) -> bool {
        unsafe { f!(self, zone_watch)(self.module, zone, who, actor.map_or(ptr::null_mut(), Actor::as_ptr)) != 0 }
    }

    // ---- props
    /// A model (of the game's, or the mod's files\models\) made as a frame: in the world (parent None: pos the
    /// world's) or on a frame (a bone, an actor's model, a prop); rot w x y z; scale <= 0: 1. Gone at the unload.
    pub fn prop_create(&self, model: &str, parent: Option<Frame>, pos: [f32; 3], rot: Option<[f32; 4]>, scale: f32) -> Option<Frame> {
        let m = cstring(model);
        let p = parent.map_or(ptr::null_mut(), Frame::as_ptr);
        let r = rot.as_ref().map_or(ptr::null(), |r| r.as_ptr());
        Frame::from_ptr(unsafe { f!(self, prop_create)(self.module, m.as_ptr(), p, pos.as_ptr(), r, scale) })
    }
    /// moved, turned, scaled in its parent's space (None, scale <= 0: as it is)
    pub fn prop_place(&self, prop: Frame, pos: Option<[f32; 3]>, rot: Option<[f32; 4]>, scale: f32) -> bool {
        let p = pos.as_ref().map_or(ptr::null(), |p| p.as_ptr());
        let r = rot.as_ref().map_or(ptr::null(), |r| r.as_ptr());
        unsafe { f!(self, prop_place)(self.module, prop.as_ptr(), p, r, scale) != 0 }
    }
    /// another parent (None: the world); keep_world: it stays where it is in the world
    pub fn prop_attach(&self, prop: Frame, parent: Option<Frame>, keep_world: bool) -> bool {
        unsafe { f!(self, prop_attach)(self.module, prop.as_ptr(), parent.map_or(ptr::null_mut(), Frame::as_ptr), keep_world as c_int) != 0 }
    }
    pub fn prop_show(&self, prop: Frame, on: bool) -> bool {
        unsafe { f!(self, prop_show)(self.module, prop.as_ptr(), on as c_int) != 0 }
    }
    pub fn prop_remove(&self, prop: Frame) -> bool {
        unsafe { f!(self, prop_remove)(self.module, prop.as_ptr()) != 0 }
    }
    /// An animation of anims\ played on the prop's model (flags sys::MML_PROP_ANIM_LOOP; weight <= 0: 1): its
    /// length in ms, 0 no such file, -1 refused
    pub fn prop_anim_play(&self, prop: Frame, anim: &str, flags: u32, weight: f32) -> i32 {
        let a = cstring(anim);
        unsafe { f!(self, prop_anim_play)(self.module, prop.as_ptr(), a.as_ptr(), flags, weight) }
    }
    pub fn prop_anim_stop(&self, prop: Frame) -> bool {
        unsafe { f!(self, prop_anim_stop)(self.module, prop.as_ptr()) > 0 }
    }
    pub fn prop_anim_state(&self, prop: Frame) -> Option<PropAnim> {
        let mut a = PropAnim::default();
        if unsafe { f!(self, prop_anim_state)(self.module, prop.as_ptr(), &mut a) } > 0 { Some(a) } else { None }
    }
    /// the prop's animation's file
    pub fn prop_anim_name(a: &PropAnim) -> String {
        crate::c_text(&a.anim)
    }
    /// A prop in the world made a physical object (None: no longer)
    pub fn prop_physics(&self, prop: Frame, physics: Option<PropPhysics>) -> bool {
        unsafe { f!(self, prop_physics)(self.module, prop.as_ptr(), opt(&physics)) > 0 }
    }
    /// an impulse (kg m/s) at a point of the world (None: its centre)
    pub fn prop_push(&self, prop: Frame, impulse: [f32; 3], at: Option<[f32; 3]>) -> bool {
        let a = at.as_ref().map_or(ptr::null(), |a| a.as_ptr());
        unsafe { f!(self, prop_push)(self.module, prop.as_ptr(), impulse.as_ptr(), a) > 0 }
    }
    /// its physical object (an actor), None none
    pub fn prop_body(&self, prop: Frame) -> Option<Actor> {
        Actor::from_ptr(unsafe { f!(self, prop_body)(self.module, prop.as_ptr()) })
    }

    // ---- the scene's lights
    pub fn scene_lights(&self) -> Vec<Light> {
        let get = f!(self, scene_lights);
        let mut v = vec![Light::default(); 128];
        let mut n = unsafe { get(self.module, v.as_mut_ptr(), v.len() as c_int) }.max(0) as usize;
        if n > v.len() {
            v.resize(n, Light::default());
            n = (unsafe { get(self.module, v.as_mut_ptr(), v.len() as c_int) }.max(0) as usize).min(v.len());
        }
        v.truncate(n);
        v
    }
    /// a light's frame (Light::frame)
    pub fn light_frame(light: &Light) -> Option<Frame> {
        Frame::from_ptr(light.frame)
    }
    /// A light changed by LS3D's own setters: colour (None kept), power and range (NAN kept), on (None kept)
    pub fn light_set(&self, light: Frame, color: Option<[f32; 3]>, power: f32, range_near: f32, range_far: f32, on: Option<bool>) -> bool {
        let c = color.as_ref().map_or(ptr::null(), |c| c.as_ptr());
        let on = on.map_or(-1, |o| o as c_int);
        unsafe { f!(self, light_set)(self.module, light.as_ptr(), c, power, range_near, range_far, on) != 0 }
    }
    /// The scene's sun: towards it, its colour times its power, the ambient light
    pub fn scene_sun(&self) -> Option<([f32; 3], [f32; 3], [f32; 3])> {
        let (mut d, mut c, mut a) = ([0f32; 3], [0f32; 3], [0f32; 3]);
        if unsafe { f!(self, scene_sun)(self.module, d.as_mut_ptr(), c.as_mut_ptr(), a.as_mut_ptr()) } != 0 { Some((d, c, a)) } else { None }
    }

    // ---- the game's effects
    /// a car's exhaust now
    pub fn car_exhaust(&self, car: Actor) -> Option<Exhaust> {
        let mut e = Exhaust { size: core::mem::size_of::<Exhaust>() as u32, ..Default::default() };
        if unsafe { f!(self, car_exhaust)(self.module, car.as_ptr(), &mut e) } != 0 { Some(e) } else { None }
    }
    /// f told every tick of every car whose exhaust the game makes, before it does; true: the game's puffs left out
    /// for that car. One hook at a time (the last mod's); exhaust_unhook takes it back.
    pub fn exhaust_hook<F: Fn(&Exhaust) -> bool + 'static>(&self, f: F) -> bool {
        let user = keep(f);
        let ok = unsafe { f!(self, exhaust_hook)(self.module, as_c!(on_exhaust::<F>, *const sys::MmlExhaust, *mut c_void; c_int), user) != 0 };
        if ok { slot("exhaust", user) } else { release(user) }
        ok
    }
    pub fn exhaust_unhook(&self) -> bool {
        let ok = unsafe { f!(self, exhaust_hook)(self.module, None, ptr::null_mut()) != 0 };
        slot("exhaust", ptr::null_mut());
        ok
    }
    /// tables\particles.def's records
    pub fn particle_def_count(&self) -> i32 {
        unsafe { f!(self, particle_def)(self.module, -1, ptr::null_mut()) }
    }
    pub fn particle_def(&self, index: i32) -> Option<ParticleDef> {
        let mut d = ParticleDef { size: core::mem::size_of::<ParticleDef>() as u32, ..Default::default() };
        if unsafe { f!(self, particle_def)(self.module, index, &mut d) } != 0 { Some(d) } else { None }
    }
    /// its model's name
    pub fn particle_def_model(d: &ParticleDef) -> String {
        crate::c_text(&d.model)
    }
    /// the named fields written into a record (from the next mission on)
    pub fn particle_def_set(&self, index: i32, def: &ParticleDef) -> bool {
        let mut d = *def;
        d.size = core::mem::size_of::<ParticleDef>() as u32;
        unsafe { f!(self, particle_def_set)(self.module, index, &d) != 0 }
    }
    /// f told of every move of every particle, after the game moved it: change pos and return true to put it
    /// there. One hook at a time; particle_unhook.
    pub fn particle_hook<F: Fn(&mut Particle) -> bool + 'static>(&self, f: F) -> bool {
        let user = keep(f);
        let ok = unsafe { f!(self, particle_hook)(self.module, as_c!(on_particle::<F>, *mut sys::MmlParticle, *mut c_void; c_int), user) != 0 };
        if ok { slot("particle", user) } else { release(user) }
        ok
    }
    pub fn particle_unhook(&self) -> bool {
        let ok = unsafe { f!(self, particle_hook)(self.module, None, ptr::null_mut()) != 0 };
        slot("particle", ptr::null_mut());
        ok
    }
    /// a weather parameter (sys::MML_WEATHER_*)
    pub fn weather_get(&self, param: i32) -> Option<f64> {
        let mut v = 0f64;
        if unsafe { f!(self, weather_get)(self.module, param, &mut v) } != 0 { Some(v) } else { None }
    }
    pub fn weather_set(&self, param: i32, value: f64) -> bool {
        unsafe { f!(self, weather_set)(self.module, param, value) != 0 }
    }
    /// the scene's weather back to its preset
    pub fn weather_reset(&self) -> bool {
        unsafe { f!(self, weather_reset)(self.module) != 0 }
    }
}
