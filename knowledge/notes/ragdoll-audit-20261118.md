# Ragdoll port audit, 2026-11-18

Reviewed the working tree of the sibling mafia-ragdoll-mml checkout. Its local build output and the installed 1.0 and 1.2 lab DLLs have SHA-256 `5505A21DB434D840BE05CD2A55C79B022B78DC1E4681B547BDB31FC2F14B7868`. The source tree contains uncommitted changes. This audit used the installed binaries.

## Reproduced in both native builds

| Issue | Experiment and observation | Repair direction |
|---|---|---|
| Opening the driver's door leaves its ragdoll collider closed | In city freeride, capture `actor.meshes` with `hidden_root=true` and `ragdoll.vehicle` for the occupied TomyhoAuto. Run `car_setdooropen 0,0,100` with the car in actor register 0. In both builds the native door bounds moved by up to 0.872 m and the window bounds by 0.799 m. All 20 Jolt part bounds stayed identical. Reloading the mod with the door open picked up the new geometry. In 1.2 door part 10's local X minimum went from -0.75 to -1.62 m. Wheel detachment was outside this experiment. | In `src/vehicles.cpp`, `Body` reuses any populated shape while the model root is the same. Track changes to relevant child poses and membership. Keep the cooked geometry of unchanged parts. |
| The Ragdolls master switch leaves a running simulation active | With an existing dead body, set the mod's `enabled` setting to false. Both builds reported `enabled=false`, `world=true`, and the same body's age rose from 1.30 to 2.11 s. The port README describes this setting as switching the whole mod off. | In `src/core.cpp`, `JoltStep` checks the setting only during world startup. Handle the off transition for existing bodies, camera and owner registrations, including the return of living actors to native animation. |
| Bullets push bodies with the corresponding switch off | Disable `bullets`, kill a spawned civil15, then deliver a type-0 `human.hit` to the corpse. The mod recorded the hit with no registered shape. In a separate frozen-time trial, 80 damage in +X at the chest changed peak part speed from 0.0001 to 1.6433 m/s in 1.0 and from 0.1582 to 1.7496 m/s in 1.2. Body age stayed at 3.53 s. This tests dispatch of a hit to the corpse. Ordinary weapon aiming was outside the trial. | `GiveShapes` checks the setting. `HitFilter` still takes type 0 and calls `Kick`. Apply the setting consistently to delivered bullet impulses. |

## Source findings

- **Time step mismatch.** In `src/jolt_world.cpp`, `ApplyKinematics` and `ApplyCabins` receive the full interval. Jolt's update then caps it at 50 ms. For a 100 ms tick and uniform translation the proxy gets half the requested displacement during the solver step. Body age, hold time and recovery use the full interval in `src/jolt_ragdoll.cpp`. Check contact timing with sustained long ticks before choosing a substep policy.
- **Water classification depends on the first instance.** In `src/scenery.cpp`, `ShapeFor` keys its cache by shared vertex storage and quantized scale/shear, then removes triangles by testing world-space water at the first instance's rotation and translation. Identical geometry placed at the waterline and on land can inherit the same face mask. Actual affected map pieces remain to be identified.
- **Repeated skeleton reads.** A successful `Ragdoll::Snapshot` goes through `Capture` and `PointsFromFrames`, which call the game's world-matrix getter twice per bone for rotation and origin. `Snapshot` repeats both calls for the stored pose. Across 20 bones this is 80 getter calls, plus a model-facing lookup for pedestrians. Read each bone matrix once and derive the points from the stored pose. Keep tick and post-draw snapshots separate, since the engine can animate between them.
- **Unused per-step results.** `CollectPushes` and its bookkeeping feed `CarPushes`, which has no caller in this mod. Actual vehicle reactions consume `CarKicks`. `CarsInWater` walks car bounds and builds maps for splash records every positive physics step, including when there are no floating ragdolls. `TakeSplashes` likewise has no caller. Preserve ragdoll buoyancy and live collision responses when trimming these paths. Frame-time savings need an A/B measurement.

## Regression coverage

The installed trunk-axes, pedestrian-motion and temporary-position test executables all passed. The native runner passed 48 assertions per build for car impacts, walking contacts and anatomy. The standard start sets pedestrian density to zero and skipped two pedestrian cases. Running those two fixtures in city freeride with pedestrians enabled passed another 13 assertions per build. Both lab processes were closed and both ragdoll config override files matched their original bytes after the settings experiments.

## Fixes and verification on the November 18 build

**Clark's Motel (04-03).** Profile 0, save 60 in these labs loads `mise04-motorest`. The old DLL
delivered four extra type-8 vehicle hits around 7.4–7.8 seconds in both builds. The actors were
`FMVSam2`, `FMVPaul_raneny`, `FMVPaul2` and `FMVTom2`. Their animated positions overlapped the car
and the mod manufactured hits for the cinematic doubles. The port now supplements the traffic
manager's car/pedestrian sweep only for traffic pedestrian elements. The game delivers vehicle hits
for mission characters and the player. Contact between an existing physical body and a car still
uses the mesh hull. No mission or model names enter the fix.

On the final DLL, both labs recorded zero bodies and zero synthesized car hits in the opening
15 seconds. All four doubles survived. The separate replay fixture also passed on each lab:
`python -B tests/game/ragdoll_motel.py --profile 0 --save 60` starts and closes the two stopped labs.

**Settings and collision.** Turning off the master switch releases living puppets/knockdowns and
clears physics, camera, watched frames and registrations. Re-enabling resumes tracking. Turning off
bullet pushes removes corpse hit capsules and remembered impulses, and suppresses impulses on delivered
type-0 hits. Door and model-part changes rebuild the affected vehicle hulls. Unchanged hulls stay
cooked; the car and cabin keep their body identities during shape changes. Car wheel spin uses the
existing rolling contact calculation. Both opening and closing the native driver's door updated its
physical bounds in each lab.

**Solver and per-frame work.** Jolt consumes the whole valid mission tick split into collision
substeps. Static pieces cache their water test per placement and share cooked geometry when scale and
the set of excluded faces agree. Each skeleton snapshot samples the 20 bone matrices once, down from
80 calls, plus the existing facing lookup for traffic pedestrians. Tick and post-draw snapshots stay
separate. Removed unused car-push and car-splash outputs and their per-step collection. Native car
reaction, wheel support and body buoyancy still have their original consumers.

**Validation.** Final DLL SHA-256 `EB85004448CB48288F80BB28B15EF935759873B628291741A8BD3F5A2A20028E`
matches both labs and the player archive. In both 1.0 and 1.2, `ragdoll_audit`, `ragdoll_car_impact`,
`ragdoll_contacts`, `ragdoll_joints` and `vehicle_support` passed 70 assertions per build. The
pedestrian dodge and pose-handover fixtures with city pedestrians enabled passed 13 more per build.
Four native test executables passed. The new `ragdoll_world_step` fixture checks per-placement water
classification after swaps and six consecutive 100 ms driven-car steps with a 1 mm tolerance.

**Timing.** In separate single-lab 1.2 runs with 13 tracked people and a 600-frame sample, the mod
averaged 0.264 ms/frame before and 0.255 after. Its mission tick averaged 0.248 and 0.239 ms/frame.
Whole-frame p95 was 18.415 and 18.668 ms respectively. One sample per binary does not establish an
overall frame-rate change. Local raw recordings, profiles and the previous binary are in
`build/ragdoll-fixes-20261118-193814160/`. The archive in the port's `build/dist/` is
`mafia-ragdoll-0.1.0-2026-11-18.zip` (SHA-256 `585f51c64c04ef92022070d33040e924148cd1b2b8e634cee2b387245ecc7869`).

## Railway doorway and first-person recovery

The owner's paused 1.0 freeride had Tommy parented to salina10, camera 17, native air state 3,
feet at carriage Y -17.804 and world Y -24.494. The street at the same X/Z was Y -6.803.
The prior recovery admitted a passenger using render-mesh roof/floor hits and stopped tracking
when the queued get-up changed animation state. The world-space pelvis fit could also slide a
passenger along the floor of a moving carriage after native boarding.

Both builds' C_human::MovementIntern use g_collision::TestLineInVehicle (1.2 0x5C9B80, 1.0
0x584CB0) on the selected carriage's 16-byte grid at railway +0xFC. The native walking probe
starts 0.5 m above local feet and goes down 1.5 m with mask 2. Each trailing carriage has its
own local frame and grid. C_railway::GameInit samples the centre deck at local Y 0.8 down 1.6 m,
which also supplies a reference when a passenger predates the mod's boarding history.

On recovery the port checks the fitted root against that floor. Native boarding queues the
railway camera. With the first-person camera active the port hides Tommy and cancels the
remaining visual pelvis fit. Native exit shows Tommy again. For a descending, unsupported
passenger below the confirmed deck, Intern_UseRailway(false) returns position and facing to
world coordinates. A world query covers only the already crossed vertical interval. Steep
braces can lie above a walkable beam or street. With no crossed landing surface, the native
fall continues with its original speed.

Hot-loading DLL `3ADCB9291E29F0ABE88A669D7B866649BBFBF9B431A453088418CE02C965EF2F`
corrected the recorded incident inside the paused mission. World Y became -6.773 at the same
X/Z, both coordinate readings agreed, the vehicle cleared, camera 1 and the visible player
frame returned, and health stayed 51.292877. The installed and loaded DLLs matched. Before
and after snapshots, native table checks, lab output and original binaries are under
`build/ragdoll-railway-20261118-202853/`. The maintained city regression is
`tests/game/test_ragdoll_railway.py`.

At a curved streetcar section, replaying the owner's local X/Z can put the recovered player
in the path of that same moving tram. Both labs recorded a separate native type 2 hit naming
the tram as attacker at the next movement step. The surface-recovery check therefore reads
health, fall speed and the landing point before that step. The elevated-train case also
continues through landing. Native hits from a moving tram remain part of normal gameplay.
