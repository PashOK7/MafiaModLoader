//! Sound and music, the HUD's pictures and markers, the game's menus' pages (docs/MODDING.md "Sound and music",
//! "The HUD", "The game's own menus").
use super::{guard, keep, release, slot};
use crate::{cstring, sys, Frame, Host, Place};
use core::ffi::{c_char, c_int, c_void};
use core::ptr;

pub use sys::MmlSoundState as SoundState;

/// A marker on the HUD (hud_marker_add): a place in the world on the screen, the radar or the compass.
#[derive(Clone, Debug, Default)]
pub struct Marker {
    /// in the world, or in `frame`'s space
    pub pos: [f32; 3],
    /// followed (an actor's model, a bone, a prop); None the world
    pub frame: Option<Frame>,
    /// sys::MML_MARKER_SCREEN | _DISTANCE | _EDGE | _RADAR | _COMPASS
    pub flags: u32,
    /// its colour (0: white)
    pub argb: u32,
    /// a picture (picture_load) drawn at its place; 0 a diamond
    pub picture: i32,
    /// its height on the screen (800x600 units; 0: 24)
    pub scale: f32,
    pub label: Option<String>,
}

/// A control of a menu page (menu_open), as the game's menu files describe theirs (mml.h MmlMenuControl).
#[derive(Clone, Debug, Default)]
pub struct MenuControl {
    pub id: i32,
    /// "wind", "stxt", "item", "butt", "slid", "chck", "bsel", "wins"... or "end"
    pub kind: String,
    pub x: f32,
    pub y: f32,
    pub w: f32,
    pub h: f32,
    /// options "A|B|C" (bslb, bsel), a format (iten), a file (rctt)
    pub text: Option<String>,
    /// 0: the kind's usual; else sys::MML_MENU_*
    pub flags: u32,
}
impl MenuControl {
    pub fn new(id: i32, kind: &str, x: f32, y: f32, w: f32, h: f32, text: Option<&str>) -> MenuControl {
        MenuControl { id, kind: kind.to_owned(), x, y, w, h, text: text.map(str::to_owned), flags: 0 }
    }
}

unsafe extern "C-unwind" fn on_page<F: Fn(i32)>(id: c_int, user: *mut c_void) {
    guard("a menu page", || (*(user as *const (F, Vec<std::ffi::CString>))).0(id))
}

fn place(p: &Place) -> (*mut c_void, Option<[f32; 3]>) {
    match *p {
        Place::Flat => (ptr::null_mut(), None),
        Place::At(at) => (ptr::null_mut(), Some(at)),
        Place::On(f, at) => (f.as_ptr(), Some(at)),
    }
}

impl Host {
    // ---- sound and music
    /// a sound moved (Place::At) or put on another frame (Place::On)
    pub fn sound_move(&self, sound: i32, to: Place) -> bool {
        let (frame, pos) = place(&to);
        let p = pos.as_ref().map_or(ptr::null(), |p| p.as_ptr());
        unsafe { f!(self, sound_move)(self.module, sound, frame, p) != 0 }
    }
    /// its state while it is known
    pub fn sound_state(&self, sound: i32) -> Option<SoundState> {
        let mut s = SoundState { size: core::mem::size_of::<SoundState>() as u32, ..Default::default() };
        if unsafe { f!(self, sound_state)(self.module, sound, &mut s) } != 0 { Some(s) } else { None }
    }
    /// An .ogg (or .mp3) streamed as the game's music, in a menu too (volume 0..1 times the options'); its id
    pub fn music_play(&self, file: &str, volume: f32, flags: u32) -> Option<i32> {
        let f = cstring(file);
        let id = unsafe { f!(self, music_play)(self.module, f.as_ptr(), volume, flags) };
        if id == 0 { None } else { Some(id) }
    }
    pub fn music_set(&self, music: i32, volume: f32) -> bool {
        unsafe { f!(self, music_set)(self.module, music, volume) != 0 }
    }
    pub fn music_stop(&self, music: i32) -> bool {
        unsafe { f!(self, music_stop)(self.module, music) != 0 }
    }
    /// The game's own city music faded out (false) or let play (true); None: no mission
    pub fn music_game(&self, on: bool) -> Option<bool> {
        match unsafe { f!(self, music_game)(self.module, on as c_int) } {
            -1 => None,
            r => Some(r != 0),
        }
    }

    // ---- the HUD's pictures and markers
    /// A texture by the game's loader (maps\ or the mod's files\Maps\; a .bmp's alpha from alpha_file); its handle
    pub fn picture_load(&self, file: &str, alpha_file: Option<&str>) -> Option<i32> {
        let f = cstring(file);
        let a = alpha_file.map(cstring);
        let id = unsafe { f!(self, picture_load)(self.module, f.as_ptr(), a.as_ref().map_or(ptr::null(), |a| a.as_ptr())) };
        if id == 0 { None } else { Some(id) }
    }
    /// its pixels
    pub fn picture_size(&self, picture: i32) -> Option<(i32, i32)> {
        let (mut w, mut h) = (0, 0);
        if unsafe { f!(self, picture_size)(picture, &mut w, &mut h) } != 0 { Some((w, h)) } else { None }
    }
    pub fn picture_free(&self, picture: i32) {
        unsafe { f!(self, picture_free)(self.module, picture) }
    }
    /// as draw_rect, the picture on it, turned by `angle` (degrees, clockwise), tinted by argb, uv a part of it
    #[allow(clippy::too_many_arguments)]
    pub fn draw_picture(&self, picture: i32, x: f32, y: f32, w: f32, h: f32, angle: f32, argb: u32, uv: Option<[f32; 4]>) {
        let u = uv.as_ref().map_or(ptr::null(), |u| u.as_ptr());
        unsafe { f!(self, draw_picture)(self.module, picture, x, y, w, h, angle, argb, u) }
    }
    fn marker(&self, m: &Marker, call: impl FnOnce(&sys::MmlHudMarker) -> c_int) -> c_int {
        let label = m.label.as_deref().map(cstring);
        let raw = sys::MmlHudMarker {
            size: core::mem::size_of::<sys::MmlHudMarker>() as u32,
            pos: m.pos,
            frame: m.frame.map_or(ptr::null_mut(), Frame::as_ptr),
            flags: m.flags,
            argb: m.argb,
            picture: m.picture,
            scale: m.scale,
            label: label.as_ref().map_or(ptr::null(), |l| l.as_ptr()),
        };
        call(&raw)
    }
    /// its id (the host copies it)
    pub fn hud_marker_add(&self, m: &Marker) -> Option<i32> {
        let id = self.marker(m, |raw| unsafe { f!(self, hud_marker_add)(self.module, raw) });
        if id == 0 { None } else { Some(id) }
    }
    pub fn hud_marker_set(&self, id: i32, m: &Marker) -> bool {
        self.marker(m, |raw| unsafe { f!(self, hud_marker_set)(self.module, id, raw) }) != 0
    }
    pub fn hud_marker_remove(&self, id: i32) -> bool {
        unsafe { f!(self, hud_marker_remove)(self.module, id) != 0 }
    }

    // ---- the game's menus: a page of the mod's own
    /// A page put over the open menu (the pause menu opened first when none is), made by the game from `controls`
    /// (the first one the page's window); f(id) when a control is clicked or changed. One page a mod at a time.
    pub fn menu_open<F: Fn(i32) + 'static>(&self, name: &str, controls: &[MenuControl], f: F) -> bool {
        let n = cstring(name);
        let mut strings = Vec::new();
        let mut raw = Vec::with_capacity(controls.len());
        for c in controls {
            let kind = cstring(&c.kind);
            let text = c.text.as_deref().map(cstring);
            raw.push(sys::MmlMenuControl {
                id: c.id,
                kind: kind.as_ptr(),
                x: c.x,
                y: c.y,
                w: c.w,
                h: c.h,
                text: text.as_ref().map_or(ptr::null(), |t| t.as_ptr()),
                flags: c.flags,
            });
            strings.push(kind);
            strings.extend(text);
        }
        // the texts live as long as the page's function (the host may read them when the game makes the page)
        let user = keep((f, strings));
        let ok = unsafe {
            f!(self, menu_open)(self.module, n.as_ptr(), raw.as_ptr(), raw.len() as c_int, as_c!(on_page::<F>, c_int, *mut c_void), user) != 0
        };
        if ok { slot("page", user) } else { release(user) }
        ok
    }
    /// the mod's page closed (a Back button's function)
    pub fn menu_close(&self) {
        unsafe { f!(self, menu_close)(self.module) }
    }
    pub fn menu_set_text(&self, id: i32, text: &str) -> bool {
        let t = cstring(text);
        unsafe { f!(self, menu_set_text)(self.module, id, t.as_ptr()) != 0 }
    }
    /// slid, pbar, wins: 0..1; chck, bslb: 0 or 1; bsel: the option; iten: the number
    pub fn menu_set_value(&self, id: i32, value: f32) -> bool {
        unsafe { f!(self, menu_set_value)(self.module, id, value) != 0 }
    }
    pub fn menu_value(&self, id: i32) -> Option<f32> {
        let mut v = 0f32;
        if unsafe { f!(self, menu_value)(self.module, id, &mut v) } != 0 { Some(v) } else { None }
    }
    /// a number's ("iten") range
    pub fn menu_set_range(&self, id: i32, min: i32, max: i32) -> bool {
        unsafe { f!(self, menu_set_range)(self.module, id, min, max) != 0 }
    }
    /// a text field's text (or any control's)
    pub fn menu_text(&self, id: i32) -> Option<String> {
        let mut b = vec![0 as c_char; 256];
        if unsafe { f!(self, menu_text)(self.module, id, b.as_mut_ptr(), b.len() as c_int) } != 0 { Some(crate::c_text(&b)) } else { None }
    }
}
