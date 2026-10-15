# Roadmap

What is done, what comes next and later, and what is known to be missing or wrong. Agreed with the owner on
2026-10-13, reordered on 2026-10-14 for the toolkit's goal ("Next"); agents keep it current (move items to Done with the date, add what they find to Known gaps).

## Decisions (the owner's)

- The host is a `.asi` loaded by Ultimate ASI Loader (`dinput8.dll`); it never unloads. Mods hot-reload.
- The MCP server is a separate process (Rust), talking to the game over the bridge; it outlives the game.
- Builds: 1.2 and 1.0 (per-build tables in `game_build.h`). The owner, 2026-10-15: the loader and the SDK must
  work on both, so everything for both is tested in both labs (at the end of a block of work, or at once where 1.0's
  code differs; not every small test); a mod made for one build is worked on and tested in that build's lab only
  (`"builds"` in its `mod.json`). The labs are listed in `mml.config.json` (`labs`); agents switch between them.
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
- **The scene from another camera** (2026-10-15; item 4, block 4a): `scene_view` (up to eight views a process): LS3D
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

## Next, by importance

**The goal** (the owner, 2026-10-14): a toolkit strong enough for whatever mod a user can think of, written by another
agent. The measuring cases span the range: engine-deep mods (**a ragdoll, shader water, realistic smoke, VR**) and
gameplay mods (**fishing, skateboarding, a working airplane, achievements**). None of them is written here, and
mafia-ragdoll is not ported: it already has a ragdoll (Jolt), water and smoke (a Direct3D 11 bridge, PhysX 2.8.4,
NVIDIA Flow) on a host of its own, and its code is the reference for what several services below must do
(`W:\github\mafia-ragdoll\src`, read only). Every service lands with an example mod (`mods/examples`), a section in
`docs/MODDING.md` and its offline test, and is seen working in both labs, 1.2 and 1.0, before it is done (the
builds' decision; AGENTS.md "Builds"). Each item names who needs it (R ragdoll, W water, S smoke,
V VR, F fishing, K skateboard, P airplane, C achievements, A the agents themselves) and its reference.

### Tier 1: foundations every large mod stands on

1. *(Done 2026-10-15: a heavy mod's lifecycle, the loader's part and the build's part; see "Done". The number is
   kept so the items' numbers stay.)*
2. *(Done 2026-10-15: a mod's own files in the game; see "Done".)*
3. **Render events and the graphics bridge** (W S V; R for posing). In three blocks: *3a, the render events*, *3b, the bridge*, *3c, the 2D pass for mods* (done
   2026-10-15, see "Done"; what is left: Known gaps). As it was asked: the game's Direct3D 9Ex device (dxwrapper);
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

20. *(Done 2026-10-16: see "Done".)* **Waiting for what happens** (A). The bridge keeps the last events (item 7's, missions, actors destroyed,
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

- **A mod's own files** (host/files.cpp): seen in the 1.2 lab with a menu's picture only; a model, a sound, an
  animation and a table of a mod's were not loaded in the game (they come through the same `dtaOpen`: about 720
  models, 240 sounds and 50 tables a freeride's load); in the 1.0 lab only the menu's picture was seen. A table the game reads only at its start, and which tables those are, was not listed.
  What opens files without rw_data is not answered: the Bink videos (binkw32 opens its own), the saves
  (`dtaOpenWrite`, not hooked). A name longer than 259 characters with the mod's folder is left out (rw_data copies
  names into MAX_PATH bytes). The build's `files\` copy and `deploy` add and replace files, never delete one that
  is gone from the source (clean `build/x86/out` or the game's copy by hand). The presaved textures are recognised
  by their names' form as LS3DF 1.2 builds them (read in its decompile); 1.0's LS3DF asked the same name for the
  menu's picture (its code not read).
- **The graphics bridge** (host/gfx.cpp): left of item 3 besides 3c: the scene's own light (LS3D lights its world
  without Direct3D lights: the sun is a default from above, the ambient light a grey; mafia-ragdoll's `scene_light`
  reads the scene's lights), antialiasing of our own (a mod with `"gfx": true` turns the game's multisampling off),
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
