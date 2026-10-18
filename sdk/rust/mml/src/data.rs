//! Data: a mod's own files in the game and the game's files read, .4ds models parsed, a mod's state in the game's
//! saves, the statistics and the profile's progress, services between mods, the script machine's state
//! (docs/MODDING.md "A mod's own files in the game", "The game's files and models", "A mod's state in the saves",
//! "Statistics, the profile's progress").
use crate::gfx::slice;
use crate::{cstring, sys, Frame, Host};
use core::ffi::{c_char, c_int, c_void};
use core::ptr;

pub use sys::{MmlModelMaterial as ModelMaterial, MmlModelNode as ModelNode, MmlProgress as Progress, MmlStats as Stats};

/// A channel with the mod's 64-bit half (channel_open): one block of shared memory, each write a whole new version,
/// a read the latest whole one. Any thread; closed with the mod's unload (not to be used after it).
#[derive(Clone, Copy)]
pub struct Channel {
    host: Host,
    raw: *mut sys::MmlChannel,
}
unsafe impl Send for Channel {}
unsafe impl Sync for Channel {}
impl Channel {
    /// A whole new version (at most the channel's size); false: too long
    pub fn write(&self, data: &[u8]) -> bool {
        unsafe { f!(self.host, channel_write)(self.raw, data.as_ptr() as *const c_void, data.len() as u32) != 0 }
    }
    /// The latest whole version copied into buf: its length and number; None: nothing written yet
    pub fn read(&self, buf: &mut [u8]) -> Option<(usize, u64)> {
        let mut version = 0u64;
        let n = unsafe { f!(self.host, channel_read)(self.raw, buf.as_mut_ptr() as *mut c_void, buf.len() as u32, &mut version) };
        if version != 0 { Some((n as usize, version)) } else { None }
    }
    /// A plain value written whole (a #[repr(C)] struct of numbers)
    pub fn write_value<T: Copy>(&self, v: &T) -> bool {
        self.write(unsafe { core::slice::from_raw_parts(v as *const T as *const u8, core::mem::size_of::<T>()) })
    }
    /// The latest version read as a plain value, with its number; None: nothing yet, or a version of another length
    pub fn read_value<T: Copy>(&self) -> Option<(T, u64)> {
        let mut v = core::mem::MaybeUninit::<T>::zeroed();
        let buf = unsafe { core::slice::from_raw_parts_mut(v.as_mut_ptr() as *mut u8, core::mem::size_of::<T>()) };
        let (n, version) = self.read(buf)?;
        if n == core::mem::size_of::<T>() { Some((unsafe { v.assume_init() }, version)) } else { None }
    }
    /// Waits until the channel holds a version after `after` (the number a read gave), at most `timeout_ms` (0: only
    /// looks): that version's number; None: none came in time. The writer wakes it at once; one waiter a channel.
    pub fn wait(&self, after: u64, timeout_ms: u32) -> Option<u64> {
        let v = unsafe { f!(self.host, channel_wait)(self.raw, after, timeout_ms) };
        if v != 0 { Some(v) } else { None }
    }
}
impl Host {
    /// The channel "<name>" of this mod, the one its 64-bit half opens (side.h channel_open) with the same size;
    /// None: another size, or no memory
    pub fn channel_open(&self, name: &str, size: u32) -> Option<Channel> {
        let n = cstring(name);
        let raw = unsafe { f!(self, channel_open)(self.module, n.as_ptr(), size) };
        if raw.is_null() { None } else { Some(Channel { host: *self, raw }) }
    }
}

/// A .4ds model read and parsed (model_open); closed when dropped (or at the unload).
pub struct Model {
    host: Host,
    id: c_int,
}
/// A node's mesh (Model::mesh): the host's arrays, valid while the model is open.
pub struct ModelMesh<'a> {
    raw: sys::MmlModelMesh,
    _model: core::marker::PhantomData<&'a Model>,
}
impl<'a> ModelMesh<'a> {
    pub fn positions(&self) -> &'a [[f32; 3]] {
        slice(self.raw.pos as *const [f32; 3], self.raw.vertices)
    }
    pub fn normals(&self) -> &'a [[f32; 3]] {
        slice(self.raw.normal as *const [f32; 3], self.raw.vertices)
    }
    pub fn uvs(&self) -> &'a [[f32; 2]] {
        slice(self.raw.uv as *const [f32; 2], self.raw.vertices)
    }
    pub fn triangles(&self) -> &'a [[u16; 3]] {
        slice(self.raw.index as *const [u16; 3], self.raw.triangles)
    }
    /// each triangle's material, 1-based (materials()'s index + 1), 0 none
    pub fn materials(&self) -> &'a [u16] {
        slice(self.raw.material, self.raw.triangles)
    }
}
impl Model {
    pub fn id(&self) -> i32 {
        self.id
    }
    pub fn nodes(&self) -> Vec<ModelNode> {
        let get = f!(self.host, model_nodes);
        let n = unsafe { get(self.host.module, self.id, ptr::null_mut(), 0) }.max(0) as usize;
        let mut v = vec![ModelNode::default(); n];
        let n = (unsafe { get(self.host.module, self.id, v.as_mut_ptr(), v.len() as c_int) }.max(0) as usize).min(v.len());
        v.truncate(n);
        v
    }
    /// a node's mesh (lod 0 the finest)
    pub fn mesh(&self, node: i32, lod: i32) -> Option<ModelMesh<'_>> {
        let mut m = sys::MmlModelMesh::default();
        if unsafe { f!(self.host, model_mesh)(self.host.module, self.id, node, lod, &mut m) } != 0 {
            Some(ModelMesh { raw: m, _model: core::marker::PhantomData })
        } else {
            None
        }
    }
    pub fn materials(&self) -> Vec<ModelMaterial> {
        let get = f!(self.host, model_materials);
        let n = unsafe { get(self.host.module, self.id, ptr::null_mut(), 0) }.max(0) as usize;
        let mut v = vec![ModelMaterial::default(); n];
        let n = (unsafe { get(self.host.module, self.id, v.as_mut_ptr(), v.len() as c_int) }.max(0) as usize).min(v.len());
        v.truncate(n);
        v
    }
}
impl Drop for Model {
    fn drop(&mut self) {
        if let Some(close) = host_fn!(self.host, model_close) {
            unsafe { close(self.host.module, self.id) }
        }
    }
}

struct FileWalk<F> {
    f: F,
    panic: Option<Box<dyn std::any::Any + Send>>,
}
// Called from inside the host's own loop: a panic must not unwind through it. It stops the walk and goes on
// unwinding once game_files has returned.
unsafe extern "C-unwind" fn on_file<F: FnMut(&str, &str) -> bool>(name: *const c_char, source: *const c_char, user: *mut c_void) -> c_int {
    let w = &mut *(user as *mut FileWalk<F>);
    let (n, s) = (crate::text(name), crate::text(source));
    match std::panic::catch_unwind(std::panic::AssertUnwindSafe(|| (w.f)(&n, &s))) {
        Ok(stop) => stop as c_int,
        Err(p) => {
            w.panic = Some(p);
            1
        }
    }
}

impl Host {
    // ---- a mod's own files in the game
    /// A game file answered from a file of the mod's choosing (absolute, or relative to the mod's folder); None:
    /// taken back
    pub fn file_add(&self, name: &str, path: Option<&str>) -> bool {
        let n = cstring(name);
        let p = path.map(cstring);
        unsafe { f!(self, file_add)(self.module, n.as_ptr(), p.as_ref().map_or(ptr::null(), |p| p.as_ptr())) != 0 }
    }
    /// Who answers a name now: Some(a mod's file's full path), None the game's own
    pub fn file_source(&self, name: &str) -> Option<String> {
        let n = cstring(name);
        let mut b = vec![0 as c_char; 1024];
        if unsafe { f!(self, file_source)(n.as_ptr(), b.as_mut_ptr(), b.len() as c_int) } == 1 { Some(crate::c_text(&b)) } else { None }
    }
    /// A file read as the game reads it (flags 0: a mod's file of that name first; sys::MML_FILE_GAME: the game's own)
    pub fn file_read(&self, name: &str, flags: i32) -> Option<Vec<u8>> {
        let n = cstring(name);
        let read = f!(self, file_read);
        let size = unsafe { read(self.module, n.as_ptr(), ptr::null_mut(), 0, flags) };
        if size < 0 {
            return None;
        }
        let mut v = vec![0u8; size as usize];
        let got = unsafe { read(self.module, n.as_ptr(), v.as_mut_ptr() as *mut c_void, v.len(), flags) };
        if got < 0 {
            return None;
        }
        v.truncate(got.min(size) as usize);
        Some(v)
    }

    // ---- the game's files and models
    /// Every file the game can open matching `pattern` ("models\\*.4ds"; None every one): f(name, source) for
    /// each in order ("a2.dta", "folder", a mod's id), true to stop; how many matched
    pub fn game_files<F: FnMut(&str, &str) -> bool>(&self, pattern: Option<&str>, f: F) -> i32 {
        let p = pattern.map(cstring);
        let tramp = as_c!(on_file::<F>, *const c_char, *const c_char, *mut c_void; c_int);
        let mut walk = FileWalk { f, panic: None };
        let n = unsafe {
            f!(self, game_files)(self.module, p.as_ref().map_or(ptr::null(), |p| p.as_ptr()), tramp, &mut walk as *mut FileWalk<F> as *mut c_void)
        };
        if let Some(p) = walk.panic {
            std::panic::resume_unwind(p);
        }
        n
    }
    /// The file a model frame came from ("Models\\Tommy.i3d"; any frame: the nearest model above it)
    pub fn model_file(&self, frame: Frame) -> Option<String> {
        let mut b = vec![0 as c_char; 512];
        if unsafe { f!(self, model_file)(self.module, frame.as_ptr(), b.as_mut_ptr(), b.len() as c_int) } > 0 { Some(crate::c_text(&b)) } else { None }
    }
    /// A .4ds read through the game's file layer and parsed ("Tommy", "models\\Tommy.4ds")
    pub fn model_open(&self, file: &str) -> Option<Model> {
        let f = cstring(file);
        let id = unsafe { f!(self, model_open)(self.module, f.as_ptr()) };
        if id == 0 { None } else { Some(Model { host: *self, id }) }
    }

    // ---- a mod's state in the game's saves (scope sys::MML_SAVE_SLOT: the game being played; _PROFILE)
    /// data empty: the key removed
    pub fn save_put(&self, scope: i32, key: &str, data: &[u8]) -> bool {
        let k = cstring(key);
        let p = if data.is_empty() { ptr::null() } else { data.as_ptr() as *const c_void };
        unsafe { f!(self, save_put)(self.module, scope, k.as_ptr(), p, data.len()) != 0 }
    }
    pub fn save_get(&self, scope: i32, key: &str) -> Option<Vec<u8>> {
        let k = cstring(key);
        let get = f!(self, save_get);
        let n = unsafe { get(self.module, scope, k.as_ptr(), ptr::null_mut(), 0) };
        if n == 0 {
            return None;
        }
        let mut v = vec![0u8; n];
        let n = unsafe { get(self.module, scope, k.as_ptr(), v.as_mut_ptr() as *mut c_void, v.len()) };
        v.truncate(n);
        Some(v)
    }

    // ---- statistics and the profile's progress
    /// What the player did, counted by the loader (scope sys::MML_SAVE_SLOT the game being played, _PROFILE totals)
    pub fn stats(&self, scope: i32) -> Option<Stats> {
        let mut s = Stats { size: core::mem::size_of::<Stats>() as u32, ..Default::default() };
        if unsafe { f!(self, stats_get)(self.module, scope, &mut s) } != 0 { Some(s) } else { None }
    }
    /// The game's own progress of the profile selected
    pub fn profile_progress(&self) -> Option<Progress> {
        let mut p = Progress { size: core::mem::size_of::<Progress>() as u32, ..Default::default() };
        if unsafe { f!(self, profile_progress)(self.module, &mut p) } != 0 { Some(p) } else { None }
    }
    /// The slots of the profile's saves on disk, ascending
    pub fn profile_saves(&self) -> Vec<i32> {
        let mut v = vec![0; 600];
        let n = unsafe { f!(self, profile_saves)(self.module, v.as_mut_ptr(), v.len() as c_int) }.clamp(0, 600) as usize;
        v.truncate(n);
        v
    }

    // ---- the script machine
    /// sys::MML_SCRIPT_* of one of the mod's programs (-1: none such)
    pub fn script_state(&self, handle: i32) -> i32 {
        unsafe { f!(self, script_state)(self.module, handle) }
    }

    // ---- services: a versioned table of functions a mod offers, other mods find
    /// The table lives as long as the mod (a static); `T` is the table's C layout (`#[repr(C)]`, extern "C" fns)
    pub unsafe fn service_publish<T>(&self, name: &str, version: u32, table: &'static T) -> bool {
        let n = cstring(name);
        f!(self, service_publish)(self.module, n.as_ptr(), version, table as *const T as *const c_void) != 0
    }
    /// Another mod's table and its version (at least min_version). Valid until MML_EVENT_SERVICES_CHANGED: find it
    /// again then; `T` must be the layout its publisher documents.
    pub unsafe fn service_find<T>(&self, name: &str, min_version: u32) -> Option<(&'static T, u32)> {
        let n = cstring(name);
        let mut version = 0u32;
        let p = f!(self, service_find)(n.as_ptr(), min_version, &mut version) as *const T;
        p.as_ref().map(|t| (t, version))
    }
}

#[cfg(feature = "json")]
impl Host {
    pub fn save_put_json<T: serde::Serialize>(&self, scope: i32, key: &str, value: &T) -> bool {
        serde_json::to_vec(value).map(|v| self.save_put(scope, key, &v)).unwrap_or(false)
    }
    pub fn save_get_json<T: serde::de::DeserializeOwned>(&self, scope: i32, key: &str) -> Option<T> {
        serde_json::from_slice(&self.save_get(scope, key)?).ok()
    }
}
