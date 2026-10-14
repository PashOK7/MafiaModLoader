# Roadmap

What is done, what comes next and later, and what is known to be missing or wrong. Agreed with the owner on
2026-10-13, reordered on 2026-10-14 for the toolkit's goal ("Next"); agents keep it current (move items to Done with the date, add what they find to Known gaps).

## Decisions (the owner's)

- The host is a `.asi` loaded by Ultimate ASI Loader (`dinput8.dll`); it never unloads. Mods hot-reload.
- The MCP server is a separate process (Rust), talking to the game over the bridge; it outlives the game.
- Builds: 1.2 first, 1.0 too (per-build tables in `game_build.h`).
- Mods in any native language through one C ABI. (LuaJIT scripts were listed here too; the owner says the roadmap
  was the agents' work, so they are parked until the owner confirms them: "Taken out or changed".)
- A 64-bit sidecar for what only exists for 64 bits (the owner, 2026-10-14: "what if a user wants a modern
  technology such as the latest PhysX, which runs only on 64 bits?"): the game stays 32-bit, a 64-bit process beside
  it hosts such libraries (PhysX 5, and the like: GPU and ML runtimes), connected to the game by shared memory.
- Agents have full access, and may launch, drive and close the game themselves, from inside (never the owner's
  real mouse or keyboard).
- The repository is English; it is a private GitHub repository (2026-10-13). Pushing,
  publishing and pull requests: ask the owner first.
- The owner's 1.2 lab is shared with mafia-ragdoll (`tools/lab.py mode loader|ragdoll`).
- The loader is measured by how easily another agent writes a mod with it (2026-10-13). Older mods
  (TrackMafia: lap telemetry, a ghost car, an overlay; `knowledge/notes/game.md`, "Cars") are not ported here: they
  show what every such mod had to build for itself, and so what the loader must give instead.
- The toolkit is built for whatever mod a user can think of, written by another agent (2026-10-14); its measuring
  cases: a ragdoll, shader water, realistic smoke, VR, fishing, skateboarding, a working airplane, achievements.
  Those mods are not written here; mafia-ragdoll is not ported (its code is the reference).

## Done

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
    read again when a file changes, `CONFIG_CHANGED`; `config_*`; the bridge's `config.get`, `config.set`);
  - **a complete example and a start**: `mods/examples/telemetry`, `tools/new_mod.py`, `docs/MODDING.md`.
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
  the base): the limiter's minimum and Present's yield patched (both builds' places in `game_build.h`, checked by
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

- **`car.restore` with dents** (2026-10-14): a car's state grows with its body's dents, and the restore refused a
  state of the same taxi taken before a crash (its size was the test of the model). `CarStateLoad` now repairs the
  car first (the scripts' `car_repair`: the game's load writes only the vertices a state lists) and checks the model
  by the counts the load walks by (`CarStateFits`: lights, body parts, seats, wheels); `car` tells `state_bytes`, and
  `car.save` survives the core's reload. Seen in the 1.2 lab: a dented state (1285) back into the repaired taxi, a
  whole one (1237) back into a crashed taxi (1437, engine 86 %: back to 1237, 100 %), the state into another taxi of
  the model, a Bolt-Thrower refused.

## Next, by importance

**The goal** (the owner, 2026-10-14): a toolkit strong enough for whatever mod a user can think of, written by another
agent. The measuring cases span the range: engine-deep mods (**a ragdoll, shader water, realistic smoke, VR**) and
gameplay mods (**fishing, skateboarding, a working airplane, achievements**). None of them is written here, and
mafia-ragdoll is not ported: it already has a ragdoll (Jolt), water and smoke (a Direct3D 11 bridge, PhysX 2.8.4,
NVIDIA Flow) on a host of its own, and its code is the reference for what several services below must do
(`W:\github\mafia-ragdoll\src`, read only). Every service lands with an example mod (`mods/examples`), a section in
`docs/MODDING.md` and its offline test, is seen working in the 1.2 lab, and in the 1.0 lab once its addresses are
found there (1.2 first, as the builds' decision says). Each item names who needs it (R ragdoll, W water, S smoke,
V VR, F fishing, K skateboard, P airplane, C achievements, A the agents themselves) and its reference.

### Tier 1: foundations every large mod stands on

1. **A heavy mod's lifecycle, the build's part** (all; small). The loader's part is done (a mod's libraries, threads
   left in its code, leaks measured: "Done"). Left: third-party libraries for mods pinned by `tools/deps.py` (Jolt
   first, built for x86) and shaders compiled in a mod's build (fxc or dxc in CMake, a `mml_add_mod` option); then
   the first heavy example mod using both, reloaded 20 times in the lab (mafia-ragdoll lost its Direct3D 11 device
   after about 20 reloads: measure whether a mod's device survives).
2. **A mod's own files in the game** (F K P C S W, any mod with new content). Models (`.4ds`), textures, animations
   (`.5ds`), sounds, tables and menu definitions shipped in the mod's folder and opened by the game by name, as if
   they were in its archives (the archives' open in `rw_data.dll`, `dtaOpen`, answered from the mods' folders first);
   a mod replacing one of the game's files (a texture, a table) while it is loaded; the order when two mods ship the
   same name; a file changed on disk seen again where the engine allows (or said where it does not).
3. **Render events and the graphics bridge** (W S V; R for posing). Events around the scene's drawing: before and
   after `I3D_driver::Render` (LS3DF `0x62030`; a ragdoll writes its bones right before it: the game re-poses in
   between), at stage 1 (the 3D world drawn, the HUD not yet), at Present. The game's Direct3D 9Ex device (dxwrapper);
   one Direct3D 11 device beside it on the same adapter, shared by every mod: the frame's colour and depth (the depth
   buffer swapped for a readable one before the world is drawn), the frame's constants (camera, projection, the
   sun's direction and colour, the fog, the ambient light, read from the device), the mods' passes in order into one
   effects target, composited over the frame; GPU timers per pass; the 2D pass (HUD, menus) drawn into a target of a
   mod's when it asks (a VR mod shows it on a panel). One owner for the depth swap and the composite: two mods doing
   them each would break each other, so it is the loader's. Reference: mafia-ragdoll `gfx.cpp`, `shaders/frame.hlsli`.
4. **The scene drawn from another camera** (W V). The mission's scene drawn a second time into a target, with a
   camera and projection of the mod's (a mirrored camera and a clip plane for a reflection, two eyes with asymmetric
   projections for VR), everything the drawing touches put back after (the camera's sector held, the sound listener
   detached, lens flares kept out, the camera's bytes restored). The game's own geometry hidden or replaced around
   Render (the water's face groups; materials found by their textures' names). Reference: `reflection.cpp`,
   `scene_camera.cpp`, `game_water.cpp` (hiding).
5. **Measuring the frame** (W S V R P; from Later). Each mod's time in each event and pass (CPU), the GPU timers of
   item 3, the game thread's sampling profiler, on the bridge and MCP: water and VR live within a frame's budget
   (11 ms at 90 Hz). Reference: `prof.h`, `gpu_timer.h`, `sampler.cpp`.

### Tier 2: the game's world as data, read and changed

6. **People, skeletons, animations** (R K F; S W as pushers). Every `C_human`, the traffic system's pooled ones too,
   as a list (the constructor's hook already sends `HUMAN_CREATED`; no list is kept); traffic pedestrians' elements
   and their own Hit (`0x4588D0`) as events; the bones by name, a pose read in one call (the 18 joints, the model
   root), written back before Render (item 3); handing a body back to the game (getting up); the game's animations
   and a mod's own (item 2) played on a person or a model, blended, stopped, their state read. 1.0's fields for
   bullets, getting up and trams (zeros today). Reference: the host's `listHumans`, `ragdoll.cpp` (`PoseSnap`),
   `getup.cpp`, `hits.cpp`; the scripts' `human_playanim`, `model_playanim`.
7. **What happens, as events** (R W S C). A bullet's path and what it struck (the game's shot queue, `NewShoot`
   0x5A84A0), explosions (place, radius, force), a car striking a person or a thing, falls, deaths with who killed
   whom, a car entered, left or stolen, an item picked up, money changed, a mission's objective done, the mission
   won or failed: today only a hit on a `C_human` and the missions' opening and closing are told. Reference:
   `hits.cpp`, `knock.cpp`, core.cpp's shot hook.
8. **The world's geometry and questions about it** (R S W K P F). The file each loaded model came from (hooked from
   the process's start: only the loader is there that early); the triangles of what the game draws around a point
   (cached, fed a few pieces a tick), the collision grids (static primitives, dynamic objects with their owners),
   cars' meshes posed with their parts, an index of every `.4ds` in the archives and its parser; one scan shared by
   every mod. Questions in `mml.h` (not only C++'s `game.h`): a ray, a sweep, what overlaps a box or sphere, the
   ground's and the water's height under a point, the actors near a point. Reference: `scenery.cpp`,
   `visual_meshes.cpp`, `collision.cpp`, `game_collision.cpp`, `model_index.cpp`, `model4ds.cpp`, `car_meshes.cpp`.
9. **The camera** (V R K P). The game's camera read and set (position, orientation, field of view, near/far), its
   projection replaced after the camera's own update (VR's per-eye frusta), the view decoupled from the aim, a camera
   of a mod's own following its own rules (a skateboard's, a plane's chase camera), a free camera; a world point to
   the screen and a screen point to a ray (markers, aiming). Reference: `freecam.cpp`, `ragcam.cpp`,
   `scene_camera.cpp`.
10. **The game's state and progress** (C F P). Money, health, the inventory and ammo (read and changed), the
    mission's objectives and its outcome, the game's statistics (kills, cars stolen, distance), the profile's
    progress (chapters, freeride unlocks); a mod's own items in the items' table (a fishing rod, a fish).
11. **The game's effects as data** (S W P). Particle emitters and the car exhaust (`0x4DEE90`) read, moved or
    replaced; `tables\particles.def`'s records; the weather's parameters; the scene's lights. Reference:
    `smoke_game.cpp`, `effects.cpp`, `frame_life.cpp`, `scene_light.cpp`.

### Tier 3: building blocks for new things in the game

12. **Props** (F K P R). Models placed in the world without being actors (a rod, a fish, a board), moved every frame,
    attached to a bone or a car's part and detached, shown, hidden, removed; with a collision shape when asked
    (the scripts' `model_create` and `create_physicalobject` are partial cases).
13. **Sound** (F K P C S W). The game's sounds and a mod's own (item 2) played in 2D or in 3D at a point or attached
    to a frame, looped, their volume and pitch changed live (an engine's note follows its revolutions), stopped;
    the music.
14. **Control** (K P F V). A mod's keys and buttons taken so the game does not also act on them; actions of a mod's
    own, bindable in the game's controls menu like its own; the player's movement handed to a mod (a skateboard
    moves him, the game's walking stands aside) and given back; a vehicle's physics replaced by a mod's (a plane's
    flight on a car's or an airplane actor's body: `C_airplane` exists); entering, leaving and seats of a mod's
    vehicle (the game's using-objects).
15. **Interaction** (F P C). Use-points with the game's own prompt ("Fish" at the water's edge), zones with enter and
    leave events, timers of the game's clock (they stop with the pause).
16. **The HUD** (C F P V). Pictures and textures in the 2D layer (an achievement's icon, a plane's instruments:
    drawing has text, lines and rectangles only), notifications in the game's style (its console, `console_addtext`,
    with a mod's own texts), markers at world positions, the compass's and the map's target.
17. **A mod's state in the game's saves** (C F). A mod's data written with each save slot and read back when that
    slot loads (and per profile), so its state follows the player's game, not only its data folder.
18. **A tuning panel** (all). A mod's settings shown and changed live in the game: a page made from its
    `config.json` (sliders, check boxes, selectors from the menus' constructor), so dozens of parameters need no page
    written by hand.

### Tier 4: what only exists for 64 bits

19. **The 64-bit sidecar** (the owner's decision; S R with PhysX 5, V if a runtime needs it): `mml-sidecar64.exe`,
    started and watched by the host, loads 64-bit mods (the same kind of manifest, built for x64), their faults
    isolated and their reloads hot as 32-bit mods'. Shared memory between the processes: channels a frame (the
    game's state out: actors, bones, car bodies; results back: poses, forces) and calls both ways for what is rare;
    Direct3D 11 textures shared across the processes, so a 64-bit side can draw or simulate into the frame. The
    first user and the test: PhysX 5 stepping a body the game shows, with a frame's round trip measured; the design
    is fixed against that measurement, not before. Whether the VR runtimes (OpenXR, OpenVR) take a 32-bit process
    is to be checked: OpenVR ships a 32-bit library; if OpenXR's runtimes do not, VR is the sidecar's second user.

### Tier 5: the agent's own tools (to write and debug those mods)

20. **Waiting for what happens** (A). The bridge keeps the last events (item 7's, missions, actors destroyed,
    programs ended, mods faulted) with a sequence number; `events.since {seq}`, `events.wait {type, filter,
    timeout_ms}`, MCP `wait` ("until the mission is loaded", "until he dies").
21. **`native.call`** (A). Any game function called from the bridge by its address and convention (thiscall,
    stdcall, cdecl, fastcall; integer, float, pointer and string arguments; a fault guarded): a hypothesis in one call.
22. **Hardware watchpoints** (A; from Later). Who writes or reads a field, on the bridge and MCP; the handler never
    reads the watched address (a recursion crashed the game once). Reference: `hwwatch.cpp`.
23. **Tests in the real game, as scripts of steps** (A). Python over the bridge (`tests/game/`): start, spawn,
    teleport, input, wait for an event, check a value, screenshot; pass or fail. First cases from the measuring mods:
    a car hits a person, a body falls into the river, an exhaust in a tunnel, a camera turned, a prop held in a hand.
24. **Structures by name** (A). `inspect <addr> as C_human` (and other classes): fields named and typed from
    `game.h` and the decompile's layouts, kept in `knowledge/` as tables of offsets.
25. **Crash triage in one call** (A; from Later): the named stack, the log's last lines and the last taps' calls.
26. **Orders for the people made** (A, for test scenes). Walk or drive to a place, follow, attack, flee: the AI's own
    jobs and the mission commands that give them, as `world.*`; the scripts' events (`setplayerfireevent`...) to a
    program of the loader's; people's models listed.

## Later

- **State snapshots for experiments** (a car's state is there: `car.save` / `car.restore`): the game's own save and
  load used as a restore point, to repeat an experiment from the same moment.
- **Rust mods**: a crate `mml` over `mml.h` (types, a safe wrapper, a `#[mml_mod]` entry macro), an example, built
  for `i686-pc-windows-msvc`.
- **The knowledge base for 1.0**: only 1.2's decompile is imported; 1.0 addresses are named through their 1.2 twins,
  and 1.0's menus (which have no twins) are read in st0rm94's annotated decompile (`re/ext/...`): import its names
  for 1.0 (`kb_search` on a 1.0 address).
- **Ghidra behind MCP** (mafia-ragdoll's `ghq`): xrefs, renames, comments written into the project; `kb_function`
  already gives the decompile, so only when cross-references by hand become the slow part.
- **Publishing**: CI for the offline tests.

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
  missing, calling any game function by its address, is item 21. Parked until someone wants mods written in Lua
  (`tools/deps.py` still pins LuaJIT): to be confirmed with the owner.
- **"The 64-bit sidecar"** was taken out as having no user, and put back the same day by the owner: modern libraries
  (the latest PhysX first) exist only for 64 bits; item 19.

2026-10-14, the second (the owner: the toolkit for a ragdoll, water, smoke and VR mod; old tasks kept, duplicates
merged, sorted by importance):

- **"mafia-ragdoll on the loader"** (Later): taken out. The ragdoll is not ported; what its host and core do for
  themselves became items 1, 3 to 9 and 11 as services for any mod, mafia-ragdoll's code their reference.
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

## Known gaps

- **fps_unlock** (mods/examples/fps_unlock): only the main menu's camera is fixed; the other systems that may do once
  a frame what they should do by the time are leads from a scan of the decompile (`knowledge/notes/game.md`, "The
  frame rate"), not checked in the game; the owner remembers more breakages above 62.5 fps that are not found yet.
  Frames of 0 ms (above 1000 fps) reach every system (532 a second in 1.2's main menu at 1520 fps).

- **A mod's unload** (host/lifecycle.cpp): a thread is found in a load's code by its EIP, the chain of saved EBPs and
  where it began; a thread inside a library compiled to use EBP as a plain register, between it and the mod's frame,
  is not seen (its DLL is freed, as before 2026-10-14). Only the top level of a mod's folder is copied (`*.dll`); a
  library a mod loads later by a bare name is not searched in its load's folder (load by the full path). Objects of
  the game that point into a mod's code (a callback it gave the game without the loader) are not found. The
  `since_load` numbers are the whole process's over the load's life: the game's own changes are in them.

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
- **Drawing for mods**: nothing is drawn while the game loads or plays its videos (no 2D pass, no loader frames).
  Text only in the game's three fonts and its encoding, about 100 characters a call; no pictures, no clipping, no
  rotation; always on top of the HUD and the menus. A cutscene's black bars are not known to it. Under plain
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
- **Settings**: no schema (a mod checks its own values); the files are polled twice a second.
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
- **1.0's bullets, getting up and trams** (`game_build.h`'s mafia-ragdoll fields) are zeros for 1.0: the loader does
  not use them; mafia-ragdoll's own port has them.
- **`tools/new_mod.py`** was tried by hand (a mod made, built, loaded and seen in the lab); the offline tests do
  not run it.
- **`menu.command` sends any integer**: a command the active menu does not expect may do anything that menu's
  handler does with it. Read the handler first (`menu.state` names it; `kb_function`).
- **The profile list exists only while the profile menu is open** (the game releases it when the menu closes):
  `menu.state` lists the profiles only then.
- **The intro videos of missions are not skipped** (only the three logos before the profile menu).
- **Knowledge base names**: a few names imported from the decompile keep calling-convention marks from its
  signatures (`GM_Menu::ExecuteMenu@<eax>`); names of ours for menus (`GM_MainMenu::OnCommand`...) are ours, not the
  game's. LS3DF.dll has few names (64 of 1599).
- **`game_build.h` has diverged from mafia-ragdoll's copy** (the start's, the menus', the world's and 1.0's
  fields): the two are kept apart (mafia-ragdoll is not ported; its copy is changed only with the owner's
  go-ahead). An address found in one is checked in the other before use.
- **`tap`**: at most 16 taps, 8 stack arguments, 4 snapshots of up to 64 bytes each; a function whose first bytes
  MinHook cannot relocate cannot be tapped.
- **The exit crash** seen in mafia-ragdoll (LS3DF's index buffer manager at exit, `knowledge/notes/game.md`) is not
  solved; it has not been seen with the loader alone yet.
- **32-bit address space**: the game and every mod share 4 GB (2 GB without the large-address flag); hot reloads
  leak a little address space each (seen in mafia-ragdoll: restart after very many).
