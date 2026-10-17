//! Control handed to the mod: keys and actions kept from the game, the player locked, a vehicle's physics (a car's,
//! the game's airplane's) replaced, read and written (docs/MODDING.md "Control").
use super::{guard, keep, release, slot};
use crate::{sys, Actor, Host};
use core::ffi::{c_int, c_uint, c_void};
use core::ptr;

pub use sys::MmlVehicleBody as VehicleBody;

/// A vehicle's step as vehicle_hook gets it: its body before the step, written by the mod when it takes the step.
pub struct VehicleStep<'a>(&'a mut sys::MmlVehicleStep);
impl<'a> VehicleStep<'a> {
    pub fn raw(&mut self) -> &mut sys::MmlVehicleStep {
        self.0
    }
    pub fn car(&self) -> Option<Actor> {
        Actor::from_ptr(self.0.car)
    }
    /// this step's time (s)
    pub fn dt(&self) -> f32 {
        self.0.dt
    }
    /// the player's car
    pub fn player(&self) -> bool {
        self.0.player != 0
    }
    /// sys::MML_VEHICLE_CAR or MML_VEHICLE_AIRPLANE
    pub fn kind(&self) -> i32 {
        let has = self.0.size as usize >= core::mem::offset_of!(sys::MmlVehicleStep, kind) + 4;
        if has { self.0.kind } else { sys::MML_VEHICLE_CAR as i32 }
    }
    pub fn body(&mut self) -> &mut VehicleBody {
        &mut self.0.body
    }
}

unsafe extern "C-unwind" fn on_vehicle<F: Fn(&mut VehicleStep) -> bool>(s: *mut sys::MmlVehicleStep, user: *mut c_void) -> c_int {
    guard("a vehicle hook", || (*(user as *const F))(&mut VehicleStep(&mut *s)) as c_int)
}

impl Host {
    /// The key kept from the game while on (the mod still sees it); let go at the unload
    pub fn key_capture(&self, key: i32, on: bool) -> bool {
        unsafe { f!(self, key_capture)(self.module, key, on as c_int) != 0 }
    }
    /// The action (action_slot's), both its bindings, kept from the game: it reads 0
    pub fn action_capture(&self, slot: i32, on: bool) -> bool {
        unsafe { f!(self, action_capture)(self.module, slot, on as c_int) != 0 }
    }
    /// The player's own control locked (the scripts' PLAYER_LOCKCONTROLS): the mod moves him
    pub fn player_lock(&self, on: bool) -> bool {
        unsafe { f!(self, player_lock)(self.module, on as c_int) != 0 }
    }
    /// f told of every vehicle's step (cars; airplanes too after vehicle_kinds): true, the game's physics skipped
    /// and the body is where it is after the step. One mod at a time; vehicle_unhook.
    pub fn vehicle_hook<F: Fn(&mut VehicleStep) -> bool + 'static>(&self, f: F) -> bool {
        let user = keep(f);
        let ok = unsafe { f!(self, vehicle_hook)(self.module, as_c!(on_vehicle::<F>, *mut sys::MmlVehicleStep, *mut c_void; c_int), user) != 0 };
        if ok { slot("vehicle", user) } else { release(user) }
        ok
    }
    pub fn vehicle_unhook(&self) -> bool {
        let ok = unsafe { f!(self, vehicle_hook)(self.module, None, ptr::null_mut()) != 0 };
        slot("vehicle", ptr::null_mut());
        ok
    }
    /// sys::MML_VEHICLES_CARS | _AIRPLANES
    pub fn vehicle_kinds(&self, kinds: u32) -> bool {
        unsafe { f!(self, vehicle_kinds)(self.module, kinds as c_uint) != 0 }
    }
    /// a vehicle's body now (a car or an airplane)
    pub fn vehicle_body(&self, car: Actor) -> Option<VehicleBody> {
        let mut b = VehicleBody::default();
        if unsafe { f!(self, vehicle_body)(self.module, car.as_ptr(), &mut b, ptr::null()) } != 0 { Some(b) } else { None }
    }
    pub fn vehicle_body_set(&self, car: Actor, body: &VehicleBody) -> bool {
        unsafe { f!(self, vehicle_body)(self.module, car.as_ptr(), ptr::null_mut(), body) != 0 }
    }
}
