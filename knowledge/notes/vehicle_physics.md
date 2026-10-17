# A car's physics (C_Vehicle): the step, the wheels, rest

How a car moves, read in 1.2's code and measured in the 1.2 lab (2026-10-15, while fixing what breaks above
62.5 fps; `mods/examples/fps_unlock`). Offsets are the `C_Vehicle`'s (`C_car + 0x70`) unless said; the fields
already in `game.md` "Cars" (position, velocity, speed, steering, gear, engine, fuel) are not repeated. **code**:
read in the disassembly at the address named; **game**: seen in the running game; **probe**: measured with
`fps_unlock.stepprobe` (below). The decompile of `Move` and `DoWheelsCollision` is unreliable (Hex-Rays' stack
analysis failed in both: parameters are misnamed and uses of the step are missing): read the disassembly.

## The step, from the frame to the integrator

- **code** `C_car::AI` 0x41F780 (thiscall(car, ms)) sets the controls (`SetPower`, `SetBrake`, `SetSteer`,
  `SetEngineOn`) and calls `C_Vehicle::Tick` 0x4DF9A0 (thiscall(vehicle, ms) -> its flags' low byte, ret 4; 0 ms is
  taken as 1) at 0x41FA2A on `car + 0x70`. The player's input (`C_player::5917F0`) sets the same controls every
  frame. `C_Vehicle::CalmVehicle` 0x4CE960 also calls Tick, with 83 ms, in a loop until the car is calm (or a time
  is up), when cars are put in place (mission start, scripts).
- **code** Tick runs the drivetrain (gears, engine turns, `MotorRot` 0x4E19D0) with `ms * 0.001`, then calls
  `C_Vehicle::Engine` 0x4E1CE0 (thiscall(vehicle, dt s, f1, f2), ret 0xC; 1.0 0x52E6D0) at 0x4E08FC. Engine runs
  `C_Vehicle::Move` 0x4E2090 (thiscall, 6 stack arguments: dt first, ret 0x18) in equal substeps no longer than
  `+0xC24` (0.028 s for the taxi): `n = dt / 0.028 + 1` when dt is longer, one Move of dt otherwise. So at 62.5 fps
  one Move of 16 ms a frame; at 400 fps one of 2.5 ms. At its end Engine stores dt at `+0x4C8` (1.0 `+0x544`).
- **code** `+0x4C8` is read by the controls' ramps, each call: `SetBrake` 0x4CB2D0 and `SetPower` in reverse
  (`+0x3CC += +0x4C8`, the brake's ramp, up to `+0x3D0`), `SetHandbrake` 0x4CB710 (the handbrake's). They are
  called once a frame, so they ramp by the time as long as Engine's step is the frame's.
- **code** Move, in order (the addresses of the parts read so far): the wheels' tyre factors (`+0xC8 += +0xCC * dt`
  per wheel, 0x4E2244..; time-correct), the wheels' contact (`DoWheelsCollision` 0x4E6AF0, twice, at 0x4E65D0 and
  0x4E6913: suspension and contact per wheel), the tyre forces per wheel (0x4E2FEE..0x4E3700, below), the
  angular velocity `+0x420 += torque * dt` (0x4E494A; time-correct), the displacement `v * dt` and the rotation
  `w * dt` (0x4E49A8..), the odometer `+0x5B4 += |v dt|`, the body's collision (`BodyCollision` 0x4E8AF0 at
  0x4E552F, thiscall, 7 arguments; called when the angular speed is over 0.05 rad/s or a flag says so, returns
  whether it touched), the position `+0x320 += displacement`, plus up to two position corrections from the
  contacts, each applied only when shorter than 0.05 m (`|c|^2 < 0.0025`, 0x4E5EF1, 0x4E5F4C), the steering
  wheels (`UpdateSteeringWheels` 0x4DDC60 with `dt * 1000`, 0x4E630A), and the rest test (below).

## Rest: when a car stops and sleeps

- **code** Move 0x4E6322..0x4E676A: when `|velocity +0x1F90| < 0.01` m/s and `|angular +0x420| < 0.015` rad/s
  (and the byte `+0x394` is set, `+0xC60` is not, and `+0x3A4` < 0.3), it zeroes the velocity, the angular velocity
  and the speed `+0x59C` and sets the flag 2 ("calm") in `+0x1A4` (1.0 `+0x224`).
- **code** Tick's end 0x4E09A0..0x4E0A60: if the car is within 0.15 m of a reference point `+8` (dist^2 < 0.0225)
  a counter `+4` adds the ms, otherwise the point moves to the car and the counter restarts. A second counter
  `+0x1A8` adds the ms while the flag 2 is set and restarts otherwise; at 1000 ms of it, or 4000 ms of `+4`, the
  flag 1 ("awake") is cleared: the car sleeps. `+0x1A4`'s bits seen: 1 awake, 2 calm, 8 off the ground,
  0x1000000 braking by itself, 0x10000000 forced on. Tick clears the flag 2 at its start.
- All of it in m/s and ms: rest itself does not depend on the step. What does is the motion the car has left
  (below).

## The wheels (S_wheel)

- **code** A wheel is 0x1D4 bytes (`new` at 0x4D0DD2, linked into a list by `+0x1D0`); the vehicle keeps an array
  of pointers at `+0xC38` and the count at `+0x4C4`. Read in `Move`, `DoWheelsCollision` and filled by the car's
  table (values of the taxi's front-left wheel, **probe**):
  - `+0x04`, `+0x08`: the wheel's frames (the second is the steered one, `UpdateSteeringWheels` writes its
    rotation `+0x9C` and flags `+0xAC`).
  - `+0x34..+0x3C`: a vector whose y `+0x38` is the wheel's load (N; 4400 standing: a quarter of the taxi's
    weight); `+0xA0`, `+0xA4` follow the load (x0.76, x0.64 standing on asphalt): the friction limits along and
    across the wheel (`+0xA4` caps the side force, 0x4E339C).
  - `+0x4C..+0x54`: the contact point's velocity; `+0x58..+0x60`: the wheel's rolling direction. The velocity's
    part across that direction is the side slip, and `+0x108` the sine of the angle between the two (0x4E2FEE..
    0x4E30A1, `fsin` of the angle 0x60FC7E returns).
  - `+0x7C`, `+0x84`, `+0x17C`, `+0x180`: written in Move 0x4E3D3B..0x4E3F9D and by the drivetrain; probably the
    wheel's spin and its rate (not read yet).
  - `+0x88` the spring's stiffness (68000 N/m), `+0x8C` its free length (0.2 m), `+0x90` the suspension force
    (N: `(free length - compression) * stiffness`, 0x4E83A7, plus the damper: the compression's speed times
    `+0x98` (1400) or `+0x9C` (2600), one or the other by a flag and its sign, 0x4E83D4 / 0x4E86AC), `+0x94` the
    last compression (m); the compression's speed is `(last - now) / dt` (DoWheelsCollision 0x4E7D37: the only
    read of dt there).
  - `+0xB8`..`+0x104`: the tyre's table (12000, 0.157 rad = 9 degrees: the slip angle of full grip, 0.9, 11.1,
    0.7, ...), the same for each wheel of a model.
  - `+0x120` flags: 8 on the ground (cleared when the suspension force is not positive, 0x4E8401), 4 the
    handbrake's wheel (with the vehicle's `+0x434`, the handbrake set by `SetHandbrake` 0x4CB75B), 0x40 touched
    this step, 0x40000000 off (skipped by every loop; not seen set). 0x10000 flips the sign of a force (0x4E34B8),
    meaning not known.
  - `+0x128` a byte, `+0x12C..+0x134` zeroed at creation; `+0x18C`, `+0x190` the tyre's health (100).
- **code** The tyre's side force (0x4E2FEE..0x4E33C2): the contact point's velocity across the rolling direction,
  made a unit vector; times the slip's sine over the full-grip angle (`+0xBC`, or `+0xE8` when the flag 0x80000000
  of `+0x120` is set; a surface's own value from the `[0x63D9C8]` tables when the wheel stands on a material),
  capped at 1; times the side limit `+0xA4`. Made of velocities and loads only: as written, it does not depend on
  the step.

## Small rotations are lost (the cause of what breaks above 62.5 fps)

- **code** Move turns the body each substep by the angular velocity: its parts in the body's axes times dt, made
  quaternions by `S_quat::Make` (LS3DF `?Make@S_quat@@QAGXABUS_vector@@M@Z`, 0x1002EE00: `w = cos(a/2)`, the axis
  times `sin(a/2)`, normalized), combined, made a matrix by `S_quat::RotationMatrix` (LS3DF
  `?RotationMatrix@S_quat@@QBG?AUS_matrix@@XZ`, 0x1002EC00, stdcall(quat, out matrix) -> out, ret 8; called at
  0x4E4D5D and 0x4E5991) and applied to the axes `+0xC3C` (forward: the model frame's z row), `+0xC48` (right: its
  x row), `+0xC54` (up: its y row) (`S_vector *= S_matrix`; **game**: read against the frame's world matrix; in 1.0
  `+0xCB8`, `+0xCC4`, `+0xCD0`, and the angular velocity `+0x49C`: 1.0's fields from `+0x1A4` on are 1.2's plus
  0x7C where checked).
- **code** `RotationMatrix` builds the matrix only when `|w| < 1` and returns the identity otherwise. A small
  rotation's `w = cos(a/2)` is exactly 1.0 as a float once `(a/2)^2 / 2` is under half a float's step below 1
  (2.98e-8): any turn under about 0.00049 rad is dropped whole, though its vector part is not zero.
- **probe** Measured: from one creeping state, in 4 ms steps the axes stayed the same to the last bit for 54 steps
  with an angular speed of 0.07..0.12 rad/s, and moved only in the steps where `|w| * dt` passed 0.00049; in 16 ms
  steps they moved until `|w|` fell under 0.03 rad/s (0.03 * 0.016 = 0.00049), then froze too.
- So the body cannot turn slower than 0.00049 rad a step: 0.03 rad/s at 62.5 fps (hardly visible), 0.2 rad/s at
  400 fps. The car's slow yaw (steering a little: a dead zone) and its rocking on the springs are not applied; its
  angular velocity grows unanswered (the springs never see the turn that would damp it), so a stopped car never
  falls under the 0.015 rad/s of rest and creeps.
- **The fix** (fps_unlock `fix_rotation`): the export hooked; a quaternion with `|w|` rounded to 1 (at most 2 float
  steps above it) and a short vector part (`|v|^2 < 1e-6`) gets its matrix by the game's own formula (`1 - 2(y^2 +
  z^2)`, `2(xy - wz)`, ..., checked against the game's function at load on a quaternion it builds); everything else
  goes to the game's function. With it, the frame's own steps give 62.5 fps's stop and steering (the numbers in
  `game.md`, "The frame rate"): no other change to the car's physics was needed for them.
- **code** Every caller of `RotationMatrix` (through the import thunk 0x60FC84 in 1.2, 0x624316 in 1.0; 26 sites in
  1.2, 35 in 1.0), 1.2's by function: `C_airplane::AI_fly` 0x40AE30, `C_airplane::GetEnginesPos`, `C_bottle::AI` /
  `PhysStep` 0x4123F0, `box_track_record_key`, `C_car_ex`'s property callback, `C_detector::CreateColls` and its
  dummy callback, `C_traffic_element::Tick` 0x452CD0 / `Activate` / `SetAnim` (pedestrians and trams), `C_PhThing::
  MoveCount` 0x45DDB0 (physical things), `C_program::Process` (three script commands), 0x4BC830 (twice, unnamed),
  `vehicle_init_strip_and_smoke_emitters`, `C_Vehicle::Move` (twice), `C_Vehicle::DoWheelsCollision` 0x4E6AF0,
  `ai_action_manager::NewPoliceCall`, `C_human::SaveGameLoad`, `C_human::PoseInterpolate` 0x578EE0 (three times),
  0x5BB570 (unnamed). **game** (`tap`, the freeride, no police): driving calls it from `Move` only (0x4E4D5D: the
  body's turn), walking adds `PoseInterpolate`'s three a frame, which convert a bone's absolute rotation to read its
  direction and turn it by `ms * 0.00314` rad (by the time): there the fix makes a near-identity rotation exact
  instead of the identity (under 0.00049 rad of difference). Those that turn by a step's angle and may have the
  car's limit, not watched: the airplane, `C_PhThing::MoveCount`, the bottle's physics, the traffic's `Tick`.

## What depends on the step (above 62.5 fps), measured

- **game** A stopped car creeps: at 400 fps the taxi circles at 0.01..0.03 m/s for 8 s after stopping, at
  62.5 fps it is zeroed 0.07..0.75 s after; the motion left is the body rocking on its springs (angular speed
  0.07..0.16 rad/s around the pitch and roll axes, turning with a period of about 4.5 s), above the 0.015 rad/s of
  rest. The engine's idle does not cause it (it creeps the same with the engine off).
- **game** Steering (2026-10-15, the taxi from the stop test's place, 3 s of throttle with 0.15 of `Steer Right`,
  the velocity read along the body's right axis `+0xC48`): the yaw rate over the third second was 0.161 rad/s at
  62.5 fps and 0.019..0.055 at 350 fps unfixed, the sideways velocity -0.002 and +0.15 m/s: the slow yaw was lost.
  (A first measurement read -1.86 against -0.49 m/s "sideways": by its size the world's x velocity, which is the
  heading as much as the slip.) Fixed (above): 0.161..0.167 rad/s, -0.02..-0.01 m/s.
- **probe** From one state, one 16 ms Tick against four of 4 ms: the body's velocity and angular velocity change
  the same (to the integration's error) both driving and creeping, with the rotation fix too (driving and steering
  at 6.7 m/s: the angular velocity -0.0225 / -0.0241, the speed +0.0429 / +0.0422 m/s); the wheels' spin fields
  (`+0x7C`, `+0x84`, `+0x17C`) and the vehicle's `+0x548`, `+0x54C`, `+0xE5C`, `+0xE60` change differently (up to
  4 times as much in the four short steps). Whether they are integrated once a call or only oscillate fast (a
  phase difference from one state) is not known; the car's speed, stop and turn measured the same over seconds.
  Next to read: Move 0x4E3C00..0x4E3FA0, and their means over a drive (`fps_unlock.path` with `dump_wheel`).

## In the SDK (2026-10-15)

- `game_build.h`, both builds, each checked in their code by `tools/check_builds.py`: `vehicleTick`, `vehicleEngine`,
  `vehicleMove`, `vehicleWheelsColl` (to hook or call), `vehicleFlags`, `vehicleStepTime`, `vehSubstepMax`,
  `vehAxes`, `vehTurn` (the angular velocity), `vehWheels`, `vehWheelCount`, `carSize`; LS3DF's `quatMake`,
  `quatRotationMatrix`, `matrixSetRot3` (each its export, and the two |w| tests).
- `game.h`: `Car::Vehicle`, `AngularVelocity`, `Forward` / `Right` / `Up`, `Flags`, `Awake`, `Calm`, `StepTime`,
  `SubstepMax`, `WheelCount`, `Wheel(i)`; the wheel's `kWheel*` fields (the suspension's checked in both builds' code;
  the frames and the load seen in both games); `mathx.h` `QuatMatrix`, RotationMatrix's formula without its loss.
- The core's `car` command lists the body (`body`: awake, calm, angular velocity, axes, step) and each wheel
  (frame, on the ground, load, force, compression, spring): seen the same in the 1.2 and 1.0 labs on the
  freeride's taxi.
- **code** `S_matrix::SetRot3` (LS3DF 1.2 0x1002E070, about 90 callers, LS3DF's own frames among them) and `SetRot`
  (it calls SetRot3) have the same loss: the identity when `|w| >= 0.99999999`, for a float `w == 1`. Their callers
  were not read: where one builds a matrix from an absolute rotation, a rotation within 0.00049 rad of none is made
  none at every frame rate; where one turns by a step's angle, it has the car's limit. Not fixed.

## Tools for this (fps_unlock)

- `fps_unlock.path {"start": true, "vehicle_offset": n, "dump_wheel": i, "dump_bytes": n}`: the player's car
  every frame (position, speed, velocity, 3 floats of the vehicle at an offset); a wheel's (or the vehicle's)
  memory every frame, read back as each float's mean over a window (`{"dump_from": s, "dump_to": s}`, `"dump_keep":
  true` keeps it as the reference, a later read lists what differs by more than 10 %).
- `fps_unlock.stepprobe {"ms": 16, "n": 1, "ms2": 4, "n2": 4, "show": [offsets]}`, then `fps_unlock.stepprobe`:
  at the player's car's next Tick, its state is copied (the vehicle 0x21AC bytes, each wheel 0x1D4, the body's and
  wheels' frames), Tick runs n x ms, the state goes back, n2 x ms2, back again; the floats whose changes differ are
  listed. Checked: 16 x 1 against 16 x 1 differ nowhere (the copy is whole). Sounds, skid marks and dents made
  inside are not undone.

## GetRVelMassJ's matrices are not the same in both builds (read 2026-10-17)

`C_Vehicle::GetRVelMassJ` (1.2 `0x4CC4B0`, 1.0 `0x518500`; check_builds.py's witness for game_build.h's
`vehInertiaA`, `vehInertiaB`) reads two 4x4 matrices: 1.2 the pair at `+0xD68`, `+0xDA8` (after the body's matrix
`+0xCE8` and its inverse `+0xD28`; it multiplies through `0x60FC54`), 1.0 the pair at `+0xD64`, `+0xDA4`, which are
1.0's body matrix and its inverse (game_build.h `vehMatrix`; 1.0 keeps the step before's pair at the C_Vehicle's
start, `vehMatrixPrev` `+0x68`; it copies 16 dwords with `rep movsd`). So the two offsets of each name are what each
build's function reads, not one field: what 1.2's pair holds was not read. Found while carrying st0rm94's fields
(tools/kb_structs.py leaves both out of knowledge/structs).
