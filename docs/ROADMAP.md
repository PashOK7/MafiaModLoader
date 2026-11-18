# Roadmap

What is done, what comes next and later, and what is known to be missing or wrong. Agreed with the owner on
2026-10-13, reordered on 2026-10-14 for the toolkit's goal ("Next"); agents keep it current (move items to Done with the date, add what they find to Known gaps).

Current handoff: [STATUS.md](STATUS.md). Update/migration details: [PATCHNOTES_2026-11-15.md](PATCHNOTES_2026-11-15.md).
Dated acceptance counts below belong to those specific runs, not an aggregate pass for today's complete tree.
Historical owner PIDs and installed hashes are receipts, not instructions to control that process now.

## Decisions (the owner's)

- Workflow (2026-10-31): follow the applicable project rule or the owner's explicit instruction. If neither
  covers the proposed action, or the instruction is ambiguous, ask before acting; do not invent an alternative
  workflow. Work and authorized publication use `main`; another branch/worktree needs explicit authorization.
  Existing authorization remains valid within its scope. AGENTS.md contains the full rule.
- The host is a `.asi` loaded by Ultimate ASI Loader (`dinput8.dll`); it never unloads. Mods hot-reload.
- The MCP server is a separate process (Rust), talking to the game over the bridge; it outlives the game.
- Builds: 1.2 and 1.0 (per-build tables in `game_build.h`). The owner, 2026-10-15: the loader and the SDK must
  work on both, so everything for both is tested in both labs (at the end of a block of work, or at once where 1.0's
  code differs; not every small test); a mod made for one build is worked on and tested in that build's lab only
  (`"builds"` in its `mod.json`). The labs are listed in `mml.config.json` (`labs`); agents switch between them.
- Mods in any native language through one C ABI. No script mods: Lua (LuaJIT) was dropped for good (the owner,
  2026-10-17; "Taken out or changed").
- A 64-bit sidecar for what only exists for 64 bits (the owner, 2026-10-14: "what if a user wants a modern
  technology such as the latest PhysX, which runs only on 64 bits?"): the game stays 32-bit, a 64-bit process beside
  it hosts such libraries (PhysX 5, and the like: GPU and ML runtimes), connected to the game by shared memory.
- Agents have full access, and may launch, drive and close the game themselves, from inside (never the owner's
  real mouse or keyboard).
- The repository is English; the GitHub repository is public (verified 2026-11-15; it was private at the
  original 2026-10-13 setup). Pushing,
  publishing and pull requests: ask the owner first.
- The owner's 1.2 lab is shared with mafia-ragdoll (`tools/lab.py mode loader|ragdoll`).
- The loader is measured by how easily another agent writes a mod with it (2026-10-13). Older mods
  (TrackMafia: lap telemetry, a ghost car, an overlay; `knowledge/notes/game.md`, "Cars") are not ported here: they
  show what every such mod had to build for itself, and so what the loader must give instead.
- The toolkit is built for whatever mod a user can think of, written by another agent (2026-10-14); its measuring
  cases: a ragdoll, shader water, realistic smoke, VR, fishing, skateboarding, a working airplane, achievements.
  Those mods are not written here. The ragdoll (the owner, 2026-10-21) becomes a loader mod in a private repository
  of its own, made from mafia-ragdoll: the loader first gets every service it needs to be written and run (item 32);
  mafia-ragdoll's code stays the reference.
- **The stages** (the owner, 2026-10-19): now, the original game: reversed, and made better with its style kept
  (fixes, speed, the toolkit), until neither the game nor the loader can be made better. Only then the next stage:
  **NVIDIA RTX Remix through its own API** (`remix_c.h` in dxvk-remix: the engine hands Remix its meshes, materials,
  skinned people, lights and camera directly, no Direct3D 9 calls intercepted), a stage of its own, not an item of
  this roadmap. What the present work leaves on the way should keep that door open: a scene's clean data (meshes
  uploaded once with their normals, materials, instances, skinning as a bind pose with weights and bones' matrices,
  lights, the camera) is what both a faster renderer for the original look and Remix's API take.

## Done

- **Railway ragdoll recovery (2026-11-18).** The port uses the carriage's native walkable floor for
  the get-up and the engine's railway exit for a stranded falling passenger. Both labs exercised
  first person, open-air falls and surface recovery. The owner's paused incident was recovered live.

- **Ragdoll player distribution (2026-11-18).** Removed the temporary J testing bind for the
  player release. The mod-only ZIP writes to the ragdoll directory. For a clean game, one extraction
  places both the pinned base and ragdoll beside Game.exe. The full MML runtime and FFmpeg video add-on ship separately. Both game builds
  passed the ragdoll checks on the base. The full runtime also ran a second mod with a 64-bit half.
  The public version number for this base is 0.1. The host hash is recorded in mml/base-version.txt.

- **Ragdoll audit and Clark's Motel (2026-11-18).** The port now leaves mission characters' vehicle hits
  to the game, follows moving car parts, releases disabled physics, and uses consistent time and water
  classification. Both labs passed the 04-03 opening and the settings and driving regressions.
  [Audit and before/after measurements](../knowledge/notes/ragdoll-audit-20261118.md).

- **Final cumulative-update regression (2026-11-15):** fixed SIB absolute-jump call-target traversal
  in the shared crash/profile reader. Corrected the native-index self-test to require proofs only for
  observed writers, with positive default-pool and conservative managed-pool cases. A separate native
  D3D9 probe verified the different Lock/Unlock classes. Complete offline suite and MCP smoke pass;
  both native builds pass 55 checks each. See knowledge/notes/release-validation-20261115.md and STATUS.md.
  Earlier failed logs remain historical evidence. Source published by the owner-authorized normal
  main push in 9d1be1ffa0b22cc66d812c606ea7a303169de208; remote verified. STATUS.md records the artifact boundary.

- **Artificial car-impact launch and missing occupied car (2026-11-15, fixed):** removed the
  port's upwards/forwards fallback impulse and its setting. Car-hit bodies use native-mesh collision
  with animation-driven muscle targets disabled. Nearby discovery and native-contact validation now
  include the player's current/last vehicle outside the general lists. A striking car is registered
  immediately. Both builds pass 29 checks including real hidden-root truck contacts. The initial owner
  installation used host34B23E81... / port5505A21D... . The later lifetime/read follow-up updates its host
  to A795A2FA... with the same port. See knowledge/notes/car-impact-20261115.md and STATUS.md.

- **Unrequested statistics companion files (2026-11-15, fixed):** collection is scoped to requesting
  mods and does no automatic persistence. Explicit save_put records determine whether a .mml exists.
  Legacy host counters are readable and omitted from new writes. Both builds verify empty saves,
  legacy-only saves, consumer unload and actual mod state surviving reload.

- **Soundscape 3D audio visualization** (2026-11-14): source PCM and native gain drive continuous
  spherical fronts. Connected collision faces form a room cutaway with separate reflected pulses.
  Native EAX3 parameters drive the modeled early level and late-envelope graph. Shift+F10 toggles
  the mod. Both builds pass 22 live checks each and 18 offline checks pass. The owner's native D3D8
  retained its 112-byte EAX property packet through the hot load. See mods/examples/soundscape/README.md.

- **Ragdoll ground-readiness starvation** (2026-11-14): placement failures no longer monopolize
  the host scenery scan. In Old Prison, the same lethal hit went from the five-second fallback
  to 0.04–0.05 s with completed ground coverage. Verified in both builds and installed in Owner
  1.0. See knowledge/notes/ragdoll-scenery-20261114.md.

- **Vehicle launch from a misposed compound collider** (2026-11-09): the owner's trace isolated
  the ragdoll contribution. The host now poses rigid actor meshes by the renderer's world-matrix flag,
  and the port measures solver deltas from the actual clamped input. Both labs and Mafia10Fixes have the fix.
- **Bullet world events through the capsule hook** (2026-11-09): restore reporting from the outer
  ray hook after final hit selection. A static impact with capsules active emits once in both builds.
  Installed in Mafia10Fixes with the vehicle fix.

- **Vehicle incident recorder** (2026-11-09): bounded occupied-car history, automatic pre-trigger
  capture on rapid acceleration or upward launch, and attribution across native and mod tick phases.
  Tested in both labs and installed in Mafia10Fixes. `vehicle.trace` reports status and saves on request.

- **Carried player retains the get-up service after mission transfer** (2026-11-09): keep the human
  registry tied to actor lifetime and discard scene-local caches on close. Native blast after transfer
  plays the fitted back get-up and returns control in both builds. Both labs have the fixed host.
  The affected session was repaired live. The persistent owner fix shipped with the vehicle recorder.

- **Ragdoll limb shots and palm contact** (2026-11-09): registered corpse capsules replace native bullet
  volumes while keeping the game's obstruction and shot processing. The actual capsule and surface point
  reach the mod. It applies the local impulse to that segment. The palm follows the wrist pose in both
  bullet tests and Jolt contact. Both builds and native Mafia10Fixes passed actual shots into both palms.
  Also fixed the ragdoll process-detach lifetime crash from Game_14248_20261109_000801.dmp.

- **Ragdoll floor and checkpoint restart** (2026-11-09): Jolt includes the native collision slabs that have
  no visual floor. Same-mission save loads report loading while the game can still render the previous actor.
  The port suspends pose writes and clears physics and camera state on `GAME_LOADED`. The recorded garage blast
  and the native off-map Game Over Retry passed in both builds. Installed in Mafia10Fixes.

- **Native blast severity for Tommy** (2026-11-09): light and medium explosion damage keep their native
  reactions. The ragdoll replaces a heavy fall selected by the engine. Tested in both builds and installed
  in Mafia10Fixes.

- **Ragdoll rest anatomy and finite walking contacts** (2026-11-08): full model rest-skeleton calibration
  supersedes the narrow trunk-sign fix. Head/limb helper axes retain posed bone orientation; joint limits
  are no longer permanently widened to a death pose. Walking contacts use anatomical segment masses and
  one force budget per person for impacts and overlap pushes together. Pre-push assembly energy/work
  attribution prevents self-induced knockdowns while preserving independent incoming impacts. Final port
  ECB39D43... passes 43 assertions per build serially and a physical 9 m/s positive-impact check in each.
  Installed in both labs and native-D3D8 Mafia10Fixes; native anatomy/contact/install checks passed.
  Evidence and limits: docs/RAGDOLL_PORT_REVIEW.md. Rare car hard-stop/get-up outlier remain tracked below.

- **Foreground mouse confinement** (2026-11-08): owner-selected fix for wheel input leaking to background
  windows under the hidden system cursor. Windowed primary windows include the resize frame in their clip;
  activation loss, modal sizing/menu loops and destruction release it. Native 1.0 gameplay and 1.2 lab pass
  actual confinement/release checks, and all eight resize arrows remain correct. Existing crash recording
  also reverified through five isolated crash regressions, including a thread without TLS.

- **Original 1.0 fix-only copy** (2026-11-08): W:/Mafia10Fixes contains
  the copied original archives and native Direct3D 8 game with only
  the loader's built-in technical fixes. Initially no mods; the owner
  subsequently requested ragdoll 0.1.0 as its only gameplay mod. It loads
  live in 1.0 with its Jolt world and scenery active. Verified
  native system renderer, gameplay start, crash-fix activation and
  gamepad hotplug watching. A process-owned timer request now also fixes
  the native periodic 30 ms frame waits. Evidence in build/native-frametime-20261108.

- **Native frame wait precision** (2026-11-08): a process-owned
  timeBeginPeriod(1) request precedes the first original limiter call
  and is matched at host teardown. Native 16 ms steps and the 62.5 FPS
  ceiling remain. A controlled request/release reproduces and removes
  periodic 30 ms waits. Fresh 1.0 gameplay and both serial lab checks
  have p95 at most 16.865 ms and no measured interval over 20 ms.

- **Bounded CMake shader builds** (2026-11-03): owner stopped reflection work to fix the compilation loop.
  FXC/DXC now have separate preprocessing/optimization deadlines (10/60 s), per-output non-waiting locks,
  logs and atomic publication. Exact-input timeout records stop identical optimizer retries immediately.
  Compiler-generated active include dependencies replace the all-HLSL glob; individual shader targets
  allow the changed heavy entry to fail before unrelated shaders/C++ build. O3/WX and water quality retained.
  Five focused tool tests pass; real FXC timeout/retry and FXC/DXC success checked. Full reflection builds,
  game tests and deployment were deliberately stopped. This bounds failure, not the underlying heavy shader's
  compilation cost; step20 remains unaccepted. Standalone Rust shader compilation is outside this change.

- **Live window resizing** (2026-10-31): ordinary windows resize/maximize/restore after launch through native
  ResetDevice, root/child synchronization, scene viewport/camera aspect and HUD/menu rebuild. Resize arrows now
  appear on border hover even while the game hides the Windows cursor; borrowed visibility increments are
  restored on client entry/leave, deactivation and sizing transitions. D3D9 resources
  recover while D3D11/sidecar ownership stays alive; SDK reset events and rollback/suspended drawing on failure.
  Fixed dxwrapper's windowed ResetEx forwarding. Native pixel/scene/liveness checks with DLSS: 59/59 passed in
  1.2/1.0; water/reflection/atmosphere images checked after resize. Details: knowledge/notes/window.md.

- **Atmosphere SDK boundary** (2026-10-31): public C ABI and safe Rust owned requests, validation, arbitration,
  frame snapshots and automatic unload/fault restoration; policy-only example with its own native settings page.
  Host code has no dependency on example ids, palette presets or example config. Migration preserves old settings;
  `docs/ATMOSPHERE_SDK.md` is the extension guide. Both builds' native lifecycle/graphics checks passed.
- **Atmospheric height fog** (2026-10-31): Options > Mods > Atmosphere, saved density/height/base/tint/sun controls.
  Shared analytic height integral over city, real displaced water depth, outgoing reflection rays and sky horizon,
  in linear light before DLSS; works without an upscaler too. No extra scene rendering or temporal noise/history.
  WARP tests actual math and water-depth composite; both native builds checked. Evidence build/atmosphere-20261031.

- **Saved rendering controls** (2026-10-31): Options > Mods > Rendering has game/custom/whole-city distance,
  100..10000 m custom range with native cache streaming and independent occluders, plus game/off/custom native
  fog (start and fade distance in metres). Foam's range doubled: old 100% is now 50%, appearance/settings retained.
  Hardware/software fog paths covered; native tests 23 (1.2) and 22 (1.0), restart/load and UI checked in both.
  Offline suite passed; build/render-controls-20261031 and knowledge/notes/render-controls.md.

- **The base** (2026-10-13): the host (game events, hook chains, native mods with hot reload, fault isolation,
  services, state across reloads, the bridge, crash dumps), the core mod (the game's live state), the MCP server
  (status, launch/close, any call, memory, log, mods, `where`, crash dumps, deploy), offline tests (a stand-in game)
  and an MCP smoke test, the lab's modes.
- **The knowledge base** (2026-10-13): every function of the 1.2 game from MafiaReverse, our names, notes;
  `kb_search`, `kb_function`, named addresses in crash reports, `where` and `tap`.
- **`tap`** (2026-10-13): watch any function from the bridge or MCP; startup taps from `mml\config.json`.
- **The start** (2026-10-13): `game_launch {"profile", "go": "freeride"|"menu"}` and `tools/start.py`: the logo
  videos skipped, a profile taken and the default freeride started by the menus' own commands (about 3 s from the
  launch); `menu.state`, `menu.command` for any menu.

- **Eyes and hands** (2026-10-13): `screenshot` (the game's own frame as MCP image content, a short sequence for
  motion) and `input` (a timed sequence of the game's actions, pressed whatever they are bound to; keys as the
  fallback; an exact turn by an angle), both from inside the game: the owner's devices and the window's focus are
  not touched. The start takes the freeride's densities (`"police": 0`); `menu.value` reads and sets a menu's
  components.

- **The mod author's kit** (2026-10-13; the owner's direction: the loader is measured by how easily another agent
  writes a mod). What TrackMafia built for itself (threads, a window over the game, a settings window, hotkeys read
  from Windows, its own memory reads and build detection) the host now gives, each part seen working in the 1.2
  lab:
  - **drawing in the game's frame** (`mml.h` `draw_text`, `draw_text_width`, `draw_line`, `draw_rect`; the
    bridge's `draw.*`): the game's own text and 2D primitives, at the end of its 2D pass, in 800x600 units, in
    `screenshot`; a fault while drawing turns that mod's drawing off (provoked in the lab: the game went on);
  - **keys and actions for mods** (`key_code`, `key_down`, `key_pressed`, `action_slot`, `action_held`,
    `action_pressed`, `hotkey` with modifiers): what the game read, injected keys included;
  - **the car in the SDK** (`game.h` `PlayerCar`, `LastCar`, `Car`: position, velocity, speed, steering, gear,
    engine speed, fuel, engine health; `CarStateSave` / `CarStateLoad`, the game's own vehicle state functions),
    the core commands `car`, `car.save`, `car.restore`; 1.2 seen live, 1.0 checked against its files;
  - **the mission as events** (`MISSION_OPEN` and `MISSION_LOADED` with the name, `GAME_PAUSED`, `GAME_RESUMED`;
    `mission_name`, `mission_state`, `game::MissionName`);
  - **a mod's files and settings** (`mod_data_dir`; `config.json` defaults with the changes merged over them,
    read again a frame or two after a file changes: both folders watched, 2026-10-21; `CONFIG_CHANGED`; `config_*`;
    the bridge's `config.get`, `config.set`);
  - **a complete example and a start**: `mods/examples/telemetry`, `tools/new_mod.py` (its C++ and Rust mods made,
    built, loaded and taken away by the offline tests, 2026-10-21), `docs/MODDING.md`.
- **The game's own menus, extended** (2026-10-13; the owner's direction: an achievements mod, or any like it, must
  put its entry into the game's real interface, not an overlay; the base for such mods, not the mod). `mml.h`
  `menu_item` (an item in one of the game's menus, added by the game's own function for growing its lists),
  `menu_open` (a page of the mod's own, made by the game's factories from control descriptions, over the open menu
  or over the pause menu it opens), `menu_close`, `menu_set_text`, `menu_set_value` / `menu_value` (sliders,
  progress bars); the bridge's `menu.item`, `menu.page`, `menu.page.set`, `menu.items`; `menu.state` shows every
  open menu's controls. Seen in the 1.2 lab: the item in the pause menu (the window grown, walked by the keyboard
  in its place, Enter delivered), the telemetry example's page (texts, a progress bar, buttons that change
  settings, Back), a page opened with no menu open, a mod's reload with its item shown (taken over) and with its
  page open (closed), offline tests for the no-game answers.
- **A constructor of the game's interface, and the menus' mouse** (2026-10-13; the owner: "as complete a
  constructor as the game allows"). Pages take the game's kinds with their values: texts, rows, buttons, lines,
  progress bars, sliders, check boxes, switches and selectors with the mod's own options (the game's text database
  answers a range of the loader's ids), numbers with a range, text fields (typed into), pictures (image files the
  engine loads as textures) and scrolled lists (the game's scroll bar); `menu_set_range`, `menu_text`;
  `menu.def` shows any of the game's own definitions; items fit menus whose rows are closer than their height (the
  main menu). Agents drive menus as a player: `input.mouse` / MCP `input` `click`, `mouse`, `wheel` (the menus'
  cursor put exactly on a point, the game's own hover focus and click), `menu.click {id}`, `input.text`. Seen in the
  1.2 lab: every kind on one page over the main menu, each clicked and read back; items in the main menu, clicked
  with the mouse; the freeride started, a slider dragged and a dialog answered by mouse alone; the telemetry
  example's check box and switch.

- **Other projects' knowledge** (2026-10-14; the owner: "take the most from what others have reversed"): eight
  open projects on Mafia found and cloned into `re/ext/` (MafiaHub's MafiaOnline and Oakwood, reM and mafia-re for
  1.2; st0rm94's 1.0 reverse; mEdit, OpenMF, Insanity3D); `tools/kb_external.py` brings 3146 Game.exe names and
  1141 LS3DF names into the knowledge base, each address checked against our files (st0rm94's 1.0 names carried
  to 1.2 by pairing 2726 functions by their code), and the scripts' 396 commands with their 1.2 handlers.
  `knowledge/notes/external.md`: where to read what, by topic.

- **1.0, checked in the 1.0 lab** (2026-10-14; `docs/VERSIONS.md`): the host, the events, drawing, the car,
  keys, settings and the core mod run on 1.0 as on 1.2; the actions and the texts ported (the controls' arrays sit
  4 bytes lower in 1.0); screenshots on plain Direct3D 8 with antialiasing (the screen copied, only while the game
  is in front).
- **1.0 at 1.2's level: the start and the menus** (2026-10-14; the owner: "full support of 1.0, at the level of
  1.2"). 1.0's screens are other classes with no command handler (`host/menus10.cpp`, `docs/VERSIONS.md`): the
  start (the AVI logos, the profile, the main menu, the freeride and its densities), `menu.state`, `menu.command`,
  `menu.value`, `menu.def`, `menu.click`, the menus' mouse, the mods' items and pages (1.2's kinds asked for get
  1.0's), all seen in the 1.0 lab; the same in 1.2's lab afterwards. `check_builds.py` checks 1.0's menu addresses.
- **The game in the background** (2026-10-14; the owner: "can the game start without taking the focus?"):
  `game_launch` starts it without activating its window (by default), and the loader keeps the game from activating
  it (`start.json`'s `background`: user32's activating calls answered for the game's windows; LS3D's SetFocus,
  ClipCursor and SetCursorPos skipped while it is not in front). Keys, the menus' mouse and screenshots work there;
  the 1.0 lab got dxwrapper's Direct3D 9, as the 1.2 lab has. `game_launch` tells `in_front`.
- **The game's script machine for agents and mods** (2026-10-14; the owner: "pump up the agent's hands, as many
  abilities as there can be", for mods too; docs/SCRIPTS.md): a program in the missions' language compiled by the
  game's own compiler and run by its interpreter (`C_program`), one step a mission tick: every command the missions
  have. MCP `script` (run and wait: the state, the failing line, the registers) and `script_commands` (opcodes and
  handlers of both builds, st0rm94's notes read in place); the bridge's `script.run`, `script.state`,
  `script.stop`, `script.list`; mods' `script_run`, `script_stop`, `script_state` (the telemetry example's F7
  repairs the car). Seen in both labs: registers, waits, loops stopped by their time; a car teleported, a car
  cloned beside the player, rain, the traffic hidden, an explosion at an actor, a weapon given, a compile error
  named by its line.

- **The world without a program** (2026-10-14; the roadmap's "more hands", its first part; docs/SCRIPTS.md "The world
  without a program"): a car or a person of any model made new by the game's own functions (the scripts'
  `ACTOR_DUPLICATE` recipe with the caller's model file; the game's car list by name), taken out again (its model
  with it), moved and turned, seated in a car, armed by an item's name (`tables\predmety.def` read in memory). MCP
  `spawn`, `teleport`, `give`; the bridge's `world.*`; mods' `actor_spawn`, `actor_remove`, `actor_teleport`,
  `actor_seat`, `item_id`, `give_item` (the telemetry example's F6). `ScriptExec`: a program run at once, in the same
  frame. Seen in both labs: a Bolt-Thrower spawned, entered and driven; Paulie spawned with a Thompson, put into the
  player's car and riding in it; Sam armed; removals (the model gone too); refusals (a car's model as a person, no
  such file, a car someone sits in).

- **The start, completed** (2026-10-14): `game_launch` and `tools/start.py` take the game, by its own menus, into any
  freeride (`kind`: city-small, city-night, countryside-night, city, countryside; `place`: the spawn menu's row or
  its text), a chapter of the profile's saves (`"go": "load"`, `save`: its number or a part of its name; several or
  none: the error lists them), a new game, the freeride extreme. Seen in both labs: the night city at Hoboken (1.0),
  the countryside by day at its second place and the city at Hoboken (1.2), the 17th chapter's save by name (1.2) and
  number (1.0). The texts' code page is now the game's language's (the French 1.0's "Piège" was read as "Pičge").

- **A heavy mod's lifecycle, the loader's part** (2026-10-14; "Next" item 1): each load runs from its own folder in
  `mml\live\` with copies of every DLL of the mod's folder (its libraries are found, the mod's folder is never
  locked; a bare-name `LoadLibrary` in `mml_mod_load` finds them too). At an unload every other thread is looked at
  (EIP, the saved-EBP chain, its start): a thread still in the load's code keeps it mapped until it leaves, instead
  of a crash. A library left loaded is named, a reused old copy warned about, and each unload measures the process
  against the load; `mod.reload {times}` measures a reload loop. Offline test mod `heavy` (a library imported, one
  loaded by name, a thread left, a pool work item, leaks); in the 1.2 lab core and telemetry reloaded 20 times each:
  0 MB, 0 handles, 0 threads lost a reload, nothing kept by mistake.

- **The frame rate freed: the example mod `fps_unlock`** (2026-10-14; the owner's request, MafiaD3D11's A132 as
  the base): the limiter's minimum made 1 ms (a hook since 2026-10-25: A132's `jmp` gave a frame of 0 ms, which broke
  the intro's cutscene) and Present's yield patched (both builds' places in `game_build.h`, checked by
  `check_builds.py`; bytes checked before, put back at the unload), a cap of its own on a high-resolution timer, the
  frame rate and the game's clock step shown, `F11` / `enabled` to compare at once. Above 62.5 fps the main menu's
  camera started at full speed (its spring stepped once a call): fixed by a hook on `GM_Menu::TickCamera` that does
  the spring by the time, measured equal to 62.5 fps within 15 ms on one flight at about 1000 fps. The taxi's
  acceleration and running measured the same unlocked (1.2 lab; `knowledge/notes/game.md`, "The frame rate").

- **fps_unlock in 1.0** (2026-10-14): its two LS3D places run in the 1.0 lab (440..1315 fps); 1.0's menu camera,
  inlined into its menu loop and counted in frames (10 substeps a frame, no time), fixed by pointing the loop's 12
  `fmul` operands at the mod's own coefficients (the constants are shared with the rest of the game), measured equal
  to 62.5 fps at 1284 fps. The coefficients live in the process's heap, so a mod that faults leaves no pointer into
  its freed DLL; `fps_unlock.camera` records the scene camera's frame in both builds.

- **fps_unlock: a person's step at curbs** (2026-10-15; the owner's report: above 62.5 fps Tommy walks into a curb
  instead of stepping up; running, at a higher step). The cause: the person's collision sweep and its collide-and-
  slide make four lengths once a call (the sphere's start 5 mm up, the aim 15 mm down, the skin 1 cm off each
  contact, the ground's fall restarted each frame), balanced at 16 ms; at short steps a curb's edge is slid down. Fixed
  there (`fix_walk`): the 22 operands that read them point at the mod's floats, set for each call to the lengths of its
  step's time, the feet kept 1 cm above the ground as the game keeps them. Measured in both labs at 190..730 fps with
  `fps_unlock.path` and `fps_unlock.sweepprobe` (the sweep called from one place at any step): the owner's curb, the
  shop's ledge, stairs, the freeride spawn's curb of growing height climbed exactly where 62.5 fps climbs them, within
  a few cm and 0.02 s (`knowledge/notes/game.md`). A first fix that asked the sweep about a 16 ms step is gone. Both
  builds' functions in `game_build.h` (`humanSlide`, `humanSlideOwner`), the operands checked by `check_builds.py`.

- **fps_unlock: a stopped car at rest, the steering's dead zone** (2026-10-15; the owner's report: above 300 fps a
  stopped car creeps by millimetres for seconds and a mission waiting for it to stop waits). The cause: LS3D's
  `S_quat::RotationMatrix` gives the identity for any turn under 0.00049 rad, so a car's body cannot turn slower than
  0.2 rad/s at 400 fps (0.03 at 62.5): its rocking is never answered and it creeps, its slow yaw is lost. Fixed in
  the engine's function (`fix_rotation`: the export hooked by name, the matrix built for those quaternions); the car
  runs the frame's own steps: at rest 0.23..0.43 s after slowing down (62.5 fps: 0.07..1.47 s), a yaw rate of
  0.161..0.167 rad/s for 0.15 of steering (62.5 fps: 0.161). A first fix that ticked slow cars in 16 ms steps hid
  the cause and is gone. `fps_unlock.path` records the car every frame, `fps_unlock.stepprobe` compares one step with
  several shorter ones from one state; `knowledge/notes/vehicle_physics.md` has the car's step, wheels and rest.

- **The car's physics in the SDK** (2026-10-15): a car's rigid body and wheels for every mod, both builds:
  `game::Car` gives the angular velocity, the body's axes, calm and asleep, the step, the wheels (`kWheel*`: frames,
  load, suspension); `game_build.h` has Tick, Engine, Move and the wheels' collision to hook, and LS3DF's quaternion
  functions (`quatRotationMatrix`...; `mathx.h` `QuatMatrix` is its formula without the loss). Each address and
  offset is checked in both builds' code by `tools/check_builds.py` (wrong values put in by hand fail it); the core's
  `car` lists the body and each wheel, seen the same in the 1.2 and 1.0 labs. `fps_unlock` uses them (its step probe
  now reads 1.0's layout too).

- **`car.restore` with dents** (2026-10-14): a car's state grows with its body's dents, and the restore refused a
  state of the same taxi taken before a crash (its size was the test of the model). `CarStateLoad` now repairs the
  car first (the scripts' `car_repair`: the game's load writes only the vertices a state lists) and checks the model
  by the counts the load walks by (`CarStateFits`: lights, body parts, seats, wheels); `car` tells `state_bytes`, and
  `car.save` survives the core's reload. Seen in the 1.2 lab: a dented state (1285) back into the repaired taxi, a
  whole one (1237) back into a crashed taxi (1437, engine 86 %: back to 1237, 100 %), the state into another taxi of
  the model, a Bolt-Thrower refused.

- **A heavy mod's lifecycle, the build's part** (2026-10-15; "Next" item 1): third-party libraries for mods pinned
  by `tools/deps.py` and built with the loader (Jolt Physics 5.6.0 first: target `Jolt`, x86 SSE2, `/MT`; fetched by
  the first configure, again when its pin changes), shaders compiled in a mod's build (`mml_add_mod(... SHADERS
  <file>:<entry>:<profile>)`: the Windows SDK's fxc for shader model 5 and lower, dxc for 6; a header of bytecode a
  shader), and the first heavy example, `mods/examples/boxes` (a Jolt world on its own job threads, a Direct3D 11
  device drawing it with the mod's HLSL and counting its pixels on the GPU, its world kept across reloads). Built from a
  clean copy of the repository (`cmake --preset x86`, `cmake --build --preset x86-release`); offline tests
  (`boxes_tests`: the world, the device, a BMP of its picture, reloads, 20 in a row, settings); in the 1.2 lab 20
  reloads with frames between and 100 in a row: 143 devices made, each drew, none lost, each released to its last
  reference, 0.0 MB, 0 handles, 0 threads lost a reload (`docs/MODDING.md`, "A heavy mod's build").

- **A mod's own files in the game** (2026-10-15; "Next" item 2): every file under a mod's `files\` is a game file by
  its path there (`rw_data.dll`'s `dtaOpen` answered from the mods first, from the process's start), new or in
  place of the game's; the later mod in load order answers a name two ship; mods of files alone (`"type": "files"`,
  `mml_add_files_mod`); `file_add` (a file of the mod's choosing for a name: a patched copy of a table),
  `file_source`, `file_read` (the game's own under a mod's: `MML_FILE_GAME`), `MML_EVENT_FILE_CHANGED`; changes on
  disk seen; the game's presaved copies of a replaced texture hidden; `files.list`, `files.which`, `files.read`;
  `deploy` copies subfolders and replaces a file the game holds open. Offline tests (`files_tests`: a stand-in
  `rw_data.dll`, 46 checks; `mcp_smoke.py`: deploy); in the 1.2 lab the example `freeride_picture` replaced the
  freeride menu's picture (its presaved copy hidden), a new picture deployed into the running game showed at the
  menu's next opening, the game's own came back at the mod's unload; the same in the 1.0 lab (1.0's LS3DF asks the
  same presaved name), with a mod made for 1.2 only refused there (`"builds"`) (`docs/MODDING.md`, "A mod's own
  files in the game").

- **The render events** (2026-10-15; "Next" item 3, block 3a): `MML_EVENT_RENDER_BEGIN`, `_WORLD` (the 3D world
  drawn, the HUD not yet), `_END`, `MML_EVENT_PRESENT`, with the scene, whether it is the mission's world and the
  game's device; `render_device` (8, or a wrapper's Direct3D 9), `render_camera`; the game's Direct3D 9 state kept
  around the mods' drawing (a state block, the render target, the depth buffer); `render.state`. The example
  `mods/examples/world_marker` drew a diamond and a beam over the player in the 1.2 and the 1.0 labs, hidden by a
  building in front of it, under the HUD. On the way: a mod's fault is logged with where (`<module>+<offset>`),
  what it touched and the event (`docs/MODDING.md`, "Drawing in the 3D world").

- **The graphics bridge** (2026-10-15; "Next" item 3, block 3b): one Direct3D 11 device beside the game's 9Ex shared
  by every mod (`gfx_pass`, `MmlGfxFrame`, `mml/gfx/frame.hlsli`): the readable depth buffer, the frame's colour and
  linear depth, the camera, the world's fog and sun (from the game's draw calls), the passes in order into one
  effects target composited over the frame under the HUD, each pass's CPU and GPU time (`gfx.state`); the game's
  device made without multisampling while a mod says `"gfx": true`; the loader's own shaders and the mods' share
  `mml_shaders`. The example `mods/examples/depth_pulse` (a scanner's wave from the player) in the 1.2 and 1.0
  labs, about 0.65 ms a frame for the bridge; 10 reloads of it: 0 MB, 0 handles, 0 threads.
- **The 2D pass for mods** (2026-10-15; item 3, block 3c): `gfx_ui` takes the HUD and the menus into a shared target
  for the passes (`MmlGfxFrame::ui`), off the screen or also on it; `mods/examples/hud_panel` showed the HUD and the
  pause menu on a panel in the world in the 1.2 and 1.0 labs, and the HUD unchanged on the screen with `take`.
- **The scene from another camera** (2026-10-15; item 4, block 4a): `scene_view` (currently eight per owner, 64 total): LS3D
  draws the mission's scene again before the frame, from a mod's camera (a pose, an asymmetric frustum of four
  tangents, a clip plane, the game camera's room or its own), into targets of the graphics bridge that the passes
  get (`MmlGfxFrame::views`: colour, linear depth, matrices); the game camera, the scene's callback, its listener and
  its lens flares put back; `MML_EVENT_VIEW_WORLD` for mods drawing into the views (`world_marker` does);
  `view.state`, `view.show`. The example `mods/examples/rear_mirror` (mirror, stereo, reflect, check) in the 1.2
  and 1.0 labs: the left half of the frame drawn again equal to the frame, a puddle reflecting the taxi, a lamp post
  and the street with the ground clipped out, two eyes of a headset; a 640x200 mirror 1.5..1.7 ms of CPU, a
  reflection of the frame's size 2.9 ms; reloads: 0 MB, 0 handles, 0 threads once warm. `check_builds.py` checks
  the camera's and the scene's addresses in both builds. The same day, the owner's test: a turning mirror standing
  in the world (`rear_mirror` `spin`) in both labs, the player and the street in it; the sky is no longer cut by a
  view's clip plane (LS3D draws it about the origin: the plane is off while its view is set), and passes run over
  the views when their mod asks (`gfx_pass_views`: `depth_pulse`'s wave in the mirror). `world_marker` and `boxes`
  ship disabled.
- **The game's own geometry hidden or replaced** (2026-10-15; item 4, block 4b): LS3D's face groups chosen by the
  file name of their texture (the animation's frame now), left out of the frame's drawing, the views' or both
  (`geom_hide`; the three m_RenderFGroup functions that draw every mesh hooked, nothing in the game's memory changed,
  no pointer kept between frames), surveyed (`geom.survey`: every texture a frame draws) and captured (`geom_capture`:
  the triangles in the world's space, read from the buffers LS3D hands Direct3D); the passes now laid over each other
  (premultiplied over: a pass's 0 leaves the ones before it). The example `mods/examples/river_water` (the city's
  river hidden and drawn anew with waves, its reflection, the bed seen through it) in the 1.2 and 1.0 labs;
  `check_builds.py` checks the three functions in both builds. With 4a, item 4 is done.
- **Measuring the frame** (2026-10-16; item 5): every entry into a mod timed by its site, the loader's heavy parts,
  the frames (Present to Present: average, median, p95, worst, hitches with what took them) on `prof.state`; the
  game thread sampled every 0.5 ms with its phase, its mod and the game's caller of the driver (`prof.sample`,
  MCP `profile` naming the game's functions); `frame_stats` for mods (`river_water.status` shows it). Seen in the
  1.2 and 1.0 labs (the river's reflection: 41 % of the game thread in 1.2); offline tests and `mcp_smoke.py`.
- **Waiting for what happens** (2026-10-16; item 20): the bridge keeps the last 2000 events with a sequence number
  (every event the host sends mods but each frame's, and the host's own: a mod unloaded or faulted, a script
  ended), answered on its own thread (`events.since {seq, types, filter}`: while the game loads too); MCP `wait`
  (polls it until an event of the types with the filter's fields comes). Seen in the 1.2 lab (a freeride's
  missions opened, loaded and closed; a person spawned; a script ended); offline tests and `mcp_smoke.py`. The
  roadmap's `events.wait` on the bridge is MCP's `wait` (a bridge call that blocked would hold its client's line).
  What item 7 will add (deaths, shots, cars entered) comes through the same way.
- **`native.call`** (2026-10-16; item 21): any function called from the bridge on the game thread by its address and
  convention (stdcall, cdecl, thiscall, fastcall), with integers, addresses, floats, doubles, strings and out-buffers,
  its integer, pointer, float (ST0), 64-bit or string result; ESP measured around the call (a wrong convention or
  count of arguments shows, and the stack is put back), the x87 stack left as found, a fault guarded and named.
  Offline: every convention, floats, a buffer, a wrong convention, a fault (11 checks); in the 1.2 lab the game's
  text 1403 ("Game Over") by its text lookup (thiscall on the text database), in the 1.0 lab the same ("Fin de
  partie").
- **Hardware watchpoints** (2026-10-16; item 22): up to four on the game thread (DR0..DR3), write (changes) or
  access, 1, 2 or 4 bytes; each piece of code recorded once with its count, the game's return addresses on the
  stack and the values; the handler reads nothing in access mode and allocates nothing. `watch.set`, `watch.show`,
  `watch.clear`; MCP `watch` names the code. Offline (a kit field written by a command: found twice, access mode,
  refusals); in both labs it found what writes a person's bone (`C_human::Update` running LS3D's animation).
- **What happens, as events** (2026-10-16; item 7, block 7a): `MML_EVENT_SHOT` (C_game::NewShoot), `EXPLOSION`
  (C_game::NewExplosion), `CAR_EXPLODED` (C_car::CarExplosion), `HUMAN_DIED` (each person's alive flag every frame,
  the killer the last who hit him in 3 s) and `VEHICLE_ENTERED` / `_LEFT` (each person's vehicle every frame); all
  on `events.since` and MCP `wait`. Both builds' addresses checked by `check_builds.py`. The example
  `mods/examples/tally`; seen in the 1.2 and 1.0 labs (the player's shots, his leaving the taxi, a person killed by
  a script, a car blown up with its explosion and the hit it gave the player).
- **What happens, the rest** (2026-10-16; item 7, block 7b): `MML_EVENT_BULLET_HIT` (what a bullet struck that is
  not a person: the moving objects from the bullets' tick's collRayTest, the static world from the impact NewShoot
  kept in the bullet's record, told when the bullet comes to it), `CAR_CRASH` (C_car::Was_Collision: each contact
  with the world or a car changing the car's speed by 1 m/s or more), `HUMAN_FELL` (the state 3 left: the fall's
  speed and height), `ITEM_GAINED` / `_LOST` (each person's items by id and rounds, every frame) and
  `MONEY_CHANGED` (C_game +0x3640, every frame). A car striking a person is his `HUMAN_HIT` (types 5 and 8, the car
  the attacker). Both builds' addresses and offsets checked by `check_builds.py` (Was_Collision paired, its mass and
  surface reads, the shot queue, the static impact's flag, the landing's state and speed). Seen alike in the 1.2
  and 1.0 labs: the taxi driven into a wall (9.5 and 9.9 m/s, 1520 kg, surface 33) with the driver's hit; the
  Thompson's bullets in the wall (the normal toward the shooter), in the taxi (`struck` it, kind 0x40) and in
  Paulie (only his HUMAN_HIT); a fall of 8.44 m at 13 m/s with its hit of type 6 (112 damage) in the same frame;
  the Thompson dropped (lost, -147 rounds), picked up from the ground (1.2: the game's "Pickup/Use" menu, which pauses
  the game; 1.0: taken at once, the only item there), a Colt given to one who had it (rounds only, +14), the Magnum
  taken; the money set.
- **A mod's own items** (2026-10-16; item 10, block 10b): `item_define` adds a record to the game's items (the table
  moved once into the game's own heap, `gameNew`, with room for 128; the count raised), copied from the game's item
  it is made like, with its label (the game's texts 3300, 3400 and 3500 + id answered by the loader, an id given
  only where all three are free in the game's database), model and flags; ids kept by "mod/name" in
  `mml\items.json` (an id whose mod is not loaded is an inert placeholder). `item_find`; `items.define`,
  `items.state`; `world.items`' `mod`. Both builds' table and `gameNew` checked by `check_builds.py`. Seen alike in
  the 1.2 and 1.0 labs: "Fishing rod" (made like the bucket, its model) and "Trout" (like a key) at 35 and 36,
  given, the rod dropped (the bucket's model on the ground) and picked up ("Fishing rod collected", in his hand in
  1.0), the trout taken; ITEM_GAINED and _LOST for each; after a restart the ids kept (placeholders until defined).
- **Statistics, the profile's progress** (2026-10-16; item 10, block 10c): `stats_get` (what the player did, counted
  by the loader from the events: kills, deaths, shots, hits, cars got into, crashes, falls that hurt, items, the
  distance on foot and in vehicles with teleports left out, the time played, money gained and lost;
  as of 2026-11-15 collection is opt-in and in memory, with persistence owned by the requesting mod),
  `profile_progress` (the profile manager's own:
  the chapter of the newest story save, the race, steal and freeride bonus-car letters, the garage, the freeride's
  level word), `profile_saves`; `stats.state`, `profile.progress`. Seen alike in the 1.2 and 1.0 labs: profile 4
  (chapter 20, 105 saves, "ABCDEFGHIJPR" bonus cars) and the 1.0 lab's "Run" (chapter 6, French save names); a kill
  with the Thompson (and its hits and shots), a fall of 8 m, a crash into a wall, items given, money set; the
  profile's totals kept after a restart in the original 2026-10-16 test. That automatic persistence contract
  was replaced on 2026-11-15; explicit save_put records now determine durable state and companion files.
- **The water, zones for everyone** (2026-10-16; items 8 and 15, block 6a of the order): `world_water` (the static
  world's faces of the water's surfaces 31 and 44 under a point, rays going on below what is above them: its
  height, the bed, covered), `zone_watch` (a zone watching every person, every car, or one actor; the crossings with
  the actor, on `events.since` too); `world.water`, `zone.add {who}`. Seen alike in the 1.2 and 1.0 labs: the river
  by Little Italy's bridge at y -9.99 (surface 31, no bed in the collision; covered under the embankment), the taxi
  and the player crossing a zone of cars and people, Paulie entering one and leaving it as he fell into the river,
  a zone watching the taxi alone.
- **A prop's animation and physics** (2026-10-17; item 12, block 6b of the order): `prop_anim_play`,
  `prop_anim_stop`, `prop_anim_state` (the scripts' `model_playanim` and `model_stopanim`: the animation cache's set
  given to the model's SetAnimation, the model on the mission's list of animated models), `prop_physics`,
  `prop_push`, `prop_body` (the scripts' `create_physicalobject`: a C_public_physics on the prop's frame holding its
  own reference to it; its GameDone's placement written before it goes, so the prop stays where it lies, or comes
  out of the scene when removed); `prop.anim`, `prop.physics`, `prop.push`. Both builds' addresses checked by
  `check_builds.py` in the three handlers. Seen alike in the 1.2 and 1.0 labs: a prop of Tommy's model walking in
  place (`walk1`, looped and once), a bat falling, pushed 4 m, moved while physical (made again at its new place),
  left where it lay, removed while physical (gone from the screen), a mission changed with an animated physical prop
  in it.
- **The game's files and models** (2026-10-17; item 8, block 6c of the order): `game_files` (the archives'
  directories read with the game's keys, the same in both builds; the game's folder's and the mods' files over
  them), `model_file` (the name LS3D keeps in every model frame, +0x154: no hook needed), `model_open`,
  `model_nodes`, `model_mesh`, `model_materials`, `model_close` (a .4ds parser after mafia-ragdoll's);
  `files.game`, `model.file`, `model.info`. Seen alike in the 1.2 and 1.0 labs: 3223 models listed, 3220 parsed (two
  scenes of version 27, one model of 58 refused), the player's and the taxi's files named.
- **Shaders on the game's own drawing** (2026-10-17; item 27, blocks 7a and 7b of the order): `shader_add`,
  `shader_remove` (a mod's Direct3D 9 shaders set on the game's draws a rule picks, through geom.cpp's face group
  and DrawIndexedPrimitive hooks, the game's state put back after each; HLSL compiled by d3dcompiler_47 with the
  contract's header answered from the host), `sdk/include/mml/shaders/game.hlsli` (the constants and the fixed
  pipeline's stages, fog, a face's normal), `sdk/include/mml/shaders/reference.hlsl` (the game's picture);
  `shader.add`, `shader.remove`, `shader.state`, `shader.survey`. Measured first: the game's draws carry lit
  colours, no normals, Direct3D's lighting off, a handful of stage set-ups (both builds alike). Seen alike in the 1.2
  and 1.0 labs: the reference on all 362 draws of the freeride's start, the frame the game's (a mean difference of
  0.25 of 255; 0.08 % of the pixels over 8: the car's moving exhaust); no cost seen at the game's 62.5 frames a
  second. The owner found the one difference of a first version: a sign behind a wall showing through it, the
  position computed in another order than the game's (now its order).
- **The sun's shadow and the game's lights for shaders** (2026-10-17; item 27, block 7c of the order):
  `shader_shadows` (a shadow map from the scene's sun, R32F on the game's device, drawn before the mission's world
  from block 6d's meshes around the camera and the cars near it, its texels snapped; every state put back),
  game.hlsli's `mml_ShadowViewProj`, `mml_Shadow`, `mml_SunDir`, `mml_SunColor`, `mml_SunAmbient`, the game's point
  and spot lights near each draw (`mml_GameLights`, `mml_GameLight`); `shader.shadows`, `shader.state`'s `shadows`.
  Seen alike in the 1.2 and 1.0 labs: by day the taxi's shadow on the parking lot and the building's on the pavement
  (the map 0.5 ms a frame, 140 to 250 casters, 8 to 17 thousand triangles); at night a car's headlights (its own
  spots) and the street lamps lighting the street through `mml_GameLight`. Found on the way: `mml_FaceNormal`
  (block 7b) gave the normal pointing away (the street's y down): corrected and measured.
- **The sun's shadow map in the frame's passes; volumetric light** (2026-10-17; item 27 and item 3, after 7c, the
  owner's demonstration): the map made shared (Direct3D 9Ex) and opened by the graphics bridge, bound to t2 of every
  pass with its matrix in `cbuffer MmlFrame` (`MmlGfxFrame::sun_shadow`, frame.hlsli `SunLit`, `SunShadow`); the map
  drawn with a slope-scaled bias, `MmlShadowSettings::bias` in metres. `mods/examples/sun_shafts`: god rays, each
  pixel's ray marched through the map at half size (single scattering, Henyey-Greenstein, Beer-Lambert), a blur and
  an upsample that keep depth edges, about 0.2 ms of GPU at 1280x960. Seen alike in the 1.2 and 1.0 labs (the
  shadows in the air under the trees and behind the buildings; the owner saw the rays from the buildings). Found:
  with only red written to the R32F map, Direct3D 11 saw the map as cleared (knowledge/notes/game.md).
- **Doubles in the mods' code on the game thread** (2026-10-17; a gap the settings panel found, the owner asked to
  fix it): measured in both builds, the game enters the loader's frame with the x87 at 24 bits (`0x007F`: its
  Direct3D made without `D3DCREATE_FPU_PRESERVE`), so 32-bit code's doubles came out as floats wherever the x87
  computed them. Every call into a mod (mods.cpp's Safe* gates: events, commands, posted calls, passes, menus, load,
  unload) and the loader's own pages now run at 53 bits (`0x027F`) and give the game its word back, a fault too;
  `native.call` calls the game with the game's word. `fpu.state`. The stand-in game of the tests keeps 24 bits as the
  game does: a mod's 1/3 on the x87 is the double, and the stand-in gets its 24 bits back after every frame. Seen in
  the 1.2 and 1.0 labs (the panel's values written clean: 0.0152, not 0.015200000256).
- **A tuning panel** (2026-10-17; item 18, the owner put it before the order's 8): Options > Mods (`optionsb`, both
  builds) lists the loaded mods whose `mod.json` has `"settings"`; each opens a page of the game's own controls
  (sliders, check boxes, selectors) made from it, changes made live (config.cpp `ConfigSetLive`: the mod told at the
  next frame, the file written once the changes stop), Defaults and Back; Esc returns to the list as the game's pages
  do. The menus got the loader as an owner (two: the list, a mod's page over it). `panel.state`, `panel.open`.
  `sun_shafts` the first user. Seen alike in the 1.2 and 1.0 labs (the owner moved the sliders and watched the haze).
- **The meshes the game draws** (2026-10-17; item 8, block 6d of the order): `scene_focus`, `scene_pieces` (the
  scene walked a little each frame, scanned every 0.25 s around every mod's box: the objects' meshes read once from
  LS3D's buffers, their space found by the engine's bounding sphere; after mafia-ragdoll's scenery and
  visual_meshes), `actor_meshes` (an actor's visuals posed now, another model hung on it left out); `scene.focus`,
  `scene.pieces`, `actor.meshes`. Seen alike in the 1.2 and 1.0 labs: the freeride's start, 140 m around the player:
  about 100 pieces and 4 400 to 5 600 triangles, none unreadable nor out of place, the walk of 24 000 frames 2 ms in
  all; the taxi's 24 parts (body, four doors and windows, wheels) with Tommy in it left out.
- **Interaction** (2026-10-16; item 15): `zone_add` (spheres the player crosses, on foot or in his car, in the world
  or following a frame), `use_point_add` (a record of the loader's in the game's own using objects, C_game +
  0x29EC: what Action finds on foot, offered alone or in the game's own choice menu with a label that is a text id
  of the loader's in the game's database; the pick read from FindNearObjects' list and ExecuteMenu's result),
  `timer_add` (the mission's clock), `interact_remove`. `interact.list`, `zone.add`, `use.add`, `timer.add`,
  `interact.remove`; ZONE_ENTERED, ZONE_LEFT, USED, TIMER in `events.since`. The HUD's and the using objects'
  functions checked in both builds by `check_builds.py`; seen in both labs: "2. Fish here" under "1. Get in the
  front" beside the taxi, its pick a USED with no car entered, the car's pick entering it; a point alone taken by
  Action at once; one on Paulie's model taken, and gone when he was removed; the mission's close leaving none; a
  zone entered, a timer at 2 s (1.0).
- **The HUD** (2026-10-16; item 16; host/hud.cpp, draw.cpp): pictures opened by the game's own texture loader
  (`picture_load`, IGraph::CreateITexture with the HUD's own flags: a .tga's alpha, a .bmp's "+" twin), drawn turned,
  tinted and cut (`draw_picture`, `draw.picture`); the game's console (`hud_console`, ConsoleAddText) and help line
  (`hud_help`); markers (`hud_marker_add`, `_set`, `_remove`; `hud.marker`, `hud.remove`, `hud.list`) on the screen
  (projected with the camera of the frame being drawn, held at the screen's edge when off it, with the distance), on
  the radar (RadarAddObject) and as the compass' target (CompassSetDestination and the HUD's flag 0x80, as the
  scripts' SETCOMPASS; given back to the mission's target), a place of the world given to them as an LS3D dummy
  frame. The HUD's flags and the compass' target paired in both builds by `check_builds.py` (1.0's target is not
  0x5000 further as the rest: 8 less). Seen in both labs alike: a marker on the taxi held at the screen's left edge
  "Taxi 19 m" while the compass pointed behind, a second one taking the compass (the needle turned to it) and giving
  it back when removed, the compass hidden again and the HUD's flags as they were when the last went; a marker on
  Paulie gone with him; the scope's `2sight.bmp` with its `+` twin (the circle clear), `1numbers.tga` tinted red
  and turned; console and help lines; markers hidden under the pause menu; leaving to the main menu left none.
- **Animations, getting up, the pedestrians' hits** (2026-10-16; item 6, block 6b; host/anims.cpp, humans.cpp):
  `human_anim_play` (C_human::Do_PlayAnim, a file of anims\, its length), `human_anim_stop` (Do_StopAnim),
  `human_anim_replace` (ChangeAnim; given back at the mod's unload), `human_anim_state` (the animations machine's
  time and length by GetActiveAnimTime, his state, the playing number and its file in the game's table, lying on
  the back or the front), `human_get_up` (the pose last drawn, the person put where it lies and turned, the game's
  get-up 111 or 338 queued as Do_ShockDown queues one, the host blending into it); MML_EVENT_PEDESTRIAN_HIT from a
  hook on the traffic walkers' Hit. `human.anim`, `human.anim_state`, `human.get_up`, `human.anims`. 1.0's fields
  for bullets, getting up and trams filled (twins with the same code, offsets read; `check_builds.py`). Seen in both
  labs alike: "boj box kombo" (640 ms) and "x panika drep stativ" (1000 ms) played on Paulie, the machine's time
  moving; the player's standing (`breath05a.i3d`) replaced by the panic crouch and given back; Paulie laid on his
  back and on his front by a held pose, `lying` 1 and -1, and getting up from each at the same place (the back's
  get-up sitting up, the front's pushing up on all fours); a scripted explosion killing two (1.0) and one (1.2)
  traffic pedestrians: PEDESTRIAN_HIT with their place, damage, type 2 and model frame.
- **A mod's state in the saves** (2026-10-16; item 17; host/saves.cpp): `save_put`, `save_get` by scope
  (`MML_SAVE_SLOT`, `MML_SAVE_PROFILE`); MML_EVENT_GAME_SAVING before the game writes a save, MML_EVENT_GAME_LOADED
  after it loads one or starts a new game (slot -1); the slot's state written beside the game's save
  (`savegame\mafia<p>.<s>.mml`) by a hook on SaveGameSave, read back by one on SaveGameLoad, cleared by one on
  SaveGameNewGame; the profile's written at once; ProfileDelete's hook takes a profile's `.mml` files. `save.state`,
  `save.get`, `save.put`. The four functions and the manager's offsets read in both builds by `check_builds.py`.
  Seen in both labs alike: the freeride's start a GAME_LOADED (slot -1) and its autosave a GAME_SAVING (slot 570); a
  chapter (1.0 06-03, 1.2 Old Prison) loaded with no state, a key put, the game's SaveGameSave called, the game
  closed and the chapter loaded again: the key back, and the profile's key across the restarts.
- **Control** (2026-10-16; item 14, block 14a): `key_capture` (a key kept from the game: LS3D's keyboard state
  cleared after mods read it), `action_capture` (an action's slots written 0 after the update, the player's values
  kept for `action_held`), `player_lock` (C_player::LockControls and Do_Breath, as PLAYER_LOCKCONTROLS),
  `vehicle_hook` (C_Vehicle::Engine: a mod steps a car's body; the host writes the body, its matrices and the model
  frame, and keeps the car awake), `vehicle_body`. `input.capture`, `player.lock`, `vehicle.body`, `control.state`.
  Checked in both builds by `check_builds.py` (1.0 keeps the step before's matrices at the C_Vehicle's start); seen
  in both labs: a captured Accelerate or W/Up kept the taxi still (5 m without), the locked player stood, `hover`
  flew the taxi 60 m 1.5 m over the street and turned it, and let go it fell. The primary and secondary bindings
  read in the code (knowledge/notes/game.md, the owner's question). The example `mods/examples/hover`.
- **Sound** (2026-10-16; item 13): `sound_play` (LS3D's I3D_sound: 2D as the menus' own sounds, in the world at a
  place, or following a frame; volume, pitch, range, looped), `sound_set`, `sound_move`, `sound_stop`,
  `sound_state`; music (`music_play`, `music_set`, `music_stop`: LS3D's IShow, the game's own streaming, times the
  options' music volume, looped by its end event) and `music_game` (C_game::CityMusicPaused). `sounds.list`,
  `sound.play`, `sound.set`, `sound.stop`, `music.play`, `music.set`, `music.stop`, `music.game`. Checked in both
  builds by `check_builds.py`; seen in both labs: the pitch and the volume at the DirectSound buffer (22050 Hz times
  0.5 and 1.5: 11025, 33075; volume 0.5: -30.6 dB), a 3D sound on the taxi following it, the world's sounds held by
  the pause, a track played to its end and released, a looped one started again, the city's music paused
  (C_game +0x2FC0). The example `mods/examples/engine_note`.
- **Props** (2026-10-16; item 12, block 12a): `prop_create` (a model by the game's model cache as an LS3D frame with
  no actor: in the world, linked into its room as a spawned actor's model, or attached to any frame: a bone, an
  actor's model, a car, another prop), `prop_place`, `prop_attach` (its world place kept or its local one),
  `prop_show`, `prop_remove`, `actor_frame`, `frame_world`; the host removes a mod's props at its unload and every
  prop at the mission's close, and puts props on a destroyed actor into the world. `props.list`, `prop.create`,
  `prop.place`, `prop.attach`, `prop.show`, `prop.remove`. The example `mods/examples/props_demo`; seen in both
  labs (a bat on a car carried 13 m with it; on Tommy's back through his walk, his arrest and a game over's retry;
  in Paulie's hand, left in the world when he was removed; none left after the mod's unload or the mission's close).
- **The weather** (2026-10-16; item 11, block 11d): `weather_get` / `weather_set` / `weather_reset` (the mission
  scene's LS3D weather: its 15 parameters, read and set through its own getter and setter, a flag, an int, a colour
  or a float's bits each, as the scripts' `weather_setparam`, with `C_game::OsefujPocasi` after `on` and
  `max_count` as they call it); `weather.state`, `weather.set`, `weather.reset`. The scene's vtable slots and the
  game's function checked in both builds. The example `mods/examples/storm` seen in both labs (rain on, slanted,
  everything put back exactly at its unload). With it, item 11 is done.
- **The game's particles** (2026-10-16; item 11, block 11c): `particle_hook` (every move of every particle the game
  shows, the emitters' puffs and the pooled effects: where the game put it, how far it moved it, since when; the mod
  may put it elsewhere, and it goes on from there), the particle visuals' vtables (PRTB, PRTE) patched at Update and
  SetParent in both builds (`check_builds.py`: each written before its type's store, its slots the frame's own);
  `particles.state`. The example `mods/examples/wind` seen in both labs (the taxi's puffs carried 7.5 m downwind in
  their life; about 10 000 moves a second in the city, no fault).
- **The car exhaust and particles.def** (2026-10-16; item 11, block 11b): `car_exhaust` (a car's pipes in the world,
  the way they blow, the jet by the engine's turns, the car's velocity at each, the emitter's additions, as the
  game's `C_Vehicle::UpdateExhaustParticles` reads them), `exhaust_hook` (told before the game puffs, every tick,
  every car; 1 leaves the game's puffs out), `particle_def` / `particle_def_set` (tables\particles.def's 23 records);
  `exhaust.state`, `particles.defs`, `particles.def.set`. 1.0's exhaust found by its code and paired with 1.2's
  instruction by instruction; `check_builds.py` checks every field it reads. The example `mods/examples/exhaust_fx`
  seen in both labs (the taxi's puffs replaced; the game's own exhaust tinted red from the mission's opening).
- **The scene's lights** (2026-10-16; item 11, block 11a): `scene_lights` (the level's I3D_light frames: type,
  colour, power, range, cone, place, direction, sectors), `light_set` (through LS3D's own setters, the vtable checked
  first), `scene_sun` (the brightest directional light and the ambient light beside it); the graphics bridge's
  `g_sun`, `g_sunColor`, `g_ambient` take them when the frame has no Direct3D light (`gfx.state` `sun_from`);
  `lights.list`, `light.set`. I3D_light's vtable in both builds checked by `check_builds.py`. The example
  `mods/examples/dusk` seen in the 1.0 and 1.2 labs (the whole city tinted, put back at its unload).
- **The game's state** (2026-10-16; item 10, block 10a): `money` / `money_set` (the freeride's money, C_game
  +0x3640, through the game's `C_game::SetScore`: the HUD shows it), `human_health` / `human_health_set` (+0x644 of
  +0x604; the player's HUD number told through `G_IndicatorsClass::PlayerSetLives` as a hit tells it),
  `human_items` (the G_Inventory at C_human +0x480 slot by slot), `item_ammo_set`, `human_take_item` (the scripts'
  `human_delweapon`); `state.money`, `human.health`, `human.items`, `human.ammo`, `human.take`. Both builds' setters
  checked by `check_builds.py`. The example `mods/examples/bounty` (money for kills, health back, endless ammo)
  seen in the 1.2 and 1.0 labs: a spawned Paulie shot by the player paid $50 on the HUD.
- **Questions to the world** (2026-10-16; item 8, block 8a): `world_ray` (g_collision::_TestLineH, the bullets'
  and the AI's test: the static world and the moving objects), `world_sweep` (TestSphereB, the moves' sphere test),
  an actor ignored the game's way (its objects on the tests' exclusion list, DynSetNextCollOwnerMult, the list put
  back after), `world_near` (the actors within a radius, nearest first), `world_shapes` (the static grid's
  triangles, boxes, spheres and poles and the moving objects' boxes and spheres in a box); `world.ray` (from the
  camera: what the crosshair looks at), `world.sweep`, `world.ground`, `world.near`, `world.shapes`. Both builds'
  addresses checked by `check_builds.py`. The example `mods/examples/probe`; seen in the 1.2 and 1.0 labs (the
  ground under Tommy, a ray through him hitting him without `ignore` and the taxi with it, a sweep stopping at the
  taxi, the shapes around him); offline tests with no game.
- **The camera** (2026-10-16; item 9, block 9a): `camera_override` (the frame of the mission's world drawn from a
  mod's camera: its pose, an asymmetric frustum, its range, its room; the camera's bytes put back after the frame,
  the scene views starting from it), `camera_project` and `camera_ray` (the last frame's camera, overridden or not,
  800x600 units); `camera.set` (an agent's camera held for a time), `camera.release`, `camera.state`. The example
  `mods/examples/orbit_cam` (an orbit around the player while he is played, a label over his head) seen in the 1.2
  and 1.0 labs; `camera.set` from above the taxi in both; offline tests with no game.
- **Another kind of vehicle: the game's airplane** (2026-10-17; item 14's rest, block 8c of the order): the game's
  `C_airplane` (the freeride Extreme's plane and saucer, two missions' planes) flown by a mod: `vehicle_kinds(mod,
  MML_VEHICLES_AIRPLANES)` and the mod's `vehicle_hook` is told of each airplane's AI step (`kind`
  `MML_VEHICLE_AIRPLANE`); returning 1 the route's flight is left out (its AI runs with its flight state 0: the
  propellers, the sounds and the damage gauge go on) and the body is the plane's: place, rotation, speed and facing;
  the Update after it moves its collision too. `vehicle_body` and `vehicle.body` read and write an airplane's body;
  `vehicle.move {car, velocity, turn, ms}` is the bridge's own driver for a car or an airplane (an agent flies the
  plane somewhere, or tries a vehicle's surroundings). game_build.h `planeAI`, `planeUpdate` (slots 13 and 14 of its
  vtable, read in both builds: the same fields), check_builds.py's checks. Seen in the 1.2 and 1.0 labs alike: the
  Extreme's idle plane lifted 24 m and turned (its model and its collision box there: a ray met it), rolled 70
  degrees (its frame's axes the body's), flown in a climbing circle by a probe mod (cars no longer told once it asked
  for airplanes only).
- **A mod's keys chosen in the game's menus** (2026-10-17; item 14's rest, block 8b of the order): a `"key"` row of the
  tuning panel (Options > Mods): a click waits for the next key with its modifiers (input.cpp's key taken: the game
  gets no key meanwhile, the key taken kept from it until it is let go; Esc keeps the setting) and makes it the mod's
  hotkey setting, live. `sun_shafts`' on-and-off key the first. Found on the way and fixed: 1.2's pages lost a text
  set before the game made them (a page opened with no menu open waits for the pause menu): menus.cpp applies it
  when the page is made. Seen in the 1.2 and 1.0 labs (F9, F10 taken; the new key toggling the effect; Esc).
- **The game's own camera** (2026-10-17; item 9's rest, block 8a of the order): `game_camera` (its mode with the
  game's own name for it, the car it follows, its field of view and range), `game_camera_mode` (the game's
  G_Camera::SetMode, as its camera-change key: on foot behind and from the eyes, in a vehicle its six modes; only what
  the player's state allows), `game_camera_fov` (through the camera's own setters, held and put back whenever the game
  changes it, the game's own value back at 0 or the holder's unload); `camera.game`. game_build.h `cameraSetMode`,
  `cameraModeTable`, `gameCamera`, checked by check_builds.py. Seen in the 1.2 and 1.0 labs: the bonnet's, the driver's
  head's and the eyes' cameras by the game's own code, 103 degrees held through the camera's changes and getting out of
  the car, the game's 70 back.
- **Structures by name** (2026-10-17; item 24, block 9a of the order): `knowledge/structs/` (131 structures, 1412
  fields) made by `tools/kb_structs.py` from the SDK's headers (their fields named once, their offsets read from
  them), both builds' constructors (36 vtables in 1.2, 29 of them in 1.0, the actor kinds, the base classes, the
  members embedded, the sizes) and st0rm94's 1.0 fields carried to 1.2 by aligning the paired functions' memory
  accesses (639 carried, 690 left at 1.0 only); MCP `inspect` (an object's class by its vtable, every field valued
  for the build running, pointers named and followed, paths like `player.inventory` and `game.camera`, the dwords no
  field covers; a layout with no game), `kb_search` finding fields. Seen in the 1.0 and 1.2 labs alike: the player
  told as C_player by its vtable (0x63C908, 0x625878), its weapons (the Thompson's 50 and 100 rounds), its frames by
  name, its car (C_car by its vtable) and the car's C_Vehicle with st0rm94's carried fields reading in 1.2 what they
  read in 1.0 (the speed limiter's 1000.5, the throttle's ramp, the engine's timer), the game's camera and its mode.
- **Crash triage in one call** (2026-10-17; item 25, block 9b of the order): the crash filter's stack is the likely
  calls (code with a call before it, a direct call's target holding a frame above: what finished calls left is
  dropped), and beside each dump a report of what dies with the process (the stack with the mods named, the log's
  last lines, the taps' last calls, the last events, the mods, the mission), written on a thread of its own with a
  time limit; the dump again without the heaps when the full one fails (a partial copy, seen in the stand-in and in
  1.2); since 2026-10-25 a dump of a few MB (the stacks, the globals of the game's and ours, the memory around the
  faulting thread's registers three pointers deep; the heaps whole only with MML_FULL_DUMP=1: a tester's were 0.3 to
  2.3 GB); a mod's fault with its stack (log, `MOD_FAULTED`). MCP `crash_triage` summarizes the failure, named stack,
  mission, mods, recent event types and mod faults in a running game. The `details` option includes the raw diagnostic
  fields. Seen in both labs on a crash made on
  purpose (the player's frame pointer spoiled): `C_game::Tick+0x328` (1.2) and its 1.0 twin `+0x302`, called from
  `C_mission::Tick` under the loader's mission hook; in 1.0 also through `MafiaRagdolls.asi`'s.
- **Restore points** (2026-10-17; "State snapshots" from Later, block 9c of the order): `restore.save` (the game's
  SaveGameSave, whole: every actor and program of the mission and the mods' state, into slot 599, which no save of
  the game uses, then moved to `mml\restore\<name>.*`, the game's slot put back), `restore.load` (copied back, C_game's
  state set to "load": the game loop loads it at its own place; then the slot put back, the one-shot flag that skips
  the next script save cleared, RESTORE_LOADED), `restore.list`, `restore.delete`; MCP `restore` (a load returns once
  loaded, with the player); a save already in slot 599 stops them. Found and fixed on the way: a load in the same
  mission was told as the player's doing (happen.cpp's money and items compared with the frame before the load:
  MONEY_CHANGED of the whole difference, counted as spent by the statistics): `HappenGameLoaded` takes them anew.
  Seen in both labs: the freeride (the car moved back from 90 m away, the money, a spawned car gone, the mods' state,
  0.3 s), a chapter (1.0's Salieri's bar, 1.2's Old Prison: the player put back to the centimetre, the mission going on
  with its objective), another mission's point opening that mission and back; the profiles' saves byte for byte as
  they were.
- **A game folder of any name** (2026-10-17; a speedrunner's report: every start crashed in a Cyrillic folder):
  every path in the host is UTF-8 and Windows is asked through its wide functions (host/paths.cpp); the mods' own
  files reach the game's ANSI file layer through rw_data's CreateFileA link, opened by their wide path (any folder's
  name); json is dumped with the replace handler everywhere in the host (a name in the game's code page threw once);
  `draw_text` takes UTF-8 (turned into the code page of the game's fonts) and `game::TextUtf8` turns the game's texts
  into UTF-8; `mml/path.h` for mods. **Kept so** by `tools/check_paths.py` in the offline tests: no ANSI path
  function, file stream by a narrow path, `std::filesystem` or strict json dump in the host, the SDK, the mods and the
  tests. Seen in both labs through junctions named `Игра` and `Mafia ☃ 試` (the game, the mods, a mod's file read by
  the game's own rw_data); offline in the same two names.
- **Games side by side** (2026-10-17; the owner: "you must always be able to run your own copy, 1.2 and 1.0 in
  one test"): Game.exe quits at its entry when its mutex "Mafia Launcher Super Mutex Shit" exists (one Mafia on the
  machine; MafiaEntryPoint 1.2 0x5BEBE0, 1.0 0x5F99B0); the loader gives it a mutex of its folder (by its volume and
  file index: a junction is the same folder), made first thing, so games of different folders run together and a
  second game of a running one's folder quits without touching its log or bridge.json. The MCP server takes `game`
  on every tool that touches a game (a lab's build or a folder; one bridge connection a game, calls to different
  games at the same time), `game_status` lists every lab's game; `lab.py` tells a lab's game by its process's path
  (not any Game.exe). Seen: the 1.2 and the 1.0 labs launched at once into their freerides (8.7 s), two 3 s waits
  in 3.0 s, trigger_view in both, screenshots of both; offline (two folders, the same folder again, a junction, a
  game holding the game's own mutex) and mcp_smoke (`game` on a second stand-in game).
- **Trigger view** (2026-10-17; now `mods/examples/debug_views`, triggers): the mission scripts' triggers drawn in the world, a
  debug view for mission and mod authors: detectors (their command now, signal, DETECTOR_INRANGE's radii, a use
  point's reach as the Action key finds it and its menu line, DETECTOR_WAITFORHIT's boxes) and the scene's areas
  (ISPOINTINAREA's meshes, the player inside); `debug_views.triggers.list` as data. With it the SDK's `script_commands.h`
  (each command's number in both builds: `game::ScriptCommandName`, `game::ScriptOpcode`; `{` and `}` were missing
  from the table), the detectors', programs' and areas' layouts in game.h, and the host's frame of 4096 drawn items
  told when full (`draw.state` `dropped_last_frame`; the view's areas once took it whole and hid its detectors with
  no word). Seen in both labs: 1.2's Old Prison (46 detectors, 4 areas, the crowbar's "Pick up crowbar"), the
  freerides' music and district areas, 1.0's Salieri's bar.
- **Race view** (2026-10-17; now `mods/examples/debug_views`, race; `sdk/include/mml/game/racing.h`): the race of mission 6 (06-03
  "Before The Race", the autodrom `mise06-autodrom`) drawn in the world: each gate as the line between its track
  node and its outer marker (numbered, 0 the finish, the player's gate yellow), each racer as a pole with its label
  (place, name, gate, lap of 5, zone t) and a line to the gate it heads for. `debug_views.race.state` gives the race as
  data and checks the layout (each gate's midpoint against its two nodes: 0 in both builds). The racing layouts of
  both builds in racing.h (C_racing, its circuit's gates, the entrants: `racing.md`'s PC table is 1.0's; 1.2's
  differ: the gates at +0x44 in both, the entrant's gate +0x54 in 1.2 and +0x5C in 1.0, place +0x5C and +0x64, lap
  +0x64 and +0x6C). Seen in both labs in the race: 27 gates, places 1..13 with no repeat (the player 12 of 13 in
  both, the HUD's own), laps 0 of 5. The mod draws only a race whose player is named "Tommy Angelo" (the layout's
  check); outside a race nothing.
- **People, skeletons, poses** (2026-10-16; item 6, block 6a): the host keeps every `C_human` (`humans_list`,
  `human_player`), a person's skeleton by the bones' names (`human_skeleton`: 18 joints, the pelvis, the root), a
  pose written for the frame's drawing (`bone_set` in `RENDER_BEGIN`, each posed model updated once after);
  `humans.list`, `human.skeleton`, `human.pose` (held for a time). The example `mods/examples/puppet` (a wave, a
  T-pose) seen in both labs on the screen and in a scene view's mirror; offline tests with no game.

- **Mods in Rust** (2026-10-17; the order's item 10): the crate `sdk/rust/mml`. `mml::sys` is the whole of `mml.h`
  made by bindgen (`tools/rust_sys`, committed, checked by the offline tests; its layouts asserted at compile time
  for i686: MmlHost 684 bytes, MmlEvent 84, as the C side); over it a safe layer: closures for events, commands,
  hotkeys, menus' items, scripts, timers, zones and use-points (freed at the unload, or when removed), `command_json`
  with serde types, state and settings, memory, hooks, the world's actors, the game's state, the HUD's lines, sound;
  the rest through `host_fn!` (each member checked against the loader's table). A panic is logged with its place and
  raised as `0xE0525553` into the loader's fault guard (the trampolines are `extern "C-unwind"`). `mml::entry!(load)`
  makes the exports. CMake's `mml_add_rust_mod` runs cargo (`build/x86/rust`), `tools/new_mod.py --rust` makes one.
  The example `hello_rust` seen in the 1.2 and 1.0 labs (its line on the frame, a car spawned, the console's lines,
  a hotkey rebound by `config.set`, 10 reloads: 0 MB, 0 handles, 0 threads a reload, its counts kept); offline:
  the test mod `kit_rs` (typed params, errors, a timer, `post` from a thread, state across a reload, the closures
  dropped at the unload, a panic faulting the mod as a C mod's fault).
- **Everything in Rust** (2026-10-17, the same day; the owner: "everything must be possible in Rust"): the safe
  layer covers the whole host table (`gfx.rs`, `people.rs`, `world.rs`, `media.rs`, `control.rs`, `data.rs`
  beside lib.rs: passes, scene views, the camera override, the geometry, shaders, meshes, skeletons, animations,
  props, lights, effects and their hooks, sound and music, pictures, markers, menu pages, control, vehicles,
  files, models, saves, statistics, services); `mml::game` is the C++ game headers' Rust form (the builds' tables,
  the layouts' constants, Detect's signatures and the scripts' commands made by `tools/gen_rust_game.py` from
  game_build.h, racing.h, game.h and the csv, checked by the offline tests; game.h's helpers and mathx.h ported);
  the feature `windows` types the Direct3D 11 and 9 objects; `sdk/rust/mml-build` compiles a mod's HLSL in its
  build.rs (the CMake build hands it its fxc and dxc). The example `depth_pulse_rs` (depth_pulse in Rust: a D3D11
  pass, its shader from mml-build, the player's place by mml::game) seen in the 1.2 and 1.0 labs, the orange ring
  and its grid on the ground from above, 10 reloads: 0 MB, 0 handles, 0 threads a reload. `kit_rs.live` in both
  labs: 47 of 47 (the game's layouts against the host's answers: the player, his place, his model's frames, the
  game's world matrix, back1 by FindChild, the car's body and its whole state; the async parts at the second call:
  10148 and 9833 triangles captured, the particles', vehicles' and exhaust hooks called thousands of times, the
  scenery scanned); a scene view seen by a pass (427 and 430 frames), a menu page of Rust's controls built by the
  game and closed by its Back button, in both builds. Then what was left open: the offline tests fail when a member
  of `MmlHost` has no safe method (169, all covered); a call posted and not run is freed at the unload (kit_rs posts
  one in its own unload: its drop logged at each of 10 reloads, never run); the game's Direct3D 9 device from Rust in
  RENDER_WORLD (its viewport 1280x960 read, a magenta square cleared into the world's picture, under the menus, in
  both builds); `MenuItem::set` in the pause menu (the item moved from under Options to under Load Game and renamed,
  one item, its click reaching the closure, in both builds); 10 reloads of kit_rs: 0 MB, 0 handles, 0 threads from
  the fourth on (the second and third +2 and +1 threads, -2.5 and -1.2 MB, no thread in the mod's code: warming).
  `mml::game` covers the C++ game headers whole (racing.h holds only the layouts, the scripts' table its two
  lookups).

- **Both builds in Ghidra** (2026-10-18; from Later, the owner: "all the tools, organised properly", the 1.0 port and
  every service's search for "who calls, who writes" having been done by hand): Ghidra 12.1.4 pinned in
  `tools/deps.py ghidra` (its PyGhidra venv from the archive's own wheels), a project a build in `re/ghidra/` (a
  cache: `tools/ghidra_cli.py setup` makes it from the labs' binaries and the repository), a resident server a build
  (`tools/ghidra_server.py`, both builds at once), MCP `ghidra_read`, `ghidra_find`, `ghidra_write`. The projects
  carry the knowledge base's names by rank (`tools/ghidra_names.py`; the SDK's game_build.h names, which mafia-ragdoll
  also gave, kept as aliases beside the decompile's class names: `at=humanHit` is C_human::Hit), 1.0's through the
  twins, MafiaReverse's signatures as prototypes (__usercall and values of unknown types left out; 4117 of 1.2's
  4833), and the code reached only through pointers made functions (1.2 Game.exe 4258 -> 5891 functions). What is
  written goes into the project and the repository at once (names.csv, names_10.csv, annotations.csv); `forget`
  takes it back; `where` and `crash_triage` name 1.0 addresses by names_10.csv first. Tests: run_tests.py (the names
  and prototypes, with no Ghidra), mcp_smoke.py (the tools from cold servers, both builds).
- **The knowledge base for 1.0** (2026-10-18; from Later): st0rm94's names at their own 1.0 addresses,
  every one (`knowledge/game.exe/external_10.csv`, 5028, by `tools/kb_external.py`; 2288 of them have no 1.2 twin:
  1.0's own code, its menus). His export's binary is another no-CD copy of the same build (the same size, another
  MD5): 4911 of his 5098 addresses start a function in the 1.0 lab's Game.exe in Ghidra, 112 lie inside one, 75 in
  none. The 1.0 Ghidra project takes them (2801 names -> 5082), `where` and `crash_triage` name 1.0 addresses by
  them (ours in names_10.csv over them), `kb_search` finds them in `functions_10`. 1.0 has no decompile of its own
  in the knowledge base (`kb_function` stays 1.2's): `ghidra_read` with build "1.0" decompiles it.
- **Orders for people** (2026-10-18; item 26): `human.order {actor, code, act, flt, at}` (mods: `human_order`,
  `human_order_state`, in C and Rust) puts the missions' own people's commands (`ENEMY_*`) into a person's own
  program and hands him to it as the scripts' ENEMY_FORCESCRIPT does (`C_entity::ForceScript`): the game's AI walks,
  runs, follows, fights, flees and gets into cars; `human.go`, `human.follow`, `human.attack`, `human.flee`,
  `human.enter` (to the door, then in), `human.stop`, the event `ORDER_ENDED`. What the game needed that a spawned
  person lacked: the program's hand-back level (+0x60, as `C_program::Init` sets it for a program with
  instructions), and an order that never reaches END (END takes its person out of the game's ticks for good: the
  loader's own wait ends it, the code's `end` jumps there). Seen in both labs by `tests/game/test_orders.py` (19
  checks a build, two runs in a row). Then (2026-10-18) driving: `human.drive {actor, car, to}` (in as the driver,
  then the game's `enemy_car_moveto` by the road network), the named orders' outcome in ORDER_ENDED (`arrived`,
  `distance`, `seated`: the game ends a drive it gave up as one that arrived) and their `retries`; the road
  network's nearest node, `world.road` and mods' `road_near` (C, Rust). Seen in both labs: a drive of 290 m
  arrived 3.9 m from its road end, a drive asked to within 1 m ended short, was given again once and told so.
  Then (2026-10-18) a calmer drive: a car facing away from its way is backed round first by the game's own reverse
  (`start` "turn", the default; "place", "as_is"), the game's own unsticking (15 s at the chase's speed, no limit) is
  ended once the car has got 6 m away (`calm`), `speed` caps the way's speeds; ORDER_ENDED tells `start`,
  `start_angle`, `unstuck`. And `camera.game {follow: car}`: the game's own camera behind any car (G_Camera::SetCar),
  given back when the car is destroyed. Seen in both labs (`test_orders.py` drive_facing_away, `test_world.py`
  camera_follows_car: 40 checks a build).
- **Tests in the real game** (2026-10-18; item 23): `tests/game/` (`mml_game.py` a lab's game over its own MCP
  server, `run.py` the labs at once, a fresh launch into a quiet freeride of each kind a module asks, a restore
  point before each case). The cases: the orders; a car driven into a person (HUMAN_HIT, the car's hit type); a
  camera turned and given back; a prop in the hand while he turns and walks; a car made over the open river (the
  whole city); an exhaust under the elevated railway (the pipe behind, pointing back, emitting, the deck over it).
  42 checks a build, two full runs in a row in both labs. They found `spawn`'s car over water flung 315 km (fixed).
- **The 64-bit sidecar** (2026-10-18; item 19 but its first heavy user): `mml-sidecar64.exe`, started by the loader
  in a job object and started again when it ends, loads the mods' 64-bit halves (`"entry64"`, `sdk/include/mml/side.h`)
  from fresh copies (hot reload), a fault unloading the half alone. A half has the whole bridge, the game's events,
  commands of its own; the two halves share channels (shared memory, whole versions: mml.h and side.h
  `channel_open`) and textures (Direct3D 12 shared textures the 64-bit half draws, opened on the graphics bridge's
  device for the 32-bit half's passes: `gfx_shared_texture`; C and Rust). The frame's round trip measured in both labs
  by `mods/examples/side_picture`: the picture of each frame drawn by the 64-bit half 0.4 ms after the frame's start
  and shown in the same frame's pass, every frame (docs/MODDING.md "64-bit halves"). Offline: `side_test` (both
  halves, both channels' ways, 30 000 versions a second read across the processes with none torn, reloads, faults, the
  sidecar killed).
- **PhysX 5 in the 64-bit halves** (2026-10-18; item 19's first heavy user): PhysX SDK 5.9.0 pinned in
  `tools/deps.py` (its `physx/` folder and license kept), built by the x64 preset as an external project (target
  `PhysX5`: static, `/MT`, CPU only). `mods/examples/physx_crates` (two halves): crates stepped by PhysX on the
  game's own static collision (`world_shapes`' triangles cooked into a PhysX mesh, its boxes, spheres and poles),
  pushed by the people and cars around them (their collision shapes as kinematic bodies: a person's narrow sphere
  drawn out into a capsule, a car's whole hull), shown as the game's props. Both labs at once: the poses of
  2343 of 2344 frames (1.2) and 2345 of 2346 (1.0) were that frame's own step, whole 0.3 to 0.4 ms after the frame's
  start; 0.02 ms a frame on the game thread; reloads keep the pile, a fault of the 64-bit half leaves it standing
  (docs/MODDING.md "64-bit halves").
- **NVIDIA Flow 2 in the frame** (2026-10-18; item 19a's first part, the owner's ask: smoke as NVIDIA's demos have it,
  as the proof that what was never made for 32 bits goes into the game): Flow 2.2.0 (the newest; its folder of the
  PhysX 5.9.0 archive) pinned in `tools/deps.py flow` with the newest Slang (2026.20), premake (5.0.0) and DXC
  (1.9.2609) instead of NVIDIA's packman, built by its own premake through `third_party/flow/premake5.lua` (Flow's
  script under premake 5.0: its `flags` and `{COPY}`, Slang 2026's split DLLs and C++17) as an external project of
  the x64 preset; `mml_flow(<id>)`. `mods/examples/flow_smoke` (two halves): smoke, fire and steam sources (`emit`
  in front of the player, at a place, on the player's car; F10), simulated on a sparse grid and ray-marched with
  self-shadowing by Flow on Vulkan in the 64-bit half, over this very frame's colour and depth (CUDA between Flow's
  memory and the shared Direct3D 12 textures), the people and cars carrying the gas, the ground holding it. Both labs:
  Flow's GPU time 0.3 to 0.4 ms, a round trip of about 3 ms on the game thread, all but 2 frames of thousands with
  their own frame's gas; a reload keeps the sources, a fault of the 64-bit half leaves the frame without gas and
  without a wait, `mod.reload` brings it back (docs/MODDING.md "64-bit halves"; Known gaps: the first start's shader
  compile, the CPU waits).
- **pet1kk's developer tools brought in** (2026-10-18; PR 2, a standalone ASI for 1.2, not merged: a prebuilt binary,
  the game's menu files patched, hooks of the places the loader keeps): its findings checked in both builds' code
  (knowledge/notes/game.md "pet1kk's developer tools"; C_player::Hit's start corrected), then made the loader's for
  1.2 and 1.0: `human_invulnerable` / `human.invulnerable` (the game's harm not run for a person, the loader's own hook
  of C_human::Hit), `hud_game` / `hud.game` (the game's HUD hidden: DrawAll not run), `web_nodes` / `world.web` (the
  pedestrians' web, check.bin), `material_name` / `world.materials` (the surfaces' names from the game's own
  materials.def; `world.shapes` names them), game.h's distance tests' params, the web's and the items' layouts,
  `ActorTypeName`; C and Rust. `mods/examples/devtools` (kept from harm, endless ammunition, flying, a free camera, the
  field of view, the HUD; Options > Mods) and four new parts of `debug_views` (triggers: the distances the scripts
  test; `collision` as filled solids, `web`, `names`). With it: `action_held` gives an action from both its bindings,
  the settings pages laid out apart from the game (`panel.layout`) and checked offline for overlaps
  (`tests/run_tests.py` ui_tests), tabs as wide as their text, the mods' 2D drawing under the game's menus, the
  collision view a graphics pass. Seen in both labs (1.0: a fall of 16.6 m took 163.8 health, the same fall with
  `human.invulnerable` none).
- **The game's videos played by the loader** (2026-10-21; from a modder's upscaled intros that did not play, FFmpeg
  the owner's choice: knowledge/notes/videos.md): IShow's open, close and full-screen loop hooked in both builds
  (and the game's file search for a video's name), the video played with FFmpeg 9.0.2 and dav1d (an LGPL build from
  source, third_party/ffmpeg/build.py, delay-loaded from `mml\ffmpeg`): any container and codec built (H.264, HEVC,
  VP9, AV1, Bink 1 BIKb to BIKk, Indeo...), the mods' `files\` first (mp4, mkv, webm, mov, bik, avi), then the game's
  folder; 32-bit colour at the file's size, fitted with its aspect; the sound on the game's DirectSound; any key
  skips; the bridge served meanwhile (screenshots of a video). A file FFmpeg cannot play (Bink 2) is logged with its
  reason and the next tried; none, or no FFmpeg: the game's own player. `video.state`, `video.find`, `video.play`,
  `video.skip`; events VIDEO_STARTED, VIDEO_ENDED, VIDEO_REFUSED. Seen in both labs: the logos at the start (1.2's
  BIKi through FFmpeg, a mod's 1080p H.264 logo2 in both; 1.0 shows its logos for the first time on this machine),
  1.2's intro by the new game's script command (a mod's AV1), 1.0's by `video.play` (VP9), a Bink 2 file passed
  over, `video.skip`, the game's own Bink without FFmpeg. Offline: `tests/run_tests.py` video_tests. Not seen yet:
  a real `BIKk` file (Known gaps).
- **The videos decoded by the GPU** (2026-10-21; the owner: "a 4K 60 fps intro?"): host/video_hw.cpp, FFmpeg with
  D3D11VA and DXVA2 (and its own AV1 decoder): the driver asked for the codec's profile, bit depth and size; D3D11VA on
  the loader's Direct3D 11 device on the game's adapter, else DXVA2 on its own 9Ex device (the real one under
  dxwrapper's wrapper); each picture turned for the screen by the GPU's video processor into a texture shared with
  the game's device (copied through the CPU under DXVK); the CPU's path on all cores, scaled to the screen; late
  pictures left out. Measured, both labs, six 4K60 clips (H.264, HEVC, VP9, AV1, 8 and 10-bit): before, 10-bit at
  12 to 14 pictures a second and the 8-bit ones costing the game thread 7 to 8 ms a picture of upload; after, 600 of
  600 for all six, the game thread 0.5 ms a picture (the journal).
- **A crash's stack as exact as a profile's** (2026-10-21; Known gaps "Crash triage"): one reader of a thread's calls
  for the sampler and the crash filter (host/stack.cpp): walked by the frame pointers through the system's and the
  loader's frames to the first of the game's, then each return address with its call's target, read through jumps,
  the chains' `jmp [head]`, MinHook's trampolines (a detour's call of the original is the hooked function) and
  `mov reg, [abs]` / `call reg` (how today's compiler calls a hook's original). The report keeps them (`calls`);
  `crash_triage` and `crash_inspect` keep a frame only where its call leads to the frame below (a tail call's caller
  kept once a frame above leads to it: the profile too), count what they drop, and name every frame: the game's by
  the knowledge base, the loader's, the mods' and the stand-in's by their PDBs when the GUID is the dump's image's;
  the verdict counts the dump's modules in the Windows folder as the system's. The process's exit line walks the
  same way. Offline (the stand-in): crashes in the game's code, under two mods' hooks and in a mod's hook, the exit.
- **The size things are drawn at** (2026-10-23; the owner's "Bounce" video): `actor_scale` (mml.h), `actor.scale`,
  `actors.scaled`: any actor or traffic pedestrian drawn at another size around its pivot, pulsing to a beat, its
  model frame scaled for the world's Render alone so its physics and collision stay its own (an A/B drive from one
  restore point: the same within the input's timing). Seen in both labs: cars, a parked traffic car, Paulie, a tram.
  `camera.set {follow}` films any actor from its own space. `mods/examples/bounce` pulses everything near the player
  (knowledge/notes/scale.md).
- **A gamepad plugged in while the game runs** (2026-10-24; the owner: the first of the game's bugs fixed through
  the loader): both builds open their DirectInput controllers once at the start (IJoy.dll; none attached: no base at
  all), and the profile's bindings of a joystick that is not open are dropped. host/joyplug.cpp watches the HID
  device interfaces (CM_Register_Notification; a poll thread where it is missing) and, on the game thread before the
  controls' update, opens the game's joysticks again through the game's own functions when DirectInput lists a
  controller the game does not have (or has and cannot read), the profile's bindings given again: `joy.state`,
  `joy.rescan`, `joy.hotplug`, `joy.drop` (knowledge/notes/ijoy.md).
- **Mafia Fusion Fix as a mod** (2026-10-25; the owner: understand what it does from its sources and build it as a
  mod of the loader, not beside it): `mods/fusion_fix`, ThirteenAG's modules (WidescreenFixesPack d35057d, MIT) kept
  as written against `ff.h` (its patterns found at run time in the modules as shipped, `injector::WriteMemory` as
  `code_patch`, `safetyhook::create_mid` as `hook_mid`, its inline hooks as `hook`): the HUD, map, texts, menus, FOV
  (Hor+), videos and cutscene borders at the screen's aspect, the draw distance, the shadows' z-bias, the HUD's lines
  and the cursor at the screen's scale, the borderless window, the eyes' animations, the cars' scratches, the radar's
  map, the free camera, the sticks' dead zones; 15 parts, all on in both builds; its ini as live settings (Options >
  Mods), every patch and hook taken back and made again on a change (90 ms), the HUD laid out again by the game's
  own ResolutionChanged (`hudResolutionChanged`). Three of its faults found and fixed in the port (1.0's camera
  hooked inside a function, 1.0's car speed read at 1.2's offset, a tail call's z-bias read past its arguments).
  Beside it: `game_launch {"resolution": [w, h]}` (the screen's size for one run, the registry left alone), the mods'
  2D space kept 800 wide over the screen when the HUD's units are not, MinHook's threads walked with NtGetNextThread
  (31 hooks: 1.2 s to 15 ms).
  2026-10-31: enabled in the owner's 1.2 lab with borderless off to retain window resizing. Native screen-size
  changes now refresh the port's aspect/HUD/map/menu/cursor constants through its owned Apply lifecycle. Existing
  FOV/HUD controls in Developer tools > The camera verified through native controls and three resizes in both
  builds (23/23 checks, including actual pixels); no duplicate controls needed.
  Later the same day: those FOV checks proved only the stored angle, not the effective projection. Fixed the
  reported water conflict: shared SDK projection ownership protects explicit scene-view/override frusta and
  absolute held FOV; Fusion Fix refreshes native policy before views inherit it. Native actual-matrix tests pass
  in both builds, including reload/unload and saved-game loads (knowledge/notes/camera-projection.md).
- **Beautiful water** (2026-10-28; the owner: "a visual, no heavy physics"): `mods/examples/river_water` made anew.
  The wind's ripples from a JONSWAP spectrum by an inverse FFT on the GPU (three cascades, their slopes' mipmaps:
  a pixel's mean slope and the variance it cannot draw, scaled to a calm city river's slope variance); the planar
  reflection followed through its depth and blurred by that variance, streaked up and down as real water is; the
  body's absorption and scatter lit as the game lights its own; the sun's glitter (Cox and Munk's facets) out of the
  shadow map's shadows; foam at walls; the water's planes found by level and the reflection drawn for the one the
  screen shows most of, none while no water is on the screen (occlusion queries). Its settings on the game's Options
  > Mods page. A fault of the loader's found on the way and fixed: a `geom_capture` asked during a frame ended with
  that frame, empty (host/geom.cpp). Seen in the 1.2 and 1.0 labs; cost (bench, one lab, the embankment): 1.2 +8.0
  ms (the reflection 7.1, the water 0.9), 1.0 +2.5 ms. What is left: Known gaps, "The water".
  2026-10-31: added independent Small ripples (shared SDK spectral-band control) and Reflection softness
  (explicit artistic lobe-width control). Large waves can retain sharp, distorted reflections; the wind no longer
  forces the only possible material appearance. Defaults preserve the previous spectrum/softness. Native 1.0
  12 checks and current 1.2 four checks passed, plus CPU/WARP and full offline tests; knowledge/notes/water.md.
  The follow-up adds independent Wave width/animation speed, Wind fetch, Reflection strength and native RGB
  controls for body/absorption colors. Shared SDK lengths keep FFT dispersion, slopes and geometry sampling
  consistent; integrated phase avoids jumps when speed changes. Shared panel kind `color` stores one #RRGGBB
  setting. Waves/Material/Lighting tabs; previous settings/default behavior preserved. Native 15 checks EACH
  build plus CPU/WARP and full offline suite pass (build/water-controls-20261031).
  Subsequent visual-reference iteration is IN PROGRESS: the owner rejected the uniform blue candidate despite
  passing tests. Reduced UI to two tabs/12 rows, refitting wave energy, directional Fresnel/radiance filtering,
  painted-sky brightness, crest foam and reflected ray tracing. See JOURNAL and build/water-reference-20261031.
  Do not claim reference appearance achieved yet; compare images directly, then finish native/performance checks.
  2026-11-01: owner accepts wave geometry but rejects lighting/reflection/sun glitter. Architecture audit found
  finite camera-dependent environment coverage, uncalled direct-sun BRDF, broad painted-sky amplification and
  uncoupled body scattering/double exit transmission. Native frozen-wave, DLAA-off pitch/sun evidence recorded.
  Replacement design and ordered acceptance gates: docs/WATER_LIGHTING_ARCHITECTURE.md. Preserve wave geometry;
  implement directional environment/valid hits, direct sun, then coupled volume transport.
  Approved rebuild IN PROGRESS: an intermediate lighting preview is installed in both labs. Sky-only native
  scene views and the optional C/C++/Rust capability query pass 15 native checks per build plus actual WARP
  coherent-cube/reset tests. Water now samples world sky directions, a separate Beckmann/Smith finite-disc sun
  and coupled homogeneous scattering with exit transmission once. Actual WARP optics and 15 native controls/
  pixels/reload checks per build pass. Wave geometry and owner settings remain unchanged. This is not the full
  fix: objects still use legacy planar queries; world geometry/material coverage with same-frame moving
  occluders (including the barge), visible dark surface streaks, reference acceptance and cost remain pending.
  Evidence/backups: build/water-lighting-rebuild-20261101. Owner offered to review this intermediate appearance.
  Owner REJECTED that preview: black streaks, white edges, pitch-dependent bright waves and a red hull-colored
  patch behind the barge. Follow-up build/water-artifacts-20261101 reproduces negative-depth hull hits in WARP
  (fails before/ passes after validity fix). Camera-dependent ShoreWeight and discarded downward reflected rays
  also need architectural treatment. The painted cloud opening cannot be replaced by an assumed point sun.
  No new visual acceptance or completed world-geometry service is claimed.
  Partial hull-depth/shore validity and image-driven cloud-opening emitter corrections are now installed in BOTH
  labs (9EB96D6C...); the owner reports partial improvement, not acceptance. Camera-independent triangle/TLAS
  query core passes actual WARP tests including moving occluders and explicit incomplete coverage. Native draw
  source ABI (C/C++/safe Rust, UV/prelit colors/material states/posed buffers) passes 11 source/lifecycle checks
  in BOTH labs (12 checks each after exact native mip transfer was added). Original material texture bytes/mips
  now transfer to D3D11 without scene-image sampling. Complete native scene/material snapshot and production water integration
  remain IN PROGRESS; the current water still uses the legacy finite object image. No added tuning sliders.
  Explicit unculled native mesh preparation now passes 16 collection/context/pose/fault checks in EACH lab.
  Fixed native buffer publication and LOD-selector side effects; all CPU packets read, including after 1.2
  pitch/yaw changes. Real sector-lit buffers, no rasterization, inactive streaming/unsupported
  geometry reported as unknown. Pitch/yaw preserve the source geometry hash; off-camera prop FOV/move/removal
  pass. Full copy remains 38.8ms, metadata-only 10.2ms including diagnostic work: immutable geometry caching and
  same-frame instances are next, alongside ray-hit material evaluation and production integration.
  Targeted wave-visibility correction now installed in BOTH labs (896F21FA...): downward reflection rays
  can hit/re-reflect from the actual FFT water field; fine back-facing normals trigger a front-surface query
  instead of a zero-light material. Native A/B pixel tests pass 5 in EACH build, identical wave phase/shape:
  nearly-black ROI pixels 8,923 -> 591 (1.2), 15,553 -> 233 (1.0); flat-water radiance retained within one byte.
  This is a bounded height-field/two-additional-bounce approximation, not a complete folded-surface path tracer.
  Native low-water cost is about +5ms GPU in 1.2; acceleration remains needed. No new artistic settings.
  The object query is STILL the finite legacy image: city disocclusion/material integration is not completed.
  Owner accepted the black-band fix and requested the completion plan: docs/WATER_REFLECTION_PLAN.md records
  28 small gated steps, with early native hit/color checkpoints, material/streaming/lifetime coverage, caches,
  production cutover, cost and spatial/DLSS acceptance in both labs. The checklist is pending, not a delivery claim.
  Owner questioned software tracing cost: added prerequisite step0, an early hardware DXR capability/prototype
  and end-to-end comparison before committing production traversal to D3D11 software. Existing software query
  remains the correctness reference. Current slow water does NOT yet execute that object BVH; wave marching,
  material/tessellation work, the old scene view and synchronization need separate measurements.
  LATEST owner decision supersedes hardware-first: the CURRENT OVERALL WATER LOOK is accepted; first finish
  reflection correctness, then restore the missing water shadows, and ONLY THEN optimize. Plan now records
  explicit A/B/C order, shadow steps S1-S6, deferred hardware comparison O1 and image/motion regression gates.
  Preserve waves/color/foam/highlights/lighting/filtering; an FPS gain cannot justify an unapproved quality loss.
  Reflection block started: steps1-18 completed (repeatable native cases in both builds, actual missing-source
  inventory, docs/SCENE_RADIANCE_SNAPSHOT.md contract, coherent SDK GPU upload and native CPU/GPU hit diagnostics:
  14 checks per lab, off-camera depth layers/move/remove/camera invariance). debug22 exposes missing regions;
  Initial1.0 streaming gaps are now closed by step14's owner-scoped native block residency. Added a diagnostic capture clock;
  accepted shader code and normal optical parameters unchanged. No world-object reflection cutover yet.
  Native texture pages, stable primitive attributes and encoded/prelit material reconstruction now pass both
  builds (including real hull/tree/house/bridge samples). Step10 alpha/sidedness passes WARP,224 native D3D9
  comparisons and both labs' real fence hole to a deeper material (0.049952 ->10.751377m).
  Ordered blending, current instances, required billboard/SMMR deformation and independent streaming now pass
  both builds (steps11-14). Step15's explicit raw colored reflection preview now passes native bank/actual-barge
  captures in both builds; shader resource bindings exclude old image colors/depth. Full-copy preview is slow,
  explicitly diagnostic; normal water still uses the legacy image until18. Subsequent cache/publication
  work16/17 must preserve current source data, accepted optics and coherent owner/device/scene lifetimes.
  Step16 owned content caches now pass native9/lab plus current WARP/native-texture mutation tests. Complete
  current vertex/index/UV/prelit/mip validation precedes immutable reuse; no pointer/TTL assumption. Step17
  is next: broad1.0 repeated texture-page allocation fails with E_OUTOFMEMORY and skips the water pass.
  Dark triangles in that case are exposed underwater geometry, as the owner identified, not reflected objects.
  Step17 now passes that broad allocation repro: unchanged GPU payloads share by exact current owned bytes/
  texel identities, current instances/constants still publish coherently. Page staging is sized once and old
  CPU scene leases retire. Native continuous10/lab + pose/geometry32/lab and full offline checks pass; source
  validation remains expensive and optimization is deferred. Next18 is the normal-material world-query cutover.
  Step18 complete: normal per-segment world material installed3804791D... both, native-normal-v1 inspected8
  frames/lab plus current instance/pose32/lab pass. Resolved-camera region7/lab and complete prior residency
 12/lab pass; current host offline ALL PASSED. Finite total object path600m, no projected-depth object lookup
  or silent old-image fallback. LOD0/unfogged native colors remain20/21; projected shore and old capture19/23.
  Next19 is a local world-geometry waterline constraint, preserving accepted waves beyond contact.
  Step19's first nested BVH prototype6D4B4530... failed the native barge gate: bridge late/off exposed the
  underwater geometry. Exact local CSR candidate bins now pass the CPU/WARP distance/publication gate;
  Step19 complete: CSR native bank/FOV/actual-barge cases pass both; compact-loop0ECC4EBF... installed both
  fixes the1.2 barge-down shutdown (same3 inverse/7 refinement steps,96 search steps, rays and bounces).
  Normal PS/DS use current world contact with no projected shore depth; native down frames inspected both.
  Full spatial25 and production frame cost remain pending. Next20 is per-hit filtering/virtual observers.
  Both labs restarted after shutdown, original owner settings checkpoint retained.
  Owner override 2026-11-04: make the accepted-looking preview usable before resuming step20.
  Both labs now run the same 14 accepted shaders with wrapper-verified texture revisions and reordered
  texture-page reuse. Live 1.2 upload can fall from about120 MB to43 MB/frame, and held bank snapshots
  reuse11 buffers with64 bytes uploaded. Native source preparation and geometry reads still dominate
  the CPU, and the bank view also reached178 ms in the water surface pass. Lab usability remains open.
  The owner subsequently resumed step20 and deferred final motion acceptance. The focused GPU gate now
  checks the actual water material provider's per-hit mip and alpha selection. The isolated wave/object
  path initially compiled alone in23.67 s. The new production split compiles in2.22/24.20/4.84 s
  and renders water in both labs with the actual hit filter. Final motion and frame-cost gates remain open.
  Separate PS/CS diagnostic entries remove the remaining shader build timeout. The standard x86
  mod target builds and AD227F37... is installed and running in both labs.
  Optimization is now authorized with fixed resolution and visual quality. Normal water skips the obsolete
  mirror-camera render. The world query reuses slab reciprocals and avoids retracing a proven prefix.
  Vertex/index updates skip read-only unlocks and byte-identical plain writes. Stage timings separate
  ray preparation, transport and composition. Remaining costs include repeated native scene collection,
  the game shadow pass and synchronization in the D3D9 bridge. See the 2026-11-04 journal measurements.

## Dated ragdoll follow-ups

The entries in this block retain their investigation dates and explicit fixed/open status. They are not
all pending work. Current priorities follow under "Next"; STATUS.md records the cumulative handoff.

- **Airport shotgun misses (fixed 2026-11-09):** the capsule terminal mid-hook's entry altered EFLAGS before
  saving them. The native branch rejected living candidates after any capsule registration. Reserving the
  return slot with LEA preserves the flags. Same-save replay went from zero hits with active capsules to
  1/1 single and 10/10 shotgun hits. Native living hits and corpse palm hits passed in both builds.
- **Ragdoll interaction boundary (2026-11-09):** only dead actors publish bullet capsules. The car receives
  solver velocity changes only after a solid ragdoll contact in that step. Pedestrian movement keeps native
  sweep results, including world obstacles near moving cars. The dedicated car-hit correction and animated
  proxy fallback remain. Verified drive with zero mod delta on a clear path and real reaction on body contact
  in both builds. Installed and restored the live airport mission in W:/Mafia10Fixes.
- **Sideways pedestrian seed after a crowd strike (fixed 2026-11-09):** overall velocity now comes from
  consecutive samples of the native pedestrian position. Bone animation only contributes relative motion,
  with zero net linear momentum. A 20 cm pose jump no longer seeds a 12 m/s launch. Both builds passed the
  pedestrian handover and car-hit regressions. Installed in Mafia10Fixes. The exact crowd route and native
  collision box versus Jolt wall geometry have not been replayed.
- **Garage blast sequence and delayed recovery (fixed 2026-11-09):** a nonlethal blast moved Enemy1 via the
  mod, so the next native blast left him alive and `detector_energy` never armed the car transfer. Living NPCs
  keep native blast reactions. The original script now transfers `forVfor4` with Tommy seated in the 1.0 replay.
  A quiet supported knockdown can hand off to native get-up even while Jolt is awake. Verified in both builds.
- **Rare ragdoll car hard stop during dodge (owner, 2026-11-08; open):** reported once at low speed. Controlled
  free-street dodge/strike did not reproduce it; a separate apparent reproduction was the native curb, confirmed
  without the mod. New bounded hard-stop telemetry records native speed change, Jolt contribution and nearby
  pedestrian animation/proxy gap. Preserve the next real incident before choosing a collision policy change.
- **Get-up root-motion continuity (fixed 2026-11-08):** reproduced nearly four metres of visible displacement
  after a successful one-frame fit. Rebase the native cached TCK trajectory and discard the obsolete fall's
  root-track cross-fade; expanded 20-assertion block passed in both builds. docs/RAGDOLL_PORT_REVIEW.md.
- **Ragdoll port parity (owner, completed 2026-11-08):** staged current-pose get-up, owner cancellation,
  pedestrian grid following, reference car impulse location, solid striking hull and measured hull/capsule
  fallback through native vehicle-hit handling. All 18 focused assertions passed in both builds serially;
  missing-native-proxy taxi-wave contacts also passed in both. Final binaries installed and checked in
  W:/Mafia10Fixes with original D3D8. Evidence and scope limits: docs/RAGDOLL_PORT_REVIEW.md.

**The goal** (the owner, 2026-10-14): a toolkit strong enough for whatever mod a user can think of, written by another
agent. The measuring cases span the range: engine-deep mods (**a ragdoll, shader water, realistic smoke, VR**) and
gameplay mods (**fishing, skateboarding, a working airplane, achievements**). This repository ships the shared
services and examples, rather than promising finished versions of every measuring case;
the ragdoll port now lives in the separate mafia-ragdoll-mml repository. The original mafia-ragdoll has a
ragdoll (Jolt), water and smoke (a Direct3D 11 bridge, PhysX 2.8.4, NVIDIA Flow) on its own host;
its code remains the reference for what several services below must do
(its `src/`, read only). Every service lands with an example mod (`mods/examples`), a section in
`docs/MODDING.md` and its offline test, and is seen working in both labs, 1.2 and 1.0, before it is done (the
builds' decision; AGENTS.md "Builds"). Each item names who needs it (R ragdoll, W water, S smoke,
V VR, F fishing, K skateboard, P airplane, C achievements, A the agents themselves) and its reference.

## Next, by importance

The November cumulative update is published. Active follow-up covers the captured allocator hang,
the local lifetime/read repairs and the explicitly open issues in STATUS.md.
The original service roadmap below retains its completed markers and longer-term dependencies.

**The order of work** (the owner, 2026-10-16; one block at a time, each seen in both labs): the functions themselves
matter, not example mods for them (no "fishing" example for items 15 and 16).
1. *(Done 2026-10-16; see "Done".)* Item 17, a mod's state in the game's saves.
2. *(Done 2026-10-16.)* People and animations (item 6's rest: animations played and blended, getting up, traffic pedestrians' hits,
   1.0's fields).
3. *(Done 2026-10-16.)* The events still missing (item 7's rest: what a bullet struck, a car striking a person or a
   thing, falls, items picked up, money). No "mission won or failed" and no objectives: the story is linear, a mission's script loads
   the next one when it is done (CHANGE_MISSION: MISSION_CLOSE, MISSION_OPEN already tell it), a failure is the
   player's death (HUMAN_DIED), and the objectives are texts the scripts show (`mission_objectives`), read in them.
4. *(Done 2026-10-16.)* A mod's own items in the inventory (item 10's rest).
5. *(Done 2026-10-16.)* The statistics and the profile's progress (item 10's rest: kills, cars stolen, distance,
   chapters, unlocks).
6. *(Done: 6a 2026-10-16, the water's height, zones for people and cars; 6b 2026-10-17, a prop's animation and
   physics; 6c, the game's files, each model's file, .4ds read; 6d, the meshes the game draws, an actor's posed; see
   "Done".)* The world and its geometry
   (item 8's rest: the water's height, each model's file, the drawn triangles, cars'
   meshes, the `.4ds` index; item 12's rest: a prop's collision shape and animations; zones for people and cars).
7. *(Done: 7a and 7b 2026-10-17, 7c 2026-10-17; see "Done".)* Shaders on the game's own drawing (item 27; the owner, 2026-10-17:
   put first, the base agents build "real" shaders on). In blocks: *7a* the service (a mod's Direct3D 9 shaders set on the game's draws it chooses, with the
   frame's constants under one documented contract), *7b* reference shaders that draw every kind of the game's draw
   as its fixed pipeline does (the same picture, checked by screenshots in both labs: the starting point a mod
   copies and changes), *7c* what per-object shading needs further (a shadow map from the sun through the scene
   views; the lights near an object).
8. *(Done 2026-10-17: 8a, the game's camera modes and a field of view it keeps; 8b, a mod's keys chosen in the game's
   menus; 8c, the game's airplane flown by a mod; see "Done". Left: Known gaps.)* The camera and control (item 9's rest: the game's own camera logic; item 14's rest: a mod's actions in the
   controls menu, another kind of vehicle).
9. *(Done 2026-10-17: 9a structures by name, item 24; 9b crash triage in one call, item 25; 9c restore points; see "Done".)* The agent's tools (items 24, 25 and the
   snapshots in Later: structures by name, crash triage in one call, restore points).
10. *(Done 2026-10-17: the Rust crate; see "Done". No script mods: Lua dropped.)* Mod languages.
11. *(Done 2026-10-18: see "Done".)* Orders for the people made (item 26).
12. *(Done 2026-10-18: see "Done".)* Tests in the real game, as scripts of steps (item 23).
13. *(Done 2026-10-17, before 8 at the owner's wish; see "Done".)* The tuning panel (item 18).

### Tier 1: foundations every large mod stands on

1. *(Done 2026-10-15: a heavy mod's lifecycle, the loader's part and the build's part; see "Done". The number is
   kept so the items' numbers stay.)*
2. *(Done 2026-10-15: a mod's own files in the game; see "Done".)*
3. *(Done 2026-10-15: 3a the render events, 3b the bridge, 3c the 2D pass for mods; see "Done". What is left: Known
   gaps.)* **Render events and the graphics bridge** (W S V; R for posing). As it was asked: the game's Direct3D 9Ex device (dxwrapper);
   one Direct3D 11 device beside it on the same adapter, shared by every mod: the frame's colour and depth (the depth
   buffer swapped for a readable one before the world is drawn), the frame's constants (camera, projection, the
   sun's direction and colour, the fog, the ambient light, read from the device), the mods' passes in order into one
   effects target, composited over the frame; GPU timers per pass; the 2D pass (HUD, menus) drawn into a target of a
   mod's when it asks (**3c**; a VR mod shows it on a panel). One owner for the depth swap and the composite: two mods doing
   them each would break each other, so it is the loader's. Reference: mafia-ragdoll `gfx.cpp`, `shaders/frame.hlsli`.
4. *(Done 2026-10-15: 4a, the views, and 4b, the game's geometry hidden or replaced; see "Done". What is left: Known
   gaps.)* **The scene drawn from another camera** (W V). The mission's scene drawn a second time into a target, with a
   camera and projection of the mod's (a mirrored camera and a clip plane for a reflection, two eyes with asymmetric
   projections for VR), everything the drawing touches put back after (the camera's sector held, the sound listener
   detached, lens flares kept out, the camera's bytes restored). The game's own geometry hidden or replaced around
   Render (the water's face groups; materials found by their textures' names). Reference: `reflection.cpp`,
   `scene_camera.cpp`, `game_water.cpp` (hiding).
5. *(Done 2026-10-16: see "Done"; what is left: Known gaps.)* **Measuring the frame** (W S V R P; from Later). Each mod's time in each event and pass (CPU), the GPU timers of
   item 3, the game thread's sampling profiler, on the bridge and MCP: water and VR live within a frame's budget
   (11 ms at 90 Hz). Reference: `prof.h`, `gpu_timer.h`, `sampler.cpp`.
27. *(Done 2026-10-17: 7a the service, 7b the reference shaders, 7c the sun's shadow map and the lights near a draw
    (blocks of the order); see "Done". What is left: Known gaps.)* **Shaders on the game's own drawing** (W S V and every mod that changes the look: per-pixel light, normal maps,
    reflections on cars, wet streets, shadows; the owner, 2026-10-17). Item 3's passes see the finished frame (its
    colour, depth, camera): what a surface is made of, its normal, its light are gone by then. The game draws with
    Direct3D 8's fixed pipeline (no shaders of its own: the lights, the lightmaps' second texture, alpha test, fog are
    device states), through dxwrapper's Direct3D 9 in both labs. The loader already sees each of its draws
    (`host/geom.cpp`'s DrawIndexedPrimitive hook: the visual, the face group, the texture) and the world's lights and
    fog (`host/gfx.cpp`). The service: a mod gives Direct3D 9 vertex and pixel shaders (compiled by the build, as
    `boxes`' HLSL) for the draws a rule picks (a texture's name, the visual's kind, the draw's class); the host sets
    them on those draws with the frame's constants (the world, view and projection, the camera, the lights, the fog,
    the material, the time) in registers one header documents (`mml/shaders/game.hlsli`), and puts the device's state
    back after each. The draws' classes (vertex-lit, lightmapped, alpha-tested, translucent, additive, fogged or not)
    each get a reference shader that draws them as the game does: switched on, the picture stays the game's (screenshots
    compared in both labs); a mod copies one and changes it. Plain Direct3D 8 (no wrapper) is not served.
28. **People's skinning extended** (R and every mod with new characters; asked on the community's Discord,
    2026-10-17: more than two bone weights a vertex, models of many bones). How LS3D skins (read 2026-10-19 in 1.2's
    LS3DF, 1.0's not yet): `I3D_singlemesh`'s vtable slot 0x9C (`ls3d+54F60`) runs on the CPU each time the bones'
    stamp changes. The mesh's vertices are sorted by bone; each bone's record (0x68 bytes) has `+0x40` vertices wholly
    its own (one matrix), `+0x44` vertices blended with one other bone (its index at `+0x48`, 1-based, 0 the mesh's
    root) by a weight each (`+0x64`): two influences at most, in the data itself. It writes position and normal
    (0x18 bytes a vertex) into a cache; `ls3d+65A70` then lights the vertices on the CPU from those normals, and the
    draw gets position, colour and UVs (FVF 0x142): no normals or bones reach the GPU. Two steps:
    - **28a, N weights on the CPU** *(done 2026-10-20: host/skin.cpp, mml.h skin_set and skin_get; the journal,
      MODDING "People's skinning", knowledge/notes/skinning.md)*: PoseLod hooked; a model a mod gives a skin (a
      file `models\<name>.skin.json` among its files: the marker is the file, not a flag in the .4ds, so the .4ds
      stays as every tool reads it; or skin_set) is skinned with up to 8 bones a vertex into the same cache, any
      joint of the mesh's subtree (past LS3D's 64), each bone with its own matrix at rest; every other model keeps
      LS3D's own code. Both builds, plain Direct3D 8 too. Seen on Tommy's weights smoothed to 4 bones a vertex
      (`tools/skin_smooth.py`): a bent knee's crease rounded in both labs. An exporter (Blender) is a separate piece
      of work.
    - **28b, on the GPU** *(done 2026-10-20: mml.h skin_gpu, MML_SHADE_SKINNED, mml/skin.hlsli; the journal; what it
      costs: Known gaps)*: while a mod asks, every person (a mod's skin or LS3D's own) is skinned by a vertex shader
      (the palette in a float texture the vertex shader reads, 8 bones a vertex) and lit as LS3D's CPU lit him (LS3D's
      own LightVertices choosing the lights, its sum repeated), the rest pose with the face's morph, MikkTSpace
      tangents; LS3D's CPU skinning, copies and lighting skipped. A rule's pixel shader gets the normal and the
      tangent, a rule's vertex shader the skinned inputs; the sun's map and the motion vectors take them. Both
      builds, the same picture; under a Direct3D 9 wrapper only.
    - **28c** *(done 2026-10-20: mml.h shader_constants, shader_vertex_data, MML_SHADE_VERTEX_DATA; host/vdata.cpp)*:
      the Discord's other asks for item 27: **a mod's own constants** (c160..c223 of both stages, per rule or every
      rule of the mod) and **its own vertex data** (a second stream mirroring each of LS3D's vertex pools: the mesh's
      normal, its MikkTSpace tangent, a float4 of the mod's, in LS3D's vertex order). Both builds; a lightmapped object
      drawn from a vertex set of its own is left out (Known gaps).
29. **The renderer's bottleneck at long view distances** (the owner, 2026-10-19: the whole city in view runs at
    about 30 fps on any machine, on two or three cores, though its triangles are few for a modern GPU). Not measured
    yet; the leads, from what is known of LS3D: one game thread does everything; every face group is a draw call of
    its own (362 to 497 a frame at the freeride's start with a 280 m far plane), through Direct3D 8 and a wrapper's
    translation; LS3D lights the vertices of what it draws on the CPU and refills dynamic vertex buffers each frame
    (Lock and Unlock per mesh; skinned people too, item 28); the sectors' and LODs' culling and the sorting run on the
    CPU too. First `profile` (the game thread sampled, named) and `shader.survey` (the draws, their kinds) with the
    camera's far plane pushed (`I3D_camera::SetRange`, `ls3d+B7B0`), to know which of them it is; then the fix that
    fits: lighting in the shaders of item 27 (static buffers, no CPU lighting), draws batched or instanced, work moved
    off the game thread where LS3D's data allows. A 64-bit process does not help a CPU-bound draw loop by itself.
    The way there is item 31.
31. **Everything on the GPU** (the owner, 2026-10-20: GPU skinning cost more than it saved, +0.9 ms a frame in 1.2
    and +1.3 in 1.0 with 48 people, because each skinned draw switched the device from the fixed pipeline to
    shaders and back and read its state with Get*; a rule shading every object cost some 4 ms a frame in 1.0 the same
    way). The fixed pipeline is left behind: every draw of the frame through shaders that stay bound, the state the
    game sets followed instead of asked, then the CPU's work moved to the GPU (skinning, lighting, vertex data that
    stays put) and the draws made fewer. What is known (knowledge/notes/game.md "How the game's world reaches
    Direct3D"): LS3D draws one face group a draw (362 to 497 a frame at the freeride's start), Direct3D's lighting
    off, the vertices lit on the CPU (FVF 0x142 or 0x242: position, colour, one or two uv), vertex fog, a handful of
    stage set-ups; objects drawn from vertex pools written when a place changes hands (host/vdata.cpp); people skinned
    and lit on the CPU when their bones or the sector's lights change (knowledge/notes/skinning.md). The sampler's
    first look (item 30): LS3D's visibility (`SetVisibleRecMainPass`, the portals) about 15 % of 1.2's game thread,
    the draws' calls into Direct3D 10 %, 1.0's vertex buffers' Lock 14 %.
    **How each step is measured** (item 30's tools, both builds, every step): MCP `bench` against the step before
    (and against the game's own drawing, A), in three still scenes (`state.time 0`) and one moving; `prof.counters`
    for the device's calls (draws, state calls by caller, Get*, locks, switches, the driver's time); `profile` for
    where the game thread goes; the picture against the game's (a difference per channel: mean, the worst, the
    image). A step is done when its picture is the game's and its own target is met in both labs.
    - **31a, the ground to measure on.** *(2026-10-26: the far scene's ground done in both labs: `render.set
      {whole_city: true}` draws the whole city, every block of the city's cache active, no occluders, no visual left
      out by its last LOD's distance, the game camera's far plane held at 5000 m, all given back by `false`
      (host/render.cpp); `render.set {occlusion, lod_quality}`, `camera.set {near, far}`, prof's `hot_ms` per visual
      class. The MCP `scene` tool: near, far, night, crowd, alike in both labs (seen); `bench {picture: true}`:
      each configuration's frame and their difference per channel; the baselines of four scenes in both clean labs
      and the reference shaders' price, in the journal (2026-10-26): far is draw-bound (31g), the crowd
      skinning-bound (31d), near visibility-bound (31h); the reference on every draw costs +4 ms near, +259 ms far.)* (1) Standard scenes, made by one call and alike in both builds: the
      freeride's start (near), the city with the far plane pushed to 1000 m and beyond (`I3D_camera::SetRange`,
      ls3d+B7B0: a call to set it, held while measured), the city by night (1053 lights), and a crowd of 48 walking
      people (the time going). (2) The picture compared: `bench` with `picture: true` takes each configuration's
      frame of the still world and gives the difference (mean and worst per channel, the pixels over a threshold,
      the difference as an image). (3) The CPU's work LS3D does per frame measured for every visual, not only
      people: LightVertices, the vertex pools' copies, PoseLod (`hot_ms` widened). (4) The baselines, recorded in
      the journal: each scene's frame, its game thread, Present, the GPU, the counters, the profile's top functions;
      and `bench` of the reference shaders (item 27) on every draw against the game's own drawing: the price of
      today's way, the number the next steps must beat.
    - **31b, the state followed, not asked.** *(2026-10-26, done in both labs: host/d3dmirror.cpp, the reference on every
      draw +258 to +52 ms in the far scene, +4 to +0.9 near; the Get* calls of a draw 0 (read once a frame), only the
      registers that differ and that the shaders read written. What is left is 31c's.)* The device's Set* calls (the outer links of item 30's counters, made
      a mirror) keep a copy of the state the game set: transforms, lights, material, fog, stages, samplers, streams,
      the vertex format. The shaders' constants come from it, each block written only when it changed (the view and
      the projection once a frame, the fog once, a draw's world). Measure: the host's Get* calls a frame 0; the
      constants' calls a draw; `bench` of the reference on every draw against 31a's (the 4 ms of 1.0 should mostly
      go); the picture unchanged.
    - **31c, shaders that stay bound.** *(2026-10-26, done in both labs: d3dmirror.cpp lends a rule's shaders,
      the game's given back lazily; the game's SetTransform kept while a vertex shader is lent; constants made once a
      state and compared only where a draw changes them. The reference on every draw: all 34 800 draws of the far
      scene through shaders, switches 0, the driver's time under the draws 12.7 to 7.7 ms; +258 ms in the morning to
      +5.5 (41.0 against 46.6) far, +0.23 near, +0.27 night, both builds; the picture: mean 0.23 of 255 far, 0.05 near.
      Not met: "equal or faster": one constants call a draw (4.9 ms far) and the host's work a draw remain; static
      geometry without a world matrix a draw (31f, 31g) is where it goes.)* With the GPU path on, every draw of the frame goes through shaders (the
      reference for each kind of draw the game makes, or a mod's rule); the fixed pipeline is not put back between
      draws, a shader or a constant set only when it differs from the bound one; the game's own fixed-pipeline state
      is still followed (31b) and becomes the shaders' inputs. The 2D pass (the HUD, the menus) and what the game
      draws outside the world keep the fixed pipeline, switched once at their start. Measure: `switches` near 0 a
      frame; the driver's time a draw; `bench` against the game's own drawing: equal or faster (the target: no
      price for being programmable); the picture the game's (mean under 0.5 of 255).
    - **31d, people skinned on the GPU, on that base.** *(2026-10-26, in both labs: the GPU's skinning follows the
      device through the mirror and lends its shaders; every person's palette in one texture, a row each, filled
      with one lock a frame (a lock a person before: 5 to 10 us each on 9Ex), the one before only for the motion
      vectors; the host's skinning shaders' constants made and compared as the rules'. The moving crowd: the GPU
      against LS3D's CPU skinning 1.0 -0.4 ± 1.2 ms (was +1.8), 1.2 -0.1 ± 0.5 (+0.9): even, not yet faster.
      LS3D's share gone (visuals.singlemesh 0.98 to 0.27 ms, skin.* to 0.2); what is left is the host's own work a
      person's draw (some 1.6 us and ten calls: the streams, the declaration, the palette) and the GPU's time
      (0.67 to 1.22 ms, the vertex texture fetches). Still world: +0.68 ms, the picture the same (0.13 %).)* Item 28b's palette and lighting in the bound shaders (no
      switch a person); LS3D's skinning, copies and lighting skipped. Measure: the crowd scene, CPU skinning
      against GPU: the GPU's faster (28b's +0.9 / +1.3 ms becomes a gain); `hot_ms` skin.* (LS3D's share gone).
    - **31e, objects lit on the GPU.** *(2026-10-26: weighed, waits: LightVertices about 0.3 ms of the night scene.)*
      The normals from 31's vertex data (item 28c's second stream), LS3D's choice of
      lights for a visual taken as for people (its LightVertices run on one vertex, or the sector's lights read),
      LS3D's lighting of objects skipped. Measure: LightVertices's time a frame (31a's widened `hot_ms`) to 0; the
      night scene (the most lights); the picture.
    - **31f and 31g, done together** *(2026-10-26, both labs: `render.set {batch: true}`, host/batch.cpp)*: each
      opaque group of a static I3D_object captured once from LS3D's own draw (its vertices lit by LS3D read from the
      pool with a NOOVERWRITE lock, put into the world, kept in the host's buffers: 31f), then drawn with every group
      of the same state in one draw (a batch a state: textures, stages, alpha test, depth and its bias, fog, samplers;
      the index buffer of the groups LS3D asked for this frame: its visibility kept exactly), LS3D's own call for the
      group not made (31g); the batches asked for so far drawn before anything LS3D draws itself (its order kept: a
      road's marks and decals on the batched ground). The whole city from 120 m up: 1.2 40.1 to 22.7 ms (34 800 draws
      to 5 100 and 922 batches), 1.0 40.1 to 22.8; near, night, the crowd even (their few hundred draws). The
      picture: far 0.27 of 255 in both builds, 1.03 % of the pixels over 8 (the silhouettes' edges: the vertices put
      into the world on the CPU), near 0.04, night 0.015. Left to
      LS3D: blended groups, environment maps, the backdrop, depth not written or tested, objects that move (twice in a
      second), objects with one such group (their layers keep LS3D's order), the people and every other visual class.
      *(2026-10-27: an object whose groups are all batched is not given to LS3D's I3D_object::m_AddFGroups while
      nothing it would do changes (`render.set {batch_objects}`, on with `batch`): its groups asked for at once, the
      batches drawn at their place in LS3D's sort; the whole city from 120 m up 1.2 21.96 to 18.48 ms, 1.0 21.90 to
      18.56 (the picture the batches' reference: 1.0 against LS3D 0.276 of 255, 1.04 %); near -0.05 ms, night
      -0.03 to -0.04, the crowd the same, in both builds. 10 262 objects, 26 800 groups a frame not through LS3D.
      The batches' memory held: a group not asked for in 30 s is dead (the city's streaming makes its blocks' objects
      again elsewhere), a batch a quarter dead is rebuilt, every 300 frames whatever its size: far, near and far
      again three times in 1.0 keeps 3.6 million vertices and 845 MB of address space free (it was 6.5 million and
      608 MB).)*
      Opt-in until it has been driven through the missions. Not done: the lightmapped objects (I3D_lit_object), a
      Reset on DXVK (the batches' buffers are D3DPOOL_DEFAULT), the rules' shaders on batches.
    - **31f, vertex data that stays put.** Each LOD's vertices uploaded once into buffers of the host's (positions,
      uv, normals: what the GPU needs, lit or skinned there) and drawn from them; LS3D's pools and their copies left
      unused for those draws. Measure: locks a frame and their time (1.0's 14 % of the game thread), the far scene.
    - **31g, fewer draws.** What stays the same from frame to frame merged: a sector's static face groups by
      material into one buffer and one draw, repeated models (lamps, trees, cars of one model) instanced. Measure:
      draws a frame, the driver's time, the far scene's frame (item 29's 30 fps).
    - **31i, the game's buffers renamed.** *(2026-10-27, done in both labs: host/vbrename.cpp, `vb.state`, `vb.set`,
      `vb.check`, `vb.selftest`.)* LS3D locks its vertex and index pools whole with no flags (1.2 lod::SetUV for the
      env-mapped objects, 1.0's pool lock ls3d+A9D0; 1.0's DrawIndexedPrimitiveList once for each person's shadow): each
      lock waited for the GPU. Every D3DPOOL_DEFAULT buffer the game makes (64 KB and more) gets a copy in memory
      (MEM_WRITE_WATCH) its locks are given, and copies of ours on the GPU the pages written go into at the Unlock, one
      the GPU no longer reads (event queries), drawn from then on; NOOVERWRITE and DISCARD kept as the game asked, rings
      given back. A/B against the game's own locks (`vb.set {rename: false}`), one lab at a time, a quiet machine: the
      menu 1.2 0.750 to 0.694 ms, 1.0 0.761 to 0.713; near 1.2 1.435 to 1.206, 1.0 1.548 to 1.264; the crowd 1.2 6.77
      to 6.16, 1.0 6.94 to 5.87; far 1.2 37.08 to 34.69, 1.0 39.77 to 36.88; far batched (1.2) 24.49 to 22.00. The
      picture the game's (the scenes' own flicker); `vb.check` 0 pages differing in both. With the GPU busy (other
      programs) far more: near 6.9 to 2.2 ms. The whole city: 176 buffers, 22 MB in memory, 40 MB of copies.
    - **31h, the visibility.** *(2026-10-26: measured on the batched far frame, 1.2: SetVisibleRecMainPass about
      13 %, 3 ms (near 15 %); it tests each child against the four planes again whatever its parent's result: an
      "inside" mask handed down is the first step. Before it: m_AddFGroups skipped for objects whole in batches
      (JOURNAL, 2026-10-26, "the request made cheaper").)* Not the GPU's work but the far scene's other bottleneck: LS3D's portals and frame
      walk on the CPU. After 31a's numbers: cached per camera cell, made cheaper, or occlusion moved to the GPU
      (queries, a depth pyramid). Measure: the profile's share of `SetVisibleRecMainPass` and the portals at 1000 m.
    Under a Direct3D 9 wrapper only (dxwrapper, DXVK), as items 27 and 28; plain Direct3D 8 keeps the game's own
    drawing. The order may change with 31a's numbers: a step whose share is small waits.
32. *(Done 2026-10-21: 32a to 32f, each seen in both labs; the ragdoll's own repository is next, the owner's.)*
    **The ragdoll's toolkit** (R; the owner, 2026-10-21: the ragdoll becomes a loader mod in a private repository of
    its own, and the loader gives it everything it needs first). What mafia-ragdoll's host and core do with raw hooks
    and addresses (its `hits.cpp`, `knock.cpp`, `getup.cpp`, `frame_life.cpp`, core.cpp's pedestrians) becomes host
    services for any mod, both builds, each with its bridge method, its offline test, MODDING and the Rust crate:
    - **32a, a person's hit before the game's** *(done 2026-10-21: mml.h hit_filter, human_hit, pedestrians_list;
      host/hits.cpp, host/pedestrians.cpp; MODDING "Hits before the game")*. A mod's filter on every hit a person takes, before the game reacts:
      C_human::Hit, the pedestrians' Hit, and a bullet on a pedestrian before the system's Hit wakes the element (a
      dead one would cry out). It sees the hit (type, dir, pos, damage, attacker, the part) and lets the game have
      it, changed or not, or takes it (the game's Hit does not run: a corpse's hit is the mod's). HUMAN_HIT and
      PEDESTRIAN_HIT tell a taken one as taken. And `human_hit`: the game's own Hit called on a person or a
      pedestrian (a knock-down by a body is the game's car hit with no car and no damage: its fall, its get-up).
    - **32b, bullets on a mod's shapes for a person** *(done 2026-10-21: mml.h bullet_shape; host/hits.cpp; a dead
      C_human's own collision follows his bones, a dead pedestrian's is out of the grid: knowledge/notes/game.md)*. A mod gives a person's body as capsules (a ragdoll's parts,
      wherever the body lies); the bullets' ray (collRayTest from the bullets' tick) tests them too, and the nearest
      nearer than the game's hit is given to the game as a hit on that person (his own collision object, the game's
      part test for a C_human: the blood, the part, the Hit, then 32a's filter). The exclusion list the bullets'
      tick keeps and a guard against the same stretch given again are the host's (mafia-ragdoll hung the game so).
    - **32c, a frame's release told** *(done 2026-10-21: mml.h frame_watch; host/framelife.cpp; MODDING "A
      frame's release")*. A mod watches the frames it keeps (a model, its bones); the host tells it
      before LS3D frees one (I3D_frame's and the meshes' Release reaching zero), so nothing is used after free.
    - **32d, traffic pedestrians as people** *(done 2026-10-21: humans.cpp and anims.cpp take an element; MODDING "Hits
      before the game")*. Their elements listed (with their model, place, health), their
      skeleton read and posed (human_skeleton, bone_set), handed back by human_get_up (getup.cpp's way: out of the
      wait, the collision back in the grid, pedAnimStart), and hit (32a's human_hit, pedHit).
    - **32e, drawing in the world** *(done 2026-10-21: mml.h draw_world; host/worlddraw.cpp; MODDING "Drawing in
      the world")*. Lines, boxes, spheres and capsules in the world's space, tested against the
      frame's depth or over it, from mods (each frame or held for a time) and from the bridge (`draw.world`): what a
      physics mod draws to see its bodies, joints and contacts.
    - **32f, seen in the game** *(2026-10-21, both labs: a corpse's skin follows bones written 8 s after his death;
      a dead pedestrian's element and model stayed 2 minutes beside the player, not given to a new pedestrian: its
      recycling not seen yet; bullets on a corpse: 32b)*. Whether a dead person's skin follows bones a mod writes after his death animation
      ends; what the traffic does with a dead pedestrian's element (kept, recycled: 32c tells it); a corpse hit by
      a bullet where the body lies, in both labs.

### Tier 2: the game's world as data, read and changed

6. *(Done 2026-10-16: 6a the list, the skeleton, poses; 6b animations, getting up, the pedestrians' hits, 1.0's
   fields; a model's (a prop's) animations with block 6b of the order, 2026-10-17; see "Done". What is left: Known
   gaps.)* **People, skeletons, animations** (R K F; S W as pushers). Every `C_human`, the traffic system's pooled ones too,
   as a list (the constructor's hook already sends `HUMAN_CREATED`; no list is kept); traffic pedestrians' elements
   and their own Hit (`0x4588D0`) as events; the bones by name, a pose read in one call (the 18 joints, the model
   root), written back before Render (item 3); handing a body back to the game (getting up); the game's animations
   and a mod's own (item 2) played on a person or a model, blended, stopped, their state read. 1.0's fields for
   bullets, getting up and trams (zeros today). Reference: the host's `listHumans`, `ragdoll.cpp` (`PoseSnap`),
   `getup.cpp`, `hits.cpp`; the scripts' `human_playanim`, `model_playanim`.
7. *(Done 2026-10-16: 7a, shots, explosions, cars blown up, deaths with their killer, vehicles entered and left; 7b,
   what a bullet struck, cars' crashes, falls, items, money; see "Done". What is left: Known gaps.)* **What happens, as events** (R W S C). A bullet's path and what it struck (the game's shot queue, `NewShoot`
   0x5A84A0), explosions (place, radius, force), a car striking a person or a thing, falls, deaths with who killed
   whom, a car entered, left or stolen, an item picked up, money changed, a mission's objective done, the mission
   won or failed: today only a hit on a `C_human` and the missions' opening and closing are told. Reference:
   `hits.cpp`, `knock.cpp`, core.cpp's shot hook.
8. *(Done: 8a 2026-10-16, the questions in `mml.h` (a ray, a sweep, the actors near a point, the collision's shapes, the
   ground under a point) answered by the game's own tests; the water's height (6a), the files each model came from and
   the `.4ds` read (6c), the drawn triangles around a place and cars' meshes posed (6d); see "Done". What is left:
  Known gaps.)* **The world's geometry and questions about it** (R S W K P F). The file each loaded model came from (hooked from
   the process's start: only the loader is there that early); the triangles of what the game draws around a point
   (cached, fed a few pieces a tick), the collision grids (static primitives, dynamic objects with their owners),
   cars' meshes posed with their parts, an index of every `.4ds` in the archives and its parser; one scan shared by
   every mod. Questions in `mml.h` (not only C++'s `game.h`): a ray, a sweep, what overlaps a box or sphere, the
   ground's and the water's height under a point, the actors near a point. Reference: `scenery.cpp`,
   `visual_meshes.cpp`, `collision.cpp`, `game_collision.cpp`, `model_index.cpp`, `model4ds.cpp`, `car_meshes.cpp`.
9. *(Done: 9a 2026-10-16, the frame drawn from a mod's camera, a world point on the screen and back; 8a of the order
   2026-10-17, the game's camera modes and a field of view it keeps; see "Done". What is left (its distance, an aim
   from another camera): Known gaps.)* **The camera** (V R K P). The game's camera read and set (position, orientation, field of view, near/far), its
   projection replaced after the camera's own update (VR's per-eye frusta), the view decoupled from the aim, a camera
   of a mod's own following its own rules (a skateboard's, a plane's chase camera), a free camera; a world point to
   the screen and a screen point to a ray (markers, aiming). Reference: `freecam.cpp`, `ragcam.cpp`,
   `scene_camera.cpp`.
10. *(Done 2026-10-16: 10a the freeride's money, health, the inventory and its rounds, items taken; 10b a mod's
    own items; 10c the statistics and the profile's progress; see "Done". What is left: Known gaps.)* **The game's state and progress** (C F P). Money, health, the inventory and ammo (read and changed), the
    mission's objectives and its outcome, the game's statistics (kills, cars stolen, distance), the profile's
    progress (chapters, freeride unlocks); a mod's own items in the items' table (a fishing rod, a fish).
11. *(Done 2026-10-16: 11a the scene's lights, 11b the car exhaust and `particles.def`, 11c the game's particles
    moved, 11d the weather; see "Done"; what is left: Known gaps.)* **The game's effects as data** (S W P). Particle emitters and the car exhaust (`0x4DEE90`) read, moved or
    replaced; `tables\particles.def`'s records; the weather's parameters; the scene's lights. Reference:
    `smoke_game.cpp`, `effects.cpp`, `frame_life.cpp`, `scene_light.cpp`.

### Tier 3: building blocks for new things in the game

12. *(Done: 12a 2026-10-16, props made, placed, attached, shown, removed; 2026-10-17 (block 6b of the order) their
    animations and physics; see "Done". What is left: Known gaps.)* **Props** (F K P R). Models placed in the world without being actors (a rod, a fish, a board), moved every frame,
    attached to a bone or a car's part and detached, shown, hidden, removed; with a collision shape when asked
    (the scripts' `model_create` and `create_physicalobject` are partial cases).
13. *(Done 2026-10-16: see "Done"; what is left: Known gaps.)* **Sound** (F K P C S W). The game's sounds and a mod's own (item 2) played in 2D or in 3D at a point or attached
    to a frame, looped, their volume and pitch changed live (an engine's note follows its revolutions), stopped;
    the music.
14. *(Done: 14a 2026-10-16, keys and actions kept from the game, the player locked, a car's physics replaced;
    2026-10-17, a mod's keys on its settings page and the game's airplane flown by a mod; see "Done". What is left
    (actions in the game's controls menu, riding a vehicle that is not a car): Known gaps.)* **Control** (K P F V). A mod's keys and buttons taken so the game does not also act on them; actions of a mod's
    own, bindable in the game's controls menu like its own; the player's movement handed to a mod (a skateboard
    moves him, the game's walking stands aside) and given back; a vehicle's physics replaced by a mod's (a plane's
    flight on a car's or an airplane actor's body: `C_airplane` exists); entering, leaving and seats of a mod's
    vehicle (the game's using-objects).
15. *(Done 2026-10-16: see "Done"; what is left: Known gaps.)* **Interaction** (F P C). Use-points with the game's own prompt ("Fish" at the water's edge), zones with enter and
    leave events, timers of the game's clock (they stop with the pause).
16. *(Done 2026-10-16: see "Done"; what is left: Known gaps.)* **The HUD** (C F P V). Pictures and textures in the 2D layer (an achievement's icon, a plane's instruments:
    drawing has text, lines and rectangles only), notifications in the game's style (its console, `console_addtext`,
    with a mod's own texts), markers at world positions, the compass's and the map's target.
17. *(Done 2026-10-16: see "Done"; what is left: Known gaps.)* **A mod's state in the game's saves** (C F). A mod's data written with each save slot and read back when that
    slot loads (and per profile), so its state follows the player's game, not only its data folder.
18. *(Done 2026-10-17: see "Done".)* **A tuning panel** (all). A mod's settings shown and changed live in the game: a page made from its
    `config.json` (sliders, check boxes, selectors from the menus' constructor), so dozens of parameters need no page
    written by hand.

### Tier 4: what only exists for 64 bits

19. *(Done 2026-10-18: the sidecar, its channels and shared textures, PhysX 5's first user: see "Done"; left: VR's
    question, and PhysX's GPU part: below.)* **The 64-bit sidecar** (the owner's decision; S R with PhysX 5, V if a runtime needs it): `mml-sidecar64.exe`,
    started and watched by the host, loads 64-bit mods (the same kind of manifest, built for x64), their faults
    isolated and their reloads hot as 32-bit mods'. Shared memory between the processes: channels a frame (the
    game's state out: actors, bones, car bodies; results back: poses, forces) and calls both ways for what is rare;
    Direct3D 11 textures shared across the processes, so a 64-bit side can draw or simulate into the frame. The
    first user and the test: PhysX 5 stepping a body the game shows, with a frame's round trip measured; the design
    is fixed against that measurement, not before. Whether the VR runtimes (OpenXR, OpenVR) take a 32-bit process
    is to be checked: OpenVR ships a 32-bit library; if OpenXR's runtimes do not, VR is the sidecar's second user.
19a. **What 32 bits cannot do: PhysX's GPU part** (the owner, 2026-10-18: the crates only showed that the way works; the
    test is what was impossible before). Particles (PBD: water, smoke as particles), cloth and deformable bodies (FEM
    soft bodies) on the GPU, in a 64-bit half, shown in the game (a shared texture, or meshes). Needs the CUDA Toolkit
    12.8 to build PhysX's GPU projects (`PhysXGpu`, a DLL beside the half) and a Volta GPU or newer to run.
    *(2026-10-18: NVIDIA Flow 2's smoke and fire done first, the owner's ask: `flow_smoke`, see "Done". PhysX's GPU part
    is built (CUDA from `tools/deps.py cuda`, `mml_physx_gpu`) and `physx_gpu` (water, cloth, a jelly) loads on the GPU
    in both labs. 2026-10-19: seen working in both labs (water, cloth, a jelly; MODDING "PhysX's GPU part"): the water
    no longer falls through the game's ground (speculative CCD, 8 iterations), its thin sheet is drawn as a wet film.
    Left: the film mirrors only the fog's colour (no reflection of the world: a screen-space reflection would), the
    sheet's edges are its particles' clusters (round blots, not a puddle's line), 32 768 particles (30 m3, the default)
    is a lot of water for a street.)*
19b. **Collision from the visible models** (the owner, 2026-10-18): the physics meets what the player sees, not the
    game's boxes, spheres and capsules. The host already gives the meshes the game draws (`scene_pieces` around a box,
    `actor_meshes` an actor's parts posed): the static world as their triangles, a prop as the convex hull of its
    `.4ds`, people and cars as their posed parts.

19c. *(2026-10-19: made, `dlss_nr`; NVIDIA's model itself not seen yet: below, "Known gaps".)* **NVIDIA DLSS 5 Neural
    Rendering over the game's own frame** (the owner, 2026-10-19: the game as it is, no ray tracing, with DLSS 5 NR;
    a guide for a friend's RTX 5060). NGX's DLSSNR through Vulkan in a 64-bit half, the frame, its depth, motion
    vectors and a mask from the graphics bridge, the picture back under the HUD; settings, a hotkey, a split screen.
    *(2026-10-24: DLSS as designed: the bridge's upscaler, `gfx_upscale`: the world drawn at the mode's render size,
    the jitter in the projection, the textures' mip bias DLSS's, the HUD at the frame's size; the devices ordered on
    the GPU under DXVK (shared fences), and DXVK in the dlss_nr pack; the frame's times in the log once a minute.)*

### Tier 5: the agent's own tools (to write and debug those mods)

20. *(Done 2026-10-16: see "Done".)* **Waiting for what happens** (A). The bridge keeps the last events (item 7's, missions, actors destroyed,
    programs ended, mods faulted) with a sequence number; `events.since {seq}`, `events.wait {type, filter,
    timeout_ms}`, MCP `wait` ("until the mission is loaded", "until he dies").
21. *(Done 2026-10-16: see "Done".)* **`native.call`** (A). Any game function called from the bridge by its address and convention (thiscall,
    stdcall, cdecl, fastcall; integer, float, pointer and string arguments; a fault guarded): a hypothesis in one call.
22. *(Done 2026-10-16: see "Done".)* **Hardware watchpoints** (A; from Later). Who writes or reads a field, on the bridge and MCP; the handler never
    reads the watched address (a recursion crashed the game once). Reference: `hwwatch.cpp`.
23. *(Done 2026-10-18: see "Done"; a body falling into the river and a road tunnel are in Known gaps.)* **Tests in
    the real game, as scripts of steps** (A). Python over the bridge (`tests/game/`): start, spawn,
    teleport, input, wait for an event, check a value, screenshot; pass or fail. First cases from the measuring mods:
    a car hits a person, a body falls into the river, an exhaust in a tunnel, a camera turned, a prop held in a hand.
24. *(Done 2026-10-17: see "Done".)* **Structures by name** (A). `inspect <addr> as C_human` (and other classes):
    fields named and typed from `game.h` and the decompile's layouts, kept in `knowledge/` as tables of offsets.
25. *(Done 2026-10-17: see "Done".)* **Crash triage in one call** (A; from Later): the named stack, the log's last
    lines and the last taps' calls.
26. *(Done 2026-10-18: see "Done"; driving to a place, the scripts' events to a loader's program and the people's
    models listed are in Known gaps.)* **Orders for the people made** (A, for test scenes). Walk or drive to a
    place, follow, attack, flee: the AI's own jobs and the mission commands that give them, as `world.*`; the
    scripts' events (`setplayerfireevent`...) to a program of the loader's; people's models listed.
30. *(Done 2026-10-20: the journal; MODDING "Measuring the frame".)* **Measuring without guessing** (A; the owner,
    before item 29): each frame's record (Present's blocking, the game thread's CPU, the GPU's span), the device's
    calls counted by kind and caller with the driver's time under the draws, the sampler's stacks checked frame by
    frame and made functions (the loader's by its PDB, 1.0's by the projects' function bounds), two configurations
    compared in one run with an interval (`bench`), a still world (`state.time`).

## Later

- *(Done 2026-10-17, block 9c: restore points; see "Done".)* **State snapshots for experiments** (a car's state is
  there: `car.save` / `car.restore`): the game's own save and load used as a restore point, to repeat an experiment
  from the same moment.
- *(Done 2026-10-17; see "Done".)* **Rust mods**.
- *(Done 2026-10-18; see "Done".)* **The knowledge base for 1.0**.
- *(Done 2026-10-18; see "Done".)* **Ghidra behind MCP**.
- **Publishing**: CI for the offline tests.
- **Every format of models, textures and animations** (the owner, 2026-10-20: if it costs no speed; not now). The
  loader already answers the game's own file opening from the mods' `files\` (files.cpp, dtaOpen): a texture of
  another format (PNG, TGA, DDS with its mipmaps and block compression), a model (glTF, FBX, OBJ) or an animation
  (glTF's, FBX's, BVH) converted once, when the game opens it, into what LS3D reads (its BMP and TGA, .4ds, .5ds),
  and kept converted in the mod's data folder: nothing changes in the frame, the cost is the first load's. What LS3D
  cannot hold (more bones a vertex, normal maps) goes through items 27 and 28.
- *(Done 2026-10-21; see "Done".)* **The game's videos**.
- **Motion vectors without a mod** (the owner, 2026-10-20): a bridge method that asks for them and draws dlss_nr's
  check view (the frame before moved by them against this one) over the frame, so an agent checks them with no mod
  enabled (today only dlss_nr asks for them and shows them).

## Taken out or changed

2026-10-14, the first review (the owner: the roadmap was the agents' work and may hold absurd items):

- **"The time of day"**: the game has no clock. Day and night are separate scenes (the freeride's `freeitaly` and
  `freeridenoc`; a mission's own); `GAME_NIGHTMISSION` / `C_game::SetNightMission` (0x47B590, `C_game + 0x2D44`)
  only switch what lights do at night (cars' lamps), not the sky (MafiaOnline's notes agree). Night is the night
  freeride: the start takes it (done, 2026-10-14).
- **"A knock-down that always works"**: a ragdoll mod's own work, on items 6 and 7.
- **"Scenarios run by the loader"** (a file of steps the loader plays): a second scripting engine inside the game for
  what a script outside does with the bridge; what was missing is waiting for an event (item 20). Now item 23, in
  Python.
- **"LuaJIT script mods"** and **"`lua.eval` for agents"**: argued as "a hypothesis tried in five lines without
  building a DLL", which `mem_read` / `mem_write`, `tap`, the script machine and hot reload now cover; the one thing
  missing, calling any game function by its address, is item 21. Dropped for good by the owner (2026-10-17: a
  Lua runtime would serve people writing gameplay scripts, not the agents who write the mods here); LuaJIT is no
  longer pinned in `tools/deps.py`.
- **"The 64-bit sidecar"** was taken out as having no user, and put back the same day by the owner: modern libraries
  (the latest PhysX first) exist only for 64 bits; item 19.

2026-10-14, the second (the owner: the toolkit for a ragdoll, water, smoke and VR mod; old tasks kept, duplicates
merged, sorted by importance):

- **"mafia-ragdoll on the loader"** (Later, original 2026-10-14 decision): the monolithic port task was taken
  out of this repository; its host/core needs became items 1, 3 to 9 and 11 as services for any mod.
  The later ragdoll port lives in mafia-ragdoll-mml, with the original source retained as reference.
- **"Events to MCP as they happen"** and the events for mods: the mods' events are items 6, 7 (people, what happens);
  the agents' stream is item 20, which carries them too.
- **"Profiling"**, **"Hardware watchpoints"**, **"Crash triage"** moved up from Later (items 5, 22, 25): water and VR
  need the frame's budget measured; watchpoints and triage are how engine work is debugged.
- **"Orders for the people made"** moved down to the agents' tools (item 26): none of the four mods needs it, test
  scenes do.

- **The goal widened** (the owner: "something silly too: fishing, skateboarding, a working airplane, achievements;
  the toolkit must be strong enough for anything a user thinks of"): what such mods need and the four engine-deep
  ones did not became items 2 (a mod's own files in the game), 10 (the game's state and progress) and Tier 3 (props,
  sound, control, interaction, the HUD, a mod's state in the saves); animations joined item 6, queries item 8,
  the game's events item 7, a mod's own camera and world-to-screen item 9.

2026-10-17, the Rust crate:

- **"A `#[mml_mod]` entry macro"**: an attribute macro needs a proc-macro crate of its own (syn, quote) in every
  mod's build for what one declarative macro does: `mml::entry!(load)` makes the three exports around a plain
  `fn load(host) -> Result<(), E>`.

## Known gaps

- **Vehicle skid-model cleanup crash (2026-11-16):** owner PID22132 on validated host A795A2FA...
  faults at native1.0 Game+111AA4 while closing mise19-pauli. Wheel index1 retains a shlaif01.i3d
  model whose vtable is zero; its virtual Release reads address zero. Dump, log and matching binaries
  are preserved. A disposable-buffer experiment exactly matches the corrupted fields/header to native
  local-matrix calculation with a receiver 0x68 before the damaged model. UpdateWMatrixProc inlines
  that same writer. A stale HUD car was then observed after taxi-to-Pauli loading: world_near uses it
  after native deletion, and a disposable SDK-call fixture produces the exact damaged words. Both
  pre-fix labs fail the native deletion/mission-close lifetime tests. The host repair clears the borrowed
  reference and passes4 checks per build, full offline/MCP regression, and the owner's taxi-to-Pauli
  lifecycle check. Host BD3BD3C4... is installed in both labs and Mafia10Fixes; diagnostic removed.
  The original occurrence's writer stack remains absent.
  Normal replay success is not attribution evidence. See [evidence](../knowledge/notes/crash-20261116.md).

- **Live allocator/recovery hang (2026-11-15, captured):** owner PID32624 stopped after a guarded
  Jolt-step failure. Retained TLS fault stack shows constraint destruction/Clear faulting in the native
  heap SList helper; Faulted then blocks allocating JSON from the affected allocator. Three x86 snapshots,
  including full memory, confirm the unchanged wait. The first guarded exception and earlier heap writer
  remain unknown. Lab ASan caught a separate stale read in `Ragdoll::Apply`. A focused fixture verifies
  the pending destructor-notification fix on both builds. After repairing stale scene-frame reads and
  bounded name overreads, combined host/port-ASan completes100 car/body cycles with zero reports and
  zero guarded native errors. Focused native regression passes22 assertions per build. The validated
  host is now installed in the owner copy, with startup/lifetime checks and a verified backup. The first
  guarded exception and the relation of these defects to the owner incident remain open. See
  [incident evidence](../knowledge/notes/hang-20261115.md).

- **Pedestrian teleport at impact, 2026-11-14**: the second trace records a 2.4 m intermediate
  native model rewrite and at most 0.185 m between consecutive post-world pelvis samples.
  At capture time the visible displacement was unassigned. Subsequent draw observation isolated
  one pre-physics native dodge rewind (see the separate fixed mechanism below). This does not assign
  every earlier incident. See knowledge/notes/teleport-20261114-1637.md and STATUS.md.

- **Owner's intermittent pedestrian-to-car teleport** (2026-11-11): fixed two concrete port defects:
  the native sweep restored a position already committed by its hit handler, and a same-tick pose
  change was lost when ragdoll initialization selected the previous tick. A controlled 1.5 m pose
  change reproduced the latter; the corrected transition passes in both builds. The reported
  incident had no pre-hit pose sample, so its exact cause remains unproven. The owner then reported an
  unnatural fall and another teleport on the updated mod. Both had zero initial skeleton gap; first
  solver displacements were 0.0414 and 0.0017 m. These numbers do not explain the visible anomaly.
  The port now retains up to 128 samples per contact over 3.5 s, before/after tick and render-begin
  pose application. `ragdoll.handover` reads them even after recovery; default state stays compact.
  2026-11-12: the owner narrowed this to an apparent right-corner-to-left-corner transfer. Five initial
  contacts had zero initial pose gaps. An added post-world matrix observer retained six more contacts
  from the owner's confirmed recurrence, all with zero previous-world/pre-hit and initialization gaps.
  Their post-world pelvis matrices match physics; the last body begins accelerating about 48 ms after
  creation. This does not prove that the mesh pixels followed those matrices, or establish the cause.
  Bounded per-body car contact records now include normal, depth, car axes/origin and dropped counts;
  observer port 2F791D64... is installed in owner PID 21276 generation 3, pending another recurrence.
  Same-model native corner runs in the labs have not reproduced the exact reported transfer.
  Evidence: build/pedestrian-handover-20261111 and build/pedestrian-teleport-20261112.
  Do not call the intermittent report resolved.

- **Traffic pedestrian without ragdoll (2026-11-14)**: the `civil31.I3d!` model name was passed to
  the native-asset SDK without conversion, so calibration failed and the native death animation
  took over. SDK now resolves this alias to `civil31.4ds`. Skeleton equality and actual ragdoll
  creation on a test actor with the exact marked name pass in both labs. Fix installed in the
  owner's copy. Two incidents have matching pre/post hit positions and later displacements of
  6.19 m and 6.86 m. The exact movement interval remains unresolved. The installed ragdoll
  observer now records native and physical positions independently and persists large candidate
  changes. Both builds passed the observer fixture with a simulated pose change, with and without
  a physical body. Evidence: build/pedestrian-no-ragdoll-20261114 and build/pedestrian-trajectory-20261114.

- **Bumper-camera car collision (2026-11-14, fixed)**: ragdoll used the visible-mesh list, so a
  camera-hidden vehicle root could leave the physical world with no car shape and select OnHit's
  fallback impulse. SDK actor_meshes_ex(HIDDEN_ROOT) supplies native visual geometry independently
  of root visibility. Both labs verify preserved visibility behavior plus real truck/corpse contacts
  under the bumper camera. Owner PID 19940 has a registered 3000 kg TruckBx00 with its root hidden.
  Installed host 1BEB2DD9... / port 97B7E11E...; build/vehicle-hidden-root-20261114. Shapes use per-part
  convex hulls of the loaded native mesh, not a box proxy. A separate native-animation root rewrite
  was captured (5.202 m root movement versus 0.095 m physical movement). Its rendered effect remains open.
  Owner reported another immediate-at-impact teleport at 16:04 on 2026-11-14. Evidence retained;
  the current contact's tracked initial pose gap is zero and after-world pose error <=0.0158 m.
  This does not explain the visible report. knowledge/notes/teleport-20261114-1604.md.

- **Pedestrian dodge rewinds before car impact (2026-11-14, fixed in the ragdoll port):** matching
  draw and contact traces record a 2.388 m visible jump before the first physics step. An A/B test in
  both builds reproduces a roughly 2.4 m rewind from the native animation-start sweep. The port
  sweeps from the last accepted root, preserving collision with obstacles ahead and native pose
  handover. Earlier uncaptured or post-handover movements remain separate investigations.
  See knowledge/notes/pedestrian-dodge-20261114.md.
  On November 14, PID 27624 loaded port 87379006..., generation 2; sixteen live checks passed per build,
  including the corrected living-pedestrian get-up fixture. November 15's newer port is recorded in Done.
  Earlier unmatched/post-handover reports remain open; a short owner replay is not blanket acceptance.

- **Engine gain/timbre after pause** (2026-11-12): cause isolated in a standalone synthetic-tone
  reproduction: DSBCAPS_LOCDEFER Stop/Play restores old effective gain AND rate while getters report
  the newer values. The owner correctly rejected the earlier gain-only workaround; it is removed.
  Native Init now selects LS3D's existing software-buffer path when hardware 3D capacity is zero,
  preserving real hardware/EAX providers. Actual buffers retain spatial/frequency/volume controls.
  Standalone acoustic regression and five native-allocation assertions per build pass. FF4E79EC...
  installed in both labs and Mafia10Fixes PID 10752. The owner's long recording retains the 161 Hz
  line after pause; 80 Hz..20 kHz post-pause/post-throttle bands agree within 0.09 dB. Lowest-band
  variation and approximate layer-fit residuals are documented; no identical-waveform claim.
  Mission and exact sounds/music/cars/dialogue 1/0/1/0 restored, paused. Owner listening check of
  this replacement is pending. knowledge/notes/audio.md; tests/dsound; build/engine-pause-20261112.
  Separate stutter report below remains open.
    Owner-requested EAX launch on 2026-11-13: game-local DSOAL/OpenAL Soft installed in Mafia10Fixes;
    PID 24008 verifies native EAX3 flags 0x300002 and both live property interfaces. Native software-only
    setting disabled with backup and owner-authorized elevation. Acoustic EAX A/B and pause regression
    on this provider remain pending (build/eax-20261113); no host change or lab deployment in this step.

- **EAX environment fidelity** (2026-11-14): owner reports accumulating indoor rumble and similar
  outdoor echoes. Both native updaters select/blend sector sound blocks and submit a custom EAX3
  environment; the active provider is pinned DSOAL/OpenAL Soft, not a host reverb implementation.
  PID 19940's Primary sector readback has 1 s decay, 300 ms early-reflection delay, 100 ms additional
  late delay, diffusion 0.4 and explicit echo depth 0. Native sector delay values agree. This does
  not reproduce or explain the reported indoor event. Capture an affected sector/source/parameter
  sequence and its audio before assigning fault or tuning. No settings/binaries changed.
  knowledge/notes/eax-environments.md.

- **Sound-dependent gameplay / speedrun audit** (2026-11-13, original 1.0 logic): engine start
  has a real sound-position gate with a 300-ms margin; camera-distance sound disabling uses
  60/110 m thresholds. Matched background DSOAL EAX3 on/off tests give 3119/3120 ms medians,
  three trials each; no instant start or repeatable EAX3 gain established. Native channel eviction
  saving the incoming buffer's cursor for the victim reproduced silently in background; byte/ms
  queue mismatch verified in code. Naturally triggered strategies, resume consequences, other cars
  and mission/dialogue timing remain open. Do not fix these research targets without an owner request.
  Further research: background bridge only, no computer-use/window activation (owner clarification).
  knowledge/notes/audio-logic.md; build/eax-20261113. Temporary lab provider/config removed, labs closed.

- **Mission-transition hang** (2026-11-14, Mafia10Fixes 1.0): PID 21004 ended through Windows after
  a long nonresponsive load; AppHangB1/cross-thread, no fresh exception dump. Last scene mise18-mesto,
  no next open line. Historical cause remains unestablished. A separate, controlled scheduling test
  proved native Ogg Close can kill its reader while it owns rw_data's shared critical section,
  deadlocking ov_clear/dtaClose. Three induced-hang snapshots captured; exact test PID 9596 closed.
  Cooperative stop/join implemented with native refill/backend/EOF semantics retained. Controlled
  archive-lock regression, 32 immediate closes, and native buffer policy checks pass in BOTH labs
  (12 checks/build). Also 100 eight-stream cycles and six restore loads/build. Installed in owner
  EAX copy: controlled close passes, 12 alternating scene loads finish, flags 0x300002 retained;
  final host BB4B59FD..., owner PID 5332 paused at save 495, both labs closed. Do not require a rare-event
  route replay or equate this induced defect with the original event. knowledge/notes/hang-20261114.md;
  build/hang-20261114-113358; build/ogg-shutdown-fix-20261114.

- **sound.compat timeout metadata**: MCP call_value inserts timeout_ms into method params; the strict
  read-only sound.compat rejects any nonempty object, including this transport metadata. Ordinary {}
  diagnostic queries work. Observed in the Ogg regression; no tool behavior change in this fix.

- **Audio stutter during the owner's run** (2026-11-11): preserved profiler state, p95 35.287 ms and
  maximum frame 84.196 ms, with ragdoll callback cost about 0.361 ms/frame. No audio output/underrun
  timing was captured. Concurrent lab and offline GPU testing is a possible confounder, not an
  established cause. Both labs and the test stand-in were stopped; no audio fix is claimed.

- **Launch completion with core disabled** (2026-11-11): MCP game_launch waits for core's `player`
  command even after host start.state is done. A valid native run with only ragdoll loaded therefore
  times out. Host restore successfully restored the saved mission; its optional MCP player result is
  null. Add readiness independent of core before treating this timeout as a game/start failure.

- **Source of a stale 1.0 vehicle shadow receiver** (2026-11-09): the observed crash at
  `C_game::Done+0x669` is guarded in both builds. The original cause of the invalid ForAca vtable
  remains unknown. See the journal entry for PID 15344 and the two-blast lab replays.

- **1.2 lab forced ExitProcess** (2026-11-09): a direct call to the LS3DF ExitProcess import returned
  `0xc0000409` (WER parameter 7, MafiaModLoader.asi) with and without the ragdoll loaded. No new loader dump
  was written. This is a separate failure from the solved 1.0 Jolt destructor access violation. The normal
  game-menu exit has not been shown to reproduce this lab-only observation.

- **Intermittent get-up continuity sample** (2026-11-08): one repeated 1.0 check recorded a 0.721 m
  horizontal step; the earlier and final runs passed. A timestamped follow-up trace had a largest step of
  0.063 m and a correctly fitted native trajectory. The failing run did not retain timestamps/state for that
  step, so neither sampling delay nor a real intermittent displacement has been established. Preserve a full
  trace on the next failure; do not claim all get-up discontinuities eliminated. Evidence: build/ragdoll-joints-20261108.
- **Native D3D8 screenshot with RTSS** (2026-11-08): the owner's active Mafia10Fixes process continued ticking
  and answering memory queries while MCP screenshot timed out waiting for Present. Native D3D8 capture exists
  in host/screenshot.cpp; the overlay/hook interaction is unconfirmed. Use provided images and memory for the
  current ragdoll investigation. No computer use (owner instruction); capture-tool repair is separate work.
- **RTSS focus-return hook scan (fixed 2026-11-08):** both dumps at RTSSHooks+12AB prove the scanner adds
  thunk+6 to our x86 absolute FF 25 operand as if it were RIP-relative, exactly reproducing both invalid
  read addresses. Chain thunks now use equivalent FF 24 25 abs32 (no-base/no-index SIB), retaining atomic
  pointer relinking and all CPU state. Actual RTSS recognizer deterministically faults on the old encoding
  under SEH and returns normally on the new one 20 times. Stand-in chain/mid-hook regression: 23 checks;
  30 activation-message cycles pass in 1.0, 1.2 and native Mafia10Fixes. Owner reports manual focus return
  works but stresses the previous rarity; acceptance is based on exact reproduction, not a short idle run.
- **Same-mission restore and props** (2026-11-02): a native load into the same mission need not run
  PropsMissionClose; bridge-created props remain registered/alive. Restore does not currently rewind that
  separate host-owned prop registry. Native tests must explicitly remove their exact created props rather
  than assuming restore cleans them. Two leftover off-map fade fixtures per lab were identified/removed;
  the residency fixture now cleans its prop explicitly. Broader prop/save semantics are not changed here.

- **Window resize scope** (2026-10-31): ordinary windowed mode only. A border drag commits after release;
  continuous drawing inside Windows' native modal sizing loop is not implemented. Exclusive fullscreen and
  graphics-bridge borderless mode retain their existing presentation behavior.

- **Agent instruction size** (2026-10-30): AGENTS.md exceeds Codex's default 32 KiB instruction budget; read it
  fully from disk. The early warning and docs/AGENT_SETUP.md explain the larger budget setting. The native Mafia
  connection is now verified after restart (both labs launched, input and image blocks checked, both closed), so
  client activation is resolved. The terminal helper remains text-only; use native image results for visual QA.

- **The water: what is left** (2026-10-28; updated 2026-10-30, `mods/examples/river_water`). The game's old
  bridge-shade layer has been identified as the untextured watercity.stin frames and is already hidden by
  `shade: frame:water*.stin*` (knowledge/notes/water.md); the earlier instruction to find its texture was stale.
  The moving wall/water gap is fixed (2026-10-30): the pipelined DLSS picture was a frame older than the water
  pass's mask, depth and reflection. Negative-order graphics passes now run before the upscaler; water is -120,
  composited with the current world before DLSS. Upward free-camera motion checked at 10 FPS in both builds,
  DLAA in 1.2 and Quality in 1.0, pipeline on; the owner confirmed the fix. A separate static reflection-foot
  repair samples contact texels without blending the adjacent sky; that alone had not fixed the motion problem.
  Underwater scenery is no longer transmitted: a uniform virtual depth (default 10 m) replaces incomplete bed
  depth, scene colour and subsurface geometry shadows. Both builds checked with a prop above/below the surface.
  Water shadow direction is selectable (2026-10-30): Water > Shadows from > Sun (default, visible backdrop sun)
  or Scene light (the light used for the original baked shadows). Both actual shadow directions and menu choices
  checked in 1.2 and 1.0. The new SDK shader_shadows_from request selects the shared map's direction; independent
  maps for simultaneous consumers wanting different suns are not implemented. The latest active explicit request
  wins, legacy requests do not override it; missing visible sun falls back to the scene light.
  Shadows now use LS3D's posed materials each frame, including alpha tests and complete railway models (owner
  verified both wagons). Blocker-dependent PCSS softness is available. The old 0.25 s mesh scan is diagnostic only.
  3D waves (2026-10-30): a grid-clipped, distance-tessellated surface uses the FFT's height and horizontal
  displacement, with a separate water depth buffer for crest occlusion. Both builds checked at low/grazing views
  and in wireframe; GPU geometry counters confirm actual displaced rendering. Settings: 3D waves and Wave height.
  Contact attenuation uses fresh reflection depth and is view-dependent; it cannot pin cut wall feet without that
  reflection. The reflection camera remains on the mean plane. Displaced water now supplies temporal depth and
  previous-wave/camera motion before DLSS, with matching jitter; the owner confirms reconstruction works well.
  Owner follow-up (2026-10-31): in perfectly calm/flat water, fast camera motion with DLSS makes reflections
  drift as if a static picture received the surface's strong motion vectors. Queue a separate temporal repro:
  hold wind/height at zero, compare DLSS off/on and camera translation/rotation, inspect reflected-object versus
  water-surface motion and disocclusion/history masks. Surface depth/motion alone may not describe reflected
  radiance. Investigate after the current visual/material block; do not claim the cause is confirmed yet.
  Auxiliary renders skip LS3D's audio update block; the owner confirms sound no longer stutters.
  Painted sun/cloud reflection is accepted by the owner (2026-10-31): sky-depth HDR expansion before roughness
  filtering, soft `sun_radius` support (18 degrees default), no independent analytic disc. Offscreen water now
  conservatively skips the full surface/FFT and reflection; asynchronous visibility is invalidated by camera or
  displacement-setting changes. The owner resumed the saved 19-to-5 FPS case with infinite draw distance.
  The current world-ray path has a separate 30 FPS gate at native 1440x810. The first CPU profile measured
  about 280 ms in source collection at a 384 ms frame. A controlled single-lab A/B now isolates the largest
  remaining cost to collecting about 16,000 native reflector draws every frame (133 to 139 ms). The same
  view renders at 55.9 FPS with stock water. Remove redundant static source replay through verified native
  state and content revisions, then compare the full-resolution GPU transport on equivalent hardware queries.
  Local tree work and part of instance placement now use bounded CPU workers. In the 1.0 lab, compact
  indexed decode and bounded staging also address expensive CPU reads from native buffers. Keep producer
  reads on the game thread and cap x86 staging when building the retained static source path.
  Native index revisions now prove nearly all index reads in the sampled 1.2 bank view, reducing the
  observed geometry-copy stage to about 7 ms in 1.2 and 10 ms in 1.0. Native revision lifecycle tests
  passed in both labs. In 1.2, a fresh-water diagnostic without DLAA averaged 4.41 FPS, with 63 ms
  waiting for D3D11; with DLAA the D3D9 bridge still times out in the bank view. Native group replay
  and the full-resolution GPU transport remain separate priorities.
  A reversible DXVK run keeps DLAA and water active with zero Wait11. Ordinary managed-texture
  revision stamps reduced collection from 536–561 ms to 74–79 ms in the moving 1.2 bank view.
  The latest bounded texture upload and parallel TLAS run measured 3.34 FPS and 619 ms p95.
  Geometry and texture counters isolated a 21.10 MB per-draw attribute/material update.
  Shared mesh attributes pass WARP equivalence, and exact material interning reduced a
  sampled table to 0.56 MB. One moving 1.2 DXVK/DLAA checkpoint reached 6.66 FPS at
  166 ms p95. FVF reuse passed mirror checks in both labs and the late DXVK CPU
  shadow wait is gone on the GPU-fence path. A later 16384-square shadow copy
  took 178 ms on Vulkan; the shared R32F alias now removes it on supported DXVK
  with exact samples and the native-copy fallback retained. Shared packed BLAS
  and compact attribute leases cut frozen packing and placement from about
  10 ms to 2.9 ms with pixel-exact water. A source-domain opt-in excludes
  geometry beyond the existing optical and waterline reach. The frozen
  1.2 comparison dropped 3,903 native draws with pixel-exact water. A
  moving 1.2 DXVK/DLAA run reached 8.49 FPS and 136.53 ms p95, with 109.13
  ms median game-thread CPU. Host-owned background finalization overlaps
  the native bridge on 1.2. A moving comparison reached 8.69 FPS and
  135.68 ms p95, at 102.65 ms median CPU. A volatile pose tail now keeps
  stable BLAS prefixes available for exact partial uploads; sampled
  upload medians fell from about 14 to 7 ms. Owned material/placement
  staging cut the native collection median to 47.94 ms in a later
  comparison. The same worker now also builds the current waterline
  from checked plane heights. The latest 1.2 candidate sampled 10.45 FPS
  with zero late frames and pixel-exact water in the fixed comparison.
  Unresolved mesh preparation uses an 8 MiB owned staging budget and
  the existing two mesh build workers. Both labs have this version.
  Current-revision texture slot reuse passed both labs, with unchanged
  frozen pixels. Private retired packed-archive reuse passed both labs.
  In 1.2 A/B/A/B it reduced packed geometry from 8.3–9.8 to 2.7 ms,
  with 10.12/10.52/10.28/10.58 FPS and exact frozen water pixels.
  The one retired slot is capped at 32 MiB and uses sole ownership plus
  exact stable content revisions. Eight-mesh worker grouping preserved
  pixels but its in-process interval included zero, so it was removed.
  Native static packet reuse remains a CPU target. An explicit metadata-only
  prototype preserved native hashes but deferred warm-state restoration
  still gave 10.29/10.21/10.30/10.15 FPS in A/B/A/B. Its SDK and host changes
  were removed. The 30 FPS gate is open.
  The full native-city x64 Vulkan fixture matches software and hardware
  traversal bit-for-bit across 3,499,200 transport float4 vectors at 1440x810.
  Fractional anisotropic cube sampling differs from D3D11 on Vulkan.
  D3D12 preserves that sampler exactly and gives exact software/hardware
  transport at 17.97/12.15 ms with GPU-shared native images. Rare DXC/FXC
  arithmetic differences remain. D3D11-created NT textures pass two-frame
  GPU-fenced roundtrips in every required format and full captured extent.
  The full isolated GPU relay now returns all 3,499,200 transport float4
  vectors per arm to D3D11 exactly, with the output wait queued only after
  successful submission and signal. This is not production integration.
  SDK cross-process producer/consumer helpers now pass four x86/x64 GPU
  roundtrips, immutable replacement, mutable inputs and generation restart.
  SDK DXR bounds construction is GPU-only and preserves the native fixture.
  The persistent x64 water worker and same-frame fallback are connected
  behind an unsaved opt-in diagnostic. Separate allocation/content revisions,
  persistent NT texture ownership and one deferred flush remove the initial
  live driver waits. A bounded preceding-frame completion wait avoids repeated
  software work while a hardware result is pending. Paired full-river
  1440x810 DLAA output improves 11.024 to 11.629 FPS, only 5.5 percent.
  Seed about 23 ms plus actual transport about 30 ms already exceeds the
  full-frame budget. The owner requested an optimization architecture review
  and a stop to new experiments/integration. Evaluate a shared authoritative
  water surface and conservative GPU intersection hierarchy against the
  complete residual frame budget before more micro-optimizations. See the
  journal audit. Hardware remains opt-in. The 30 FPS gate is open.
  Actual-fixture attribution now gives material/radiance-query bypass 3.754 ms
  versus its adjacent full 12.891 ms, while wave bypass gives 9.767/14.971 ms.
  These output-changing ceilings put transport material evaluation ahead of
  a wave-only rewrite. Native primary seed repair costs about 24 ms in the
  full-river 1.2 view before spatial bounds. The ordered seed now checks a
  GPU-built wave interval and rejects whole quads proven behind opaque depth.
  Covered pixels matched bitwise in the same-frame seed comparison. The
  sampled seed is about 16–17 ms full and 6 ms behind the road. Primary
  intersection still needs less work per ray to reach the full-frame budget.
  The exact tap-address optimization is in the shared sampler. An opt-in DXR
  facet path retained every result of the full-size city fixture while cutting
  transport from 18.856 to 12.665 ms with the sampler change. Check native
  frame time, cross-build stability, and grazing-edge and motion parity before
  considering the facet path for normal use.
  Native tests show 11.27–11.64 FPS on the full-river 1.2 DXVK view and
  7.48 FPS on 1.0's 9Ex path. Temporary 1.0 DXVK GPU ordering reached
  14.60 FPS at the same 1440x810 with four samples. Exact revision matching
  reuses the whole facet acceleration structure while its inputs are stable.
  With spatial bounds on the same native setup the full-river medians are
  74.31 ms in 1.2 and 123.71 ms in 1.0. Holding source behind the road raises
  1.2 from about 17 to 36 FPS, while the full view remains GPU-limited.
  The compact facet worker now keeps each original sample but schedules the active rays
  in GPU queues. Paired native frame medians are 67.15 ms in 1.2 and 118.60 ms in 1.0.
  Staging upward hit selection and opaque shading reduced 1.2 worker trace from 25.81 to
  18.41 ms. Both labs passed same-frame transport comparisons in motion. The latest
  serial release medians are 64.61 ms in 1.2 DXVK and 97.56 ms in 1.0 9Ex at native
  1440x810 with the opt-in hardware facets. Temporary 1.0 DXVK on the preceding host
  reached 74.71 ms. The 30 FPS gate remains open.
  Source inventory associates 88 percent of current 1.2 draws with city cache blocks.
  The host keeps content revisions stable across writes that match the published bytes. The
  native resource probe reduced false invalidations from hundreds to tens in both builds.
  The retained source path still needs final membership, binding and material validation.
  The C++ and Rust offline retained-cache replay agrees on ordered output for both native
  instance archives. Refreshing only dirty groups is much cheaper after the data is known.
  Language choice had little effect on that interval. The new cache is being developed
  in Rust. The same-frame native probe skipped 33 preparation calls and 73 observer
  draws for one sector block in both builds, with matching effective material and
  geometry inputs. The subsequent live cross-frame parity probe matched native
  draw order and effective inputs in both builds through moving, hidden and
  reappearing props. The integrated trial replayed over 6,600 native rows
  per warm frame in each build. The SDK consumes Rust's retained material
  templates with current owned meshes, skipping repeated Add preparation.
  Packed receipt validation costs about 1.3 ms after capture, and hot reload
  clears and rebuilds the cache. The 1.0 stream-out diagnostic confirms
  current-marker rejection and same-frame full fallback on late writes.
  The branch collector skips complete current subtrees, with exact
  ordered native parity across moving and hidden props in both builds.
  An owned mesh batch supplies up to 7,600 warm rows without retaining
  their normalized host buffers. The Rust ordered delta planner stages
  2–6 percent of accepted placement/order/material buffer bytes. Both
  remain trial paths without a repeatable whole-frame gain.
  A same-phase native comparison confirms that grouped owned allocation
  revisions reproduce every direct range check. It remains an opt-in
  proof of ownership with comparable postflight cost.
  The Rust ordered topology tape also matches current native traversal
  and draw publication, including partial repair after a local topology
  change. Its in-place repair removes some repeated native traversal,
  but the current proof offsets most of that saving. The tape remains
  isolated while the larger source and GPU costs are addressed.
  The x86 Rust planner reproduces current ordered mesh and material
  IDs and primitive ranges. Live same-frame audits match the prior
  publication in both builds, with lower sampled worker staging time.
  A temporary 1.0 DXVK run removes the 9Ex CPU handoff wait and reaches
  60–66 ms at native size. Current source and worker join are still
  expensive. The planner and bridge comparison remain isolated.
  The opaque waterline cache matches the full current model in both
  builds, but its frame timing does not improve reliably. Native source
  attribution points to per-marker proof and consumer replay plus the
  remaining ordinary native preparation as the larger CPU targets.
  Live owned GPU pages and consistent full-frame savings remain open.
  Rust's spatial DXR probe preserves all four-sample output bytes while
  reusing 837 previous bottom-level GPU addresses in an archived transition.
  Serial moving-view tests in both labs do not establish a full-frame gain
  because the existing flat structure already avoids most builds there.
  The probe stays isolated. Primary wave intersections, current shadow
  work and native collection remain the larger timing targets.
  Native 1.0 rigid source preparation now batches the pool publication boundary.
  The matched probe reduced both buffer-tree scans from about 13–14 to 4 ms
  per collection. Both labs passed source and device state checks.
  A retained native texture bank would move the original trilinear taps to
  hardware. The fixture saved about 1.9 ms with one-byte final differences
  at 123 water pixels. See the frame architecture review in WATER_REFLECTION_PLAN.md.
  The matched native frame has about 19.10 ms outside water, so a 20 ms
  water-only result would still exceed the full GPU budget.
  Source replay, the synchronous same-frame shadow receiver readback,
  late caster submission and GPU transport remain above the 30 FPS budget.
  The water shadow replay preserves full native transport with only 9.57%
  of the original map's tiles. Live conservative receiver tiles and
  native stencil preserve the sampled water view, but do not improve
  frame time while caster traversal/submission stays complete.
  The x64 Vulkan capability and shared-texture probes preserved complete
  RGBA32F seed and result texels and directly accessed both R32F shadow
  extents. A standalone x86/x64 GPU roundtrip preserves all four slices,
  and the full transport shader compiles with ray queries. The original
  native material fixture passes on Vulkan, including mip-aware alpha,
  compact attributes and coplanar order. Next gates are complete frame
  publication and transport, native motion parity and end-to-end frame
  time with all four samples.
  Ordered water seed raster already passed native visual comparison in 1.2.
  See WATER_REFLECTION_PLAN.md for the quality gate and JOURNAL.md for measured progress.
  Finite receiver triangles and tighter light-camera box steps reduce unrelated caster draws without reducing
  range or quality; both builds passed integrated tests. The initial mid-frame attempt exposed missing native
  IGraph render-property and viewport restoration; both are now preserved, validated against both binaries.
  Long-distance receiver-only consumers fit the map from SAME-frame opaque depth, then intersect the actual
  finite water geometry. Near-sector visuals, world shader rules and view-pass consumers retain early shadows.
  Start-view same-run A/B: 109.65 to 56.39 ms, -48.6% +/-0.146 ms; captured frames pixel-identical. Disabled DLSS
  also releases motion replay now. Main city and moving-camera reflection costs remain; batching was evaluated
  (56.15 to 27.35 ms) but left off because its silhouette differences violate this task's visual requirement.
  Details/evidence and the earlier failed experiment are preserved in WATER_RENDER_PLAN.
  Shadow hide rules now cover the light camera too, excluding baked watercity.stin shade from casters. A shared
  tracked device mirror and shader-read constant masks remove redundant per-caster device queries. Receiver-fit
  coverage replaces water's fixed 120 m shadow boundary while retaining density (up to the device's texture cap).
  The measured 35--40 FPS camera and ongoing stage timings are in docs/WATER_RENDER_PLAN.md. Engine visibility,
  reflection and required device ownership waits remain costs to investigate; this is not an ideal-optimum claim.
  The reflection costs a whole second drawing of the scene (7.1 ms in 1.2 at the embankment, the GPU's mostly):
  narrowing its culling to the water's part of the screen, or a smaller picture, not tried. Water at another level
  (the fountain at 2.4 m) reflects the sky (one reflection view: the plane the screen shows most of). The night's
  water, rain on it, wakes of boats and splashes are not done (the owner: no physics; a visual first).
  Drawn by a graphics pass over the finished frame, the water covers what the game draws translucent over it (car
  glass, particles, smoke) and writes no depth for them: the right place is the game's own drawing of its water,
  after the opaque world and before the translucent (a Direct3D 9 shader rule with the reflection, the ripples and
  the scene so far as its textures: no Direct3D 11 at all, no bridge's wait). At grazing views the ripples' normals
  lift the reflected rays so much that a wall's reflection gives way to the body's colour near the waterline: to
  be looked at (masking of facets turned away, the slopes' filtering along the view).

- **menu.value on a settings selector** (2026-10-30): setting Water's shadow selector to 1 returned 0 and did not
  change it in either build. start.cpp SafeSetValue calls the generic game menu setter, whereas the mod page's
  MenuPageValue dispatches by control kind. The real menu interaction works (1.2 cycles on click; 1.0 opens a
  dropdown, then the option is clicked). The generic bridge setter needs investigation and a separate authorized
  tool fix; it was not changed as part of water rendering.

- **The motion vectors cost a third of a far frame** (2026-10-26, item 31a; seen in the 1.2 lab with dlss_nr on, the
  whole city from 120 m up: about 35 000 draws a frame, 91 ms). `profile`: host/motion.cpp's MotionAfterDraw and
  DrawAgain about 17 % of the game thread, its per-draw hash lookups (the visuals' `Key` and the skins' `SkinKey`
  maps, `_Find_last`) 11 % self. Fine at the freeride's start (500 draws), not at a far view: a lookup a draw kept
  flat (an index stored on the visual, or one table probed once per visual) is the fix; measured on a clean lab
  first (dlss_nr and fusion_fix off).
- **The reference shaders' picture: what is left** (2026-10-26, item 31a; both builds alike). In the far scene 0.87 %
  of the pixels differ by more than 8 of 255 from the game's: thin edges (the vertices transformed in the shader
  against the fixed pipeline's, the depth at near 0.05 and far 5000) and the bridge's shadow on the water (the owner
  told what it is), which the reference draws otherwise: a shadow's draw state not yet read (shader.survey).
- **The whole city's far frame: what is not looked at yet** (2026-10-26). Whatever the game hides by other rules
  than the city's blocks, the occluders and the LODs' distances (a frame's own hide distance, the portals of
  sectors other than the street's) was not searched for: no far object was seen missing (the owner compared the
  frame, 1.2 and 1.0), and LOD quality 100 adds only finer LODs (892 draws more, no new object in the picture's
  difference). The depth buffer's precision at near 0.05 and far 5000 was not looked at (no fighting seen).
- **The game's own bugs: what host/fixes.cpp leaves** (2026-10-25, knowledge/notes/game.md; always on). The options
  are written only when the game quits by its menu's Quit (not at Alt+F4 or a closed window: the game's own way).
  1.0 writes HKEY_LOCAL_MACHINE where it reads it: in the 1.0 lab the write is refused (RegCreateKeyExA 5, no
  registry virtualization for that process) and nothing is kept; a player's 1.0 keeps its options through the
  VirtualStore (the owner's) or as an administrator. The loader's own write guard (the run's `resolution`,
  `fullscreen`, `adapter` given their registry's values back in that write, host/start.cpp) is seen in 1.2 only, 1.0
  never writing there. A Sniper Mode key that does conflict with another is still reset at the load (the game's
  intent). The key names were seen in a Russian layout and a joystick in the system's Russian; another language's
  font (a Czech or Russian edition) keeps DirectInput's names whenever they are in its code page. Not yet seen on
  the tester's machine. The empty traffic slot (C_traffic_car::AI, a policeman asking for a car): the stub is in both
  labs' memory, the crash's condition was not made again.
- **Stale index owners at exit: guarded in 1.2; lifetime origin unassigned** (seen in the tester's crashes of 2026-10-25: four of five at the
  menu's Quit, LS3DF+0x5AC2E in C_IBManager::m_FreeAllIBuffers from IGraph::Close; knowledge/notes/game.md "The exit
  crash"): the game is closing anyway, but each leaves a dump of hundreds of MB that a player then sends.
  The 2026-11-03 tester dump, inspected 2026-11-06, confirms a single stale index-slice owner slot in reserved
  memory at shutdown. Custom river_water was absent. On November 6, host/fixes.cpp added a final-driver-cleanup-only
  guard: validate all three owner words and writable memory before clearing, while retaining native buffer cleanup.
  Two native 1.2 tests deliberately decommitted an allocated slice owner's page; both hit the guard without a dump,
  and the second's process handle confirmed exit code 0. Unit tests and both builds' address checks pass.
  The configured tester copy exited cleanly without this patch after city -> menu -> Quit, so the original trigger
  remains intermittent. Creation/free attribution is still required to fix the earlier lifetime violation.
  Candidate is installed in lab 1.2 only; the tester copy and 1.0 are unchanged (1.0 lacks these owner stores).
- **AI detour with node index -1 fixed in both labs** (tester crashlogs.zip, October 29):
  gameplay crash at Game+1007B2 in ai_path::GetSpecNodeC during mise18-mesto. The captured one-waypoint route
  has node index FFFFFFFF; native code reads table base 569CF00C minus 30 bytes, hitting PAGE_NOACCESS.
  Both builds now send that one-waypoint case through the existing plane-based detour arm. Native tests
  generated a detour around a parked car with index -1 and exercised the original solver with a valid index.
  The tests reproduce the route state from the dump in freeride. The original mission sequence remains
  a separate reproduction case. See knowledge/notes/game.md.
- **ASI mods that write Game.exe without VirtualProtect: only the pages they write are opened** (2026-10-26,
  host/asi_compat.cpp): libMafiaCountrysideMap 2.0's countryside map works in 1.2 (seen in the lab: Tab in the
  countryside freeride). The page stays writable for the run (such a plugin writes again later); a plugin that
  writes the game's data while the game reads it on another thread is not made safe by this. Not seen with another
  such plugin, nor in 1.0 with a real one (this one supports 1.2 alone: 1.0 is the offline test's).
- **`vb.set {rename: false}` under `render.set {batch: true}`: 0.57 % of the far frame's pixels differ** (2026-10-27,
  1.2, host/vbrename.cpp): at the silhouettes (worst 213 of 255), stable in each mode. The batched picture with the
  renaming is batch.cpp's reference exactly (0.271 / 0.228 / 0.187 of 255 against LS3D's, 1.02 %), and `vb.check`
  finds every buffer's copy drawn equal to what the game wrote in both modes: the A mode alone (a measure's
  baseline) draws otherwise there. Not found why; without batches both modes' pictures are the game's.
- **A presaved 4096x4096 map drew invisible on 9Ex** (2026-10-26, knowledge/notes/game.md): the packs'
  `fullmap_1.565` as the Tab map's texture was drawn and showed nothing in the 1.2 lab (9Ex, our dxwrapper's
  managed textures in video memory), while the same file showed in the tester's copy on DXVK and the .bmp showed
  in the lab. Suspect: MmlManaged (third_party/dxwrapper/patch.py) with how LS3D fills a presaved texture. Not
  isolated: an A/B in the lab with the presave (MmlManaged on and off), then the texture's levels read back.
- **Gamepad hotplug** (host/joyplug.cpp). A physical Keychron controller arrived after native enumeration and
  before notification registration in Mafia10Fixes on 2026-11-10. The startup check and its retry cover that gap.
  Both labs reopened real attached devices after a synthetic drop with zero notifications. A physical reconnect
  after registration and the fallback poll thread (no CM_Register_Notification: Windows 7, Wine) still need
  direct verification. The running mission's force feedback effects stay on
  the old IJoys until the next mission's load. `input.actions` shows a joystick's button names (IJoy's, in the
  system's code page) as the game's language's (a Russian system's "Button 0" came out as "Êíîïêà 0"): converted with the wrong
  code page.
- **The graphics bridge on Direct3D 9Ex waits for both devices each frame** (2026-10-24): 9Ex has no fence and no
  keyed mutex, so the game's thread waits for Direct3D 9's work before the passes and for Direct3D 11's after them
  (event queries): the CPU and the GPU take turns, which a GPU-heavy frame pays for (a player's RTX 5060 laptop at
  2560x1600 held near 30 fps in every DLSS mode). Under DXVK the two devices are ordered on the GPU (shared fences:
  the dlss_nr pack ships DXVK's d3d9.dll for it). For 9Ex itself the way would be D3D9On12 (Direct3D 12 fences under
  Direct3D 9: IDirect3DDevice9On12's Unwrap/ReturnUnderlyingResource), through dxwrapper: not tried.
- **DXVK's hand-over still costs the game's thread** (2026-10-24): each hand-over flushes DXVK's command stream and
  waits for its CS thread and submission thread to have submitted everything (FlushRenderingCommands,
  LockSubmissionQueue: the interop's only way to put a submission of ours in its order), about 0.7 ms a frame at
  1280x960 on the RTX 3070 (`gfx.state` timing.hand_ms), two a frame (and two more with frame generation). Not the
  GPU's time: that is ordered by the fences.
- **The world drawn smaller: pretransformed draws** (2026-10-24, gfx_upscale): a draw with screen-space vertices
  (XYZRHW) between the world's start and stage 1 is not scaled with the viewport. None was seen in the freeride's
  picture in either lab (the lens flares and the 2D come after stage 1), but the world's draws were not surveyed by
  their vertex format: a mission's effect drawn so would sit at the frame's size in the render size's corner.
- **Fusion Fix's remaining omissions** (2026-10-25, mods/fusion_fix). Not ported: WriteSettingsToFile (the
  registry's settings in savegame\settings.bin: with an empty file the game started at 800x600, the tester's case),
  ConsoleGamma and SMAA (shaders on d3d8to9's device at LS3D's render's end: for the loader, graphics passes to be).
  Seen in both labs at 1920x1080 and 2560x1080 (`game_launch` resolution): the HUD and radar round and in the
  corners, the FOV Hor+ (and the game's own Vert- back when the mod unloads), the pause menu, the radar's map (the
  game's own map's name and transform: a countryside map plugin's whole map on the radar too, 2026-10-26), the
  free camera, the settings applied live, a hot reload. Not seen: a cutscene's borders, the credits' missing line,
  every menu at a wide screen (the main menu's panels, the garage, the race's), the eyes at a mission's load, the
  scratches, a pad's dead zones, the borderless window (whether the labs ran in a window was not looked at), the HUD's texts at 1.1's layout
  (no 1.1 here). The pack's layout: back to Ultimate ASI Loader as dinput8.dll and dxwrapper (D3d9to9Ex 0) over DXVK
  (2026-10-25: as d3d8.dll Ultimate ASI Loader loaded the tester's ASI mods into Setup.exe, which they stopped). Seen
  in a copy of his folder with his mods: Setup.exe's dialog, the game, the brightness drawn in a window; fusion_fix's
  part `ijoy` (a pad's dead zones) not found there (one of his mods patches IJoy.dll first); his
  libMafiaCountrysideMap2.0.asi faulted once at the start (caught, the game went on; since 2026-10-26 its writes
  are let through: host/asi_compat.cpp). Not seen there: frame
  generation's caps, the memory with his textures. The trees' leaves look white in that copy whatever the layout
  (without his ASI mods, his maps\, DXVK, on 9Ex alike; green in the 1.2 lab): his game's own files, not looked into.
- **Frame generation on a full-screen device** (2026-10-25, the RTX 5060 friend's log of 2026-10-24: 800x600 full
  screen, "the showing on the game's thread: a full-screen device"; he saw 20 fps and a long input lag with frame
  generation): a full-screen device gets no showing thread (host/gfx.cpp: one swap chain alone shows there), so the
  generated pictures are shown on the game's thread, the waits between them on it too, and the interval they are
  spaced over is measured from one Present to the next, the waits in it: the game's rate falls to its own over
  (pictures + 1). Measured 2026-10-25 with that way forced in a window (`gfx.state {"show_thread": false}`, 1.2 lab,
  RTX 3070, the stand-in): 2x the game 93 fps on the thread, 53 on the game's thread (shown 186 / 106); 4x 46 / 21.5
  (shown 185 / 86): the tester's 20 fps. Done the same day: Setup's full screen is made a borderless window while a
  mod has "gfx": true (host/fullscreen.cpp); seen on a third monitor in both labs (9Ex) and in the pack's layout on
  DXVK. Not seen: the Reset path (LS3D resets with its full screen's parameters after the options' resolution: made a
  window again there, untested), the game's exclusive full screen without a gfx mod since `game_launch`'s
  `fullscreen` (a monitor and the focus taken). DXVK numbers its adapters otherwise than Direct3D 9 (`adapter` 0, 1
  and 2 all gave the primary monitor there: the borderless window always lands on it under DXVK).
- **Frame generation on Direct3D 9Ex costs the game's thread its waits** (2026-10-25): the showing never holds the
  game (its own thread, its own Direct3D 11 swap chain), but on 9Ex the frame is handed to Direct3D 11 by waiting on
  the CPU for Direct3D 9 to finish it and for Direct3D 11 to have read it (no fences: the 9Ex gap above), 1.9 + 1.0
  ms a frame measured at 1280x960 (the game capped at 60: 57.2 fps with 4x against 59.8 without; at 90: 75.6 against
  88.7 at 1920x1080). On DXVK none (89.2 against 88.7). The way out is the same as the 9Ex gap's.
- **DLSS on an RTX 50 with the upscaler** (2026-10-24): the render-size world, the mip bias and the GPU-ordered DXVK
  path are seen on the RTX 3070 (SR alone, frame generation's stand-in); DLSS-NR at the render size, DLSS-FG itself
  and the frame rate on the friend's RTX 5060 laptop wait for his next logs (the log's minute line now says whether
  the CPU or the GPU holds the frame).
- **The game faults at its exit in LS3DF+0x5AC2E** (from +0x5BA73, the index buffers' release in drvClose; 2026-10-23
  and 2026-10-24, the RTX 5060 friend's DXVK game, after the session): not reproduced in the labs, not looked into.
- **MCP tools take an unknown parameter without a word** (2026-10-23): `mem_write {"type": "f32", "values": [2, 2,
  2]}` wrote three u32 2s (the parameter is `as`): a taxi's scale became denormals and it vanished. The tools should
  refuse a parameter they do not know.
- **High x86 dump memory-map addresses are sign-extended** (2026-11-06, mcp/src/crash.rs). The tester's
  92CA3CF4 owner slot is in a MEM_RESERVE region with base FFFFFFFF92C6D000. `crash_inspect` looks up the
  unsigned 32-bit address and reports it missing. Normalize x86 MemoryInfoList bases before lookup. The
  triage above used a separate read-only dump parser to verify the actual region. Tool code remains unchanged.
- **A far actor is filmed in an empty world** (2026-10-23): `camera.set {follow}` on a tram 580 m from the player
  showed a ground plane and a few props: the city's sectors are drawn around the player. Not looked into.

- **The 64-bit half outlives a crashed game** (2026-10-22): after the game died mml-sidecar64 kept running and kept
  the dead Game.exe's process object alive, so the next start of that folder quit at once (seen three times). The
  sidecar must end when its game ends (wait on the game's process handle). And a crash's dump fails when the address
  space is spent (0 bytes, 0x8007001F: no memory left in the process to write it): a dump written from outside (by
  the sidecar) would not.
- **A game on DXVK hangs in its exit after a crash** (2026-10-22): the game's own exception filter (after ours) ends
  the process with the CRT's exit; ExitProcess kills the other threads, and LS3D's release of its device then waits in
  DXVK's d3d9.dll (SleepConditionVariableSRW) for a thread already gone: the process stays with one thread forever
  (the player's "freezes"; ended here by terminating that thread). To do: a crash, once reported, ends by
  TerminateProcess (no DLL's detach run), as Windows' own error reporting does.


- **The game's videos** (2026-10-21): (1) a real `BIKk` file is not seen played in a lab (none at hand: RAD's
  compressor shows its licence at its first start, the owner's to accept; FFmpeg's decoder reads `BIKk` by its
  source, the public samples are BIKb, BIKf and BIKi). (2) The sound's start is not corrected for DirectSound's
  output latency (some tens of ms). (3) A device lost during a video (plain Direct3D 9's exclusive full screen left)
  is not handled: the frames are not drawn until it comes back. (4) Plain Direct3D 8 (no dxwrapper) leaves the videos
  to the game's own players. (5) The options' volume is not applied (the game's Bink path does not either). (6) HDR
  (PQ or HLG) streams are not tone-mapped: the GPU's video processor gets their matrix and range, not their transfer,
  so they show flat; none was tested. (7) The DXVA2 path and the copy path are seen only on a GeForce (RTX 3070) and
  under DXVK's d3d9.dll; another vendor's driver may refuse a shared texture or a P010 surface (the CPU's path then,
  logged).
- **Measuring** (item 30, 2026-10-20): (1) *(fixed 2026-10-20: the journal)* the loader's crash filter was set
  again every frame. (2) A return address after a call through a vtable is
  kept unchecked: a stale one there still counts in a function's total. (3) The GPU's span includes its waits for
  the CPU's commands; a frame's GPU work alone is not measured. (4) The machine's other load moves frames by up to
  0.3 ms between segments: `bench`'s interval holds it, but a quiet machine narrows it tenfold; the power plan and
  the CPU's clock are not read. (5) `bounds.csv`, `bounds_10.csv` are the Ghidra projects': `ghidra_cli.py bounds`
  again after functions are made there. (6) The sampler's least interval on a timer is 1 ms (Windows' high
  resolution timer); shorter ones keep a core busy.
- **People skinned on the GPU** (item 28b, 2026-10-20): (1) it does not speed up the plain game: each person's draw
  switches the device from the fixed pipeline to shaders and back (measured with 48 people, about 300 draws: +0.9 ms
  a frame in 1.2, +1.3 ms in 1.0, the CPU's own part down to 0.2 ms): item 31 (every draw through shaders that stay
  bound, the state followed, not asked; 31d). (2) A LOD with an environment map of modes 2 to 5 or lit flat stays LS3D's
  (`why_cpu`). (3) `geom.capture` of a person the GPU skins reads the drawn buffer, which is no longer filled (the
  rest pose instead). (4) The lights' places in the mesh's space are taken when LS3D would light again (bones or the
  sector's lights changed), as LS3D's own colours are.
- **A rule's vertex data on lightmapped objects** (item 28c, 2026-10-20): some I3D_lit_object draws come from a
  vertex set of their own, split for the lightmap (in the city: no LOD record, BaseVertexIndex 387 against the mesh's
  1992, 134 vertices against 133): their order is the lit object's, not yet read, so their second stream stays zeros
  (`why_left_out`, 13 of some 460 draws in the city's view). A LOD of the same count of vertices is taken as the same.
- **The shader rules' constants** (item 27, measured 2026-10-20): each draw a rule takes asks the device for about 60
  states (the transforms, the fog, the stages, the eight Direct3D lights, the material); over every object (about 750
  draws, reference.hlsl) that is some 4 ms a frame in 1.0. The states that change once a frame (the view, the
  projection, the fog) or once a visual could be kept; a person's draw without a rule already reads 25 (CoreConstants).
  Item 31b.
- **The demo's LS3D names** (2026-10-20, `tools/kb_demo.py`, knowledge/notes/external.md): 1255 of the demo's 3109
  functions have no place in LS3DF (qhull, DirectShow, the editor's code, tiny ones folded away); a function 1.2
  rewrote much (I3D_singlemesh::PoseLod, 47 % like 1.0's) pairs only through a hand-checked name of both builds
  (`names.csv` and `names_10.csv` alike), so 1.2 lacks a few. The demo's prototypes go into 1.2's project only where
  MafiaReverse gave none, and only into functions whose prototype is not set already (`sync` keeps one set before).
  The repository carries none of the demo's names (its licence): a machine without the demo has the twins only.
- **dlss_nr** (2026-10-19): DLSS 5 NR's own picture has been seen once, on a player's RTX 5060, far too dark: the
  mod gave the model linear values where it takes gamma ones (fixed, as RTX Remix does; not yet seen again).
  NVIDIA's `nvngx_dlssnr.dll` refuses every GPU but an RTX 50 (Blackwell), and the labs have an RTX 3070. Everything up to NGX's answer is seen in both labs (the
  Vulkan device on the game's adapter, NGX started, its verdict, the round trip of the frame, the inputs as debug
  views); the feature's creation and evaluation, their image layouts and the picture's look wait for an RTX 50 (the
  owner's friend's 5060). The motion vectors are the bridge's (2026-10-20: the camera's and what moves by itself,
  people as wholes; see "The motion vectors" below); pipelined a frame behind since 2026-10-23. Stage `other_gpu` (the game on a CPU's own graphics beside the GeForce, as a player's
  RTX 5060 machine ran it) is not seen: the labs' machine has NVIDIA GPUs only.
- **A thread without TLS in a player's game** (2026-10-19): the Mafia 1.2 of a player's mod pack (Freeride Deluxe,
  the 4 GB patch, a Czech retail release; Windows 11 24H2, the game on "AMD Radeon(TM) Graphics") had a thread made
  with SkipThreadAttach that died in dxwrapper's hooks; the same package ran on the player's clean game. Fixed where
  it can be (dxwrapper built with `third_party/dxwrapper/patch.py`, the crash filter and the taps without TLS; both
  tested offline by such a thread, the release dxwrapper dies there as it did for the player), but not yet run on
  that player's mod pack, and the thread's maker is not known (no dump: the old crash filter died too). dxwrapper's
  upstream (elishacloud/dxwrapper) has the same code on master: an issue or a pull request for it waits for the
  owner's go-ahead. The same player's clean Steam game ran the fixed package (its logs: no crash).
- **The graphics bridge on DXVK** (2026-10-19): made and seen in both labs with DXVK 3.1.1 (`tools/deps.py dxvk`): the
  frame, the views and their passes, the 2D pass, the sun's shadow map. Not seen: ReShade's Vulkan layer on top (the
  player's setup; it is installed system-wide, not tried in the labs), a DXVK older than 3.x (its interop interfaces
  are from 2.1; `gfx.state` says what is missing), DXVK's own `d3d8.dll` instead of dxwrapper's (the bridge needs a
  9Ex device underneath: off, and it says so).
- **The motion vectors** (host/motion.cpp, 2026-10-20): a wheel's spokes spinning show at their edges in the check
  view; billboards move as the camera; translucent draws keep what is behind them (a car's shadow decal mismatches at
  its edge either way); where a person's arm passes close over his body a few pixels hatch (two surfaces at nearly
  one depth: the draw again's LESSEQUAL picks either). People's limbs have their own motion since the same day
  (smCopySkinned's copies; seen in both labs, a walking copy of Tommy, `dlss_nr` debug 1 and 5, and under DXVK in
  1.0 with the city freeride's crowd at 30 to 40 m: 21 LODs known, 80 draws again, 0.011 ms of copies). Seen in both labs, 9Ex and DXVK (the freeride's traffic,
  the player's car driven, `dlss_nr` debug 1, 5, 6).
- **The developer tools' block** (2026-10-19): seen in both labs, but not yet the distances `triggers` draws (a
  mission waiting on a GETACTORSDIST) nor the `names` and `web` views looked at closely in 1.0.
- **Wireframe** (2026-10-18, from PR 2): not made. The proper way is LS3D's own driver state (Insanity3D's
  RS_WIREFRAME = 7: the driver sets D3DRS_FILLMODE and keeps a flag); its SetState in LS3DF (the driver's vtable
  +0x14) is not found yet. Setting the fill mode from a mod at RENDER_BEGIN would also reach the HUD (the loader's
  state block at RENDER_WORLD puts the wireframe back for the 2D pass); PR 2's other plugin set it on each draw call.
- **DEBUG_TEXT** (2026-10-18, from PR 2): the retail game knows the keyword but runs nothing for it (PR 2's claim, not
  checked here); a mission author's debug lines would be an event and a line of the log. Its compile (DecodeInstruction
  1.2 0x463960, 1.0 0x5B1B60; ParseArgs 1.2 0x461C70, its 1.0 twin not paired yet) and the dispatch (1.2 0x46D3FE,
  1.0 0x5BB31E) would take three hooks in the script machine.

- **The sidecar** (2026-10-18): a channel has no way to wait for a new version (a 64-bit half reads it when `FRAME`
  wakes it, as `side_picture` and `physx_crates` do, or polls); PhysX's measure did not need one (its poses were whole
  1 ms before the frame read them), a heavier step might. A channel is gone when both halves have closed it, so its
  versions start again after a reload of both. A shared texture has no lock or fence between the processes: the halves
  keep to the rule of two textures written in turn (docs/MODDING.md). Textures can be made only once the graphics
  bridge runs (a mod with `"gfx": true` on at the start); the formats are the ones measured on one GPU (an RTX 3070).
- **PhysX 5 CPU crate example** (2026-10-18 baseline): `PhysX5` remains the CPU target; the later
  `physx_gpu` example already builds/uses GPU PhysX via pinned CUDA components (item19a). Vehicle and
  CharacterKinematic libraries are in the source but not built (a line of `CMakeLists.txt` each). `physx_crates` is one
  way: the crates do not push the game's people and cars back (they are kinematic to PhysX), and the people and cars
  are the game's collision shapes, not their models (item 19b). A crate flung farther than the widest region of
  statics (64 m) or through the world leaves the pile.
- **NVIDIA Flow in `flow_smoke`** (2026-10-18):
  - The first start on a machine waits for NVIDIA's driver to compile Flow's several hundred compute shaders (5 minutes
    the first time in the 1.2 lab, 100 s in the 1.0 lab; again whenever the sidecar is rebuilt: 110 s in the 1.0 lab
    on 2026-10-19). Since 2026-10-19 Flow keeps a pipeline cache of its own in the mod's data folder
    (`third_party/flow/patch.py`, `vk_pipelines.bin`, 8.4 MB): the sidecar rebuilt again, Flow started in 0.49 s from
    it. What is left: the first start in each game folder (each has its own data folder) and after a new driver.
  - The gas at half size (2026-10-19, the default) is brought up by four taps weighted by the depth: a thin thing
    in front of the gas narrower than two pixels (a wire, a lamp post far away) takes the gas's colour of what is
    behind it at its edge. Measured on one scene only (the freeride's start, a fire and a smoke); the ray march's
    cost did not fall to a quarter (0.56 to 0.25..0.33 ms: Flow's per-block work does not scale with the pixels).
  - This Flow consumer uses bone capsules rather than the native posed-skin source interface; clothes' shapes
    are not part of its current collision representation. Host scene-source support does not change it automatically.
  - The meshes' collision (`flow_smoke.collision {"mode": "meshes"}`) is off by default: the owner found it still
    behaving strangely after the bodies were made solid behind their faces; what is wrong was not found yet.
- **An actor made over the open river sleeps** (seen 2026-10-18, 1.2): a car spawned there is drawn but its model
  frame's stamp (`+0x13C`, what C_game::Tick wakes a sleeping actor by) stays -1, so it floats where it was made
  instead of falling in; a person put in the air there stands (a standing NPC does not fall). A ragdoll or a water
  mod moves such a body itself. Why the stamp is not set was not looked for.
- **Orders for people** (host/orders.cpp): a person with a script of his own only (a C_entity: spawn's, the
  missions' NPCs); the traffic's pedestrians are another class, not ordered. A mission's NPC whose own script has
  ended is refused (the game ticks him no more: act state 2, out of C_game's lists; putting him back would make him
  one of the temporary actors, which the game deletes as its own). An order replaces his program: a mission NPC's own
  script is gone for the rest of the mission (it is not saved and put back). A drive's outcome is measured by the
  place (the car within `radius` of the place or of the road's end), not read from the game's chase record (it is
  gone when the command ends); `enemy_car_moveto`'s `natvrdo` and a drive by someone not at the wheel were not
  tried, nor `enemy_car_hunt`; neither are the scripts' events to a program of the loader's (`setplayerfireevent`...)
  nor a list of the people's models (`spawn` takes a model file's name). `attack` is the game's own aim: in the labs
  the first hit came 5 to 30 s after the first shot. `human.drive` is the game's chase drive (C_sledovacka, the
  police's and Sergio's), not the traffic's: started with the car facing away from its way's first leg (a parking
  lot, a car turned round), it turns on full lock across the pavement and cuts back to the way through what is
  there (2026-10-18, 1.0: about 20 crashes, Tommy beside him from 200 to 56, pedestrians run down); started on the
  road facing its first leg, the same 800 m drive gave three light knocks on posts and no one hurt
  (`knowledge/notes/game.md`, "Drives"). Since then the car is backed round first and the game's own unsticking kept
  short (`start`, `calm`); what is left is the game's: the driver cuts corners (it changes its target within 20..40 m
  of a node), stops about 4 m short of a `stop` and stands there with a collision foreseen until the unsticking moves
  it on (ORDER_ENDED about 5 s later), and a car truly stuck (one spawned into a wall) is unstuck again and again
  without end (9 times in 2 minutes: `unstuck`; `retries` do not help it). The knocks on the posts of one street
  (1.0's whole city, x -1774, z 130..220, surface 19) were looked into briefly only: in the small city there is
  nothing on that curb line (rays along x -1773..-1777, no actors within 50 m); the whole city was not looked at.
  `camera.game` follows a car only: a person on foot (an NPC walking) has no camera of the game's to put on him.
  The same drive in both labs (2026-10-18, a taxi, 3.2 km across the city, `speed` 16.7) once went 18 to 20 m/s
  for 12 s through the tunnel east of Little Italy in 1.2 only, over its road's 13.3 and over `speed`, when the
  two games' worlds were in other phases (each its own freeride start); only the chase's way round an obstacle
  (TickDriver, `+0x208` 1, its limit 1000) drives faster than the road, and the watcher did not record the
  driver's fields then. Started from one save in both labs it went at most 13.9 m/s in both and was not seen
  again. Two drives from one save still part by up to 34 m: the driver's speed factor (`+0x1C8`, 0.65..1.0) is
  drawn anew every 5 s from the game's own random numbers, another in each process.

- **A game folder no ANSI code page spells** (a Cyrillic name on a 1252 system, a CJK one on a 1251 system): the
  game, the loader and the mods' own files work there (both labs, 2026-10-17), but **dxwrapper does not**: its
  `d3d8.dll` stub looks for `dxwrapper.dll` by an ANSI path, shows "Could not find DxWrapper.dll functions will be
  disabled!" and the game draws on plain Direct3D 8, where the loader's Direct3D 9 services (the graphics passes,
  the shaders on the game's drawing, the scene views) are not offered. Not the loader's code; a player whose folder
  is named in the system's own code page (Cyrillic on a Russian Windows) is not affected. Paths longer than
  MAX_PATH are not handled anywhere (nor by the game).
- **The frame's drawn items** (host/draw.cpp): 4096 for every mod together, first come first served: a mod that
  draws much can leave none to the mods after it (now told: the log, `draw.state` `dropped_last_frame`); no share
  per mod.
- **Trigger view** (mods/examples/debug_views, triggers): the scene is walked for areas every 3 s (3.6 ms that frame in Old
  Prison); people with a script (C_entity) are read only for the areas they test, not drawn; a detector's radii are
  every DETECTOR_INRANGE of its program, not the one it waits in; a use point in a frame's space was not met (its
  matrix is applied as the game applies it, not seen).
- **Restore points** (host/saves.cpp, `restore`): a point is the game's own save, so it holds what the game saves:
  what it does not (a sound playing, a particle in the air, the camera's smoothing, a mod's state it did not put at
  GAME_SAVING) is not put back; mods are told GAME_LOADED with slot 599. Made in a mission in play only (not in the
  main menu or the pause menu); a load waits for the pause menu to close. Saving in the middle of a cutscene, a car
  chase's scripted part or a race was not tried; neither were the extreme freeride and the racing. A point is of its
  build (1.2's will not load in 1.0) and is loaded into the profile selected. 1.0's slot 599 is known free only
  because its missions are 1.2's (its texts do not name the freerides' saves): a save found there stops the points.
- **Mafia10Fixes garage incidents** (2026-11-09 and 2026-11-10). The prior guard covers one vehicle-shadow
  receiver during save cleanup. A later live hang stayed in heap validation while closing an enemy shadow
  in the mission tick. The next day's dump records a separate failure in the loader's per-mod profiling
  after a garage blast. A later load crashed in `C_actor::ResetPlacement` under `C_game::Done`.
  Another blast was followed by a software shadow rasterizer crash. Comparison with intact geometry
  identified a rotation and flag write into the shadow's index array. The SDK pose writer now checks the
  frame's layout before writing. In three live garage replays the check rejected a non-frame pose target
  after the second blast and the game kept running. An owner replay then logged that rejection and
  crashed about 1.5 seconds later while rasterizing another shadow with an out-of-range vertex index.
  Both game builds now reject the affected shadow before rasterization and log the first rejection.
  The writer behind the later corruption and its relationship to the other crash signatures remain open.
- **Vehicle orientation through the bridge** (2026-11-11). `vehicle.body` documents `forward` and
  `up`, but its JSON handler consumes only `pos`, `velocity` and `turn`. The SDK body API has axes.
  A rollover fixture must verify the resulting native orientation. The bridge gap remains open.
- **Crash triage** (host/stack.cpp, host/crash.cpp, `crash_triage`): the stack is the sampler's walk and check
  (2026-10-21), not an unwind. A call through an object or a register loaded otherwise than from a fixed place (a
  vtable's) cannot be checked: kept, so a leftover of such a call stays when no checked frame above it takes it out.
  A call that goes back through the system into the game (a window procedure called by DispatchMessage) is not
  followed: the system's frames above the first frame of the game's are not scanned, and the game's frames past
  them are held and dropped unless a frame above leads to them. A tap on a function stands first in its hook chain:
  its callers' calls lead to the tap's thunk (not in a module) and stay unchecked. A frame's name by a PDB needs the
  PDB of the image that crashed (its GUID is matched against the dump's; without a dump it is trusted); the game's
  are named by the knowledge base (1.0's by its own names, its twins', else `FUN_<start>` by the bounds). The log's
  line and a mod's fault (`MOD_FAULTED`) carry the host's rough check (a direct call's target within 32 KiB below a
  frame kept), not MCP's. Not yet seen on a crash in the labs (offline only, the stand-in game: the labs were busy).
  A crash on a thread whose stack the filter cannot see (a stack overflow's guard page) has only the faulting
  address. The dump keeps the memory around the faulting thread's registers and three pointers deep (enough for the
  tester's traffic crash: the manager, its slots, each slot's car), not the heaps: an object reached only from another
  thread or a global (beyond the globals themselves) is missing unless MML_FULL_DUMP=1 was set before the game
  started. A dump that fails is written again without those ranges. A hang
  is not a crash: `where` stays the tool for it. A fault that kills the process without an exception (a fast fail)
  writes nothing.
- **The structures** (knowledge/structs, `inspect`): st0rm94's fields are carried to 1.2 only where the paired
  functions touch them (690 of 1329 stay 1.0's: the AI's, the police's and other classes whose functions were not
  paired, and fields only read through a pointer elsewhere); a carried field is the accesses' majority, not seen one
  by one (those of C_actor, C_human, C_car and C_Vehicle read the same in both labs). LS3D's classes (I3D_frame...)
  have no vtables in the table (their constructors are in LS3DF, not read) and their st0rm94 fields were carried
  through Game.exe's accesses only: `sector`, `mesh` on a model frame read values that are not pointers (seen in
  1.0, the build he reversed). C_Vehicle's 1.0 vtable was not found (its constructor has no paired twin); embedded
  C_Vehicle has no size. An object's class is told by its vtable only (an actor's kind is not used). The SDK's
  `vehInertiaA`, `vehInertiaB` are left out: 1.0's GetRVelMassJ reads the body's matrix pair there (vehMatrix's
  offsets) where 1.2's reads the pair after it (`knowledge/notes/vehicle_physics.md`). Found with `gaps`, not named:
  G_Camera `+0x08` held the player (1.2, in a car).
- **A mod's own files** (host/files.cpp): seen in the 1.2 lab with a menu's picture only; a model, a sound, an
  animation and a table of a mod's were not loaded in the game (they come through the same `dtaOpen`: about 720
  models, 240 sounds and 50 tables a freeride's load); in the 1.0 lab only the menu's picture was seen. A table the game reads only at its start, and which tables those are, was not listed.
  What opens files without rw_data is not answered: the Bink videos (binkw32 opens its own), the saves
  (`dtaOpenWrite`, not hooked). A name longer than 259 characters with the mod's folder is left out (rw_data copies
  names into MAX_PATH bytes). The build's `files\` copy and `deploy` add and replace files, never delete one that
  is gone from the source (clean `build/x86/out` or the game's copy by hand). The presaved textures are recognised
  by their names' form as LS3DF 1.2 builds them (read in its decompile); 1.0's LS3DF asked the same name for the
  menu's picture (its code not read).
- **The graphics bridge** (host/gfx.cpp): left of item 3 besides 3c: the sky's own sun (the scene's lights feed the
  sun and the ambient light since item 11a), antialiasing of our own (a mod with `"gfx": true` turns the game's multisampling off),
  the frame's depth as a Direct3D 11 depth buffer for libraries that test against one (mafia-ragdoll's
  `BuildSceneDsv`), the readback of a pixel's depth (its `Aim`). Plain Direct3D 8 (no wrapper) has no bridge. The
  passes run in the mission's world only, not in a menu's scene. At most 64 passes. Measured on one NVIDIA RTX 3070.
- **Scene views** (host/view.cpp): the lens flares kept out of a view and the sound listener detached while it
  draws are done as LS3D's code reads (QueueVisual, the scene's +0x284), not seen in the game: no flare was on screen
  in the freeride where the views were tried, and the sound was not listened to. A view is drawn only in the mission's
  world (not a menu's scene), on the game thread, before the frame (its passes get it at once). A pass run over a
  view (`gfx_pass_views`) costs a wait for each device more per view (a frame-sized mirror with `depth_pulse` in it:
  4.3 ms of CPU in the 1.0 lab): one wait for all the views of a frame is not done. The sky is told by its view
  matrix being the camera's backdrop view, bit for bit (it was, every drawing, in both labs). `MML_VIEW_OWN_SECTOR`
  (a camera in another room) was not tried. The first two or three reloads of `rear_mirror` added 2..13 handles each (taken to be
  Direct3D's own warming up: views made and taken back by mode changes added none); then 0.
- **Interaction** (host/interact.cpp, item 15): zones are spheres; the traffic's pedestrians (not C_humans) are not
  watched by them.
  A use-point is on foot only (the game's using objects are; nothing offers one in a car), its reach a horizontal
  circle with the game's own 1.5 m up or down; the menu's line is the label as it is (no icon, no hint of what
  comes). A label's text id stays for the game's life (one a distinct text: the same label gets the same id).
- **The HUD** (host/hud.cpp, item 16): **the radar was not seen with a mark**: the game draws its radar from the
  mission's radar map where the mission turns it on (the HUD's flag 0x80000), and none of the freerides tried does
  (setting the flag in `freeitaly` made DrawAll read a null: the loader never sets it); which missions show it was
  not looked for. The map has no target of its own: SETCOMPASS's SetPlayerNaviPoint (1.2 0x58F160) writes a global
  nothing reads, so the compass is the only target. Markers are drawn over the world, not hidden by walls; a marker
  on a frame gives the radar and the compass that frame's origin (its `pos` offset is for the screen only). A
  device lost and made again (alt-tab, a new resolution) was not tried with pictures (LS3D's own textures, the HUD's
  among them, come back by its texture manager).
- **A mod's state in the saves** (host/saves.cpp, item 17): ProfileDelete's hook was not run in a lab (it deletes
  a profile's saves: read in the code only). The saves were made in the tests by calling SaveGameSave from the
  bridge, not at the game's own save points: in 1.2 the save made so at the start of "Old Prison" loaded into a
  Game Over (the game's file shrank from 3733 to 1215 bytes; restored from a copy); the mod's state came back all
  the same. Found 2026-10-17 (block 9c): SaveGameSave's argument 0 keeps only the temporary actors (the scripts'
  AUTOSAVEGAME), which is what that save had; a whole save (1) of Old Prison's start loads in place and the mission
  goes on (`restore`). A save in the story is made by the scripts (SaveGameFromScript, which calls SaveGameSave): not tried.
- **Control** (host/control.cpp, input.cpp, item 14a): **actions of a mod's own in the game's controls menu**
  are not made: the menu's rows are fixed binding slots the profile saves (60 in 1.2); growing them means the
  bindings' vector, the held/pressed arrays and the profile's copy (`G_LoadSaveClass + 0x1F4`), not read yet. A mod
  binds its keys through its settings (`hotkey`, `key_down`) and captures them. **Seats and the using-objects of a
  mod's vehicle** are the game's car's (`actor_seat`). **The airplane** (`C_airplane`, block 8c) is flown by a mod
  but nobody rides it: it has no seats or using-objects; the player aboard would be a person standing in a moving
  vehicle as in the tram (C_human +0x98 its owner, his model in its frame, TestMoveCollInOwner), not tried with a
  plane. Given back to the game, a plane flying its route turns back to it from where it is (its AI steers to the
  route's point): read in AI_fly, not seen (the Extreme's plane stands idle until a script starts it, and
  `airplane_start` needs a route's number of the scene's web: not looked for). Its AI's own collision test (AI_fly's
  sphere test) is left out with its flight; the mod tests its way itself (`world_sweep`). In a replaced step the wheels are not run (their suspension and turning stay as they were) and the
  sectors are not relinked by the host (a car flown 60 m drew fine; far flights not tried). The player's lock is
  the scripts' (a mission's own lock and unlock act on the same flag).
- **Sound** (host/sound.cpp, item 13): **heard by no one yet**: every check was made on LS3D's and DirectSound's
  state (playing, the position moving, the buffer's frequency and volume), none by ear; the owner listens once
  (`engine_note`, `sound.play`). No panning of a 2D sound (I3D's ambient sounds have none; ISND_sound's SetPanning
  would need a sound of the ISND layer). The cone of a 3D sound is all round. LS3D's IShow leaks 3 handles a track
  played and stopped (the game's own music too: the same Stop, Close, Release as C_game::DestroyIShow). A mod's own
  `files\sounds\` file was not tried (the open goes through dtaOpen, as a texture's). The music's volume is set when
  it starts and with `music_set`: a change of the options' music volume while it plays is not followed.
- **Props** (host/props.cpp, items 12a, 6b): a model of a mod's own `files\models\` is opened by the same cache that
  `files.cpp` answers, but was not tried; a prop on a car's part (a wheel, a door) was not tried, only on the car's
  model. A prop in the world is linked into the room at its place on each `prop_place` with a position (6 stones a
  frame showed no cost in the labs; not measured with `profile`). **Animations**: one channel (LS3D's model has
  eight, blended by weight; the mission's list steps a model only while its channel 0 plays); a mod's own
  `files\anims\` file was not tried. **Physics**: the game's box from the model's bounds, nothing finer; a physical
  prop lies still, in the air too, until it is pushed or given a velocity: the taxi driven into one at about 4 m/s
  was stopped by it and did not move it; bullets and explosions were not tried on one; a person walking into one
  was not tried. Its friction (the scripts' 0.45) is not offered. A physical prop moved is made again a frame
  later, at rest (its velocity is not carried).
- **The weather** (host/effects.cpp, item 11d): the 16th parameter of LS3D's switch (a flag, bit 0x200) has no
  keyword in the scripts and is left out; `weather_reset` leaves `on` as it was (the scene's own reset). The rain's
  sound is not reached (whether the scene's weather plays one was not looked at).
- **The particles** (host/effects.cpp, item 11c): one hook at a time; a particle unlinked by one of LS3D's direct
  calls of SetParent (five in LS3DF, not through the vtable) has its end found only by a five seconds' silence (its
  frame may be reused before: `first` is then missed); a pinned particle (frame flag 0x80) is not told. The pooled
  effects were counted (thousands of moves in the city) but not looked at on the screen: which effects they are
  (fire, steam, leaves) was not told apart.
- **The exhaust and particles.def** (host/effects.cpp, item 11b): one exhaust hook at a time (the last mod's); a record
  changed after a mission's load shows from the next one (the emitters copied it); the emitters themselves (LS3D's
  I3D_emitor, their parameters) are not reached. `exhaust_fx` draws its puffs as 2D squares over the frame (no depth
  test): a smoke mod draws in the 3D world (the render events, the graphics bridge).
- **The scene's lights** (host/lights.cpp, item 11a): found once a mission (13 ms in the 1.2 lab: not repeated),
  so lights made later (a car's lamps: 39 to 44 lights between two launches of the same freeride) need
  `lights.list {"rescan": true}`; the list holds frames that LS3D may free (each read guarded, each change checks the
  vtable). The sky's own sun (its quad in the backdrop, mafia-ragdoll's `FindSkySun`) is not read: `scene_sun` is
  the directional light's direction. Not done of item 11: particle emitters, the car exhaust (0x4DEE90),
  `tables\particles.def`, the weather's parameters.
- **The game's state** (host/state.cpp, item 10a): the money is the freeride's only (the story keeps none); health
  written does not pass the game's hit (no wound, no sound), and a death is not written. Not done of item 10: the
  game's statistics, the profile's progress (chapters, the freeride's unlocks), a mod's own items in the items'
  table. (A mission's objectives and outcome were dropped: the story is linear, see "The order of work".)
- **Questions to the world** (host/collide.cpp, item 8a): the static mask is 0 (every primitive the game does not
  always pass, 0x20): the bits the bullets pass besides (0x200000 half the time, 0x400000) are not named. The water
  (world_water) is the collision's faces of surfaces 31 and 44: shallow water (44) was not met in a test, and the
  river has no bed in the collision (a mod's own depth is its to choose). A
  traffic pedestrian's objects are owned by the pedestrian system (`actor` is it, not a person), and `world_near`
  lists actors only.
- **Shaders on the game's own drawing** (host/shaders.cpp, blocks 7a, 7b): only under a Direct3D 9 wrapper (plain
  Direct3D 8: no service); shader model 3 is assumed by the contract (ps_2_0 sees 32 of its 76 registers). Only
  what LS3D draws face group by face group (objects, people, lightmapped objects, billboards) is offered: the sky's
  backdrop, particles, the HUD and the menus are drawn otherwise. The draws carry no normals (a face's normal from
  derivatives is flat); the meshes' own normals (block 6d's buffers) are not handed to the shaders. The texture
  stages' arguments are taken as the game sets them (texture, then what came before): another argument (the texture
  factor's colour, the specular) is not followed by `mml_FixedStage`. Compared on one view of the freeride's start in
  each lab (the city by day); the night, the rain, the countryside, the interiors and a mission were not compared;
  the scene views (mirrors) take rules too but were not looked at. The constants are read from the device at each
  draw a rule takes (Get* calls: on a pure device they would fail; dxwrapper's is not pure). The cost was not
  measured above the game's 62.5 frames a second.
- **The x87 around the mods' code** (host/fpu.cpp): a mod's detour on a game function (`hook`) is called by the game
  and runs with the game's 24-bit word (it sets 53 bits itself when it needs doubles: MODDING "Doubles on the game
  thread"); a game function a mod calls from its callbacks runs with the mod's 53 bits (the game's x87 maths a little
  more precise there: nothing seen to change). The loader's own code outside the mods' calls and its pages (its
  hooks, the bridge's methods) keeps the game's word.
- **The sun's shadow and the game's lights** (host/shadows.cpp, block 7c): since 2026-10-30 the default is LS3D's
  actual posed drawing, with alpha-tested textures and all railway parts. Shadow-only box visibility planes,
  occlusion disabled and distance LOD corrected solve the old missing-caster camera problem in both builds.
  No 0.25 s mesh scan drives this path; that limitation remains only in diagnostic/fallback `casters: "meshes"`.
  The main pass's depth-write state is explicitly restored (LS3D's cached reset omits it); real-game regressions
  check opaque zwrite after repeated on/off transitions in both builds. One map of a fixed extent around the camera
  (no cascades: past it, lit); only streamed scene geometry is available, fractional glass transmission is not
  modeled, PCSS softness is bounded to 32 texels. Interior portal visibility has not been exhaustively tested.
  The scene views (mirrors) sample the frame's map
  (drawn around the game camera, not the view's eye); not looked at in a view. The lights a draw gets are the eight
  nearest its visual's bounding sphere's centre among those reaching it: a long piece (a street) may get far lamps
  before a near one. Seen in the city's freerides only (day and night), not in the rain, the countryside, the
  interiors or a mission. **The headlights' cone on the street** the game draws at night is not one of the face
  groups LS3D draws (the night's survey has every face group at pass 0, no second pass), so no rule takes it: what
  draws it was not found (left for later).
- **The sun's shafts** (mods/examples/sun_shafts): no rays past `distance` (the map's extent; the haze beyond is
  even); the rays now follow the alpha-tested tree shadows (2026-10-30); single scattering only; translucent things
  (glass) get the haze of what is behind them. Looked at in the city by day in both labs; the countryside's sun was
  found once its lights were read (the first query, before the scene's lights were scanned, had none).
- **The tuning panel** (host/panel.cpp): sliders, check boxes, selectors and keys (no colour or text fields: a
  `tint` is set with `config.set`); a mod's key is chosen on its page in Options > Mods, not in the game's own
  Controls screen (optionsc1: its rows are `vidc` controls on the game's 60 binding slots, whose arrays and the
  profile's fixed 0x2D0-byte copy would have to grow for a mod's: left, the profiles untouched); a key, not a mouse
  button or a joystick's; the slider's arrows step as the game's own do (a click held repeats); the list of
  mods is made when it opens (a mod loaded while it is open is there at its next opening); the Options reached from
  the main menu were not looked at.
- **The meshes the game draws** (host/meshes.cpp, block 6d): the finest LOD only; plain and lightmapped objects
  only (billboards, particles, skinned people, morphs, mirrors are left out); a piece is told by its bounding sphere
  touching a box (a long piece may hold far triangles). Frames LS3D frees during a mission are not followed (each
  read is guarded; the next walk forgets them). A mesh whose vertices were changed by the game after it was read (a
  car's dents for `actor_meshes`: read once a mesh) is read again only when its LOD's vertex count or start
  changes. No mover events: `version` and `moving` are read by the mod. The scene's numbers (about 100 pieces in
  140 m at the freeride's start) were not compared with what the screen shows.
- **The game's files and models** (host/models.cpp, block 6c): the archives' keys are the game's table's, written in
  the host (not read from the game): an archive the game gets with other keys (a mod's own .dta) is not listed. The
  game's folder's own files are listed only under the folders the archives hold. A frame of the mission's scene has
  no file (the scene's `.4ds` is not named for it). The .4ds versions 27 and 58 (three files) are not read; a
  skinned mesh's weights, a morph's targets, a sector's and an occluder's geometry, lights and the mirror's are
  skipped. The 1.0 lab holds another project's `scripts\MafiaRagdolls.asi` (Ultimate ASI Loader loads it beside the
  loader): it hooks LS3D's model loading; what else it changes in 1.0's tests was not looked at.
- **The camera** (host/view.cpp, item 9a): only the drawing is the mod's: the game's camera logic (its spring
  behind the player, its distance, its collision with walls, the car's cameras), the player's aim and crosshair (it
  stays in the screen's middle, now pointing where the game's camera looks) and the sound's listener are the game's.
  The game's own camera (block 8a, host/gamecam.cpp): its modes and its field of view are a mod's now; its distance
  behind the player on foot is not: G_Camera +0x24 (5.0, st0rm94's "camera distance") is not read for it (12 written,
  the camera stayed where it was: measured in 1.0), and where the on-foot and the cars' distances come from was not
  found (G_Camera::Tick's and WantPos' constants are the leads). An aim from a camera other than the game's (the
  view decoupled from the aim, a VR head) is not done. Drawn only in the mission's world, not a menu's scene.
- **Statistics** (host/stats.cpp): "cars stolen" is not counted: the game keeps no such count, and its theft's own
  path (the police's C_shvestky::CarSteal, a driver thrown out: Do_ThrowCocotFromCar) was not reached in a test (the
  player only opened a spawned car's door); `cars_driven` counts every car he gets into. Only the player's deeds are
  counted. The old ten-second host persistence is removed as of 2026-11-15. Collection is opt-in and in memory;
  consumers must persist their own counters with save_put. Legacy host records remain readable. The
  freeride level word's values (3 in both labs' profiles) are not decoded beyond "3 and up: the freeride's settings
  count"; the garages of both test profiles were empty.
- **A mod's own items** (host/items.cpp): a save holding a mod's item was not loaded in a test (the ids are kept for
  it); a model of a mod's own `files\models\` was not tried (the game opens the item's model by its name, as files.cpp
  answers). The game's first item array is left allocated (6.5 KB: a pointer to one of its records may be kept). An
  item made like a firearm was not tried (its sounds, its HUD and the gun shop may index by id). At most about 60
  items (the game's text ids after 3300 + id).
- **What happens** (host/happen.cpp): the killer of `HUMAN_DIED` is the last attacker of `HUMAN_HIT` in 3 s (seen
  2026-10-16 in both labs: the player's Thompson killing a spawned Paulie, aimed by `world.ray` from the camera); a
  death the game queues with no hit (a script's) has none. `SHOT`'s direction is not a unit vector (its length is the weapon's reach). Deaths and vehicles come from
  the people the loader knows (C_human), a frame late at most. `CAR_CRASH`'s 1 m/s is a compromise against a car
  pressed on a wall (a contact every frame); several contacts of one blow come as several events in the same frame.
  `BULLET_HIT` is not told for a glass the bullet breaks on the way (the tick's own path, not looked into) nor for
  the traffic pedestrians (PEDESTRIAN_HIT); it finds the bullet's record by its place, and a bullet a mod hooked
  collRayTest around before the loader (outermost) would not be seen. A jump is the game's animation (state 4), not
  a fall: only a fall (state 3) gives `HUMAN_FELL`. Items are counted by id: one item swapped for another of the
  same id in the same frame is no change; the rounds spent are not told. The game's own pickup function (inside the
  "itempickup" menu, vtable 0x625920) is not hooked: a pickup is seen as its inventory's change.
- **Animations, getting up** (host/anims.cpp, item 6b): an animation human_anim_play starts is in the animations
  machine, not in the person's +0x70 (which stays his own life's: `human_anim_state`'s `playing`); his state turns 4
  only for an animation with a track (.tck) of its own. A mod's own animation file in `files\anims\` was not tried
  (the game opens it by name as it opens its own). Getting up was tried on people posed by the bridge, not on a body
  the game itself knocked down (its own queue then plays: the mod's call replaces it). Traffic pedestrians cannot be
  handed a get-up (done 2026-10-21, item 32d: human_get_up takes an element), and they are not
  listed (only their hits are told). `world.ray` with MML_WORLD_MOVING did not hit a walking pedestrian's box that
  `world.shapes` showed (not looked into).
- **People** (host/humans.cpp): a freeride lists 36 people besides the player with the model name `road_bl_E` at
  the origin (both builds): the traffic system's pool, not placed yet, whose actor + 0x68 is not a person's model
  (no skeleton). Traffic pedestrians walking the streets were not checked against the list. A pose is drawn only:
  the game's logic, collision and aim keep the animation's. Animations played by a mod, getting up and 1.0's fields
  are 6's other blocks.
- **Measuring the frame** (host/prof.cpp): the passes' GPU times are in `gfx.state`, not in `prof.state`; a mod's
  own hooks (its detours inside the game's functions) count as the game's time (the sampler shows them under the
  mod's DLL). A scene view with passes in it waits for both devices once more each (in the 1.2 lab about 4 % of the
  game thread with `river_water`): one wait for all of a frame's views is not done. MCP's `profile` names 1.0's
  Game.exe through its 1.2 twins; 1.0's LS3DF has no names (the knowledge base for 1.0, Later).
- **The game's own geometry** (host/geom.cpp): the meshes LS3D draws face group by face group (objects, lit
  objects, morphs, single meshes, billboards) are covered; what it draws otherwise (particles, the sky's backdrop,
  landscape patches, lens flares, the 2D) is not. A capture holds what the frame drew (a far or culled group is
  not in it). Hidden by texture name only (not by frame name or position). The river was the only replacement seen;
  replacing a texture's picture is the files' way (item 2).
- **The 2D pass for mods** (host/gfx.cpp): the game's additive 2D (if any) comes back blended, not added, when it
  is put back over the frame (`take`); the menus' cursor is the screen's; the last frame's is what the passes get.
- **`game_launch` in front**: once (2026-10-15, the 1.0 lab, `background` by default) it answered `"in_front": true`;
  whether the window was clicked was not known. Watched for.
- **`menu.click` in 1.0** once did not close the freeride screen (its "Retour", clicked right after the screen was
  opened and a screenshot taken); clicked again later it did. Seen once, the cause not found (the screen's opening
  not finished?).
- **`menu.state`** did not list the pause menu's "Exit Game?" confirmation (seen 2026-10-15: it was on screen, the
  reply showed only the game menu under it); Enter answered it. Not looked into.

- **fps_unlock** (mods/examples/fps_unlock): the main menu's camera, a person's step at curbs and LS3D's small
  rotations are fixed. The rotation fix was measured on the player's taxi on flat ground in 1.2 (the stop, slow
  steering, acceleration) and in 1.0 (the stop; 1.0's lab reaches 185 fps only); handling at speed, crashes and
  braking from speed were not compared, nor traffic. LS3D's `S_matrix::SetRot3` / `SetRot` drop small rotations
  the same way (about 90 callers, not read, not fixed). The other users of `RotationMatrix` that turn by a step's angle (the airplane, `C_PhThing`, bottles, the
  traffic's pedestrians and trams) were not watched. A car's wheel-spin fields change differently in one 16 ms step
  than in four of 4 ms (`vehicle_physics.md`: oscillation or a per-call integration, not known). The step at curbs is
  fixed at its cause (the sweep's lengths made once a call); running up a flight of stairs is still about 7 % faster
  above 62.5 fps, fixed or not (another cause, not found), and a person stands 8 mm nearer a wall. The other systems that may do once a frame what they should do by the
  time are leads from a scan of the decompile (`knowledge/notes/game.md`, "The frame rate"), not checked in the
  game. Frames of 0 ms (above 1000 fps) reach every system (532 a second in 1.2's main menu at 1520 fps). The step's
  fix was measured on the player only (other people use the same sweep: not watched); a sweep after a frame of 16 ms
  or more is the game's own (below 62.5 fps the game's own behaviour stays).

- **A mod's unload** (host/lifecycle.cpp): a thread is found in a load's code by its EIP, the chain of saved EBPs and
  where it began; a thread inside a library compiled to use EBP as a plain register, between it and the mod's frame,
  is not seen (its DLL is freed, as before 2026-10-14). Only the top level of a mod's folder is copied (`*.dll`); a
  library a mod loads later by a bare name is not searched in its load's folder (load by the full path). Objects of
  the game that point into a mod's code (a callback it gave the game without the loader) are not found. The
  `since_load` numbers are the whole process's over the load's life: the game's own changes are in them.

- **A heavy mod's build** (CMakeLists.txt, tools/deps.py, mods/examples/boxes): only Jolt is pinned for mods (PhysX
  2.8.4, OpenVR and the like when a mod needs them). The first configure needs the network (a library already in
  `third_party/_deps` is not fetched again). Jolt is built as a part of the loader's build in its configuration
  (RelWithDebInfo: CMake's `/O2 /Ob1`, not Jolt's own Release flags); its unit tests are not built here (577 pass on
  the same source and options in mafia-ragdoll's Release build); the x86-debug preset was not built. A shader's
  `#include` of a file outside its own folder does not compile it again when that file changes (only `.hlsli` beside
  it are watched). A mod's own Direct3D 11 device is on the default adapter, not looked up as the game's (two GPUs:
  not tried); measured on one NVIDIA RTX 3070 and WARP (the stand-in game), not on AMD or Intel. `boxes` needs no
  game address; in the 1.0 lab it drew and reloaded 10 times (2026-10-15).

- **The script machine** (docs/SCRIPTS.md): programs step only in the mission's tick (not in the menus, not while
  paused). Of the ~390 commands a handful were run (`let`, `if`, `goto`, `wait`, `findactor`, `findnearactor`,
  `getactiveplayer`, `getactorframe`, `frm_setpos`, `actor_setpos`, `actor_duplicate`, `car_repair`,
  `human_addweapon`, `weather_setparam`, `setcitytrafficvisible`, `explosion`, `getgametime`); the others behave as
  the game makes them, untried. `call`, `callsubroutine` and the event registrations reach other programs: untested
  with one of the loader's (a program ends at `end`, an event to it may come after it is freed). The game's
  argument parsing is lenient (a bad number is 0): only an unknown command or unparsable arguments fail the
  compile. A program's `dim_*` larger than the loader's 256 / 64 / 64 makes new, zeroed registers. A command that
  faults stops its program; the game's state the command had changed by then is left as it is.

- **The world** (host/world.cpp): only cars (type 4) and people (27, the NPC kind, with no script and no AI order:
  they stand) are made; dogs, boxes and other actor kinds are not offered. A person's model is checked for the
  bones the shot code reads; any other model the game opens is taken for a car without a check of its own (a car's
  `Init` refused nothing seen so far). People's models are not listed (they are files: `call humans` shows those in
  use). Spawned actors are kept out of saved games (as the cheat car); whether a saved game made with one around
  loads cleanly was not tried. `teleport` moves a person in a tram in the tram's space (frm_setpos). `world.remove`
  takes out only what `world.spawn` and mods made, and nothing a person sits in or that sits in a car.
  `ScriptExec` cannot run a command that waits for frames (an error names its line). Mods' world functions were
  run in the 1.2 lab through the telemetry example (actor_spawn, actor_remove) and in the offline harness (every one
  refused with no game); `actor_teleport`, `actor_seat`, `give_item` from a mod only there (they are the bridge's
  `world.*`, which ran in both labs).

- **The start**: without `profile` the profile menu waits for the player. The freeride's densities count only for a
  profile that has opened the freeride's settings (the log warns). A freeride kind the profile has not opened is
  started all the same (start.state notes it). `"load"` takes a save the profile has (no save file, no chapter):
  playing on from it writes the profile's checkpoint saves as the game does. `"new"` and `"extreme"` were not run
  in the lab (the main menu's commands, read in its handler); `"new"` plays the intro mission first, and the
  launch waits for a player until `wait_s`. On 1.0 a step's command is the screen's next tick's answer, so the start
  is a frame longer a step (3.0 s from the launch to the freeride in the 1.0 lab). Racing and the tutorial are not
  offered.
- **The texts' code page** is the language's (Czech, Polish, Hungarian, Slovak 1250; Russian 1251; the rest
  1252): only English and French were seen; another language's code page is a guess.
- **Input**: the menus' mouse is driven only while a menu is open (outside one, the mouse is the player's
  aim: use `turn` and `Aim X`).
  `turn` is exact on foot only: the car's and the free look's cameras have their own code, and `Aim Y`'s scale is
  not measured (down is positive). The driving actions' names come from the code that reads them (0x5917F0), not
  from the menu's rows; slot 32 is read by nothing found. A hold is counted in the controls' updates: the game's
  loop and the menus' loops, not the loading screens.
- **Screenshots on plain Direct3D 8 with a multisampled back buffer** (the 1.0 lab before 2026-10-14): the screen
  is copied, so only while the game's window is the one in front (else refused: once it copied another program's
  window over the game); antialiasing off or a Direct3D 9 wrapper (both labs have dxwrapper now) gives the game's
  own frame.
- **The game in the background**: the loader keeps the game from activating its own window for the whole run (a
  click on it activates it; the game's own later calls to come in front, after an Alt+Tab, are answered without
  effect too). Whether LS3D or the sound play the same with the window inactive for long runs was not looked at;
  the game drew, ticked and took input in the tests. `tools/start.py` (the owner's start, to play) is not in the
  background.
- **Screenshots** need the game to present frames (not while it loads or is minimised). On plain Direct3D 8 a
  multisampled back buffer cannot be read (the lab runs through dxwrapper's Direct3D 9, where it is resolved).
  Every shot goes through a raw file on disk (5 MB at 1280x960; the MCP server deletes it).
- **Basic 2D primitives for mods**: nothing is drawn while the game loads or plays its native videos without
  the ordinary 2D callback. draw_text uses the game's three fonts and its encoding, about 100 characters a call;
  draw_rect/draw_line have no general clip/rotation API. Pictures are available separately through draw.picture/
  HUD SDK helpers; graphics passes and world meshes have their own contracts. The basic overlay is drawn over
  the HUD and menus. A cutscene's black bars are not known to it. Under plain
  Direct3D 8 it is the same code (the game's own drawing), but it was only run under the lab's dxwrapper.
- **Keys for mods**: the mouse (buttons, axes) is not given by itself, only through the game's actions. The keys
  were tested with injected ones (agents never touch the owner's keyboard): a real key goes through the same
  buffer, but nobody pressed one in the tests. `key_pressed` asked from both `FRAME` and `MISSION_TICK` sees one
  press twice. Hotkeys also fire in the menus.
- **The car**: `Car::Damage` is the engine's health only (the body's dents are not a number). `CarStateLoad` puts a
  state back into the same car or the same model (repairing it first: a car_repair sets it on its wheels where it
  is before the state moves it); it was tried on the player's taxi and spawned cars, in 1.2 only since dents count
  (1.0's `carRepair` checked against its files), not on traffic or racing cars, and a state is not kept across missions. `LastCar` is the HUD's pointer: whether
  the game clears it when that car is destroyed was not looked at (read it through `mem_read`, as `call car` does).
  TrackMafia's ghost-car pointer and its recipe of copying the car's memory are not offered (see
  `knowledge/notes/game.md`, "Cars").
- **The mission's events**: a pause is told only when a menu stops the world (cutscenes and the map are not
  events); `MISSION_LOADED` is the first tick, not a flag of the game's.
- **Other projects' names** (`external.csv`): 2726 of st0rm94's 5098 1.0 functions are paired with 1.2's; the
  rest differ too much or are too small to tell apart. A name from another project names what that project
  understood; 3 of 889 checked differ in class from the decompile's. MafiaOnline's contract rows count only where
  their bytes are ours. Field layouts (st0rm94's are 1.0's) are not imported: read in place.
- **Settings**: no schema (a mod checks its own values).
- **The game's menus on 1.0** (`docs/VERSIONS.md`): 1.2's kinds asked for are 1.0's that do the same, not the
  same: a switch (`bslb`) is a drop-down like the selector (`cmbs`); a button's colour (`MML_MENU_BUTTON_*`) and a
  row's (`MML_MENU_ITEM_RED`) are not 1.0's (its buttons and rows are drawn black, grey when disabled); a text's
  font and colour are its record's (`MML_MENU_FONT_*`, `MML_MENU_TEXT_*` translated); a text field takes 31
  characters. A change that sends no id (a check box, a selection) is told to the page's mod when its tick sees the
  value changed. A number (`iten`) steps by the loader as the game's own screens do it (+1 on its id). The racing
  and multiplayer screens' names are st0rm94's, not 1.2's (not compared). A screen rebuilt in place (its
  OnExchange: a language or resolution change) loses the mods' items until it is made again. Items in screens other
  than the pause and main menus, and pictures (`rctf`), were made but not looked at on the screen.
- **The game's menus, extended**: the pause menu and the main menu were extended in the lab (both builds); items in
  other menus (and in a scrolled list such as the load menu's) were not tried. The kinds `edin`, `sldi`, `rari`,
  `list`, `lstc`, `vscr`, `comb`, `cmbw`, `grid`, `vidc`, `mtxt`, `htxt`, `ntxt`, `stxb`, `tinp` are made from a
  description but their data and values are not offered (`menu.def` shows where the game uses them). Containers do
  not nest (the game's "end" returns to the page's window). Whether the engine reads a picture's file again
  after it changed on disk (or keeps the texture it made) was not looked at. The wheel does not scroll the game's lists
  (the game gives it to the focused control). `input.text` types the US layout's characters only. Texts are interned and never freed (a mod that sets texts every frame grows the
  set). One page a mod at a time; a page closed by Esc lives until the game deletes it, so a `menu_open` in the
  same frame as the close is refused. An item copies its neighbour's text id: a language change re-reading texts
  by id was not looked at.
- **`world.remove` leaves a person's model frame alive** (2026-10-21, both labs): after a spawned person is removed
  his model keeps one reference (frame.watch saw no release in 5 s; a prop's prop.remove frees its frame at once).
  Who holds it (the scene, the actor's destructor, the duplicate's source) was not looked for.
- **1.0's bullets, getting up and trams** (`game_build.h`'s mafia-ragdoll fields): filled for 1.0 on 2026-10-16
  (the twins of 1.2's functions, checked by check_builds.py), not yet run in a 1.0 game by the loader (item 32 runs
  them); mafia-ragdoll's own copy of the table still has zeros there.
- **`menu.command` sends any integer**: a command the active menu does not expect may do anything that menu's
  handler does with it. Read the handler first (`menu.state` names it; `kb_function`).
- **The profile list exists only while the profile menu is open** (the game releases it when the menu closes):
  `menu.state` lists the profiles only then.
- **The intro videos of missions are not skipped** (only the three logos before the profile menu).
- **Knowledge base names**: a few names imported from the decompile keep calling-convention marks from its
  signatures (`GM_Menu::ExecuteMenu@<eax>`); names of ours for menus (`GM_MainMenu::OnCommand`...) are ours, not the
  game's. LS3DF.dll has few names (64 of 1599).
- **`game_build.h` has diverged from mafia-ragdoll's copy** (the start's, the menus', the world's and 1.0's
  fields): the original standalone game's table and this loader's table have separate ownership. The loader
  port is a different repository; changing either sibling still requires the owner's authorized scope.
  An address found in one is checked in the other build before use; do not copy unrelated local edits wholesale.
- **`tap`**: at most 16 taps, 8 stack arguments, 4 snapshots of up to 64 bytes each; a function whose first bytes
  MinHook cannot relocate cannot be tapped.
- **The exit index-owner crash** (`knowledge/notes/game.md`): later reproduced in a loader tester's 1.2 copy.
  Final driver cleanup now guards stale owner clearing in that build. The source of the stale lifetime is still
  unresolved; this guard is not a claim that every exit failure has the same cause.
- **32-bit address space**: the game and every mod share 4 GB (2 GB without the large-address flag); hot reloads
  leak a little address space each (seen in mafia-ragdoll: restart after very many).
