# Current project status

Snapshot: 2026-11-18. This handoff describes the cumulative source update and local incident follow-up. Read [AGENTS.md](../AGENTS.md)
for project instructions and [the documentation map](README.md) for the complete set of guides/notes.
Historical test counts and installed hashes elsewhere are dated receipts, not the status of a fresh checkout.

The separate ragdoll mod's player release (`A20F181B…`) includes the railway fix and registers no manual
fall hotkey. Both labs and the owner's Mafia10Fixes have this DLL. The two [player downloads](https://github.com/PashOK7/mafia-ragdoll-mml/releases/tag/v0.1.0) are
mod-only for existing loaders and a single-extraction ZIP with MML 2026.11.18-71d2377a for clean games. The clean base passed 38 assertions
per game build without FFmpeg, a graphics wrapper or the x64 sidecar. MML also has a full player
runtime and a separately installable FFmpeg package. The full and add-on installations both played
the same H.264 test clip in game.
[Ragdoll audit and incident notes](../knowledge/notes/ragdoll-audit-20261118.md).

## Publication and artifacts

- [MML player runtime 2026.11.18](https://github.com/PashOK7/MafiaModLoader/releases/tag/2026.11.18) provides the tested base, the full runtime, and the optional video add-on. The public SDK example retains its original telemetry implementation.

- Before the November 15 publication, the directly queried GitHub `main` and local HEAD were both
  `294814bf8f6f72437c443c2c8c430929a9c2141e` (commit dated 2026-10-31). GitHub listed no releases.
  November fixes were present in local sources/binaries but absent from that published main.
- The owner-authorized cumulative source update was published on 2026-11-15 in
  [9d1be1f](https://github.com/PashOK7/MafiaModLoader/commit/9d1be1ffa0b22cc66d812c606ea7a303169de208),
  with all 268 reviewed paths. The normal fast-forward push to
  [PashOK7/MafiaModLoader main](https://github.com/PashOK7/MafiaModLoader/tree/main) was verified directly
  against the remote. This receipt is a documentation-only follow-up to that source commit.
  No binary release/tag was created; installed ASI/MCP binaries still require the documented update cycle.
- Radar car lifetime fix published on2026-11-17 as
  [01317cb](https://github.com/PashOK7/MafiaModLoader/commit/01317cb17266945cd04fc6b83a314789b2525df6),
  normal fast-forward to origin/main verified by git ls-remote. Six paths only: HUD/actor hook,
  two native tests and their standalone documentation/map entry. Telemetry and other cumulative
  local experiments remain unpublished. No binary release/tag created. Installed owner runtime
  below is a cumulative local build, not a claim that this narrow source commit reproduces it.
- Host, SDK, MCP server and shipped examples live here. The ragdoll port lives in the separate
  [mafia-ragdoll-mml repository](https://github.com/PashOK7/mafia-ragdoll-mml).
- Keep ASI/PDB, mod DLL/PDB, optional sidecar and MCP executable identities distinct. A correct new file on
  disk does not replace an already loaded module. Reconnect MCP clients after rebuilding the server.

## Current behavior that agents must preserve

| Area | Current contract |
|---|---|
| Builds | Host/SDK/MCP/shipped examples target 1.2 and 1.0; verify both for shared work. A mod's manifest may narrow its builds. |
| Human registry | Scene close clears scene caches/poses, not all live humans. Native destruction removes an entry. CHANGE_MISSION can carry Tommy unchanged. |
| Pedestrians | Traffic elements are distinct from C_human actors. Use the correct registry/identity and current skeleton. |
| Get-up | Fit the actual native animation; `human_get_up_pose` copies the current physical pose. Cancel only the requesting owner's handover. |
| Car/body contacts | Report measured contacts through native damage/taxi/police handling after the solver. Root visibility does not define physical existence. |
| Saves | Explicit mod save records determine `.mml` creation. Empty state creates none. Legacy `mml/stats` is readable, excluded from new writes. |
| Statistics | First valid mod `stats_get` starts collection for that owner's lifetime; last consumer unload stops it. Counts are in memory; mods own persistence. |
| Audio | Capability-based software allocation addresses deferred restart gain/rate rollback. The Ogg worker exits cooperatively before archive cleanup. EAX DSP belongs to the provider. |
| Bridge timeout | Queued expired requests are cancelled. An executing request owns its storage and can finish after timeout/disconnect. Inspect state before retrying writes. |
| Source snapshots | Borrow only in the valid callback; copy owned data, preserve exact materials/current transforms and declare missing coverage. Failure cannot publish an old snapshot as current. |
| Background work | Game bridge input requires no desktop input. `window.focus` is explicit and can still be refused by Windows; inspect the actual foreground result. |

## Confirmed fixes and their limits

- **Carried-player registry loss:** fixed by following object lifetime rather than scene lifetime. The November 15
  real CHANGE_MISSION/blast/get-up fixture passed seven assertions per build. It proves this specific case.
- **Native car discovery/hidden geometry:** current/last vehicles remain discoverable outside general lists.
  `actor_meshes_ex(HIDDEN_ROOT)` exposes actual native parts while preserving child visibility and nested-model
  exclusions. It does not create box proxies or force the renderer to show the car.
- **Ragdoll car launch policy:** the separate port removes `car_push` and the artificial upward/forward impulse,
  disables animation muscle targets for vehicle-impact bodies and registers the striking native-mesh compound
  before the first step. This requires updating the port too.
- **Pedestrian dodge rewind:** a matched native draw/contact trace and both-build A/B test isolate one ~2.4 m
  pre-physics rewind. The separate port shortens the two dodge sweeps to the remaining path. Other incidents
  do not inherit this diagnosis without matching evidence.
- **Late rifle-death activation:** scenery preparation retries no longer starve other scan work. The controlled
  prison case fell from the five-second fallback to roughly 0.04–0.05 s; all sources are not assumed instantaneous.
- **Native defects:** single-waypoint navigation sentinel handling, bounded shutdown index-owner clearing,
  vehicle-shadow receiver/raster index guards, frame-wait precision and controller startup-watch timing.
  Guards prevent the reproduced invalid use; they do not identify every producer of corrupted/stale data.
- **RTSS focus-return scan and stack attribution:** SIB absolute hook-chain thunks avoid the reproduced
  recognizer failure. The crash/profile target resolver now follows that SIB encoding as well as the older
  absolute form. The hooked crash regression verifies the actual detour target; CPU hook semantics are unchanged.

## Open investigations and unfinished features

| Item | What is established / what remains |
|---|---|
| Vehicle skid-model cleanup crash, November 16 | Owner PID22132 faults on a zero skid-model vtable. A confirmed stale HUD car can make SDK world_near invoke the matching matrix writer on recycled memory; a disposable fixture reproduces the exact three damaged words. Actual actor destruction still invalidates the borrowed car before callbacks. November17 corrected the overbroad mission-close clear: CHANGE_MISSION carries live cars and needs their radar binding preserved. Carry/destruction/menu tests pass8 checks/build. Host71D2377A... is now in both labs AND the owner copy, running in owner PID31464 after restore to the saved airport moment. Fix published in01317cb. Original dump attribution remains open. See [dump investigation](../knowledge/notes/crash-20261116.md). |
| Allocator/recovery hang, November15 | Owner PID32624: a guarded Jolt step failed; cleanup faulted freeing a constraint; host diagnostics then blocked allocating from that heap. Three x86 snapshots/full memory retained. The first exception and earlier corrupting writer remain unassigned. Local follow-up repairs missing destruction notifications, stale scene-frame reads and two bounded name overreads. The combined host/port-ASan fixture completes100 cycles without a report; native regression passes22 checks per build. The relation to the original hang remains open. See [incident evidence](../knowledge/notes/hang-20261115.md). |
| Intermittent pedestrian teleports | Several concrete handover/dodge/discovery defects were fixed. Earlier unmatched and post-handover reports remain open. Preserve native, physical and rendered identities/timestamps. |
| Historical mission-load hang | Windows ended a nonresponsive load without an exception dump. A controlled Ogg archive-lock deadlock was reproduced and fixed separately; the original event's exact cause is unestablished. |
| EAX rumble/echo fidelity | Native sectors author EAX3 parameters; DSOAL/OpenAL Soft processes them in the owner configuration. The owner later heard decay and deferred this inquiry. No claim of equivalence to original hardware EAX. |
| Audio stutter | No synchronized underrun/output trace established its cause. Concurrent GPU tests can confound listening; run acoustic checks separately. |
| Sound-driven speedrun behavior | 1.0 engine start has sound-position gates. The measured EAX on/off medians did not establish an instant start or repeatable advantage. Research is not a requested gameplay fix or a rules approval. |
| Soundscape | PCM/gain-driven fronts, sampled room surfaces and modeled reflections/envelope are illustrative. They are not provider DSP taps, a complete acoustic room solver or proof of audible wet output. |
| River-water reflections | World-source integration and world shore geometry are present. Filtering/moving-camera acceptance, lighting, complete lifecycle, obsolete capture removal, shadows, temporal stability and final performance gates remain open in WATER_REFLECTION_PLAN. |
| Native D3D8 screenshots with RTSS | Earlier timeouts did not prove a game hang; pixel capture/hook interaction remains unassigned. |
| Restore points and props | Same-mission load does not necessarily reset bridge-owned prop registrations. A fixture must clean up the exact props it created. |
| Launch with core disabled | MCP completion still depends on optional core `player` and can time out after native loading succeeds. Inspect `start.state`, `human.health`, `save.state`; do not label that reply a crash. |
| Strict read-only diagnostics | `sound.compat` rejects extra fields, including MCP-inserted `timeout_ms`; ordinary `{}` works. Transport metadata filtering remains a tooling gap. |

## Final publication validation

Local final receipts are in `build/publication-final-20261115/`; earlier failed and passing preparation
receipts remain in `build/publication-prep-20261115/`. These local files are excluded from publication.

- **Complete offline suite: ALL PASSED**, exit 0 (`offline.log`). The remaining SIB resolver defect is fixed.
  The native-index fixture now verifies observed default buffers and conservative unobserved managed buffers;
  no unsupported content proof was enabled. Earlier failed runs remain historical evidence.
- **Real game: 55 passed, zero failed in each build** (`game.log`). Cases cover carried-player get-up,
  explicit/empty/legacy saves, native car geometry/contacts/support, model aliases, sound allocation and
  adversarial/immediate Ogg close. Test PIDs 17576/22188 were closed; the owner's game was not driven/updated.
- Final host SHA-256: `B5FD1DE0A280F86F24F75A27291B5CC59DE29CE36B9BB4C970D1E823706A2003`,
  installed in both labs for that validation. At that point the owner copy retained host34B23E81... . Source publication does not
  replace any running process or install binaries into another person's game.
- Full x86 build and final MCP smoke pass (`build-all.log`, `mcp-smoke.log`). The unchanged x64/MCP source
  also passed its preparation builds. Method/game generators, path/name and both-build address checks pass.
- The publication inventory covered52 project Markdown documents; the subsequent captured-hang note brings
  the current documentation to53. Local links and the complete
  publication candidate are checked, including new headers/shaders/tests and licence-sensitive identifiers.
  Ignored builds/config/game assets/dumps/local symbols stay outside the staged source set.
- Prior focused receipts: 29 assertions per build for save ownership, occupied/hidden-car discovery/contact
  and related cases; seven per build for the carried-player fixture. They are separate runs, not a full-suite total.
- Shader/compiler, native source/material, C/C++/Rust and water notes carry additional dated fixtures.
  None supplies blanket final visual or performance acceptance for the unfinished water path.

The earlier preparation's two failures are resolved, not waived. Their causes, native class comparison and
test scope are recorded in [release-validation-20261115.md](../knowledge/notes/release-validation-20261115.md).
Open feature/fidelity investigations in the table above remain open despite the passing regression suites.

## Local incident follow-up

The uncommitted November15 follow-up was installed in both labs and, at the owner's later request,
in `W:/Mafia10Fixes`. Its regular host SHA-256 was
`A795A2FA961EEDFED4E609300455F15F3E381E2B9D5F836DE69BD9C3494BFA4E`. The labs' core is `51287E34...`.
The combined instrumented host `6DB95D74...` and port `45F4F121...` completed 100 cycles with 800 bodies,
200 native cars and 100 blasts. ASan reports, guarded native errors, Jolt resets and ragdoll faults all
count zero in this run. The diagnostic build excludes global-string checks and intentional raw stack
scanning. The diagnostic native.cpp uses MSVC-ASan because of Clang's inline-assembly constraint.

The linked-frame lifetime, direct Release/destructor, vehicle geometry, model alias and camera-return
fixtures pass 22 assertions per build, exit 0. Both labs are stopped. Lab 1.0's original ragdoll DLL/PDB
and enabled water manifest have been hash-restored, and the diagnostic preload is disabled. The original
owner allocator hang remains open. The complete offline suite and MCP smoke both finish ALL PASSED,
exit 0. Receipts are `offline-after-lifetime.log` and `mcp-smoke-after-lifetime.log` in the incident
evidence directory. Fresh ordinary lab logs also contain zero native exception or mod-fault lines.

The owner installation replaces the host ASI/PDB pair. Its existing mod set has soundscape, draw_probe
and ragdoll enabled. The fresh process9192 reaches the main menu and passes both native frame-lifetime
paths, each with one notification and no remaining watch. Windows required its existing elevated launch.
The game exits through its own menu. All607 inventoried save/mod/data files retain their hashes,
including all182 saves. Preimages, a save backup and the verification receipt are under
`build/owner-loader-install-20261115-205541-128/`. This is startup/lifetime verification in the owner copy.

The November16 HUD car lifetime repair supersedes that ASI/PDB pair in both labs and the owner copy:
host `BD3BD3C4F6C84B8CFB9F624E5D74CDE914656EB6026BA2DDBE8A5B98FE22A098`,
PDB `FDDF77F351C63105CA4C58C547311D38039703E2AD8CB100B0D741D42EA284C6`.
Native lifetime tests pass4 checks per build; full x86 build, offline suite and MCP smoke pass, exit0.
Owner PID13852 verifies taxi-to-menu-to-Pauli with the original three mods: no stale HUD car,
nearby traffic readable, normal exit. All92 currently backed-up saves retain their hashes. The matrix
diagnostic is removed from the owner's mods; all games are stopped. Evidence/preimages are in
`build/crash-20261116-180803/`. This fixes the confirmed matching producer; PID22132 attribution
and the earlier allocator hang remain open.

## Updating this handoff

When a gap is fixed, update its current entry here and in ROADMAP, append the evidence to JOURNAL and keep
the older incident note explicitly dated. Record the tested source/artifact and both-build outcome. After
publishing, replace the publication snapshot with a verified remote commit/release. Do not replace a failed
receipt with a later pass or silently change a rejected explanation into a historical success.
