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
  distance on foot and in vehicles with teleports left out, the time played, money gained and lost; the game's in
  its saves, the profile's beside its .sav under the owner "mml"), `profile_progress` (the profile manager's own:
  the chapter of the newest story save, the race, steal and freeride bonus-car letters, the garage, the freeride's
  level word), `profile_saves`; `stats.state`, `profile.progress`. Seen alike in the 1.2 and 1.0 labs: profile 4
  (chapter 20, 105 saves, "ABCDEFGHIJPR" bonus cars) and the 1.0 lab's "Run" (chapter 6, French save names); a kill
  with the Thompson (and its hits and shots), a fall of 8 m, a crash into a wall, items given, money set; the
  profile's totals kept after a restart, the freeride's game counts back at 0.
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
  1.2); a mod's fault with its stack (log, `MOD_FAULTED`). MCP `crash_triage`: the verdict (what, where, the nearest
  code not the game's: a mod, the loader, another ASI), the named stack, the access, the registers, the log, the taps,
  the events, the mods; with the game running, the mods that faulted in this run. Seen in both labs on a crash made on
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
- **People, skeletons, poses** (2026-10-16; item 6, block 6a): the host keeps every `C_human` (`humans_list`,
  `human_player`), a person's skeleton by the bones' names (`human_skeleton`: 18 joints, the pelvis, the root), a
  pose written for the frame's drawing (`bone_set` in `RENDER_BEGIN`, each posed model updated once after);
  `humans.list`, `human.skeleton`, `human.pose` (held for a time). The example `mods/examples/puppet` (a wave, a
  T-pose) seen in both labs on the screen and in a scene view's mirror; offline tests with no game.

## Next, by importance

**The goal** (the owner, 2026-10-14): a toolkit strong enough for whatever mod a user can think of, written by another
agent. The measuring cases span the range: engine-deep mods (**a ragdoll, shader water, realistic smoke, VR**) and
gameplay mods (**fishing, skateboarding, a working airplane, achievements**). None of them is written here, and
mafia-ragdoll is not ported: it already has a ragdoll (Jolt), water and smoke (a Direct3D 11 bridge, PhysX 2.8.4,
NVIDIA Flow) on a host of its own, and its code is the reference for what several services below must do
(its `src/`, read only). Every service lands with an example mod (`mods/examples`), a section in
`docs/MODDING.md` and its offline test, and is seen working in both labs, 1.2 and 1.0, before it is done (the
builds' decision; AGENTS.md "Builds"). Each item names who needs it (R ragdoll, W water, S smoke,
V VR, F fishing, K skateboard, P airplane, C achievements, A the agents themselves) and its reference.

**The order of work** (the owner, 2026-10-16; one block at a time, each seen in both labs): the functions themselves
matter, not example mods for them (no "fishing" example for items 15 and 16).
1. Item 17, a mod's state in the game's saves.
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
10. Mod languages (Later: the Rust crate; LuaJIT script mods).
11. Orders for the people made (item 26).
12. Tests in the real game, as scripts of steps (item 23).
13. *(Done 2026-10-17, before 8 at the owner's wish; see "Done".)* The tuning panel (item 18).

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
27. **Shaders on the game's own drawing** (W S V and every mod that changes the look: per-pixel light, normal maps,
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
8. *(8a done 2026-10-16: the questions in `mml.h` (a ray, a sweep, the actors near a point, the collision's shapes, the
   ground under a point) answered by the game's own tests; the water's height (6a), the files each model came from and
   the `.4ds` read (6c), the drawn triangles around a place and cars' meshes posed (6d); see "Done". What is left:
  Known gaps.)* **The world's geometry and questions about it** (R S W K P F). The file each loaded model came from (hooked from
   the process's start: only the loader is there that early); the triangles of what the game draws around a point
   (cached, fed a few pieces a tick), the collision grids (static primitives, dynamic objects with their owners),
   cars' meshes posed with their parts, an index of every `.4ds` in the archives and its parser; one scan shared by
   every mod. Questions in `mml.h` (not only C++'s `game.h`): a ray, a sweep, what overlaps a box or sphere, the
   ground's and the water's height under a point, the actors near a point. Reference: `scenery.cpp`,
   `visual_meshes.cpp`, `collision.cpp`, `game_collision.cpp`, `model_index.cpp`, `model4ds.cpp`, `car_meshes.cpp`.
9. *(9a done 2026-10-16: the frame drawn from a mod's camera, a world point on the screen and back; 8a of the order
   2026-10-17: the game's camera modes and a field of view it keeps; see "Done". Left: its distance, an aim from
   another camera: Known gaps.)* **The camera** (V R K P). The game's camera read and set (position, orientation, field of view, near/far), its
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
14. *(14a done 2026-10-16: keys and actions kept from the game, the player locked, a car's physics replaced; 2026-10-17
    a mod's keys on its settings page and the game's airplane flown by a mod; see "Done". Left: actions in the game's
    controls menu, riding a vehicle that is not a car; Known gaps.)* **Control** (K P F V). A mod's keys and buttons taken so the game does not also act on them; actions of a mod's
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
21. *(Done 2026-10-16: see "Done".)* **`native.call`** (A). Any game function called from the bridge by its address and convention (thiscall,
    stdcall, cdecl, fastcall; integer, float, pointer and string arguments; a fault guarded): a hypothesis in one call.
22. *(Done 2026-10-16: see "Done".)* **Hardware watchpoints** (A; from Later). Who writes or reads a field, on the bridge and MCP; the handler never
    reads the watched address (a recursion crashed the game once). Reference: `hwwatch.cpp`.
23. **Tests in the real game, as scripts of steps** (A). Python over the bridge (`tests/game/`): start, spawn,
    teleport, input, wait for an event, check a value, screenshot; pass or fail. First cases from the measuring mods:
    a car hits a person, a body falls into the river, an exhaust in a tunnel, a camera turned, a prop held in a hand.
24. *(Done 2026-10-17: see "Done".)* **Structures by name** (A). `inspect <addr> as C_human` (and other classes):
    fields named and typed from `game.h` and the decompile's layouts, kept in `knowledge/` as tables of offsets.
25. *(Done 2026-10-17: see "Done".)* **Crash triage in one call** (A; from Later): the named stack, the log's last
    lines and the last taps' calls.
26. **Orders for the people made** (A, for test scenes). Walk or drive to a place, follow, attack, flee: the AI's own
    jobs and the mission commands that give them, as `world.*`; the scripts' events (`setplayerfireevent`...) to a
    program of the loader's; people's models listed.

## Later

- *(Done 2026-10-17, block 9c: restore points; see "Done".)* **State snapshots for experiments** (a car's state is
  there: `car.save` / `car.restore`): the game's own save and load used as a restore point, to repeat an experiment
  from the same moment.
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

- **Restore points** (host/saves.cpp, `restore`): a point is the game's own save, so it holds what the game saves:
  what it does not (a sound playing, a particle in the air, the camera's smoothing, a mod's state it did not put at
  GAME_SAVING) is not put back; mods are told GAME_LOADED with slot 599. Made in a mission in play only (not in the
  main menu or the pause menu); a load waits for the pause menu to close. Saving in the middle of a cutscene, a car
  chase's scripted part or a race was not tried; neither were the extreme freeride and the racing. A point is of its
  build (1.2's will not load in 1.0) and is loaded into the profile selected. 1.0's slot 599 is known free only
  because its missions are 1.2's (its texts do not name the freerides' saves): a save found there stops the points.
- **Crash triage** (host/crash.cpp, `crash_triage`): the stack is a scan checked by the calls' targets, not an
  unwind: an indirect call (a vtable's, a pointer's) cannot be checked, so a leftover after one may stay (seen: an
  LS3DF address between C_game::Tick and C_mission::Tick); a function longer than 32 KiB would lose its caller. The
  game's own frames are named through the knowledge base (1.0's through their 1.2 twins); LS3DF 1.0's and the mods'
  are module+offset (no PDB read). A crash on a thread whose stack the filter cannot see (a stack overflow's guard
  page) has only the faulting address. The full dump fails with a partial copy (0x8007012B) in the stand-in game and
  sometimes in 1.2: it is written again without the heaps, so an object a crash points into may be missing. A hang
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
- **The sun's shadow and the game's lights** (host/shadows.cpp, block 7c): the casters are block 6d's meshes drawn
  by the host, not what the game draws: people cast no shadow (skinned on the CPU at their draw), billboards (the
  countryside's far forest) none, and alpha-tested faces (leaves, fences) cast as whole quads (a tree's shadow is a
  solid star: seen 2026-10-17). The proper way is built and kept as an option (`shader.shadows {"casters": "game"}`,
  2026-10-17): LS3D draws the scene from the sun with its own orthographic camera (I3D_camera's SetOrthogonal, vtable
  +0x64, and SetOrthoScale +0x60; 1000B930 and 1000B8D0 in 1.2's LS3DF; its update builds a box frustum for it), each
  face group written as its depth with the game's alpha test kept (leaves cut out: seen), the sky and what lets light
  through left out, LS3D's clear made "nothing"; the map read with the drawing's own matrices (the ground in the map
  matched the ground: measured). What stops it: LS3D leaves out most objects from such a camera (101 face groups,
  1953 triangles with its eye 250 m away; 63 with it 60 m away; the meshes' way finds about 200 and 10 000; houses,
  cars and people missing: measured in both builds, the same with a perspective camera at that pose), so it is not the
  default. Next: where LS3D's drawing drops a visual for a camera (its LOD by distance and size on the screen, its
  sectors' visibility), read in Render's visibility pass. One map of a fixed extent around the camera (no
  cascades: past it, lit); the box of the scene's meshes moves when the camera has gone a quarter of it, and a mesh
  first met is in the map from meshes.cpp's next scan (0.25 s). The scene views (mirrors) sample the frame's map
  (drawn around the game camera, not the view's eye); not looked at in a view. The lights a draw gets are the eight
  nearest its visual's bounding sphere's centre among those reaching it: a long piece (a street) may get far lamps
  before a near one. Seen in the city's freerides only (day and night), not in the rain, the countryside, the
  interiors or a mission. **The headlights' cone on the street** the game draws at night is not one of the face
  groups LS3D draws (the night's survey has every face group at pass 0, no second pass), so no rule takes it: what
  draws it was not found (left for later).
- **The sun's shafts** (mods/examples/sun_shafts): no rays past `distance` (the map's extent; the haze beyond is
  even); the rays through trees are as solid as their shadows (above); single scattering only; translucent things
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
  counted. The profile's totals of the last 10 s are lost when the game is killed (it has no exit the loader hears).
  The saves of a story game carrying the game's counts were not loaded in a test (the freeride's start was: 0). The
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
  handed a get-up (they are not C_humans: mafia-ragdoll's getup.cpp shows their way, pedAnimStart), and they are not
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
