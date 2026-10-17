//! People: skeletons and poses, animations and getting up, the inventory's rounds, a mod's own items
//! (docs/MODDING.md "People, skeletons, poses", "Animations, getting up", "A mod's own items").
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

impl Host {
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
}
