//! The frame's drawing: the render events' device and camera, the graphics bridge's passes (Direct3D 11), the scene
//! drawn from other cameras, the frame's camera replaced, the game's geometry hidden and captured, shaders on the
//! game's own drawing (Direct3D 9), the game's camera logic, the frame's measures (docs/MODDING.md "Drawing in the 3D
//! world" to "The game's camera"). With the feature `windows` the COM objects come typed (the `windows` crate's
//! ID3D11Device, IDirect3DDevice9...), borrowed: the host keeps them.
use super::{guard, keep, release, slot, take, track, untrack, K_PASS, K_VIEW};
use crate::{cstring, sys, text, Actor, Host};
use core::ffi::{c_char, c_int, c_void};
use core::ptr;

pub use sys::{
    MmlFrameStats as FrameStats, MmlGameCamera as GameCamera, MmlGfxConstants as GfxConstants, MmlGfxView as GfxView,
    MmlRenderCamera as RenderCamera, MmlShadowSettings as ShadowSettings, MmlSharedTexture as SharedTexture,
    MmlViewCamera as ViewCamera,
};

/// The graphics bridge's frame as a pass gets it (mml.h MmlGfxFrame; its shaders' side: mml/gfx/frame.hlsli).
#[derive(Clone, Copy)]
pub struct GfxFrame<'a>(&'a sys::MmlGfxFrame);

macro_rules! gfx_has {
    ($self:ident, $member:ident) => {
        ($self.0.size as usize >= core::mem::offset_of!(sys::MmlGfxFrame, $member) + core::mem::size_of_val(&$self.0.$member))
    };
}

impl<'a> GfxFrame<'a> {
    pub fn raw(&self) -> &'a sys::MmlGfxFrame {
        self.0
    }
    /// ID3D11Device* (the bridge's, shared by every mod)
    pub fn device(&self) -> *mut c_void {
        self.0.device
    }
    /// ID3D11DeviceContext*: its immediate context
    pub fn context(&self) -> *mut c_void {
        self.0.context
    }
    /// changes when the device is made anew: what the mod made on the old one is gone
    pub fn generation(&self) -> u32 {
        self.0.generation
    }
    /// the frame's constants (cbuffer MmlFrame, b0)
    pub fn constants(&self) -> &'a GfxConstants {
        unsafe { &*self.0.k }
    }
    /// ID3D11Buffer* holding them
    pub fn constants_buffer(&self) -> *mut c_void {
        self.0.constants
    }
    /// ID3D11ShaderResourceView*: linear view depth, metres (t0)
    pub fn depth(&self) -> *mut c_void {
        self.0.depth
    }
    /// ID3D11ShaderResourceView*: the frame so far (t1)
    pub fn color(&self) -> *mut c_void {
        self.0.color
    }
    /// ID3D11SamplerState*: point (s0), linear (s1)
    pub fn samplers(&self) -> (*mut c_void, *mut c_void) {
        (self.0.point, self.0.linear)
    }
    /// ID3D11RenderTargetView*: the effects target (premultiplied RGBA16F), bound
    pub fn target(&self) -> *mut c_void {
        self.0.target
    }
    /// ID3D11VertexShader*: the full-screen triangle (bound): Draw(3, 0) covers the frame
    pub fn full_screen(&self) -> *mut c_void {
        self.0.full_screen
    }
    pub fn size(&self) -> (u32, u32) {
        (self.0.width, self.0.height)
    }
    /// the camera's matrices (row vectors)
    pub fn view(&self) -> &'a [f32; 16] {
        &self.0.view
    }
    pub fn proj(&self) -> &'a [f32; 16] {
        &self.0.proj
    }
    /// ID3D11ShaderResourceView*: the last frame's 2D pass (gfx_ui asked for), and how many there were
    pub fn ui(&self) -> Option<(*mut c_void, u32)> {
        if gfx_has!(self, ui_frames) && !self.0.ui.is_null() { Some((self.0.ui, self.0.ui_frames)) } else { None }
    }
    /// every mod's scene views drawn so far
    pub fn views(&self) -> &'a [GfxView] {
        if !gfx_has!(self, view_count) || self.0.views.is_null() {
            return &[];
        }
        unsafe { core::slice::from_raw_parts(self.0.views, self.0.view_count as usize) }
    }
    /// 0: the frame; a scene view's handle: the pass runs over that view (gfx_pass_views)
    pub fn in_view(&self) -> i32 {
        if gfx_has!(self, in_view) { self.0.in_view } else { 0 }
    }
    /// ID3D11ShaderResourceView*: the sun's shadow map (t2), while a mod wants it (shader_shadows)
    pub fn sun_shadow(&self) -> Option<*mut c_void> {
        if gfx_has!(self, sun_shadow) && !self.0.sun_shadow.is_null() { Some(self.0.sun_shadow) } else { None }
    }
}

#[cfg(feature = "windows")]
mod typed {
    use super::GfxFrame;
    use windows::core::Interface;
    use windows::Win32::Graphics::Direct3D11::{
        ID3D11Buffer, ID3D11Device, ID3D11DeviceContext, ID3D11RenderTargetView, ID3D11SamplerState, ID3D11ShaderResourceView,
        ID3D11VertexShader,
    };
    impl<'a> GfxFrame<'a> {
        pub fn d3d_device(&self) -> Option<&'a ID3D11Device> {
            unsafe { ID3D11Device::from_raw_borrowed(&self.0.device) }
        }
        pub fn d3d_context(&self) -> Option<&'a ID3D11DeviceContext> {
            unsafe { ID3D11DeviceContext::from_raw_borrowed(&self.0.context) }
        }
        pub fn d3d_constants(&self) -> Option<&'a ID3D11Buffer> {
            unsafe { ID3D11Buffer::from_raw_borrowed(&self.0.constants) }
        }
        pub fn d3d_depth(&self) -> Option<&'a ID3D11ShaderResourceView> {
            unsafe { ID3D11ShaderResourceView::from_raw_borrowed(&self.0.depth) }
        }
        pub fn d3d_color(&self) -> Option<&'a ID3D11ShaderResourceView> {
            unsafe { ID3D11ShaderResourceView::from_raw_borrowed(&self.0.color) }
        }
        pub fn d3d_point(&self) -> Option<&'a ID3D11SamplerState> {
            unsafe { ID3D11SamplerState::from_raw_borrowed(&self.0.point) }
        }
        pub fn d3d_linear(&self) -> Option<&'a ID3D11SamplerState> {
            unsafe { ID3D11SamplerState::from_raw_borrowed(&self.0.linear) }
        }
        pub fn d3d_target(&self) -> Option<&'a ID3D11RenderTargetView> {
            unsafe { ID3D11RenderTargetView::from_raw_borrowed(&self.0.target) }
        }
        pub fn d3d_full_screen(&self) -> Option<&'a ID3D11VertexShader> {
            unsafe { ID3D11VertexShader::from_raw_borrowed(&self.0.full_screen) }
        }
        /// a view's colour (RGBA8) and depth (R32F, metres)
        pub fn d3d_view(&self, i: usize) -> Option<(&'a ID3D11ShaderResourceView, &'a ID3D11ShaderResourceView)> {
            let v = self.views().get(i)?;
            unsafe { Some((ID3D11ShaderResourceView::from_raw_borrowed(&v.color)?, ID3D11ShaderResourceView::from_raw_borrowed(&v.depth)?)) }
        }
    }
    impl super::Host {
        /// gfx_shared_texture typed: the texture and its view, references of their own
        pub fn d3d_shared_texture(
            &self,
            name: &str,
        ) -> Option<(windows::Win32::Graphics::Direct3D11::ID3D11Texture2D, Option<ID3D11ShaderResourceView>, super::SharedTexture)> {
            let t = self.gfx_shared_texture(name)?;
            unsafe {
                let tex = windows::Win32::Graphics::Direct3D11::ID3D11Texture2D::from_raw_borrowed(&t.texture)?.clone();
                Some((tex, ID3D11ShaderResourceView::from_raw_borrowed(&t.srv).cloned(), t))
            }
        }
        /// The game's device as Direct3D 9 (the game on a Direct3D 9 wrapper, as in the labs), during the frame's
        /// drawing (RENDER_* events). A reference of its own (released when dropped).
        pub fn d3d9_device(&self) -> Option<windows::Win32::Graphics::Direct3D9::IDirect3DDevice9> {
            let p = self.render_device(9);
            unsafe { windows::Win32::Graphics::Direct3D9::IDirect3DDevice9::from_raw_borrowed(&p).cloned() }
        }
    }
}

/// The game's geometry LS3D drew (geom_capture): world-space triangles, by face group.
pub struct GeomCapture<'a>(&'a sys::MmlGeomCapture);
/// A face group of a capture
pub struct GeomGroup {
    pub visual: *const c_void,
    pub group: *const c_void,
    pub texture: String,
    pub first: u32,
    pub count: u32,
}
impl<'a> GeomCapture<'a> {
    pub fn raw(&self) -> &'a sys::MmlGeomCapture {
        self.0
    }
    /// every triangle: three corners (metres, the world)
    pub fn triangles(&self) -> &'a [[[f32; 3]; 3]] {
        if self.0.tris.is_null() {
            return &[];
        }
        unsafe { core::slice::from_raw_parts(self.0.tris as *const [[f32; 3]; 3], self.0.count as usize) }
    }
    pub fn groups(&self) -> Vec<GeomGroup> {
        if self.0.groups.is_null() {
            return Vec::new();
        }
        let g = unsafe { core::slice::from_raw_parts(self.0.groups, self.0.group_count as usize) };
        g.iter()
            .map(|g| GeomGroup { visual: g.visual, group: g.group, texture: unsafe { text(g.texture) }, first: g.first, count: g.count })
            .collect()
    }
    /// face groups whose buffers could not be read
    pub fn unreadable(&self) -> u32 {
        self.0.unreadable
    }
}

/// A rule of shader_add: which of the game's draws take a mod's Direct3D 9 shaders, and the shaders (mml.h
/// MmlShaderRule; sdk/include/mml/shaders/game.hlsli the constants, reference.hlsl the game's own picture).
#[derive(Clone, Default)]
pub struct ShaderRule<'a> {
    /// the draws' textures' names ("2kolo*.bmp"); None every one
    pub pattern: Option<&'a str>,
    /// sys::MML_SHADE_OBJECT | _LIGHTMAPPED | _BILLBOARD; 0 every kind
    pub kinds: u32,
    /// sys::MML_SHADE_OPAQUE | _ALPHATEST | _TRANSLUCENT | _ADDITIVE; 0 every class
    pub classes: u32,
    /// sys::MML_GEOM_FRAME | MML_GEOM_VIEWS; 0 both
    pub where_: u32,
    /// sys::MML_SHADE_ALL_PASSES
    pub flags: u32,
    /// HLSL the host compiles (with its entry points), or None
    pub source: Option<&'a str>,
    pub vs_entry: Option<&'a str>,
    pub ps_entry: Option<&'a str>,
    /// None: "vs_3_0", "ps_3_0"
    pub vs_target: Option<&'a str>,
    pub ps_target: Option<&'a str>,
    /// or compiled shaders (fxc's bytecode: mml-build's hlsl())
    pub vs_code: Option<&'a [u8]>,
    pub ps_code: Option<&'a [u8]>,
}

unsafe extern "C-unwind" fn on_pass<F: Fn(&GfxFrame)>(frame: *const sys::MmlGfxFrame, user: *mut c_void) {
    guard("a graphics pass", || (*(user as *const F))(&GfxFrame(&*frame)))
}
unsafe extern "C-unwind" fn on_view<F: Fn(&mut ViewCamera) -> bool>(cam: *mut sys::MmlViewCamera, user: *mut c_void) -> c_int {
    guard("a camera", || (*(user as *const F))(&mut *cam) as c_int)
}
unsafe extern "C-unwind" fn on_geom<F: FnOnce(&GeomCapture) + 'static>(cap: *const sys::MmlGeomCapture, user: *mut c_void) {
    guard("a geometry capture", || {
        if let Some(Ok(f)) = take(user).map(|b| b.downcast::<F>()) {
            f(&GeomCapture(&*cap))
        }
    })
}

impl Host {
    // ---- the render events' device and camera
    /// The game's device: version 8 its IDirect3DDevice8, 9 its IDirect3DDevice9 (on a wrapper); null none. Valid
    /// while the game keeps it: ask each frame.
    pub fn render_device(&self, version: i32) -> *mut c_void {
        unsafe { f!(self, render_device)(version) }
    }
    /// The frame scene's camera as the device has it now (in RENDER_WORLD: the world's)
    pub fn render_camera(&self) -> Option<RenderCamera> {
        let mut c = RenderCamera { size: core::mem::size_of::<RenderCamera>() as u32, ..Default::default() };
        if unsafe { f!(self, render_camera)(&mut c) } != 0 { Some(c) } else { None }
    }

    // ---- the graphics bridge (the mod says "gfx": true in its mod.json and is on when the game starts)
    /// A pass over the frame (lower `order` first); its handle. f runs each frame of the mission's world.
    pub fn gfx_pass<F: Fn(&GfxFrame) + 'static>(&self, order: i32, f: F) -> Option<i32> {
        let user = keep(f);
        let h = unsafe { f!(self, gfx_pass)(self.module, order, as_c!(on_pass::<F>, *const sys::MmlGfxFrame, *mut c_void), user) };
        track(K_PASS, user, h)
    }
    pub fn gfx_pass_remove(&self, handle: i32) {
        unsafe { f!(self, gfx_pass_remove)(self.module, handle) };
        untrack(K_PASS, handle);
    }
    /// The frame's 2D pass for the passes: sys::MML_GFX_UI_OFF, _TAKE, _HIDE
    pub fn gfx_ui(&self, mode: i32) {
        unsafe { f!(self, gfx_ui)(self.module, mode) }
    }
    /// A pass also run over every scene view's picture (its effect in mirrors and reflections)
    pub fn gfx_pass_views(&self, pass: i32, on: bool) -> bool {
        unsafe { f!(self, gfx_pass_views)(self.module, pass, on as c_int) != 0 }
    }
    /// The graphics bridge's state: Some(Ok(())) on (the passes run), Some(Err(why)) off and why (the log's reason),
    /// None not yet (no pass, or no frame of the mission's world drawn since)
    pub fn gfx_state(&self) -> Option<Result<(), String>> {
        let mut why = [0 as c_char; 512];
        match unsafe { f!(self, gfx_state)(self.module, why.as_mut_ptr(), why.len() as c_int) } {
            1 => Some(Ok(())),
            0 => Some(Err(unsafe { core::ffi::CStr::from_ptr(why.as_ptr()) }.to_string_lossy().into_owned())),
            _ => None,
        }
    }
    /// The mod's 64-bit half's texture of that name (side.h texture_create) on the bridge's device, inside a pass:
    /// its ID3D11Texture2D and view (the loader's: valid in this pass), size, format and the half's generation;
    /// None: not there (yet)
    pub fn gfx_shared_texture(&self, name: &str) -> Option<SharedTexture> {
        let n = cstring(name);
        let mut t = SharedTexture {
            size: core::mem::size_of::<SharedTexture>() as u32,
            texture: ptr::null_mut(),
            srv: ptr::null_mut(),
            width: 0,
            height: 0,
            format: 0,
            generation: 0,
        };
        if unsafe { f!(self, gfx_shared_texture)(self.module, n.as_ptr(), &mut t) } != 0 { Some(t) } else { None }
    }

    // ---- the scene from another camera
    /// A view of the mission's scene (0, 0: the frame's size): f places its camera each frame (false: left out
    /// this frame); its picture reaches the passes (GfxFrame::views). Its handle.
    pub fn scene_view<F: Fn(&mut ViewCamera) -> bool + 'static>(&self, width: i32, height: i32, f: F) -> Option<i32> {
        let user = keep(f);
        let h = unsafe { f!(self, scene_view)(self.module, width, height, as_c!(on_view::<F>, *mut sys::MmlViewCamera, *mut c_void; c_int), user) };
        track(K_VIEW, user, h)
    }
    pub fn scene_view_remove(&self, handle: i32) {
        unsafe { f!(self, scene_view_remove)(self.module, handle) };
        untrack(K_VIEW, handle);
    }

    // ---- the frame's camera
    /// The frame drawn from the mod's camera: f changes the game's (filled in) and returns true to draw from it.
    /// One override at a time (the last mod's); camera_release gives it back.
    pub fn camera_override<F: Fn(&mut ViewCamera) -> bool + 'static>(&self, f: F) -> bool {
        let user = keep(f);
        let ok = unsafe { f!(self, camera_override)(self.module, as_c!(on_view::<F>, *mut sys::MmlViewCamera, *mut c_void; c_int), user) != 0 };
        if ok { slot("camera", user) } else { release(user) }
        ok
    }
    pub fn camera_release(&self) -> bool {
        let ok = unsafe { f!(self, camera_override)(self.module, None, ptr::null_mut()) != 0 };
        slot("camera", ptr::null_mut());
        ok
    }
    /// The ray through a screen point (800x600 units): its origin and unit direction
    pub fn camera_ray(&self, x: f32, y: f32) -> Option<([f32; 3], [f32; 3])> {
        let (mut o, mut d) = ([0f32; 3], [0f32; 3]);
        if unsafe { f!(self, camera_ray)(self.module, x, y, o.as_mut_ptr(), d.as_mut_ptr()) } != 0 { Some((o, d)) } else { None }
    }

    // ---- the game's own camera logic
    pub fn game_camera(&self) -> Option<GameCamera> {
        let mut c = GameCamera { size: core::mem::size_of::<GameCamera>() as u32, ..Default::default() };
        if unsafe { f!(self, game_camera)(self.module, &mut c) } != 0 { Some(c) } else { None }
    }
    /// the camera's mode name ("P Behind")
    pub fn game_camera_mode_name(c: &GameCamera) -> String {
        crate::c_text(&c.mode_name)
    }
    /// sys::MML_GAMECAM_*: only a mode the player's state allows
    pub fn game_camera_mode(&self, mode: i32) -> bool {
        unsafe { f!(self, game_camera_mode)(self.module, mode) != 0 }
    }
    /// radians across, held (0: the game's back)
    pub fn game_camera_fov(&self, fov: f32) -> bool {
        unsafe { f!(self, game_camera_fov)(self.module, fov) != 0 }
    }

    // ---- the frame's measures
    pub fn frame_stats(&self) -> Option<FrameStats> {
        let mut s = FrameStats { size: core::mem::size_of::<FrameStats>() as u32, ..Default::default() };
        if unsafe { f!(self, frame_stats)(self.module, &mut s) } != 0 { Some(s) } else { None }
    }

    // ---- the game's own geometry
    /// The face groups whose texture matches ("00test*.bmp") not drawn where `where_` says (sys::MML_GEOM_*; 0 all)
    pub fn geom_hide(&self, pattern: &str, where_: i32) -> Option<i32> {
        let p = cstring(pattern);
        let h = unsafe { f!(self, geom_hide)(self.module, p.as_ptr(), where_) };
        if h > 0 { Some(h) } else { None }
    }
    /// one taken back (0: all the mod's)
    pub fn geom_show(&self, handle: i32) {
        unsafe { f!(self, geom_show)(self.module, handle) }
    }
    /// The matching face groups of the next frame of the mission's world, read from LS3D's buffers; f once
    pub fn geom_capture<F: FnOnce(&GeomCapture) + 'static>(&self, pattern: &str, f: F) -> bool {
        let p = cstring(pattern);
        let user = keep(f);
        let ok = unsafe { f!(self, geom_capture)(self.module, p.as_ptr(), as_c!(on_geom::<F>, *const sys::MmlGeomCapture, *mut c_void), user) != 0 };
        if !ok {
            release(user);
        }
        ok
    }

    // ---- shaders on the game's own drawing
    /// A rule's shaders on the draws it picks; its handle, or the host's (and the compiler's) message
    pub fn shader_add(&self, rule: &ShaderRule) -> Result<i32, String> {
        let s = |o: Option<&str>| o.map(cstring);
        let (pattern, source, vse, pse, vst, pst) =
            (s(rule.pattern), s(rule.source), s(rule.vs_entry), s(rule.ps_entry), s(rule.vs_target), s(rule.ps_target));
        let p = |c: &Option<std::ffi::CString>| c.as_ref().map_or(ptr::null(), |c| c.as_ptr());
        let r = sys::MmlShaderRule {
            size: core::mem::size_of::<sys::MmlShaderRule>() as u32,
            pattern: p(&pattern),
            kinds: rule.kinds,
            classes: rule.classes,
            where_: rule.where_,
            flags: rule.flags,
            source: p(&source),
            vs_entry: p(&vse),
            ps_entry: p(&pse),
            vs_target: p(&vst),
            ps_target: p(&pst),
            vs_code: rule.vs_code.map_or(ptr::null(), |c| c.as_ptr() as *const c_void),
            vs_size: rule.vs_code.map_or(0, |c| c.len()),
            ps_code: rule.ps_code.map_or(ptr::null(), |c| c.as_ptr() as *const c_void),
            ps_size: rule.ps_code.map_or(0, |c| c.len()),
        };
        let mut err = vec![0 as c_char; 2048];
        let h = unsafe { f!(self, shader_add)(self.module, &r, err.as_mut_ptr(), err.len() as c_int) };
        if h != 0 { Ok(h) } else { Err(crate::c_text(&err)) }
    }
    /// one taken back (0: every one of the mod's)
    pub fn shader_remove(&self, handle: i32) -> bool {
        unsafe { f!(self, shader_remove)(self.module, handle) != 0 }
    }
    /// The sun's shadow map for the rules' shaders (None: no longer wanted)
    pub fn shader_shadows(&self, settings: Option<ShadowSettings>) -> bool {
        let p = settings.as_ref().map_or(ptr::null(), |s| s as *const ShadowSettings);
        unsafe { f!(self, shader_shadows)(self.module, p) != 0 }
    }

    // ---- the meshes the game draws
    /// The box (the world) whose scenery the mod wants scanned; None: none
    pub fn scene_focus(&self, bounds: Option<([f32; 3], [f32; 3])>) -> bool {
        let (lo, hi) = bounds.map_or((ptr::null(), ptr::null()), |(l, h)| (l.as_ptr(), h.as_ptr()));
        // (the arrays live through the call: `bounds` is this frame's)
        let r = unsafe { f!(self, scene_focus)(self.module, lo, hi) != 0 };
        let _ = &bounds;
        r
    }
    /// The last scan's pieces touching the mod's box, to f (their arrays are the host's until the next frame)
    pub fn scene_pieces<R>(&self, f: impl FnOnce(&[MeshPiece]) -> R) -> R {
        let get = f!(self, scene_pieces);
        let n = unsafe { get(self.module, ptr::null_mut(), 0) }.max(0) as usize;
        let mut v = vec![sys::MmlMeshPiece::default(); n];
        let n = (unsafe { get(self.module, v.as_mut_ptr(), v.len() as c_int) }.max(0) as usize).min(v.len());
        let pieces: Vec<MeshPiece> = v[..n].iter().map(MeshPiece).collect();
        f(&pieces)
    }
    /// An actor's visuals now (a car's body, doors, wheels, posed), to f; None: no model
    pub fn actor_meshes<R>(&self, actor: Actor, f: impl FnOnce(&[MeshPiece]) -> R) -> Option<R> {
        let get = f!(self, actor_meshes);
        let n = unsafe { get(self.module, actor.as_ptr(), ptr::null_mut(), 0) };
        if n < 0 {
            return None;
        }
        let mut v = vec![sys::MmlMeshPiece::default(); n as usize];
        let n = (unsafe { get(self.module, actor.as_ptr(), v.as_mut_ptr(), v.len() as c_int) }.max(0) as usize).min(v.len());
        let pieces: Vec<MeshPiece> = v[..n].iter().map(MeshPiece).collect();
        Some(f(&pieces))
    }
}

/// A mesh the game draws (scene_pieces, actor_meshes): one visual's triangles at its finest LOD.
#[derive(Clone, Copy)]
pub struct MeshPiece<'a>(&'a sys::MmlMeshPiece);
impl<'a> MeshPiece<'a> {
    pub fn raw(&self) -> &'a sys::MmlMeshPiece {
        self.0
    }
    pub fn visual(&self) -> *mut c_void {
        self.0.visual
    }
    /// its vertices' space to the world now
    pub fn world(&self) -> &'a [f32; 16] {
        &self.0.world
    }
    pub fn positions(&self) -> &'a [[f32; 3]] {
        slice(self.0.pos as *const [f32; 3], self.0.vertices)
    }
    pub fn normals(&self) -> &'a [[f32; 3]] {
        slice(self.0.normal as *const [f32; 3], self.0.vertices)
    }
    pub fn triangles(&self) -> &'a [[u32; 3]] {
        slice(self.0.index as *const [u32; 3], self.0.triangles)
    }
    /// each triangle's material flags (the .4ds bits)
    pub fn materials(&self) -> &'a [u32] {
        slice(self.0.material, self.0.triangles)
    }
    pub fn version(&self) -> u32 {
        self.0.version
    }
    pub fn moving(&self) -> bool {
        self.0.moving != 0
    }
}

pub(crate) fn slice<'a, T>(p: *const T, n: c_int) -> &'a [T] {
    if p.is_null() || n <= 0 { &[] } else { unsafe { core::slice::from_raw_parts(p, n as usize) } }
}
