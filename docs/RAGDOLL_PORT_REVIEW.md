# Ragdoll port review (2026-11-08)

Historical comparison and November 8–9 follow-ups. The November 15 port removes the artificial car-impact
impulse and `car_push` setting entirely; its older impulse descriptions below are not current policy.
Use [STATUS.md](STATUS.md), [the patch notes](PATCHNOTES_2026-11-15.md) and
[car-impact evidence](../knowledge/notes/car-impact-20261115.md) for the current boundary. The port's code
lives in a separate repository; host source publication alone does not update a tester's ragdoll DLL.

The owner reports displaced get-ups, occasional abrupt car impacts, and cars passing through pedestrians
who wave for a taxi. Compare `../mafia-ragdoll` read-only with `../mafia-ragdoll-mml`; do not change the solver
or its tuning without evidence. The installed mod in W:/Mafia10Fixes is initially DLL SHA256
36E042466C59EEA672BDBB084EA118A46B0FCC166CE92310F5400A89A510BB66.

## Confirmed comparison

- Articulated bodies, joint constraints, masses, muscles and knock impulses are substantially unchanged.
  The port additionally handles explosions, vehicle teleports and sleeping-body work. These are not a simpler solver.
- The host's `human_get_up` omitted the reference's animation hold, actual pelvis yaw/position fit and side
  verification. It also projected the pelvis to static ground instead of preserving the posed root's height.
- The reference cancels a previous handover before starting a new ragdoll. The port needs an owner-scoped
  cancellation service, including for traffic pedestrians, without stopping the game's animation.
- The reference follows the pedestrian collision-grid cell while getting up; the host only moves it at the start.
- Back-get-up track lift suppression is restored using the verified shared layout `kHumanTrackLift`:
  PersonAnim 1.2 0x573E50 / 1.0 0x4942C0; EBP=this+0xBC, FSTP [EBP+0x120].
- The new optional `human_get_up_pose` copies the current physics skeleton before its destruction. The port
  maps its historical bone order to SDK order; root support height comes from the actual collision capsules.
- Pedestrian car-hit events carry the element root as pos. The fallback impulse now acts at the body centre,
  matching the reference; applying it at the feet introduced an artificial torque. Normal car/body contacts
  continue to be solved by Jolt, without an additional impulse.

Both builds passed the thirteen handover checks: displaced-pelvis sides, pause, current Jolt pose, re-knockdown,
owner unload and pedestrian grid/native walking. The first tested block was installed into W:/Mafia10Fixes
(host E30C2937..., mod A494E179...) and restarted. Further contact work follows below.

## Taxi-wave collision investigation

In both native builds, pedCarSweep checks an active element's position +0x10, skips +0x1B0 car-wait elements,
and uses +0x174..+0x17C as motion for non-walking/running animations. Native pedTick dispatches taxi wavers
(+0x1FE) into MavaNaTaxi before the ordinary walking path. MavaNaTaxi rotates and animates the model and can
convert the pedestrian into a C_human passenger. pedCarHit explicitly ends taxi waving before applying damage.
These facts do not yet prove which stage causes the reported miss. Do not clear wait flags or enlarge every
collision proxy as a speculative fix. Compare model/element positions, motion, grid membership and conversion
at the first missed contact, with and without the mod.

The taxi passenger script is original gameplay: a car stopping within 10 m converts a waving traffic element
to a C_entity, and taxi_pas.dat waits for a stopped car within 15 m, locks it and boards the passenger. A test
that stops there first is therefore a boarding test, not a car-strike reproduction. Do not change that script
or its locks speculatively. Controlled natural-wave impact tests must approach at speed from outside 10 m.

## Work order

### Contact corrections validated

- Reproduced a late hit with the taxi centre already 0.60 m past the pedestrian. Other natural strikes happen
  normally 2.6-3.1 m before the body; a single passing case cannot establish completeness.
- A deeper bug exists after contact: `jolt_ragdoll::Add` treated any overlap over 1 cm as an open-door occupant,
  disabling that car's whole hull. An external impact now explicitly retains the striking car as solid. Actual
  seated/doorway bodies retain the cabin behavior; the distinction is the source of the impact.
- Living-body capsules near moving cars now have their own sensor layer against the real captured car hull.
  The layers preserve native car/car and car/tram collisions. A measured missed contact is deferred until the
  solver returns and fed through `human_vehicle_contact`, preserving native taxi, damage and hit semantics.
  There is no larger sphere/radius substitute or unconditional flag clearing. Duplicate native hits are skipped.
- Only the car's own approach can trigger that fallback; an arm moving into a parked car cannot attack itself.
  Native knockdown thresholds also apply (pedestrian strength >16, C_human speed >3 m/s), preventing repeated
  knockdowns from gentle contacts with a stopped car. 1.0 weak/strong C_human checks passed.
- A disposable 1.0 lab case deliberately removed the native proxy of a natural waver: the physical fallback
  registered the car at approximately 2.55 m in front, ended waving, applied one native hit and retained the
  striking hull as solid. Evidence taxi-1.0-False-1794162178742819100.json in build/ragdoll-review-20261108.
  Final-code repetitions also passed in both builds: 1.0 taxi-1.0-False-1794162806861408700.json and
  1.2 taxi-1.2-False-1794162481525516500.json (approximately 2.38 and 2.40 m before the pedestrian).

## Final validation and installation

- Both builds passed all 18 assertions in tests/game/test_getup.py, serially: current physical-pose fitting,
  both get-up sides with a three-metre displaced pelvis, pause, repeat knockdown, owner unload, pedestrian
  collision-grid/native walking, gentle/strong car contact and the shared native contact service.
- The missing-native-proxy cases use real naturally waving pedestrians and actual hull/capsule contacts.
  They verify the fallback independently of native collision. They are deliberately induced misses, not proof
  of the exact cause of every native taxi collision miss. The boarding script and native flags are unchanged.
- Visual 1.2 lying/sitting/rising sequence retained the visible mesh and body location; native animation resumes.
  A capped 1.2 street sample held 62.5 FPS (median 15.991 ms, p95 17.205 ms); this is not a crowded-city guarantee.
- Final host SHA256: 5A8902E8A3611B2C8AC657F362C102D9212EDE3FB5E3A8AA2FC106EE9CA339A2.
  Final mod SHA256: 9B6447E668972692832AC829245E051EE8DF1CA406C05A5123CCF3248D8DEAE4.
  Both labs and W:/Mafia10Fixes contain these binaries and matching PDBs. The owner's copy was restarted
  (PID 15356) and passed actual-pose fitting and native completion checks with Windows' original D3D8.
  Only ragdoll is loaded there; no graphics pass, Fusion Fix or FPS unlock. RTSS attached automatically.
- Both lab processes are closed and their exact saved water manifests restored. The owner's copy remains running.
  Targeted C and Rust SDK compilation and generated-method/game-table checks passed. These focused regressions
  do not establish that every mission, vehicle and unusual pose is free of defects.

1. Restore staged root/pelvis handover in the shared animation service; cancel on re-knockdown and unload.
2. Check both get-up sides, repeated knockdown and pedestrian grid movement in both builds, serially.
3. Diagnose taxi-wave contact before the hit event; distinguish a native miss from a ragdoll response problem.
4. Compare car impact fallback impulses and captured car geometry only after a reproducible contact case exists.

Backups before host/SDK changes: build/ragdoll-review-20261108/before. Keep each result and remaining limitation
in the journal. Owner clarification is now in AGENTS.md: requested work authorizes necessary restarts of that copy.

## Follow-up: visible get-up displacement and rare hard stop

The owner confirmed that the 21:38:29 fit of pedestrian 1ABC68B4 was a visible 3.778 m displacement.
The previous 18 assertions measured fit at one instant, not continuity after the native root-motion tick.
A real native pedestrian fall reproduced a 3.966 m jump; evidence is under build/ragdoll-followup-20261108.
Do not dismiss a large correction as harmless alignment when subsequent frames have not been checked.

- Cause: the old fall's root-motion TCK could cross-fade into the get-up, and fitting the model did not rebase
  the animation machine's cached trajectory. The next native tick restored its old origin despite fit_error_m=0.
- Start a new root trajectory without retaining the old fall's track. Rebase the live track with the game's own
  RecompileMatrix (1.2 40E900, 1.0 5AB410, verified ret 4), retaining skeletal blending from the physical pose.
  Apply the same rigid change to the pedestrian's saved sweep origin and track displacement.
- New test_pedestrian_getup_root_motion_continuity samples two seconds of the real fall/get-up. Both builds passed
  the expanded 20 assertions. The repeated 1.0 sample's largest step fell from 3.966 m to 0.162 m, including
  vertical motion/normal walking. The dead-target third attempt is not counted as a get-up validation.
- The hard-stop report remains unconfirmed. An apparent reproduction at z=117.63 was the curb: a fresh run with
  ragdoll entirely unloaded also dropped 6.58 to about 0.72 m/s there. world.shapes confirms the sidewalk edge.
  A controlled native dodge on an unobstructed street, including an actual 1.0 strike, had no stone-like stop
  at pedestrian contact (largest speed loss near contact 0.083 m/s). Do not present the curb as a ragdoll defect.
- The port logs rare hard stops near a pedestrian: speeds, dt, Jolt's contribution, pedestrian animation,
  element/model gap and physical-body status. Includes both positions when choosing the nearby pedestrian,
  so a stale collision proxy is diagnosable. This is telemetry, not a claimed fix for the unconfirmed report.
- A final controlled 1.0 strike at about 4.5 m/s lost at most 0.114 m/s between samples. Below-threshold weak
  contacts also occur in a fresh modless probe; neither establishes the cause of the owner's rare incident.
- Installed follow-up host F78DCF7E539DC0ED0BC4D8E92F21561A62F6B55E7C9A934E7E1A2E87C5EB544A and mod
  FB195440FBA066B0EE2B6014F9D64057710209C57DCD284715EDD77E2BCDCD5C with matching PDBs in all three copies.
  Native D3D8 continuity checks passed in Mafia10Fixes; final PID 15164, only ragdoll loaded, RTSS attached.
  Labs closed and saved water manifests restored. The dodge hard-stop remains an explicitly open issue.

## Follow-up: twisted spine on an already lying corpse

- Read the exact closest corpse from the owner's live process, pedestrian 1AB5AAF4, rather than inferring
  its state from the screenshot. Preserved full physical bodies, bone bindings, native skeleton and initial
  landmarks in build/ragdoll-joints-20261108/live-1794166121832134200.json. Compiler-verified x86 layouts and
  the loaded DLL hash guarded the read. The renderer matched the physics within 0.04 degrees on this corpse.
- Initial pelvis/chest anatomical fronts agreed (dot 0.955), but their projections onto the unchanged standing
  root heading had opposite signs (+0.216/-0.084). Setup and Jolt Add independently flipped those frames to
  face the root heading. The middle spine started at about -179.2 degrees; initial-pose limit expansion then
  admitted that erroneous half-turn (clamped to -177.6 degrees). This is distinct from get-up blending and
  excessive bullet impulses, which previous fixes addressed.
- Both callers now share src/anatomy.h in the port. Ordered right/left hip and clavicle landmarks plus spine
  direction define the front; the model heading is only a degenerate-vector fallback. No solver, mass,
  friction, muscle, joint-limit tuning or user-facing setting was changed.
- Added ragdoll.inspect {actor|pedestrian}: initial landmarks, renderer/binding error and actual Jolt joint
  angles/limits. The opt-in ragdoll_trunk_axes target checks curved lying anatomy under opposite root headings.
  tests/game/test_ragdoll_joints.py uses native lying animations A/B and explicitly checks the captured pose
  is lying before checking settled joint limits. Render-only human.pose was rejected as a fixture: the mod
  captures its history at mission tick, before that render override. Those initial fixture results are not
  accepted as lying-start evidence.
- Corrected focused block passed 11 assertions in each build, serially. Native variant A kept the middle
  spine at -15..19 degrees, B at -15..15, without an OVER at the settled sample. Both native starts actually
  have opposite front/root projections, exercising the reported failure. Header geometry regression passed.
- One intervening 1.0 get-up check recorded a 0.721 m step. Timestamped follow-up was smooth (max 0.063 m),
  and final checks passed. The failed sample lacks timing/state: keep this intermittent result open rather
  than claiming it is harmless or fixed. The rare car hard stop remains open independently.
- New mod B3CC97E11B1170C06E4DC0ADF27DF4D82D36BF24D872551AF5D960D96236A596 and matching PDB are installed
  in both labs and Mafia10Fixes. Host F78DCF7E... is unchanged. Lab water manifests restored exactly; labs closed.
- Owner native-D3D8 copy passed six native-lying assertions plus actual loaded-DLL hash and only-ragdoll checks
  (eight total); final PID 3308, RTSS attached, render.state direct3d9=false. Evidence native-final-1794167119681974100.json.
  First restart PID 24904 crashed in RTSSHooks.dll+12AB before any ragdoll existed; crash_triage performed,
  dump retained. The unchanged next launch passed; cause of that intermittent hook interaction remains open.

## Follow-up: full rest-skeleton calibration and walking feedback

The next owner image exposed the limit of the narrow trunk-sign fix. Exact sleeping pedestrian 18BE9C94
still had an inverted head and twisted arms. Its initial skin/head alignment was plausible; its later skin
faced through the back of the torso. The neck constraint allowed -172..70, elbows up to 178, ankles huge cones.
Captured all physical bodies, skin bindings, initial landmarks, skeleton and joint reports in
build/rtss-focus-20261108/anatomy-live-1794167898534748000.json before restarting.

- General cause: helper points for head/nose, hands and feet used horizontal standing facing, losing axial
  orientation when lying. Physical axes and joint limits were then reconstructed/widened from the posed death.
  A bounded angle measured in an incorrectly constructed frame does not establish anatomical correctness.
- The port reads each model's rest skeleton through SDK model_file/model_open/model_nodes/model_close.
  Only small rest poses are cached; parsed meshes are released immediately. Physical frames/helpers retain
  full posed bone orientations. Constraint axes are calibrated in the rest skeleton and transformed separately
  with their posed parent/child. Hinge muscle targets use those same calibrated local axes/normals.
  Removed permanent initial-pose swing/twist/hinge widening. An unreadable rest skeleton is reported and left
  to native animation rather than inventing an unbounded joint. No new UI setting or threshold increase.
- Walking feedback was also real: saved knock diagnostics show Tommy knocked twice while traversing corpses.
  Per-limb point velocity after earlier kinematic pushes could turn the person's own kick/rebound into an
  incoming projectile. Whole-body COM gating alone still failed the regression twice; it was not accepted.
- Contacts now snapshot assembly momentum and kinetic energy before any person pushes. Exact impulse work
  (including rotation/effective mass) is attributed to its kinematic source person. That source's contribution
  is conservatively excluded from incoming energy; attribution follows assembly redistribution/rebound and
  is reduced proportionally to measured energy loss. A later external launch adds independent energy.
  Normal push resolution still sees the current velocity after preceding impulses. All actors use the same
  rule; no Tommy whitelist or permanent immunity. This is conservative effect attribution for native kinematic
  characters, not a replacement of the native walking controller with a fully dynamic body.
- Real walk fixture initially hit a native invisible surface-33 box at z=34.387. Ray evidence identified it;
  the final case uses open grass at z=75, requires actual crossings and actual physical corpse displacement.
  Inherited 1.2 devtools freecam initially captured Forwards; it is now unloaded/restored only for this case,
  with persisted settings untouched. Neither invalid fixture is counted as a successful collision check.
- Final block passes 43 assertions in each build serially: real native deaths of Paulie/civil15 in two lying variants, head skin
  front direction, both elbow bending axes and fixed limits, lying spine, get-up cancellation and four actual
  corpse crossings without a knockdown. A separate exact-build/map-verified physical launch at 9 m/s knocks
  over the same Tommy who just walked across the body, proving independent impacts remain active.
  Evidence: build/ragdoll-anatomy-20261108 (final-1.0-1794170951420789800.json,
  final-1.2-1794171039691990900.json and thrown-1.0/1.2-1794170975221863200/1794171070075485400.json).
- Owner's "weightless corpse / tonne-heavy Tommy" report exposed a second contact fault: the impact term
  supplied a fresh 20 kg at each touched part every frame, outside the shared force allowance. Only overlap
  correction was bounded. The living proxies now share the dynamic ragdoll's anatomical masses (foot 1.1 kg,
  shin 3.5 kg; full body still 78 kg), and full bone orientations. Impact plus overlap effort shares ONE
  maxForce*dt allowance per person across all contacts and bodies. The dynamic body's own dissipative
  incoming reaction is separate. No corpse mass inflation, no force-slider retuning, no new setting.
  Native animation remains kinematic; this is a finite-effort coupling, not a fully dynamic walking controller.
- ragdoll.state.person_push exposes aggregate requested/delivered/reaction impulses and maximum total force.
  Final walking checks delivered 950.19 N s in 1.0 and 988.34 N s in 1.2, both peaking at the configured 700 N,
  despite 2726 versus 1307 contacts. These are sums over four crossings, not single-hit impulses or a controlled
  FPS benchmark. Independent 9 m/s bodies still knocked the same walker over (39/40 N s).
- Final DLL ECB39D43FEDC5F01DA72563D9FF70E372A125F8138F5C10D7AC85E466E2C60F7 and PDB
  75FDA2FBBCEC3EA7B7D0C2E4375346E2BE625A632CC2F2003C7715C0DD579D98 installed in both labs and Mafia10Fixes.
  Native owner copy passed 28 anatomy assertions plus nine final contact/loaded-hash/only-ragdoll/D3D8 checks.
  An initial native walking fixture missed a brief >0.25 m/s velocity peak in 400 ms sampling; the physical
  displacement check now measures >1 cm from the asleep pose instead. The rerun passed, with 962.60 N s total
  delivered and 700 N peak. No production retuning to satisfy that fixture. Evidence/backups:
  build/ragdoll-contact-budget-20261108. Both labs closed, original water manifests restored byte-for-byte;
  native owner game restarted and its LS3D window verified visible. Host/RTSS settings unchanged in this block.

RTSS focus crash is now diagnosed independently: see the 2026-11-08 journal's exact two-dump arithmetic and
actual recognizer reproduction. Host 751B4154... uses the compatible absolute-SIB chain thunk. The previous
short idle run was insufficient; internal activation-message cycles and the owner's manual return work,
while deterministic old/new recognizer calls establish that the exact fault has been removed.

## Follow-up: limb bullets and bent palms (2026-11-09)

The owner reported shots passing through the hand near the parked car. The native copy had taken 117 corpse
hits without a capsule hit. The native volumes, part-test veto and terminal-stretch rejection caused different
failures along that path. The host now lets registered dead-body capsules decide the contact while native
obstacles still clip the flight. The port uses the same capsule identity to route the local impulse. Left and
right palms have separate query capsules, both keyed to their actual wrist pose. The sleeve and palm capsules
also make up the Jolt forearm collider, so the hand has ground contact in a bent pose. A separate dynamic wrist
joint is outside this change.

Native NewShoot shots into both palms passed in both serial labs and in the owner copy. The previous focused
anatomy and walking cases also passed in each lab. See the 2026-11-09 journal for the 1.0 Jolt shutdown fix,
the separate synthetic 1.2 exit failure and the final installation evidence.

## Follow-up: native gameplay boundary (2026-11-09)

- The mid-hook used for the corpse terminal gate cleared the flags expected by the game's living-hit branch.
  The shared stub now preserves the original flags before running the callback. Same-save airport replay
  confirms ordinary living hits with active corpse capsules. Only actual dead actors publish port capsules.
- Jolt can change a driven car only after a solid contact with a ragdoll in the current solver step. A living
  sensor and a stale step result cannot authorize a reaction. Both builds retained corpse contact response and
  gave zero mod velocity changes during unobstructed driving.
- Pedestrian world sweeps are native again. The removed override had guessed the hit object by its proximity
  to a moving car. The dedicated vehicle-to-pedestrian paths still correct animated positions and synthesize a
  missing native contact from the measured hull/capsule overlap. Their scope includes living pedestrians near
  moving cars. The rare reported hard stop remains open, with native and mod velocity contributions recorded.
- Other intentional writes are scoped to the affected actor or body: knocks from moving bodies, blast impulses,
  tram/seat handover, the get-up pose, and the player camera while down or rising. These remain separate from
  the normal weapon and drivetrain paths. See the shared journal for proof, limits and installed hashes.
