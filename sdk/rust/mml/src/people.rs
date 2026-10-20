//! People: skeletons and poses, animations and getting up, the inventory's rounds, a mod's own items, their skinning
//! (docs/MODDING.md "People, skeletons, poses", "Animations, getting up", "A mod's own items", "People's skinning").
use crate::{cstring, sys, text, Actor, Frame, Host};
use core::ffi::c_int;
use core::ptr;

/// A bone of a person's skeleton (human_skeleton), in sys::MML_BONE_* order.
#[derive(Clone, Debug)]
pub struct Bone {
    /// "pelvis", "back1"... "root"
    pub name: String,
    /// its LS3D frame; None: this model has no such bone
    pub frame: Option<Frame>,
    /// its world matrix now (row vectors)
    pub world: [f32; 16],
    /// its local rotation (w x y z) and position, in its parent's space
    pub rot: [f32; 4],
    pub pos: [f32; 3],
}

/// What a person plays (human_anim_state).
#[derive(Clone, Debug, Default)]
pub struct HumanAnim {
    /// the game's number of the animation playing, 0 none
    pub playing: i32,
    pub asked: i32,
    /// its file ("" none)
    pub name: String,
    /// 1 his own life, 4 an animation with a track, 6 his action queue
    pub state: i32,
    /// 1 on the back, -1 on the front, 0 neither
    pub lying: i32,
    pub time_ms: i32,
    pub length_ms: i32,
}

/// A bone of a skin (skin_set): a joint under the mesh by its name in the .4ds ("" the mesh's own space) and the
/// mesh's space at rest to the joint's (row vectors).
#[derive(Clone, Debug)]
pub struct SkinBone {
    pub joint: String,
    pub bind: [f32; 16],
}

/// A LOD's skin (skin_set, skin_get): up to 8 bones a vertex (`count` each, `index` and `weight` 8 a vertex).
#[derive(Clone, Debug, Default)]
pub struct SkinLod {
    pub bones: Vec<SkinBone>,
    pub count: Vec<u8>,
    pub index: Vec<u16>,
    pub weight: Vec<f32>,
}

/// LS3D's own skin of a model's mesh with its rest mesh (skin_get).
#[derive(Clone, Debug, Default)]
pub struct SkinMesh {
    pub mesh: String,
    pub lods: i32,
    pub skin: SkinLod,
    /// the vertices weighted by no bone (the last ones)
    pub rigid: u32,
    pub pos: Vec<f32>,
    pub normal: Vec<f32>,
    pub uv: Vec<f32>,
    pub index: Vec<u16>,
}

/// A mod's own item (item_define).
#[derive(Clone, Debug, Default)]
pub struct ItemDef<'a> {
    /// the mod's name for it ("rod")
    pub name: &'a str,
    /// what the game shows (None: the name)
    pub label: Option<&'a str>,
    /// the model put on the ground when dropped (None: like's)
    pub model: Option<&'a str>,
    /// the game's item it is made like (0: 16, a key)
    pub like: i32,
    /// sys::MML_ITEM_*; -1 like's
    pub flags: i32,
    /// the line when it is picked up (None: "<label> collected")
    pub collected: Option<&'a str>,
}

pub use sys::MmlOrderState as OrderState;

impl Host {
    /// An order for a person with a script (actor_spawn's people, the missions' NPCs): `code` in the missions'
    /// language and the people's own enemy_* commands (`enemy_move_to_frame 0, 1, run`; docs/SCRIPTS.md "Orders"),
    /// put into his own program and run by his AI; act[i] from `acts`, flt[i] from `flts`, frm[i] a frame at
    /// `points[i]`. Its id, None refused (the log says why).
    pub fn human_order(&self, human: Actor, code: &str, acts: &[Option<Actor>], flts: &[f32], points: &[[f32; 3]]) -> Option<i32> {
        let c = cstring(code);
        let acts: Vec<*mut core::ffi::c_void> = acts.iter().map(|a| a.map_or(ptr::null_mut(), Actor::as_ptr)).collect();
        let id = unsafe {
            f!(self, human_order)(
                self.module,
                human.as_ptr(),
                c.as_ptr(),
                acts.as_ptr(),
                acts.len() as c_int,
                flts.as_ptr(),
                flts.len() as c_int,
                points.as_ptr() as *const f32,
                points.len() as c_int,
            )
        };
        if id > 0 { Some(id) } else { None }
    }
    /// Where his order stands; None: no order of the loader's for him
    pub fn human_order_state(&self, human: Actor) -> Option<OrderState> {
        let mut s = OrderState { size: core::mem::size_of::<OrderState>() as u32, ..Default::default() };
        if unsafe { f!(self, human_order_state)(self.module, human.as_ptr(), &mut s) } != 0 { Some(s) } else { None }
    }
    /// The game's road network: the node nearest to `at` (what its drives, `enemy_car_moveto`, run between): its
    /// index and place; None: no mission, or a mission with no road network
    pub fn road_near(&self, at: [f32; 3]) -> Option<(i32, [f32; 3])> {
        let mut out = [0.0f32; 3];
        let id = unsafe { f!(self, road_near)(self.module, at.as_ptr(), out.as_mut_ptr()) };
        if id > 0 { Some((id - 1, out)) } else { None }
    }

    /// A person's bones (sys::MML_BONE_COUNT, in MML_BONE_* order); None: not a person known, or no skeleton
    pub fn human_skeleton(&self, human: Actor) -> Option<Vec<Bone>> {
        let mut b = vec![sys::MmlBone::default(); sys::MML_BONE_COUNT as usize];
        let n = unsafe { f!(self, human_skeleton)(self.module, human.as_ptr(), b.as_mut_ptr(), b.len() as c_int) };
        if n <= 0 {
            return None;
        }
        Some(
            b.iter()
                .map(|b| Bone { name: unsafe { text(b.name) }, frame: Frame::from_ptr(b.frame), world: b.world, rot: b.rot, pos: b.pos })
                .collect(),
        )
    }
    /// A bone's local rotation (w x y z) and/or position written, its world matrix made again: in RENDER_BEGIN,
    /// every frame the pose is to be drawn
    pub fn bone_set(&self, frame: Frame, rot: Option<[f32; 4]>, pos: Option<[f32; 3]>) -> bool {
        let r = rot.as_ref().map_or(ptr::null(), |r| r.as_ptr());
        let p = pos.as_ref().map_or(ptr::null(), |p| p.as_ptr());
        unsafe { f!(self, bone_set)(self.module, frame.as_ptr(), r, p) != 0 }
    }
    /// The actors within `radius` of a point, nearest first (type 0 any, 2 a person, 4 a car)
    pub fn world_near(&self, at: [f32; 3], radius: f32, kind: i32) -> Vec<Actor> {
        let near = f!(self, world_near);
        let mut v = vec![ptr::null_mut(); 64];
        let mut n = unsafe { near(self.module, at.as_ptr(), radius, kind, v.as_mut_ptr(), v.len() as c_int) }.max(0) as usize;
        if n > v.len() {
            v.resize(n, ptr::null_mut());
            n = (unsafe { near(self.module, at.as_ptr(), radius, kind, v.as_mut_ptr(), v.len() as c_int) }.max(0) as usize).min(v.len());
        }
        v.truncate(n);
        v.into_iter().filter_map(Actor::from_ptr).collect()
    }

    // ---- animations
    /// An animation of anims\ played as a mission plays one (flags sys::MML_ANIM_SIMPLE): its length in ms, 0 no
    /// such file, -1 refused
    pub fn human_anim_play(&self, human: Actor, anim: &str, flags: i32) -> i32 {
        let a = cstring(anim);
        unsafe { f!(self, human_anim_play)(self.module, human.as_ptr(), a.as_ptr(), flags) }
    }
    /// his own animation again at once
    pub fn human_anim_stop(&self, human: Actor) -> bool {
        unsafe { f!(self, human_anim_stop)(self.module, human.as_ptr()) > 0 }
    }
    /// one of his own animations played as another from now on (with None: his own again)
    pub fn human_anim_replace(&self, human: Actor, own: &str, with: Option<&str>) -> bool {
        let o = cstring(own);
        let w = with.map(cstring);
        unsafe { f!(self, human_anim_replace)(self.module, human.as_ptr(), o.as_ptr(), w.as_ref().map_or(ptr::null(), |w| w.as_ptr())) > 0 }
    }
    pub fn human_anim_state(&self, human: Actor) -> Option<HumanAnim> {
        let mut a = sys::MmlHumanAnim { size: core::mem::size_of::<sys::MmlHumanAnim>() as u32, ..Default::default() };
        if unsafe { f!(self, human_anim_state)(self.module, human.as_ptr(), &mut a) } <= 0 {
            return None;
        }
        Some(HumanAnim {
            playing: a.playing,
            asked: a.asked,
            name: unsafe { text(a.name) },
            state: a.state,
            lying: a.lying,
            time_ms: a.time_ms,
            length_ms: a.length_ms,
        })
    }
    /// A body the mod posed handed back to the game: he gets up from where and how it lies (blend_ms from the pose)
    pub fn human_get_up(&self, human: Actor, blend_ms: i32) -> bool {
        unsafe { f!(self, human_get_up)(self.module, human.as_ptr(), blend_ms) > 0 }
    }

    // ---- the inventory
    /// the rounds of the item in a slot (-1 leaves one as it is)
    pub fn item_ammo_set(&self, human: Actor, slot: i32, loaded: i32, reserve: i32) -> bool {
        unsafe { f!(self, item_ammo_set)(self.module, human.as_ptr(), slot, loaded, reserve) != 0 }
    }
    /// an item taken from a person (every slot with it)
    pub fn human_take_item(&self, human: Actor, item: i32) -> bool {
        unsafe { f!(self, human_take_item)(self.module, human.as_ptr(), item) != 0 }
    }
    /// A mod's own item in the game's items' table; its id (kept for its name across runs)
    pub fn item_define(&self, def: &ItemDef) -> Option<i32> {
        let s = |o: Option<&str>| o.map(cstring);
        let (name, label, model, collected) = (cstring(def.name), s(def.label), s(def.model), s(def.collected));
        let p = |c: &Option<std::ffi::CString>| c.as_ref().map_or(ptr::null(), |c| c.as_ptr());
        let d = sys::MmlItemDef {
            size: core::mem::size_of::<sys::MmlItemDef>() as u32,
            name: name.as_ptr(),
            label: p(&label),
            model: p(&model),
            like: def.like,
            flags: def.flags,
            collected: p(&collected),
        };
        let id = unsafe { f!(self, item_define)(self.module, &d) };
        if id > 0 { Some(id) } else { None }
    }
    /// the id of "mod/name" (any mod's; "name": this mod's)
    pub fn item_find(&self, name: &str) -> Option<i32> {
        let n = cstring(name);
        let id = unsafe { f!(self, item_find)(self.module, n.as_ptr()) };
        if id > 0 { Some(id) } else { None }
    }

    // ---- skinning
    /// A model's single mesh ("Tommy", "base") skinned with these LODs (LOD i the mesh's LOD i), every person of the
    /// model alike, from its next skinning; an empty list takes the mod's skin of it away. Err: why it was refused.
    pub fn skin_set(&self, model: &str, mesh: &str, lods: &[SkinLod]) -> Result<(), String> {
        let (m, me) = (cstring(model), cstring(mesh));
        let names: Vec<Vec<std::ffi::CString>> = lods.iter().map(|l| l.bones.iter().map(|b| cstring(&b.joint)).collect()).collect();
        let bones: Vec<Vec<sys::MmlSkinBone>> = lods
            .iter()
            .zip(&names)
            .map(|(l, n)| l.bones.iter().zip(n).map(|(b, c)| sys::MmlSkinBone { joint: c.as_ptr(), bind: b.bind }).collect())
            .collect();
        let raw: Vec<sys::MmlSkinLod> = lods
            .iter()
            .zip(&bones)
            .map(|(l, b)| sys::MmlSkinLod {
                vertices: l.count.len() as u32,
                bones: b.len() as u32,
                bone: b.as_ptr(),
                count: l.count.as_ptr(),
                index: l.index.as_ptr(),
                weight: l.weight.as_ptr(),
            })
            .collect();
        for l in lods {
            let v = l.count.len() * sys::MML_SKIN_INFLUENCES as usize;
            if l.index.len() < v || l.weight.len() < v {
                return Err("index and weight: 8 a vertex".into());
            }
        }
        let mut err = [0 as core::ffi::c_char; 512];
        let r = unsafe {
            f!(self, skin_set)(self.module, m.as_ptr(), me.as_ptr(), raw.as_ptr(), raw.len() as c_int, err.as_mut_ptr(), err.len() as c_int)
        };
        if r != 0 { Ok(()) } else { Err(unsafe { text(err.as_ptr()) }) }
    }
    /// Every person skinned on the GPU from the next frame while any mod wants it (false: no more); false when the
    /// device cannot (skin.state's gpu tells why)
    pub fn skin_gpu(&self, on: bool) -> bool {
        unsafe { f!(self, skin_gpu)(self.module, on as c_int) != 0 }
    }
    /// LS3D's own skin of a model's mesh (None: its first single mesh) and LOD, from its .4ds, with its rest mesh
    pub fn skin_get(&self, model: &str, mesh: Option<&str>, lod: i32) -> Option<SkinMesh> {
        let m = cstring(model);
        let me = mesh.map(cstring);
        let mut o: sys::MmlSkinMesh = unsafe { core::mem::zeroed() };
        if unsafe { f!(self, skin_get)(self.module, m.as_ptr(), me.as_ref().map_or(ptr::null(), |c| c.as_ptr()), lod, &mut o) } == 0 {
            return None;
        }
        let v = o.skin.vertices as usize;
        let k = sys::MML_SKIN_INFLUENCES as usize;
        unsafe {
            let sl = |p: *const f32, n: usize| if p.is_null() { Vec::new() } else { core::slice::from_raw_parts(p, n).to_vec() };
            let bones = core::slice::from_raw_parts(o.skin.bone, o.skin.bones as usize)
                .iter()
                .map(|b| SkinBone { joint: text(b.joint), bind: b.bind })
                .collect();
            Some(SkinMesh {
                mesh: text(o.mesh.as_ptr()),
                lods: o.lods,
                skin: SkinLod {
                    bones,
                    count: core::slice::from_raw_parts(o.skin.count, v).to_vec(),
                    index: core::slice::from_raw_parts(o.skin.index, v * k).to_vec(),
                    weight: core::slice::from_raw_parts(o.skin.weight, v * k).to_vec(),
                },
                rigid: o.rigid,
                pos: sl(o.pos, v * 3),
                normal: sl(o.normal, v * 3),
                uv: sl(o.uv, v * 2),
                index: if o.index.is_null() { Vec::new() } else { core::slice::from_raw_parts(o.index, o.triangles as usize * 3).to_vec() },
            })
        }
    }
}
