# Roadmap

What is done, what comes next and later, and what is known to be missing or wrong. Agreed with the owner on
2026-10-13; agents keep it current (move items to Done with the date, add what they find to Known gaps).

## Decisions (the owner's)

- The host is a `.asi` loaded by Ultimate ASI Loader (`dinput8.dll`); it never unloads. Mods hot-reload.
- The MCP server is a separate process (Rust), talking to the game over the bridge; it outlives the game.
- Builds: 1.2 first, 1.0 too (per-build tables in `game_build.h`).
- Mods in any native language through one C ABI, plus LuaJIT scripts; heavy logic may go to a 64-bit sidecar.
- Agents have full access, and may launch, drive and close the game themselves, from inside (never the owner's
  real mouse or keyboard).
- The repository is English; it is a private GitHub repository, PashOK7/MafiaModLoader (2026-10-13). Pushing,
  publishing and pull requests: ask the owner first.
- The lab (`W:\MafiaLAB`) is shared with mafia-ragdoll (`tools/lab.py mode loader|ragdoll`).
- The loader is measured by how easily another agent writes a mod with it (2026-10-13). The owner's older mods
  (TrackMafia: lap telemetry, a ghost car, an overlay; `knowledge/notes/game.md`, "Cars") are not ported here: they
  show what every such mod had to build for itself, and so what the loader must give instead.

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

## Next, in order

1. **The engine's own commands from MCP.** The mission scripts' interpreter (`C_program::DecodeInstruction`
   0x463960, `C_program::Process` 0x46D3B0) knows hundreds of commands (`HUMAN_*`, `PLAYER_*`, `CAR_*`,
   `ACTOR_SETPOS`, weather, time). Run one at a time from the bridge (a small program given to the interpreter, or
   the command handlers called directly): teleport, spawn a car or a person, give a weapon, kill or knock someone
   down, set the time of day. A developer console built on the game's own mechanisms.
2. **LuaJIT script mods** (`type: "lua"`): a runtime mod embedding LuaJIT (pinned by `tools/deps.py`), one Lua state
   per script mod, hot reload on change; the C API through LuaJIT's FFI (`ffi.cdef` of `mml.h`), so scripts read and
   write memory, describe the game's structures and call its functions; `lua.eval` for agents (a hypothesis tested
   in five lines, without building a DLL).
3. **Scenarios: tests in the real game.** A file of steps (start, teleport, spawn, input, wait, check a value or an
   event, screenshot) run by the loader; a pass/fail report through MCP. First user: mafia-ragdoll's cases (a car
   hits a person getting up, a body thrown into a tram).
4. **The start, completed.** Missions and saves through the load menu (the main menu's 990104, then the chapter or
   save chosen: `SaveGameSelect` / `SaveGameLoad`); the other freerides (`freeride`, `freeridenoc`, `freekrajina`,
   `extreme`: the freeride menu's kind menu (command 100) and the spawn menu its start opens for kinds other than
   28); a new game (990103). The friend's reverse of the game's interface is in
   `knowledge/notes/interface.md` (the menus' input tick and cursor, the controls by vtable, the profiles'
   functions).
5. **Structures by name.** `inspect <addr> as C_human` (and other classes): fields named and typed from `game.h`
   and the decompile's layouts, kept in `knowledge/` as tables of offsets.

## Later

- **Events to MCP as they happen**: deaths, hits, missions opened and closed, mods faulting, as a stream an agent
  can wait on ("wait until a person dies", "wait until the mission is loaded").
- **Hardware watchpoints** (`hwwatch` from mafia-ragdoll): who writes or reads a field, on the bridge and MCP. Its
  handler must never read the watched address (a recursion crashed the game once).
- **Ghidra behind MCP**: the query server `ghq` (mafia-ragdoll's `tools/ghq.py`) as MCP tools: decompile, xrefs,
  rename and comment (findings written into the project), against the Ghidra projects of both builds.
- **State snapshots for experiments** (a car's state is there: `car.save` / `car.restore`, `game::CarStateSave`):
  the game's own save and load used as a restore point, to repeat an
  experiment from the same moment.
- **Crash triage in one call**: the named stack, the log's last lines and the last taps' calls together.
- **Profiling**: the game thread's sampling profiler and per-part timings (mafia-ragdoll's `sample`, `prof`).
- **Rust mods**: a crate `mml` over `mml.h` (types, a safe wrapper, a `#[mml_mod]` entry macro), an example, built
  for `i686-pc-windows-msvc`.
- **The 64-bit sidecar**: `mml-sidecar64.exe`, started by the host, loads 64-bit mods (heavy logic past the 32-bit
  address space); shared-memory channels (lock-free rings, bulk blocks for poses and meshes); services that cross
  the process boundary.
- **mafia-ragdoll on the loader**: its host's duties taken by the loader, the ragdoll a mod; the physics published
  as a service (for the friend's VR mod: one SDK, two mods, shared services).
- **1.0**: the start's, the menus' and the controls' addresses for 1.0 (`game_build.h` has zeros there now; the
  kit's are in, by code match), everything run on the 1.0 lab copy (`W:\Mafia10LAB`); the knowledge base for 1.0
  (only 1.2's decompile is imported).
- **Publishing**: CI for the offline tests (the private repository exists since 2026-10-13).

## Known gaps

- **The start covers the default freeride and the main menu only** (see Next, 4). Without `profile` the profile
  menu waits for the player. The start runs only for 1.2. The freeride's densities count only for a profile that
  has opened the freeride's settings (the log warns).
- **Input**: 1.2 only. The menus' mouse is driven only while a menu is open (outside one, the mouse is the player's
  aim: use `turn` and `Aim X`).
  `turn` is exact on foot only: the car's and the free look's cameras have their own code, and `Aim Y`'s scale is
  not measured (down is positive). The driving actions' names come from the code that reads them (0x5917F0), not
  from the menu's rows; slot 32 is read by nothing found. A hold is counted in the controls' updates: the game's
  loop and the menus' loops, not the loading screens.
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
  press twice. Hotkeys also fire in the menus. Actions (`action_*`) are known for 1.2 only.
- **The car**: `Car::Damage` is the engine's health only (the body's dents are not a number). `CarStateLoad` puts a
  state back into the same car or the same model; it was tried on the car the player drives (standing and moving),
  not on traffic or racing cars, and a state is not kept across missions. `LastCar` is the HUD's pointer: whether
  the game clears it when that car is destroyed was not looked at (read it through `mem_read`, as `call car` does).
  TrackMafia's ghost-car pointer and its recipe of copying the car's memory are not offered (see
  `knowledge/notes/game.md`, "Cars").
- **The mission's events**: a pause is told only when a menu stops the world (cutscenes and the map are not
  events); `MISSION_LOADED` is the first tick, not a flag of the game's.
- **Settings**: no schema (a mod checks its own values); the files are polled twice a second.
- **The game's menus, extended**: 1.2 only (1.0's menu code is built differently; its addresses are zeros; LS3D's
  mouse read is known in 1.0 too, by code). The pause menu and the main menu were extended in the lab; items in
  other menus (and in a scrolled list such as the load menu's) were not tried. The kinds `edin`, `sldi`, `rari`,
  `list`, `lstc`, `vscr`, `comb`, `cmbw`, `grid`, `vidc`, `mtxt`, `htxt`, `ntxt`, `stxb`, `tinp` are made from a
  description but their data and values are not offered (`menu.def` shows where the game uses them). Containers do
  not nest (the game's "end" returns to the page's window). Whether the engine reads a picture's file again
  after it changed on disk (or keeps the texture it made) was not looked at. The wheel does not scroll the game's lists
  (the game gives it to the focused control). `input.text` types the US layout's characters only. Texts are interned and never freed (a mod that sets texts every frame grows the
  set). One page a mod at a time; a page closed by Esc lives until the game deletes it, so a `menu_open` in the
  same frame as the close is refused. An item copies its neighbour's text id: a language change re-reading texts
  by id was not looked at.
- **1.0 and the kit**: the kit's addresses and offsets for 1.0 are the twins of the 1.2 code, found by matching the
  code and checked against the 1.0 files (`tools/check_builds.py`); nothing of it was run in a 1.0 game (the lab
  in `mml.config.json` is 1.2). The start, the menus and the actions are still not known for 1.0.
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
- **`game_build.h` has diverged from mafia-ragdoll's copy** (the start's fields): when the ragdoll moves onto the
  loader there is one copy again; until then changes go to both.
- **`tap`**: at most 16 taps, 8 stack arguments, 4 snapshots of up to 64 bytes each; a function whose first bytes
  MinHook cannot relocate cannot be tapped.
- **The exit crash** seen in mafia-ragdoll (LS3DF's index buffer manager at exit, `knowledge/notes/game.md`) is not
  solved; it has not been seen with the loader alone yet.
- **32-bit address space**: the game and every mod share 4 GB (2 GB without the large-address flag); hot reloads
  leak a little address space each (seen in mafia-ragdoll: restart after very many).
