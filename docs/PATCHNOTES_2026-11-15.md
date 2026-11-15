# November 2026 cumulative update

Published source update:
[9d1be1f](https://github.com/PashOK7/MafiaModLoader/commit/9d1be1ffa0b22cc66d812c606ea7a303169de208),
2026-11-15. Compared with the previous GitHub main at
`294814bf8f6f72437c443c2c8c430929a9c2141e` (2026-10-31).
This document describes the cumulative source changes and their migration requirements. See STATUS.md
for the verified publication record. A source update is distinct from a tag/binary release or an installed host.

Read [current status](STATUS.md) for validation blockers/open investigations and
[the documentation map](README.md) for authoritative contracts. Dated implementation details below are
supported by the journal, linked notes and tests in this repository. Local evidence files are not shipped.

## Most important changes for an agent updating an existing installation

1. Replace the **host with the game closed**, then restart the exact configured game. Hot-reloading a mod
   cannot update MafiaModLoader.asi. Preserve existing saves, mod settings, wrappers and unrelated ASIs.
2. Rebuild/reconnect the **MCP server** too. An already-running client can retain an old executable/schema.
   A newly pulled guide does not add new commands to the old host or server.
3. Check **capabilities and sizes**, not just `api: 1`. The C ABI remains version 1 and grows by suffixes.
   Use `MML_HAS`; initialize sized outputs. Rust sys/game tables and safe wrappers are updated together.
4. **Do not clear every human on a map change.** A carried player can be the same native object. Invalidate
   scene pose/frame caches and follow actual native destruction for object lifetime.
5. **Own durable statistics in your mod.** `stats_get` opts into in-memory counting; it does not promise a
   persisted total. Use your own `save_put` records. Empty mod state should produce no companion file.
6. **Measure physical contacts independently of visibility.** A bumper camera can hide the native car root.
   Use explicit hidden-root mesh extraction when appropriate; keep native damage/taxi semantics.
7. **Inspect state after a timeout.** An already-executing bridge mutation may finish after the caller returns.
   Blindly retrying can apply the operation twice.
8. **Keep reproduced defects and old reports distinct.** A bounded guard, a passing lab fixture and a new
   observation do not prove the cause of an earlier crash without matching evidence.
9. **Update the ragdoll separately.** This repository ships the services it uses, not the private port's code.
   A loader-only update cannot remove an artificial impulse implemented in an old ragdoll DLL.

## Host fixes and lifecycle

### Human registry, poses and get-up

- `HumansMissionClosed` preserves the constructor registry and clears scene-specific held/dirty/last poses.
  `HumansGone` removes destroyed actors. This fixes a carried Tommy disappearing from `humans.list` and
  subsequently losing skeleton/get-up access after CHANGE_MISSION.
- Pose writes reject allocations that no longer have a live LS3D frame layout, with a bounded diagnostic.
  A committed address alone does not prove the original frame survived.
- Get-up holds and fits the actual native animation, rebases root motion, follows the pedestrian grid and
  preserves the body's support height. `human_get_up_pose` accepts a validated copy of the current physical
  skeleton; `human_get_up_cancel` ends only that mod's pending handover.
- `human.anim_state` exposes handover progress and fit information. Traffic pedestrians remain elements,
  distinct from actor humans; do not look for all traffic bodies in `humans.list`.

Contracts: [MODDING](MODDING.md), [port review](RAGDOLL_PORT_REVIEW.md),
[native game notes](../knowledge/notes/game.md). Tests include `test_getup.py` and pedestrian handover cases.

### Vehicle contacts and geometry

- Nearby-actor discovery includes current/last player vehicles even outside the general mission lists;
  native contact validation also accepts current/last/active-traffic car references.
- `actor_meshes_ex(MML_ACTOR_MESH_HIDDEN_ROOT)` traverses a hidden native model root without changing
  visibility, retaining child visibility and nested-model exclusions. Legacy `actor_meshes` keeps its behavior.
- Rigid actor geometry follows the renderer's native world-matrix flag; applying the model transform again
  could misplace the compound collider and feed a large solver reaction into the car.
- `human_vehicle_contact` reports measured relative world velocity/contact position through original hit
  handling after a solver step. It validates identities/input, ends taxi waving and retains native damage,
  police and ordinary hit events. It is not a synthetic collision detector.
- `vehicle_support_hook` augments native car-usability wheel support using measured load-bearing external
  contacts. Native damage/wheel/speed/side checks remain; temporary on-ground bits are restored before return.
- Marked native model names such as a traffic `.I3d!` alias resolve to the native `.4ds` asset instead of
  failing skeleton calibration solely because the loaded model's name is annotated.
- Scenery scanning no longer lets failed placement retries starve subsequent sources, addressing the
  reproduced multi-second delay before rifle-death ragdoll activation.

See [car-impact evidence](../knowledge/notes/car-impact-20261115.md),
[scenery readiness](../knowledge/notes/ragdoll-scenery-20261114.md) and tests for vehicle geometry/support/contact.

### Bullets and native gameplay boundaries

- Registered dead-body capsules replace obsolete native corpse hit volumes; living actors retain native
  part handling. Capsule identity reaches the hit filter. World impacts emit once after final hit selection.
- The capsule terminal mid-hook preserves the incoming EFLAGS needed by the native living-hit branch.
  The reproduced airport single/shotgun regression was checked separately from corpse limb hits.
- Shared mechanisms remain in the host; ragdoll impulses, sensing and activation policy remain in its mod.
  Do not fix a physical sensor miss by globally enlarging native collision proxies or clearing native flags.

### Native stability and timing

- A single-waypoint navigation route with sentinel node `-1` uses the existing native plane-detour arm
  instead of indexing the checkpoint table with a negative node. Real node indices retain their native path.
- 1.2 final driver cleanup checks a stale index-slice owner before clearing its fields. The native buffer
  release/manager cleanup still runs; ordinary destruction is unchanged. The original stale-owner producer
  remains unassigned.
- Vehicle-shadow cleanup validates a receiver frame, and native shadow rasterization rejects out-of-range
  indices. These protect the reproduced accesses; they do not explain every earlier corruption source.
- Process-owned frame-wait timer precision repairs the original-renderer frame pacing issue without a
  global Windows timer-policy change.
- Hook chains use an equivalent seven-byte x86 absolute SIB jump to avoid the reproduced RTSS focus-return
  scanner misinterpretation. Atomic pointer relinking remains. The shared crash/profile call-target walker
  also follows that encoding; the hooked crash fixture verifies its actual detour attribution.
- Native-index self-testing now covers both the observed default-pool class and the managed-pool class.
  Managed buffers with unobserved Lock/Unlock implementations must keep revision proofs unavailable,
  retaining ordinary data reads. This corrects the fixture's unsupported expectation without enabling an
  unsafe cache shortcut. See the final validation note for the independent Direct3D class comparison.

## Audio: allocation, Ogg shutdown and EAX

### Restarted engine sounds retain effective gain and rate

A standalone synthetic-tone test isolates deferred DirectSound allocation restarting at old effective
gain/rate even when getters report the latest values. The earlier per-play gain transition was insufficient
and has been removed. Native driver initialization now selects LS3D's existing software-buffer path only
when hardware 3D capacity is zero and no EAX provider was detected. Hardware/EAX/query-failure paths retain
native allocation policy. Spatial/frequency/volume controls remain available; Setup/registry is not rewritten.

This uses the existing software-allocation mechanism; it does not repair Windows internals or establish a
measured latency/CPU benefit. See [audio findings](../knowledge/notes/audio.md) and
[the standalone regression](../tests/dsound/README.md).

### Cooperative Ogg close

Native Ogg Close could terminate its reader while it owned rw_data's shared archive lock, then deadlock
decoder cleanup waiting on that lock. A controlled scheduling fixture reproduced it. The host now requests
stop and joins the native worker before archive/decoder cleanup, retaining native refill/backend/EOF behavior.
Per-build offsets/counts are checked; `sound.compat.ogg_shutdown` reports started/exited/joined/failure counts.

This repairs the reproduced mechanism. The earlier owner mission-transition hang had no exception dump;
its exact cause is not retrospectively proven. See [hang evidence](../knowledge/notes/hang-20261114.md).

### EAX remains provider processing

The original engine selects/blends sector parameters and submits EAX3. In the owner's separate test setup,
DSOAL/OpenAL Soft processes those properties. These provider DLLs/configurations are not automatically
installed by this loader source update. Soundscape reads EAX state; it does not add or replace reverb DSP.

Indoor rumble and similar outdoor echoes remain a fidelity investigation. The owner subsequently heard
decay and deferred it. Do not describe the current provider as a verified bit-identical original hardware
implementation. Sound-driven speedrun findings are original-1.0 research, not requested gameplay changes
or evidence of leaderboard legality. See [EAX](../knowledge/notes/eax-environments.md) and
[sound-driven logic](../knowledge/notes/audio-logic.md).

## Window, input and camera behavior

- Ordinary windowed games resize/maximize/restore through the native device reset, viewport/aspect/HUD/menu
  rebuild and failure-recovery path. Interactive drag commits after release. Exclusive fullscreen and
  bridge-borderless modes retain their separate behavior.
- Foreground windowed/borderless games confine the pointer and clear the Windows cursor in the client,
  including return to focus without a mouse move. Deactivation/modal move-size loops release confinement;
  window borders retain the native resize cursors.
- Controller discovery checks immediately after watch subscription and retries startup arrivals. Startup
  checks only missing devices, avoiding reopen solely because a foreground-only controller is unreadable
  while the lab is in the background. Actual device changes retain their readability recovery path.
- `window.focus` bypasses the loader's background activation interception only for this explicit request.
  Inspect its actual `foreground`, HWND/PID and result; a TRUE native/API result alone is insufficient.
- Projection ownership prevents Fusion Fix applying an extra FOV multiplier to an absolute host FOV or
  exact auxiliary frustum. Live resizing uses the actual current render size, including Fusion Fix UI scaling.

See [windows](../knowledge/notes/window.md), [IJoy](../knowledge/notes/ijoy.md),
[projection](../knowledge/notes/camera-projection.md) and `test_window*`/`test_camera_projection.py`.

## Save files and statistics: behavior change

The loader no longer collects/persists statistics just because it is installed. A valid mod `stats_get`
registers that owner as a consumer; the last consumer unloading stops collection. Slot counters reset on
load, profile counters are session data, and `stats.state` observes without opting in.

Persist needed values with explicit `save_put` under the mod's own key. With no durable records there is no
`.mml` file. Legacy host `mml/stats` can be read for compatibility, but new writes omit that reserved record
and preserve explicit mod records. Do not bulk-delete companions: other mods may own real data in them.
This update does not delete native saves or disable the game's own profile progress.

Migration: any achievements/statistics consumer that relied on automatic host persistence must save/restore
its own required counters. See MODDING's statistics/save sections and `test_save_ownership.py`.

## Public SDK additions

`MML_API_VERSION` remains 1. The following 18 optional calls are appended to `MmlHost`; existing offsets
are retained. Every optional call requires `MML_HAS` on hosts predating it.

| Area | Appended calls |
|---|---|
| Camera | `camera_projection_state` |
| Auxiliary views | `scene_view_features` |
| Native source geometry | `scene_draw_observe`, `scene_draw_geometry`, `scene_draw_collect`, `scene_draw_collect_ex`, `scene_draw_billboard`, `scene_draw_collect_features` |
| Source coverage | `scene_residency`, `scene_residency_state`, `scene_residency_camera` |
| CPU owned jobs | `scene_owned_submit`, `scene_owned_join` |
| People | `human_get_up_cancel`, `human_get_up_pose`, `human_vehicle_contact` |
| Vehicle support | `vehicle_support_hook` |
| Physical model geometry | `actor_meshes_ex` |

New sized source descriptors describe prepared draws, geometry, fixed stages/samplers, missing regions,
procedural billboards and residency. Preserve explicit strides/units and initialize `size`. Unknown flags,
unsupported sources, stale borrows and incomplete coverage must be handled as unavailable, not fabricated.
Scene views now allow eight per owner/64 total; advertised `MML_VIEW_SKY_ONLY` suppresses foreground,
lens flares, VIEW_WORLD callbacks and view passes for a native backdrop capture.

### Native sources, owned snapshots and ray consumers

- Draw observation is owner-scoped and opt-in. Source geometry copies only the current callback's draw;
  native batching is bypassed while observed so native face-group inputs remain available.
- Bounded draw-free collection prepares active native sources with current transforms/poses and native
  materials, restores native state and suppresses camera-path fog. Apply atmosphere along the consumer's
  actual ray segments; do not light prelit values again.
- Billboard support is explicit and observer-dependent. Only supported native axis/mode combinations are
  reconstructed per ray; unsupported deformation/material/stream regions remain explicit missing coverage.
- Source residency combines copied per-owner regions, including a current-camera-relative request. It does
  not rewrite main FOV/far/LOD/occluder preferences or authorize stale geometry after a failed collection.
- SDK helpers provide owned native texture/mip data, scene snapshots, CPU/D3D11 reference queries, optional
  DXR/facet queries, material/alpha handling, waterline construction and GPU publication/transfer primitives.
  Pointer identity, age or an unchanged address is not a content/lifetime proof.
- One CPU-only owned job per mod can overlap the frame. Submit/join on the game thread; worker data is
  owned and alive until join/unload. Workers cannot call game/host/immediate D3D APIs. Unload/fault drains work.
- C/C++ SDK, generated Rust ABI/game tables and safe wrappers are updated together. Where a Rust operation
  cannot express the ownership guarantee safely, its contract remains explicitly unsafe.

Detailed contracts: [MODDING](MODDING.md), [snapshot](SCENE_RADIANCE_SNAPSHOT.md),
[residency](SCENE_RESIDENCY.md) and the header comments.

## MCP and bridge diagnostics

| Change | Migration/meaning |
|---|---|
| `crash_triage` compact default | Named site/stack, mission, mods and recent event types; request `details: true` for raw log/register/tap/event context |
| Crash build selection | Dump image stamps select 1.0/1.2 even if the active log is missing/newer; PDB GUIDs must match |
| Dump-only triage | Reports the available fault site and identifies missing paired context instead of inventing it |
| `window.state`, `.resize`, `.focus` | Explicit size/reset/cursor/focus state; exact parameters are in PROTOCOL |
| `sound.compat` | Read-only allocation and Ogg shutdown diagnostics; old gain-workaround requests are removed |
| `vehicle.trace` | Bounded occupied-car history and automatic incident capture with native/mod phase attribution |
| `human.vehicle_contact` | Native reaction to a measured physical contact |
| `actor.meshes` `hidden_root` | Explicit visibility-independent root traversal for model geometry |
| `human.anim_state` handover | Fit/switch/progress diagnostics in addition to native animation state |
| `control.state` wheel support | Provider/query/fault diagnostics for external load-bearing support |
| Bridge timeout ownership | Queued cancellation differs from execution already in progress; check before retrying |

The compact crash view is a diagnostic presentation change, not a promise about how any external client
classifies content. Original detailed evidence remains available. No log wording establishes a crash cause.

## Optional examples and experimental renderer work

### Soundscape

New disabled-by-default example, toggled with Shift+F10. Supported PCM WAV windows at native playback
cursors and native gain drive continuous 3D fronts. Sampled collision surfaces make a room cutaway;
bounded reflected paths/pulses and the native EAX3 parameters drive an illustrative wet envelope.
Native D3D8 uses the shared legacy mesh helper with state restoration.

This is a visualization: slowed propagation, uniform loss and sampled room coverage are stated assumptions.
It does not observe the provider's actual wet PCM or prove a complete room volume. Unsupported short/PCM
sources can be missed. See its README for controls, cleared-history lifecycle and test scope.

### Draw probe

New disabled-by-default diagnostic records bounded draw-submission histories around hits. It correlates
CPU skin vertices/ranges, world bounds and identities, with explicit unmatched samples. Reports are created
without overwriting earlier incidents. Submitted CPU geometry is not proof of final occlusion/pixel coverage.

### River water

This block contains substantial in-progress source and SDK work. A world-source object path, native material
queries and world-space shore constraints replace several image-dependent mechanisms. Filtering uses
path footprints; raster seeds, compute transport, final composition and an optional 64-bit path are present.
Native buffer/content revisions, bounded caches, Vulkan resource handover and CPU-owned preparation work
address measured costs, with explicit generation/publication rules.

**Water is not finally accepted.** Reflection plan step20 and later gates remain open, including moving-camera
filtering, lighting distances, full lifecycle, obsolete capture removal, restored shadows, temporal behavior and
comparable final frame cost. Old benchmark wins apply to their exact scene/settings/artifacts. Do not enable
every graphics example or promise a performance gain merely by installing this loader update.

### Shader builds

FXC/DXC entry builds are bounded by `MML_SHADER_TIMEOUT` (default 60 s). Active include dependencies are
tracked. A repeated optimizer timeout with identical inputs fails before another identical attempt; use the
documented per-entry build targets to investigate changed inputs rather than looping an unlimited compiler.
See `tools/compile_shader.py`, `tests/test_compile_shader.py` and MODDING's shader-build section.

## Separate ragdoll update required

The sibling port includes physics/anatomy/contact/trajectory work described in the linked investigations,
including removal of `car_push`/the artificial head/body launch and the native dodge rewind correction.
Those changes are not files in this repository. Preserve the distinction between a host contract fix and a
specific port policy. Update/validate both artifacts when reproducing a port report, and keep actual loaded
DLL hashes with the evidence. A fresh host alone cannot guarantee every old port behavior disappears.

## Validation, packaging and remaining work

The current receipts/blockers are maintained in [STATUS.md](STATUS.md). x86, x64 and MCP builds pass;
MCP smoke and both-build address checks pass. The final complete offline suite reports ALL PASSED;
the two preparation failures were fixed rather than waived. Fresh native get-up/save/contact/support/model/
audio/Ogg fixtures pass **55 checks in each build**, zero failures. Water visual/performance acceptance
and the open incident/fidelity investigations remain separate.

Source publication must include new headers/shaders/test fixtures referenced by changed tracked files,
alongside generated ABI/method/game tables. Exclude build products, Rust target directories, local config,
local-only symbols, game assets/saves, extracted code and dumps. Consumer Cargo lockfiles remain tracked;
the standalone SDK library's test lockfile/target are local artifacts.

Publication uses the reviewed source manifest, recorded build/test/artifact identities and the owner's
explicit authorization, with a normal fast-forward push to the verified main. A binary release/archive is
a distinct deliverable; do not tell
testers they received a fix just because it is installed in a developer's game folder.
