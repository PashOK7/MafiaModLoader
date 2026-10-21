//! MafiaModLoader's mod kit for Rust.
//!
//! A mod is a 32-bit Windows DLL (`crate-type = ["cdylib"]`, built for `i686-pc-windows-msvc`) beside its
//! `mod.json`; the loader loads it, hot-reloads it and calls it only on the game thread. This crate is the loader's
//! C ABI, `sdk/include/mml/mml.h`, twice over:
//!
//! - [`sys`]: every type, constant and host function of mml.h as it is (made by `tools/rust_sys` with bindgen;
//!   mml.h's comments are the documentation of each member).
//! - this module: a safe layer over the parts most mods use: the log, memory, hooks, events, commands, saved state,
//!   settings, drawing, keys and hotkeys, the mission, the world's actors, the game's state, the HUD, sound,
//!   interaction, the script machine. Callbacks are closures; what a mod registers is freed at its unload.
//!   Everything else is one call away through [`host_fn!`] (the member checked against the loader's table).
//!
//! ```ignore
//! fn load(host: mml::Host) -> Result<(), String> {
//!     let frames = std::rc::Rc::new(std::cell::Cell::new(0u32));
//!     let f = frames.clone();
//!     host.subscribe(mml::EventType::FRAME, move |_| f.set(f.get() + 1));
//!     host.command("status", "frames seen", move |_args| Ok(format!("{{\"frames\":{}}}", frames.get())));
//!     mml::info!(host, "hello from Rust (game {})", host.game_build());
//!     Ok(())
//! }
//! mml::entry!(load);
//! ```
//!
//! **Threads**: every closure runs on the game thread, one at a time, and may be entered again from inside itself
//! (a handler whose call into the host sends an event): closures are `Fn`, their state in `Cell` / `RefCell`.
//! [`Host`] is not `Send`; [`Poster`] is, for work handed back to the game thread.
//!
//! **Panics**: a panic in a closure (or in `load`, `unload`) is caught at the edge of the mod, logged with its
//! message and place, and then raised as the exception [`PANIC_EXCEPTION`]: the loader's fault guard takes it as it
//! takes any fault of a mod (the mod unloaded at the next frame, `MOD_FAULTED`, the game goes on). This needs
//! `panic = "unwind"` (Cargo's default): with `panic = "abort"` a panic ends the game.
#![allow(clippy::missing_safety_doc)]

#[cfg(not(all(target_arch = "x86", target_os = "windows")))]
compile_error!(
    "mml: a mod is a 32-bit Windows DLL: build it for i686-pc-windows-msvc (`--target i686-pc-windows-msvc`, or a \
     .cargo/config.toml with [build] target = \"i686-pc-windows-msvc\" as tools/new_mod.py --rust writes)"
);

#[allow(non_camel_case_types, non_upper_case_globals, non_snake_case, dead_code, clippy::all)]
pub mod sys;

use core::cell::UnsafeCell;
use core::ffi::{c_char, c_int, c_void, CStr};
use core::ptr::{self, NonNull};
use std::any::Any;
use std::collections::HashMap;
use std::ffi::CString;
use std::panic::{catch_unwind, AssertUnwindSafe};
use std::sync::Mutex;

pub use sys::{
    MmlBulletHit as BulletHit, MmlCarCrash as CarCrash, MmlEventType as EventType, MmlExplosion as Explosion,
    MmlFall as Fall, MmlFont as Font, MmlHit as Hit, MmlHumanHit as HumanHit, MmlItem as Item,
    MmlItemChange as ItemChange, MmlLogLevel as LogLevel, MmlMissionState as MissionState, MmlShot as Shot,
    MmlVec3 as Vec3,
};

/// The exception a panic is raised as, after it is logged ("RUS" in its low bytes): the loader's log and
/// `MOD_FAULTED` name it as the fault's code.
pub const PANIC_EXCEPTION: u32 = 0xE0525553;

// ---------------------------------------------------------------------------------------------------------------
// the mod's own state: the host's table, the mod's handle, what it registered (game thread only)

struct GameCell<T>(UnsafeCell<T>);
// SAFETY: the loader calls a mod on the game thread only; nothing here is touched from another thread (Poster
// goes through the host's own post, which is thread-safe).
unsafe impl<T> Sync for GameCell<T> {}

struct Loaded {
    host: *const sys::MmlHost,
    module: *mut sys::MmlMod,
    owned: HashMap<usize, Box<dyn Any>>, // the closures handed to the host, by their user pointer
    ids: HashMap<(u8, c_int), usize>,    // a removable registration's (kind, id) to its closure (K_*)
    slots: HashMap<&'static str, usize>, // what one call replaces (a hook of which a mod has one): its closure
    grave: Vec<Box<dyn Any>>,            // freed when no closure of the mod runs any more
    unload: Vec<Box<dyn FnOnce()>>,      // on_unload's, in order
    depth: u32,                          // closures of the mod running now (nested)
}

static LOADED: GameCell<Option<Loaded>> = GameCell(UnsafeCell::new(None));

fn with<R>(f: impl FnOnce(&mut Loaded) -> R) -> Option<R> {
    // SAFETY: game thread only (above); no borrow is held while a mod's closure runs.
    unsafe { (*LOADED.0.get()).as_mut().map(f) }
}

/// Keeps a closure for the host's user pointer: freed at the unload, or by [`release`].
fn keep<F: 'static>(f: F) -> *mut c_void {
    let b: Box<F> = Box::new(f);
    let p = Box::into_raw(b);
    // SAFETY: p came from Box::into_raw just now; the Box<dyn Any> owns it from here.
    let any: Box<dyn Any> = unsafe { Box::from_raw(p) };
    with(|l| l.owned.insert(p as usize, any));
    p as *mut c_void
}

/// A closure the host no longer calls: freed once no closure of the mod runs (it may be the one running).
fn release(user: *mut c_void) {
    with(|l| {
        if let Some(b) = l.owned.remove(&(user as usize)) {
            l.grave.push(b);
        }
    });
}

// the kinds of removable registrations (their ids are counted apart)
const K_INTERACT: u8 = 0;
const K_PASS: u8 = 1;
const K_VIEW: u8 = 2;
const K_FRAME: u8 = 3;   // frame_watch: by the frame's address

/// A registration's closure, freed when it is removed (untrack) or refused (id 0: None).
fn track(kind: u8, user: *mut c_void, id: c_int) -> Option<i32> {
    if id == 0 {
        release(user);
        return None;
    }
    with(|l| l.ids.insert((kind, id), user as usize));
    Some(id)
}
fn untrack(kind: u8, id: c_int) {
    if let Some(Some(user)) = with(|l| l.ids.remove(&(kind, id))) {
        release(user as *mut c_void);
    }
}
/// The closure in a slot of which the mod has one (exhaust_hook, camera_override...): the one before it freed.
fn slot(name: &'static str, user: *mut c_void) {
    if let Some(Some(old)) = with(|l| if user.is_null() { l.slots.remove(name) } else { l.slots.insert(name, user as usize) }) {
        release(old as *mut c_void);
    }
}

/// A closure kept for a call it gets once: taken out (the caller frees it after the call).
fn take(user: *mut c_void) -> Option<Box<dyn Any>> {
    with(|l| l.owned.remove(&(user as usize))).flatten()
}

static PANIC: Mutex<String> = Mutex::new(String::new());

/// Every call into the mod: the panic caught, logged and raised as a fault of the mod's.
fn guard<R>(what: &str, f: impl FnOnce() -> R) -> R {
    with(|l| l.depth += 1);
    let r = catch_unwind(AssertUnwindSafe(f));
    let grave = with(|l| {
        l.depth -= 1;
        if l.depth == 0 { std::mem::take(&mut l.grave) } else { Vec::new() }
    });
    match r {
        Ok(r) => {
            drop(grave);
            r
        }
        Err(payload) => {
            let mut said = PANIC.lock().map(|mut s| std::mem::take(&mut *s)).unwrap_or_default();
            if said.is_empty() {
                said = payload_text(&*payload);
            }
            if let Some(h) = host_opt() {
                h.log(LogLevel::ERROR, &format!("panicked in {what}: {said}"));
            }
            std::mem::forget(payload);
            // SAFETY: a Windows API call; the loader's __except around this call into the mod takes it.
            unsafe { RaiseException(PANIC_EXCEPTION, 0, 0, ptr::null()) };
            unreachable!()
        }
    }
}

fn payload_text(p: &(dyn Any + Send)) -> String {
    if let Some(s) = p.downcast_ref::<&str>() {
        (*s).to_owned()
    } else if let Some(s) = p.downcast_ref::<String>() {
        s.clone()
    } else {
        "(a panic without a message)".to_owned()
    }
}

#[link(name = "kernel32")]
extern "system" {
    fn RaiseException(code: u32, flags: u32, count: u32, args: *const usize);
}

#[doc(hidden)]
pub unsafe fn __load<E: core::fmt::Display>(
    host: *const sys::MmlHost,
    module: *mut sys::MmlMod,
    load: fn(Host) -> Result<(), E>,
) -> c_int {
    *LOADED.0.get() = Some(Loaded {
        host,
        module,
        owned: HashMap::new(),
        ids: HashMap::new(),
        slots: HashMap::new(),
        grave: Vec::new(),
        unload: Vec::new(),
        depth: 0,
    });
    std::panic::set_hook(Box::new(|info| {
        let place = info.location().map(|l| format!(" at {}:{}", l.file(), l.line())).unwrap_or_default();
        let text = payload_text(info.payload());
        if let Ok(mut s) = PANIC.lock() {
            *s = format!("{text}{place}");
        }
    }));
    let h = Host { raw: host, module };
    match guard("load", || load(h)) {
        Ok(()) => 0,
        Err(e) => {
            h.log(LogLevel::ERROR, &format!("not loaded: {e}"));
            __unload(module);
            1
        }
    }
}

#[doc(hidden)]
pub unsafe fn __unload(_module: *mut sys::MmlMod) {
    let hooks = with(|l| std::mem::take(&mut l.unload)).unwrap_or_default();
    guard("unload", move || {
        for f in hooks {
            f();
        }
    });
    // what the closures hold dropped too (their Drop may log)
    if let Some(l) = with(|l| (std::mem::take(&mut l.owned), std::mem::take(&mut l.grave))) {
        guard("unload", move || drop(l));
    }
    // the calls posted that will not run (the host drops them with the mod)
    let posted = POSTED.lock().map(|mut p| std::mem::take(&mut *p)).unwrap_or_default();
    guard("unload", move || drop(posted));
    *LOADED.0.get() = None;
    let _ = std::panic::take_hook();
}

/// The mod's exports: `mml_mod_api`, `mml_mod_load` (calls `load(host)`: `Err` refuses the load, logged) and
/// `mml_mod_unload` (runs the [`Host::on_unload`] closures, frees what the mod registered).
#[macro_export]
macro_rules! entry {
    ($load:path) => {
        #[unsafe(no_mangle)]
        pub extern "C" fn mml_mod_api() -> u32 {
            $crate::sys::MML_API_VERSION
        }
        #[unsafe(no_mangle)]
        pub unsafe extern "C-unwind" fn mml_mod_load(
            host: *const $crate::sys::MmlHost,
            module: *mut $crate::sys::MmlMod,
        ) -> ::core::ffi::c_int {
            $crate::__load(host, module, $load)
        }
        #[unsafe(no_mangle)]
        pub unsafe extern "C-unwind" fn mml_mod_unload(module: *mut $crate::sys::MmlMod) {
            $crate::__unload(module)
        }
    };
}

/// A member of the loader's table, checked against its size (an older loader has a smaller one): the host's
/// function as `sys` gives it, `None` when the loader has no such member. `host_fn!(host, money)`.
#[macro_export]
macro_rules! host_fn {
    ($host:expr, $member:ident) => {{
        let raw: *const $crate::sys::MmlHost = $host.sys();
        let end = ::core::mem::offset_of!($crate::sys::MmlHost, $member) + ::core::mem::size_of::<usize>();
        // SAFETY: the table is at least `size` bytes long; the member is read only when it lies inside them.
        #[allow(unused_unsafe)]
        let member = unsafe { if (*raw).size as usize >= end { ::core::ptr::addr_of!((*raw).$member).read() } else { None } };
        member
    }};
}

/// `host_fn!` for the safe layer: a member the loader lacks is a panic that names it.
macro_rules! f {
    ($host:expr, $member:ident) => {
        host_fn!($host, $member)
            .unwrap_or_else(|| panic!(concat!("this loader has no MmlHost::", stringify!($member), ": update MafiaModLoader")))
    };
}

/// A shader the mod's build.rs compiled with mml-build (`mml_build::hlsl("pulse.hlsl", "PsPulse", "ps_5_0")`): its
/// bytecode, `&'static [u8; N]`, from OUT_DIR. `mml::include_shader!("PsPulse")`.
#[macro_export]
macro_rules! include_shader {
    ($name:literal) => {
        include_bytes!(concat!(env!("OUT_DIR"), "/shaders/", $name, ".cso"))
    };
}

/// Logs a line in the mod's name: `mml::info!(host, "speed {}", v)`.
#[macro_export]
macro_rules! info { ($host:expr, $($a:tt)*) => { $host.log($crate::LogLevel::INFO, &format!($($a)*)) }; }
#[macro_export]
macro_rules! warn { ($host:expr, $($a:tt)*) => { $host.log($crate::LogLevel::WARN, &format!($($a)*)) }; }
#[macro_export]
macro_rules! error { ($host:expr, $($a:tt)*) => { $host.log($crate::LogLevel::ERROR, &format!($($a)*)) }; }
#[macro_export]
macro_rules! debug { ($host:expr, $($a:tt)*) => { $host.log($crate::LogLevel::DEBUG, &format!($($a)*)) }; }

// ---------------------------------------------------------------------------------------------------------------
// small types

/// An actor of the game's (a person, a car...): its address. Valid while the game keeps it
/// (`EventType::ACTOR_DESTROYED` tells when it goes).
#[derive(Clone, Copy, PartialEq, Eq, Hash, Debug)]
pub struct Actor(NonNull<c_void>);
impl Actor {
    pub fn from_ptr(p: *mut c_void) -> Option<Actor> {
        NonNull::new(p).map(Actor)
    }
    pub fn as_ptr(self) -> *mut c_void {
        self.0.as_ptr()
    }
}
fn actor_ptr(a: Option<Actor>) -> *mut c_void {
    a.map_or(ptr::null_mut(), Actor::as_ptr)
}

/// An LS3D frame (a model, a bone, a prop): its address.
#[derive(Clone, Copy, PartialEq, Eq, Hash, Debug)]
pub struct Frame(NonNull<c_void>);
impl Frame {
    pub fn from_ptr(p: *mut c_void) -> Option<Frame> {
        NonNull::new(p).map(Frame)
    }
    pub fn as_ptr(self) -> *mut c_void {
        self.0.as_ptr()
    }
}

/// A text's alignment at its x.
#[derive(Clone, Copy, PartialEq, Eq, Debug)]
pub enum Align {
    Left,
    Center,
    Right,
}
impl Align {
    fn raw(self) -> c_int {
        (match self {
            Align::Left => sys::MML_DRAW_LEFT,
            Align::Center => sys::MML_DRAW_CENTER,
            Align::Right => sys::MML_DRAW_RIGHT,
        }) as c_int
    }
}

/// Where a sound is played.
#[derive(Clone, Copy, Debug)]
pub enum Place {
    /// 2D: heard the same everywhere (a menu's sound)
    Flat,
    /// at a place of the world
    At([f32; 3]),
    /// following a frame (a bone, an actor's model, a prop), at a point in its space
    On(Frame, [f32; 3]),
}

/// Types that are valid for any bytes (read from the game's memory): integers, floats, their arrays.
pub unsafe trait Pod: Copy + 'static {}
macro_rules! pod { ($($t:ty)*) => { $(unsafe impl Pod for $t {})* }; }
pod!(u8 i8 u16 i16 u32 i32 u64 i64 usize isize f32 f64 Vec3);
unsafe impl<T: Pod, const N: usize> Pod for [T; N] {}

/// The place a hook's original is kept, which the loader keeps right as hooks come and go:
/// `static ORIGINAL: mml::Original = mml::Original::new();`, then `ORIGINAL.get::<unsafe extern "thiscall" fn(...)>()`.
pub struct Original(UnsafeCell<*mut c_void>);
unsafe impl Sync for Original {}
impl Original {
    pub const fn new() -> Original {
        Original(UnsafeCell::new(ptr::null_mut()))
    }
    /// The rest of the chain as a function pointer of the hooked function's type (`F` must be one).
    pub unsafe fn get<F: Copy>(&self) -> F {
        assert_eq!(core::mem::size_of::<F>(), core::mem::size_of::<*mut c_void>());
        core::mem::transmute_copy(&*self.0.get())
    }
    pub fn as_ptr(&self) -> *mut *mut c_void {
        self.0.get()
    }
}
impl Default for Original {
    fn default() -> Self {
        Self::new()
    }
}

/// A fixed C array of the host's (a light's name, a model node's) as UTF-8, up to its NUL.
pub fn c_text(chars: &[c_char]) -> String {
    let bytes: Vec<u8> = chars.iter().take_while(|&&c| c != 0).map(|&c| c as u8).collect();
    String::from_utf8_lossy(&bytes).into_owned()
}

fn cstring(s: &str) -> CString {
    // a NUL inside cuts the text there, as C would read it
    CString::new(s.split('\0').next().unwrap_or("")).unwrap_or_default()
}
unsafe fn text(p: *const c_char) -> String {
    if p.is_null() { String::new() } else { CStr::from_ptr(p).to_string_lossy().into_owned() }
}

// ---------------------------------------------------------------------------------------------------------------
// events

/// An event as the loader sent it (`mml.h` MmlEvent: which member each type fills).
#[derive(Clone, Copy)]
pub struct Event<'a>(&'a sys::MmlEvent);

macro_rules! later {
    ($self:ident, $member:ident) => {
        ($self.0.size as usize >= core::mem::offset_of!(sys::MmlEvent, $member) + core::mem::size_of_val(&$self.0.$member))
    };
}
impl<'a> Event<'a> {
    pub fn raw(&self) -> &'a sys::MmlEvent {
        self.0
    }
    pub fn kind(&self) -> EventType {
        self.0.type_
    }
    /// FRAME, MISSION_TICK: seconds
    pub fn dt(&self) -> f32 {
        self.0.dt
    }
    /// ACTOR_DESTROYED, HUMAN_CREATED, HUMAN_HIT, the events of what happens
    pub fn actor(&self) -> Option<Actor> {
        Actor::from_ptr(self.0.actor)
    }
    /// HUMAN_DIED the killer, VEHICLE_* the vehicle, CAR_CRASH the car struck
    pub fn other(&self) -> Option<Actor> {
        if later!(self, other) { Actor::from_ptr(self.0.other) } else { None }
    }
    /// SERVICES_CHANGED, MOD_LOADED, MISSION_OPEN, MISSION_LOADED, CONFIG_CHANGED, FILE_CHANGED
    pub fn name(&self) -> Option<String> {
        if self.0.name.is_null() { None } else { Some(unsafe { text(self.0.name) }) }
    }
    pub fn hit(&self) -> Option<&'a HumanHit> {
        unsafe { self.0.hit.as_ref() }
    }
    pub fn shot(&self) -> Option<&'a Shot> {
        if later!(self, shot) { unsafe { self.0.shot.as_ref() } } else { None }
    }
    pub fn explosion(&self) -> Option<&'a Explosion> {
        if later!(self, explosion) { unsafe { self.0.explosion.as_ref() } } else { None }
    }
    pub fn bullet(&self) -> Option<&'a BulletHit> {
        if later!(self, bullet) { unsafe { self.0.bullet.as_ref() } } else { None }
    }
    pub fn crash(&self) -> Option<&'a CarCrash> {
        if later!(self, crash) { unsafe { self.0.crash.as_ref() } } else { None }
    }
    pub fn fall(&self) -> Option<&'a Fall> {
        if later!(self, fall) { unsafe { self.0.fall.as_ref() } } else { None }
    }
    pub fn item(&self) -> Option<&'a ItemChange> {
        if later!(self, item) { unsafe { self.0.item.as_ref() } } else { None }
    }
    /// GAME_SAVING, GAME_LOADED: the save's slot (-1 a new game) and profile
    pub fn slot(&self) -> Option<(i32, i32)> {
        if later!(self, profile) { Some((self.0.slot, self.0.profile)) } else { None }
    }
    /// MONEY_CHANGED: the money now and by how much
    pub fn money(&self) -> Option<(i32, i32)> {
        if later!(self, change) { Some((self.0.value, self.0.change)) } else { None }
    }
    /// RENDER_*: whether the mission's world is drawn (not a menu's scene)
    pub fn world(&self) -> bool {
        later!(self, world) && self.0.world != 0
    }
    /// VIEW_WORLD: the scene view's handle
    pub fn view(&self) -> i32 {
        if later!(self, view) { self.0.view } else { 0 }
    }
}

/// A script's end (script_run): its state and its registers as they ended.
pub struct ScriptResult<'a>(&'a sys::MmlScriptResult);
impl<'a> ScriptResult<'a> {
    pub fn raw(&self) -> &'a sys::MmlScriptResult {
        self.0
    }
    pub fn handle(&self) -> i32 {
        self.0.handle
    }
    /// sys::MML_SCRIPT_DONE, _FAILED, _STOPPED
    pub fn state(&self) -> i32 {
        self.0.state
    }
    pub fn done(&self) -> bool {
        self.0.state == sys::MML_SCRIPT_DONE as i32
    }
    /// why it failed or stopped ("" when done) and the line (0 none)
    pub fn error(&self) -> (String, i32) {
        (unsafe { text(self.0.error) }, self.0.line)
    }
    pub fn flt(&self) -> &'a [f32] {
        if self.0.flt.is_null() { &[] } else { unsafe { core::slice::from_raw_parts(self.0.flt, self.0.flt_count.max(0) as usize) } }
    }
    pub fn act(&self, i: usize) -> Option<Actor> {
        if self.0.act.is_null() || i >= self.0.act_count.max(0) as usize {
            return None;
        }
        Actor::from_ptr(unsafe { *self.0.act.add(i) })
    }
}

// ---------------------------------------------------------------------------------------------------------------
// trampolines: the host's callbacks into the closures (C-unwind: the loader's fault guard unwinds through them)

macro_rules! as_c {
    ($t:expr, $($arg:ty),* $(; $ret:ty)?) => {
        {
            // SAFETY: "C" and "C-unwind" are the same calling convention (cdecl); only unwinding through it differs.
            #[allow(unused_unsafe)]
            let c = unsafe {
                core::mem::transmute::<unsafe extern "C-unwind" fn($($arg),*) $(-> $ret)?, unsafe extern "C" fn($($arg),*) $(-> $ret)?>($t)
            };
            Some(c)
        }
    };
}

unsafe extern "C-unwind" fn on_event<F: Fn(Event)>(ev: *const sys::MmlEvent, user: *mut c_void) {
    guard("an event handler", || (*(user as *const F))(Event(&*ev)))
}
unsafe extern "C-unwind" fn on_command<F: Fn(&str) -> Result<String, String>>(
    args: *const c_char,
    reply: *mut sys::MmlReply,
    user: *mut c_void,
) -> c_int {
    guard("a command", || {
        let h = host();
        let a = text(args);
        match (*(user as *const F))(&a) {
            Ok(json) => {
                f!(h, reply_append)(reply, json.as_ptr() as *const c_char, json.len());
                0
            }
            Err(e) => {
                let e = cstring(&e);
                f!(h, reply_error)(reply, e.as_ptr());
                1
            }
        }
    })
}
unsafe extern "C-unwind" fn on_user<F: Fn()>(user: *mut c_void) {
    guard("a hotkey", || (*(user as *const F))())
}
unsafe extern "C-unwind" fn on_id<F: Fn(i32)>(id: c_int, user: *mut c_void) {
    guard("a callback", || (*(user as *const F))(id))
}
unsafe extern "C-unwind" fn on_once_id<F: FnOnce(i32) + 'static>(id: c_int, user: *mut c_void) {
    guard("a timer", || {
        if let Some(b) = take(user) {
            if let Ok(f) = b.downcast::<F>() {
                f(id)
            }
        }
    })
}
unsafe extern "C-unwind" fn on_zone<F: Fn(i32, bool, Option<Actor>)>(zone: c_int, inside: c_int, actor: *mut c_void, user: *mut c_void) {
    guard("a zone", || (*(user as *const F))(zone, inside != 0, Actor::from_ptr(actor)))
}
unsafe extern "C-unwind" fn on_script<F: FnOnce(&ScriptResult) + 'static>(r: *const sys::MmlScriptResult, user: *mut c_void) {
    guard("a script's end", || {
        if let Some(b) = take(user) {
            if let Ok(f) = b.downcast::<F>() {
                f(&ScriptResult(&*r))
            }
        }
    })
}
/// The calls posted and not run yet, by their id (the host's user pointer): any thread adds, the game thread takes;
/// what is left at the unload is dropped there (the host drops a gone mod's posts unrun).
type Posted = Box<dyn FnOnce(Host) + Send>;
static POSTED: Mutex<Vec<(usize, Posted)>> = Mutex::new(Vec::new());
static POST_ID: std::sync::atomic::AtomicUsize = std::sync::atomic::AtomicUsize::new(1);

unsafe extern "C-unwind" fn on_post(user: *mut c_void) {
    let id = user as usize;
    let f = POSTED.lock().ok().and_then(|mut p| p.iter().position(|e| e.0 == id).map(|i| p.swap_remove(i).1));
    if let Some(f) = f {
        guard("a posted call", move || f(host()))
    }
}
/// posted calls still waiting
pub fn posted_waiting() -> usize {
    POSTED.lock().map_or(0, |p| p.len())
}

// ---------------------------------------------------------------------------------------------------------------
// the host

/// The loader's table and this mod's handle. Copy it into closures freely; valid while the mod is loaded (the
/// closures it registers run only then). Game thread only (not `Send`); [`Host::poster`] for other threads.
#[derive(Clone, Copy)]
pub struct Host {
    raw: *const sys::MmlHost,
    module: *mut sys::MmlMod,
}

/// The host of the mod loaded now, from anywhere on the game thread (a hook's detour). Panics when not loaded.
pub fn host() -> Host {
    host_opt().expect("mml::host(): the mod is not loaded")
}
fn host_opt() -> Option<Host> {
    with(|l| Host { raw: l.host, module: l.module })
}

/// Hands work to the game thread from any thread (the host's `post`): it runs at the next frame. A call posted
/// while the mod unloads is dropped unrun.
#[derive(Clone, Copy)]
pub struct Poster(Host);
unsafe impl Send for Poster {}
unsafe impl Sync for Poster {}
impl Poster {
    pub fn post<F: FnOnce(Host) + Send + 'static>(&self, f: F) {
        let id = POST_ID.fetch_add(1, std::sync::atomic::Ordering::Relaxed);
        if let Ok(mut p) = POSTED.lock() {
            p.push((id, Box::new(f)));
        }
        let h = self.0;
        unsafe { f!(h, post)(h.module, as_c!(on_post, *mut c_void), id as *mut c_void) }
    }
}

/// A hotkey of the mod's: its combination changed in place (a setting changed).
#[derive(Clone, Copy)]
pub struct Hotkey {
    host: Host,
    tramp: sys::MmlHotkeyFn,
    user: *mut c_void,
}
impl Hotkey {
    /// The combination replaced ("Ctrl+F5"); false: not understood.
    pub fn rebind(&self, combo: &str) -> bool {
        let c = cstring(combo);
        unsafe { f!(self.host, hotkey)(self.host.module, c.as_ptr(), self.tramp, self.user) != 0 }
    }
}

/// An item of the mod's in one of the game's menus: its text and place changed in place (the host's same function
/// and user again).
#[derive(Clone)]
pub struct MenuItem {
    host: Host,
    handle: c_int,
    menu: String,
    tramp: sys::MmlMenuFn,
    user: *mut c_void,
}
impl MenuItem {
    /// the host's handle
    pub fn handle(&self) -> i32 {
        self.handle
    }
    /// its text and the control it goes under, from the menu's next opening
    pub fn set(&self, after_id: i32, label: &str) -> bool {
        let (m, t) = (cstring(&self.menu), cstring(label));
        unsafe { f!(self.host, menu_item)(self.host.module, m.as_ptr(), after_id, t.as_ptr(), self.tramp, self.user) != 0 }
    }
}

impl Host {
    /// The loader's table as mml.h has it, for [`host_fn!`].
    pub fn sys(&self) -> *const sys::MmlHost {
        self.raw
    }
    /// This mod's handle, the first argument of most host functions.
    pub fn module(&self) -> *mut sys::MmlMod {
        self.module
    }
    pub fn poster(&self) -> Poster {
        Poster(*self)
    }
    /// Called at the unload, before what the mod registered is freed (save what must survive a reload here).
    pub fn on_unload(&self, f: impl FnOnce() + 'static) {
        with(|l| l.unload.push(Box::new(f)));
    }

    // ---- identity
    /// "1.2", "1.0", or "unknown" (the loader runs without the game's hooks)
    pub fn game_build(&self) -> String {
        unsafe { text(f!(self, game_build)()) }
    }
    /// the game's folder (a trailing backslash)
    pub fn game_dir(&self) -> String {
        unsafe { text(f!(self, game_dir)()) }
    }
    pub fn id(&self) -> String {
        unsafe { text(f!(self, mod_id)(self.module)) }
    }
    /// the mod's own folder (replaced by every deploy: write nothing there)
    pub fn dir(&self) -> String {
        unsafe { text(f!(self, mod_dir)(self.module)) }
    }
    /// <game>\mml\data\<id>\: what the mod writes
    pub fn data_dir(&self) -> String {
        unsafe { text(f!(self, mod_data_dir)(self.module)) }
    }

    // ---- log
    pub fn log(&self, level: LogLevel, line: &str) {
        let l = cstring(line);
        unsafe { f!(self, log)(self.module, level, c"%s".as_ptr(), l.as_ptr()) }
    }

    // ---- memory (any address: false / None when it cannot be read or written)
    pub fn read_bytes(&self, addr: usize, out: &mut [u8]) -> bool {
        unsafe { f!(self, mem_read)(addr, out.as_mut_ptr() as *mut c_void, out.len()) != 0 }
    }
    pub fn write_bytes(&self, addr: usize, bytes: &[u8]) -> bool {
        unsafe { f!(self, mem_write)(addr, bytes.as_ptr() as *const c_void, bytes.len()) != 0 }
    }
    pub fn read<T: Pod>(&self, addr: usize) -> Option<T> {
        let mut v = core::mem::MaybeUninit::<T>::zeroed();
        let ok = unsafe { f!(self, mem_read)(addr, v.as_mut_ptr() as *mut c_void, core::mem::size_of::<T>()) != 0 };
        if ok { Some(unsafe { v.assume_init() }) } else { None }
    }
    pub fn write<T: Pod>(&self, addr: usize, v: T) -> bool {
        unsafe { f!(self, mem_write)(addr, &v as *const T as *const c_void, core::mem::size_of::<T>()) != 0 }
    }
    /// "Game.exe", "LS3DF.dll"...; None: not loaded
    pub fn module_base(&self, name: &str) -> Option<usize> {
        let n = cstring(name);
        let b = unsafe { f!(self, module_base)(n.as_ptr()) };
        if b == 0 { None } else { Some(b) }
    }

    // ---- hooks: chained with every other mod's; removed at the unload
    /// `detour` must have the hooked function's convention and arguments; it calls `original.get()` for the rest.
    pub unsafe fn hook(&self, target: usize, detour: *const c_void, original: &'static Original) -> bool {
        f!(self, hook)(self.module, target, detour as *mut c_void, original.as_ptr()) != 0
    }
    pub fn unhook(&self, target: usize) -> bool {
        unsafe { f!(self, unhook)(self.module, target) != 0 }
    }

    // ---- events, commands
    pub fn subscribe<F: Fn(Event) + 'static>(&self, kind: EventType, f: F) -> bool {
        let user = keep(f);
        unsafe { f!(self, subscribe)(self.module, kind, as_c!(on_event::<F>, *const sys::MmlEvent, *mut c_void), user) != 0 }
    }
    /// A command, called over the bridge (and MCP) as "<mod id>.<name>": `args` JSON text (an object, or "null"),
    /// Ok the reply as JSON text, Err an error message.
    pub fn command<F: Fn(&str) -> Result<String, String> + 'static>(&self, name: &str, help: &str, f: F) -> bool {
        let (n, h) = (cstring(name), cstring(help));
        let user = keep(f);
        unsafe {
            f!(self, command)(
                self.module,
                n.as_ptr(),
                h.as_ptr(),
                as_c!(on_command::<F>, *const c_char, *mut sys::MmlReply, *mut c_void; c_int),
                user,
            ) != 0
        }
    }

    // ---- state kept across the mod's own reloads (gone when the game exits)
    pub fn state_put(&self, key: &str, data: &[u8]) -> bool {
        let k = cstring(key);
        unsafe { f!(self, state_put)(self.module, k.as_ptr(), data.as_ptr() as *const c_void, data.len()) != 0 }
    }
    pub fn state_get(&self, key: &str) -> Option<Vec<u8>> {
        let k = cstring(key);
        let get = f!(self, state_get);
        let n = unsafe { get(self.module, k.as_ptr(), ptr::null_mut(), 0) };
        if n == 0 {
            return None;
        }
        let mut v = vec![0u8; n];
        let n = unsafe { get(self.module, k.as_ptr(), v.as_mut_ptr() as *mut c_void, v.len()) };
        v.truncate(n);
        Some(v)
    }

    // ---- the mod's settings (config.json beside it: the defaults; changes in its data folder)
    pub fn config_number(&self, key: &str, def: f64) -> f64 {
        let k = cstring(key);
        unsafe { f!(self, config_number)(self.module, k.as_ptr(), def) }
    }
    pub fn config_string(&self, key: &str, def: &str) -> String {
        let (k, d) = (cstring(key), cstring(def));
        unsafe { text(f!(self, config_string)(self.module, k.as_ptr(), d.as_ptr())) }
    }
    /// the value at the key as JSON text ("" the whole config); None: none
    pub fn config_json(&self, key: &str) -> Option<String> {
        let k = cstring(key);
        let p = unsafe { f!(self, config_json)(self.module, k.as_ptr()) };
        if p.is_null() { None } else { Some(unsafe { text(p) }) }
    }
    /// value: JSON text ("12.5", "\"F5\"", "null" removes the key)
    pub fn config_set(&self, key: &str, value_json: &str) -> bool {
        let (k, v) = (cstring(key), cstring(value_json));
        unsafe { f!(self, config_set)(self.module, k.as_ptr(), v.as_ptr()) != 0 }
    }

    // ---- drawing in the frame (800x600 units, 0xAARRGGBB): call each frame the thing is to be seen
    pub fn draw_text(&self, x: f32, y: f32, size: f32, argb: u32, font: Font, align: Align, s: &str) {
        let t = cstring(s);
        unsafe { f!(self, draw_text)(self.module, x, y, size, argb, font.0, align.raw(), t.as_ptr()) }
    }
    pub fn text_width(&self, font: Font, size: f32, s: &str) -> f32 {
        let t = cstring(s);
        unsafe { f!(self, draw_text_width)(font.0, size, t.as_ptr()) }
    }
    pub fn draw_line(&self, x0: f32, y0: f32, x1: f32, y1: f32, argb: u32) {
        unsafe { f!(self, draw_line)(self.module, x0, y0, x1, y1, argb) }
    }
    pub fn draw_rect(&self, x: f32, y: f32, w: f32, h: f32, argb: u32, filled: bool) {
        unsafe { f!(self, draw_rect)(self.module, x, y, w, h, argb, filled as c_int) }
    }

    // ---- keys and actions, as the game read them last
    /// a DirectInput key by name ("F5", "Ctrl", "A")
    pub fn key_code(&self, name: &str) -> Option<i32> {
        let n = cstring(name);
        let k = unsafe { f!(self, key_code)(n.as_ptr()) };
        if k < 0 { None } else { Some(k) }
    }
    pub fn key_down(&self, key: i32) -> bool {
        unsafe { f!(self, key_down)(key) != 0 }
    }
    /// the one frame it went down
    pub fn key_pressed(&self, key: i32) -> bool {
        unsafe { f!(self, key_pressed)(key) != 0 }
    }
    /// one of the game's actions by name ("Accelerate", "Fire")
    pub fn action_slot(&self, name: &str) -> Option<i32> {
        let n = cstring(name);
        let s = unsafe { f!(self, action_slot)(n.as_ptr()) };
        if s < 0 { None } else { Some(s) }
    }
    pub fn action_held(&self, slot: i32) -> f32 {
        unsafe { f!(self, action_held)(slot) }
    }
    pub fn action_pressed(&self, slot: i32) -> bool {
        unsafe { f!(self, action_pressed)(slot) != 0.0 }
    }
    /// f when the combination goes down ("F5", "Ctrl+Shift+R"); None: not understood
    pub fn hotkey<F: Fn() + 'static>(&self, combo: &str, f: F) -> Option<Hotkey> {
        let c = cstring(combo);
        let user = keep(f);
        let tramp = as_c!(on_user::<F>, *mut c_void);
        if unsafe { f!(self, hotkey)(self.module, c.as_ptr(), tramp, user) } != 0 {
            Some(Hotkey { host: *self, tramp, user })
        } else {
            release(user);
            None
        }
    }

    // ---- the mission
    /// the open mission's folder ("freeitaly", "mise01", "00menu"); "" none
    pub fn mission_name(&self) -> String {
        unsafe { text(f!(self, mission_name)()) }
    }
    pub fn mission_state(&self) -> MissionState {
        MissionState(unsafe { f!(self, mission_state)() })
    }

    // ---- the game's menus
    /// an item put under `after_id` in one of the game's menus ("gamemenu": the pause menu), from its next opening
    pub fn menu_item<F: Fn(i32) + 'static>(&self, menu: &str, after_id: i32, label: &str, f: F) -> Option<MenuItem> {
        let (m, t) = (cstring(menu), cstring(label));
        let user = keep(f);
        let tramp = as_c!(on_id::<F>, c_int, *mut c_void);
        let h = unsafe { f!(self, menu_item)(self.module, m.as_ptr(), after_id, t.as_ptr(), tramp, user) };
        if h == 0 {
            release(user);
            None
        } else {
            Some(MenuItem { host: *self, handle: h, menu: menu.to_owned(), tramp, user })
        }
    }

    // ---- the game's script machine (the missions' language; docs/SCRIPTS.md)
    /// `code` compiled now, run from the next mission tick; `done` once it ends. None: refused (the log says why).
    pub fn script_run<F: FnOnce(&ScriptResult) + 'static>(
        &self,
        code: &str,
        owner: Option<Actor>,
        acts: &[Option<Actor>],
        flts: &[f32],
        timeout_ms: u32,
        done: F,
    ) -> Option<i32> {
        let c = cstring(code);
        let acts: Vec<*mut c_void> = acts.iter().map(|a| actor_ptr(*a)).collect();
        let user = keep(done);
        let h = unsafe {
            f!(self, script_run)(
                self.module,
                c.as_ptr(),
                actor_ptr(owner),
                acts.as_ptr(),
                acts.len() as c_int,
                flts.as_ptr(),
                flts.len() as c_int,
                timeout_ms,
                as_c!(on_script::<F>, *const sys::MmlScriptResult, *mut c_void),
                user,
            )
        };
        if h == 0 {
            release(user);
            None
        } else {
            Some(h)
        }
    }
    pub fn script_stop(&self, handle: i32) {
        unsafe { f!(self, script_stop)(self.module, handle) }
    }

    // ---- the world's actors
    pub fn player(&self) -> Option<Actor> {
        Actor::from_ptr(unsafe { f!(self, human_player)(self.module) })
    }
    /// every person the game made and has not destroyed
    pub fn humans(&self) -> Vec<Actor> {
        let list = f!(self, humans_list);
        let mut v = vec![ptr::null_mut(); 256];
        let mut n = unsafe { list(self.module, v.as_mut_ptr(), v.len() as c_int) }.max(0) as usize;
        if n > v.len() {
            v.resize(n, ptr::null_mut());
            n = (unsafe { list(self.module, v.as_mut_ptr(), v.len() as c_int) }.max(0) as usize).min(v.len());
        }
        v.truncate(n);
        v.into_iter().filter_map(Actor::from_ptr).collect()
    }
    /// a car of the game's list ("Bolt Ace Tudor") or model ("fordtud00.i3d"); dir None: +z
    pub fn spawn_car(&self, model: &str, pos: [f32; 3], dir: Option<[f32; 3]>) -> Option<Actor> {
        self.spawn(sys::MML_ACTOR_CAR as c_int, model, pos, dir)
    }
    /// a person of a model ("Paulie.i3d"); he stands and does nothing until told
    pub fn spawn_human(&self, model: &str, pos: [f32; 3], dir: Option<[f32; 3]>) -> Option<Actor> {
        self.spawn(sys::MML_ACTOR_HUMAN as c_int, model, pos, dir)
    }
    fn spawn(&self, kind: c_int, model: &str, pos: [f32; 3], dir: Option<[f32; 3]>) -> Option<Actor> {
        let m = cstring(model);
        let p = Vec3 { x: pos[0], y: pos[1], z: pos[2] };
        let d = dir.map(|d| Vec3 { x: d[0], y: d[1], z: d[2] });
        let dp = d.as_ref().map_or(ptr::null(), |d| d as *const Vec3);
        Actor::from_ptr(unsafe { f!(self, actor_spawn)(self.module, kind, m.as_ptr(), &p, dp) })
    }
    pub fn actor_remove(&self, actor: Actor) -> bool {
        unsafe { f!(self, actor_remove)(self.module, actor.as_ptr()) != 0 }
    }
    pub fn teleport(&self, actor: Actor, pos: [f32; 3], dir: Option<[f32; 3]>) -> bool {
        let p = Vec3 { x: pos[0], y: pos[1], z: pos[2] };
        let d = dir.map(|d| Vec3 { x: d[0], y: d[1], z: d[2] });
        let dp = d.as_ref().map_or(ptr::null(), |d| d as *const Vec3);
        unsafe { f!(self, actor_teleport)(self.module, actor.as_ptr(), &p, dp) != 0 }
    }
    /// an item of tables\predmety.def by name ("Thompson") or number ("10")
    pub fn item_id(&self, name: &str) -> Option<i32> {
        let n = cstring(name);
        let id = unsafe { f!(self, item_id)(n.as_ptr()) };
        if id < 0 { None } else { Some(id) }
    }
    /// loaded, reserve: -1 a full magazine, the item's usual reserve
    pub fn give_item(&self, human: Actor, item: i32, loaded: i32, reserve: i32) -> bool {
        unsafe { f!(self, give_item)(self.module, human.as_ptr(), item, loaded, reserve) != 0 }
    }
    /// seat 0 the driver's, 1 beside him, 2 and 3 behind
    pub fn seat(&self, human: Actor, car: Actor, seat: i32) -> bool {
        unsafe { f!(self, actor_seat)(self.module, human.as_ptr(), car.as_ptr(), seat) != 0 }
    }
    /// an actor's model frame
    pub fn actor_frame(&self, actor: Actor) -> Option<Frame> {
        Frame::from_ptr(unsafe { f!(self, actor_frame)(self.module, actor.as_ptr()) })
    }
    /// a frame's world matrix now (row vectors: rows 0..2 its axes, row 3 its position)
    pub fn frame_world(&self, frame: Frame) -> Option<[f32; 16]> {
        let mut m = [0f32; 16];
        if unsafe { f!(self, frame_world)(self.module, frame.as_ptr(), m.as_mut_ptr()) } != 0 { Some(m) } else { None }
    }
    /// an actor's place in the world (its model frame's translation)
    pub fn position(&self, actor: Actor) -> Option<[f32; 3]> {
        let m = self.frame_world(self.actor_frame(actor)?)?;
        Some([m[12], m[13], m[14]])
    }
    /// a ray through the world (`what`: sys::MML_WORLD_*), `ignore`'s own objects passed
    pub fn world_ray(&self, from: [f32; 3], to: [f32; 3], what: u32, ignore: Option<Actor>) -> Option<Hit> {
        let mut hit = Hit { size: core::mem::size_of::<Hit>() as u32, ..Default::default() };
        let r = unsafe { f!(self, world_ray)(self.module, from.as_ptr(), to.as_ptr(), what, actor_ptr(ignore), &mut hit) };
        if r != 0 { Some(hit) } else { None }
    }
    /// a world point on the last frame's screen (800x600 units, [2] its depth); None behind the camera
    pub fn camera_project(&self, world: [f32; 3]) -> Option<[f32; 3]> {
        let mut s = [0f32; 3];
        if unsafe { f!(self, camera_project)(self.module, world.as_ptr(), s.as_mut_ptr()) } != 0 { Some(s) } else { None }
    }

    // ---- the game's state
    /// the freeride's money; None with no game
    pub fn money(&self) -> Option<i32> {
        let m = unsafe { f!(self, money)(self.module) };
        if m == i32::MIN { None } else { Some(m) }
    }
    pub fn set_money(&self, value: i32) -> bool {
        unsafe { f!(self, money_set)(self.module, value) != 0 }
    }
    /// a person's health and its maximum
    pub fn health(&self, human: Actor) -> Option<(f32, f32)> {
        let mut max = 0f32;
        let h = unsafe { f!(self, human_health)(self.module, human.as_ptr(), &mut max) };
        if h < 0.0 { None } else { Some((h, max)) }
    }
    pub fn set_health(&self, human: Actor, health: f32) -> bool {
        unsafe { f!(self, human_health_set)(self.module, human.as_ptr(), health) != 0 }
    }
    /// a person kept from the game's harm (bullets, blows, explosions, a car, a fall) by this mod, or let go
    /// (`Some(on)`), or only asked (`None`): his state now; None: not a person
    pub fn invulnerable(&self, human: Actor, on: Option<bool>) -> Option<bool> {
        let v = on.map_or(-1, |b| b as c_int);
        let r = unsafe { f!(self, human_invulnerable)(self.module, human.as_ptr(), v) };
        if r < 0 { None } else { Some(r != 0) }
    }
    /// his inventory, slot by slot (sys::MML_SLOT_*)
    pub fn items(&self, human: Actor) -> Vec<Item> {
        let mut v = vec![Item::default(); sys::MML_SLOT_COUNT as usize];
        let n = unsafe { f!(self, human_items)(self.module, human.as_ptr(), v.as_mut_ptr(), v.len() as c_int) };
        v.truncate(n.clamp(0, v.len() as c_int) as usize);
        v
    }

    // ---- the HUD
    /// the game's own HUD hidden by this mod (`Some(false)`), given back (`Some(true)`) or only asked (`None`):
    /// whether it is shown now (hidden while any mod hides it); None: not known for this build
    pub fn hud_game(&self, show: Option<bool>) -> Option<bool> {
        let v = show.map_or(-1, |b| b as c_int);
        let r = unsafe { f!(self, hud_game)(self.module, v) };
        if r < 0 { None } else { Some(r != 0) }
    }
    /// a line of the game's console (bottom left; its colour argb's RGB)
    pub fn hud_console(&self, line: &str, argb: u32) -> bool {
        let t = cstring(line);
        unsafe { f!(self, hud_console)(self.module, t.as_ptr(), argb) != 0 }
    }
    /// a line of the game's help (above the crosshair): the ms it is shown, 0 not shown
    pub fn hud_help(&self, line: &str) -> i32 {
        let t = cstring(line);
        unsafe { f!(self, hud_help)(self.module, t.as_ptr()) }
    }

    // ---- sound
    /// a sound of the game's ("x.wav": sounds\ put before it) or the mod's files\sounds\; its id, None not played.
    /// near, far: metres (<= 0: 5 and 40); flags sys::MML_SOUND_*
    #[allow(clippy::too_many_arguments)]
    pub fn sound_play(&self, file: &str, place: Place, volume: f32, pitch: f32, near: f32, far: f32, flags: u32) -> Option<i32> {
        let fl = cstring(file);
        let (frame, pos) = match place {
            Place::Flat => (ptr::null_mut(), None),
            Place::At(p) => (ptr::null_mut(), Some(p)),
            Place::On(f, p) => (f.as_ptr(), Some(p)),
        };
        let pp = pos.as_ref().map_or(ptr::null(), |p| p.as_ptr());
        let id = unsafe { f!(self, sound_play)(self.module, fl.as_ptr(), frame, pp, volume, pitch, near, far, flags) };
        if id == 0 { None } else { Some(id) }
    }
    /// volume, pitch while it plays (NAN: kept)
    pub fn sound_set(&self, sound: i32, volume: f32, pitch: f32) -> bool {
        unsafe { f!(self, sound_set)(self.module, sound, volume, pitch) != 0 }
    }
    pub fn sound_stop(&self, sound: i32) -> bool {
        unsafe { f!(self, sound_stop)(self.module, sound) != 0 }
    }

    // ---- interaction: each id goes with interact_remove, the mod's unload or the mission's close
    /// f after ms of the mission's time (again every ms when repeat)
    pub fn timer<F: Fn(i32) + 'static>(&self, ms: i32, repeat: bool, f: F) -> Option<i32> {
        if repeat {
            let user = keep(f);
            self.track(user, unsafe { f!(self, timer_add)(self.module, ms, 1, as_c!(on_id::<F>, c_int, *mut c_void), user) })
        } else {
            self.timer_once(ms, move |id| f(id))
        }
    }
    /// f once, after ms of the mission's time
    pub fn timer_once<F: FnOnce(i32) + 'static>(&self, ms: i32, f: F) -> Option<i32> {
        let user = keep(f);
        self.track(user, unsafe { f!(self, timer_add)(self.module, ms, 0, as_c!(on_once_id::<F>, c_int, *mut c_void), user) })
    }
    /// a sphere the player enters and leaves (pos in frame's space when given): f(zone, inside, who)
    pub fn zone<F: Fn(i32, bool, Option<Actor>) + 'static>(&self, pos: [f32; 3], radius: f32, frame: Option<Frame>, f: F) -> Option<i32> {
        let user = keep(f);
        let fr = frame.map_or(ptr::null_mut(), Frame::as_ptr);
        let tramp = as_c!(on_zone::<F>, c_int, c_int, *mut c_void, *mut c_void);
        self.track(user, unsafe { f!(self, zone_add)(self.module, pos.as_ptr(), radius, fr, tramp, user) })
    }
    /// one of the game's using objects: Action offers `label` within radius; f when the player takes it
    pub fn use_point<F: Fn(i32) + 'static>(&self, pos: [f32; 3], radius: f32, frame: Option<Frame>, label: &str, f: F) -> Option<i32> {
        let l = cstring(label);
        let user = keep(f);
        let fr = frame.map_or(ptr::null_mut(), Frame::as_ptr);
        let tramp = as_c!(on_id::<F>, c_int, *mut c_void);
        self.track(user, unsafe { f!(self, use_point_add)(self.module, pos.as_ptr(), radius, fr, l.as_ptr(), tramp, user) })
    }
    pub fn interact_remove(&self, id: i32) -> bool {
        let r = unsafe { f!(self, interact_remove)(self.module, id) != 0 };
        untrack(K_INTERACT, id);
        r
    }
    fn track(&self, user: *mut c_void, id: c_int) -> Option<i32> {
        track(K_INTERACT, user, id)
    }
}

// ---------------------------------------------------------------------------------------------------------------
// serde: typed commands, settings, saved state (feature "json")

#[cfg(feature = "json")]
pub use serde_json;

#[cfg(feature = "json")]
impl Host {
    /// A command whose parameters are a serde type and whose reply is one. No parameters ("null" or {}) is either
    /// of the two, whichever the type takes: `()` and a struct of defaults both work.
    pub fn command_json<A, R, F>(&self, name: &str, help: &str, f: F) -> bool
    where
        A: serde::de::DeserializeOwned,
        R: serde::Serialize,
        F: Fn(A) -> Result<R, String> + 'static,
    {
        self.command(name, help, move |args| {
            let none = match serde_json::from_str::<serde_json::Value>(args) {
                Ok(serde_json::Value::Null) => Some("{}"),
                Ok(serde_json::Value::Object(o)) if o.is_empty() => Some("null"),
                _ => None,
            };
            let a: A = match serde_json::from_str(args) {
                Ok(a) => a,
                Err(e) => match none.map(serde_json::from_str::<A>) {
                    Some(Ok(a)) => a,
                    _ => return Err(format!("params: {e}")),
                },
            };
            serde_json::to_string(&f(a)?).map_err(|e| e.to_string())
        })
    }
    /// a setting as a serde type (None: absent, or not of that type)
    pub fn config<T: serde::de::DeserializeOwned>(&self, key: &str) -> Option<T> {
        serde_json::from_str(&self.config_json(key)?).ok()
    }
    pub fn state_put_json<T: serde::Serialize>(&self, key: &str, value: &T) -> bool {
        serde_json::to_vec(value).map(|v| self.state_put(key, &v)).unwrap_or(false)
    }
    pub fn state_get_json<T: serde::de::DeserializeOwned>(&self, key: &str) -> Option<T> {
        serde_json::from_slice(&self.state_get(key)?).ok()
    }
}

// ---------------------------------------------------------------------------------------------------------------
// the rest of the host, by area (each an `impl Host`), and the game itself

pub mod control;
pub mod data;
pub mod game;
pub mod gfx;
pub mod media;
pub mod people;
pub mod world;

pub use control::{VehicleBody, VehicleStep};
pub use data::{Model, ModelMesh, Progress, Stats};
pub use gfx::{GeomCapture, GfxFrame, MeshPiece, ShaderRule, ViewCamera};
pub use media::{Marker, MenuControl, SoundState};
pub use people::{Bone, Capsule, DrawShape, HumanAnim, ItemDef, Pedestrian, SkinBone, SkinLod, SkinMesh};
pub use world::{Exhaust, Light, Particle, ParticleDef, Shape, Water, WebNode};

/// The `windows` crate this one is built with (feature "windows"): the typed Direct3D 11 and 9 interfaces.
#[cfg(feature = "windows")]
pub use windows;
