# Current project status

Snapshot: 2026-11-15. This handoff describes the cumulative source update. Read [AGENTS.md](../AGENTS.md)
for project instructions and [the documentation map](README.md) for the complete set of guides/notes.
Historical test counts and installed hashes elsewhere are dated receipts, not the status of a fresh checkout.

## Publication and artifacts

- The directly queried GitHub `main` and local HEAD were both
  `294814bf8f6f72437c443c2c8c430929a9c2141e` (commit dated 2026-10-31). GitHub listed no releases.
  November fixes were present in local sources/binaries but absent from that published main.
- The owner authorized publication of the cumulative source update to
  [PashOK7/MafiaModLoader main](https://github.com/PashOK7/MafiaModLoader/tree/main) on 2026-11-15.
  The verified remote commit will be recorded here after the normal fast-forward push. No binary release/tag
  is implied by source publication; installed ASI and MCP binaries still require the documented update cycle.
- Host, SDK, MCP server and shipped examples live here. The ragdoll port lives in the separate
  `mafia-ragdoll-mml` checkout, which had no configured Git remote at this audit. Publishing this repository
  cannot deliver that separate mod's implementation or DLL.
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
  installed in both labs. The owner copy retains its previous 34B23E81... host. Source publication does not
  replace any running process or install binaries into another person's game.
- Full x86 build and final MCP smoke pass (`build-all.log`, `mcp-smoke.log`). The unchanged x64/MCP source
  also passed its preparation builds. Method/game generators, path/name and both-build address checks pass.
- The documentation inventory now covers 52 project Markdown documents. Local links and the complete
  publication candidate are checked, including new headers/shaders/tests and licence-sensitive identifiers.
  Ignored builds/config/game assets/dumps/local symbols stay outside the staged source set.
- Prior focused receipts: 29 assertions per build for save ownership, occupied/hidden-car discovery/contact
  and related cases; seven per build for the carried-player fixture. They are separate runs, not a full-suite total.
- Shader/compiler, native source/material, C/C++/Rust and water notes carry additional dated fixtures.
  None supplies blanket final visual or performance acceptance for the unfinished water path.

The earlier preparation's two failures are resolved, not waived. Their causes, native class comparison and
test scope are recorded in [release-validation-20261115.md](../knowledge/notes/release-validation-20261115.md).
Open feature/fidelity investigations in the table above remain open despite the passing regression suites.

## Updating this handoff

When a gap is fixed, update its current entry here and in ROADMAP, append the evidence to JOURNAL and keep
the older incident note explicitly dated. Record the tested source/artifact and both-build outcome. After
publishing, replace the publication snapshot with a verified remote commit/release. Do not replace a failed
receipt with a later pass or silently change a rejected explanation into a historical success.
