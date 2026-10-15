# MafiaModLoader: guide for AI agents

A mod loader for **Mafia: The City of Lost Heaven** (2002; builds 1.2 and 1.0) made for agents first: an MCP server
gives you the live game (its memory, its objects, the loader's log, its crashes), every mod hot-reloads, and the
game can be started, queried and closed without a human at the keyboard. Read this file, then
`docs/ARCHITECTURE.md`, `docs/PROTOCOL.md`, `docs/MODDING.md`, `docs/ROADMAP.md` (what is next, what is known to be
missing) and the end of `docs/JOURNAL.md`. Keep the roadmap and the journal current as you work.

## Rules

- **Only the game folders in `mml.config.json`** (the user's own file, not in the repository): `game_dir`, the one
  in use, and `labs`, one folder a build (`{"1.2": ..., "1.0": ...}`). Never search the disk for other installs; a
  build with no lab listed: ask the user for its folder. Switching between the listed labs is the agent's to do as
  the work needs (`python tools/lab.py use 1.0`, then `mode loader` if `status` says so); put `game_dir` back where it
  was when the work is done.
- **Builds: what the work is for says where it is tested.** The loader, the SDK (`mml.h`, `game_build.h`,
  `game.h`), the MCP server and every shipped mod (`mods/core`, `mods/examples`) are for both builds, 1.2 and 1.0;
  a mod is for the builds its `mod.json` names in `"builds"` (absent: both). Work for both builds is seen working in
  both labs before it is called done: not every small step, but at the end of a block of work (a roadmap item, a
  feature, before a commit), and at once where 1.0's code differs (another address, layout or function: read in
  both builds' code, `docs/VERSIONS.md`). Work for one build stays in that build's lab: no switching, no testing
  in the other. "Not checked in 1.0" is not an end state for work for both builds: it is the next step.
- **The lab may be shared** with other projects (mafia-ragdoll's developer host is the game's `IJoy.dll` there).
  `python tools/lab.py status` tells which mode it is in; `mode loader` / `mode ragdoll` switch, keeping what they
  take out in `<game>\mml_lab\` and the saves in `<game>-backup\savegame-<date>`. Never delete another project's
  files; put the lab back in the mode the user uses when you are done.
- **The game may be driven by agents, from inside**: launch and close it (`game_launch`, `game_close`, by its exact
  pid), query and change its state through the bridge. Never send the user's real mouse or keyboard input.
- **Never commit game files, extracted assets, dumps or decompiles.** `re/` and `*.dmp` are ignored.
- **Correct over quick**: use the engine's own functions and hook points, verify every address in the disassembly,
  say plainly what is a compromise.
- **Doubt the tools.** When a tool here answers something the game contradicts (an input that "did not work", a
  value that cannot be), do not work around it and do not blame the game: read the code that is involved
  (`kb_function`, `tap`, the disassembly) until the reason is known, and write it down (`knowledge/notes/`, the
  journal). If the tool is at fault, say so to the user and, with their go-ahead, fix the tool in this repository
  (a test with it); what is not fixed now goes to `docs/ROADMAP.md` "Known gaps" (and, once the repository is on
  GitHub, an issue; a fix from another repository's work comes as a pull request).
- **Language**: everything in the repository is English. Talk to the user in the language they use.
- **Ask the user before**: pushing, publishing, opening PRs, anything outside the game folder and this repository.

## The loop

```bat
cmake --preset x86                       :: once
cmake --build --preset x86-release       :: the loader (build/x86/out/MafiaModLoader.asi) and the mods (build/x86/out/mml/mods/<id>/)
python tests/run_tests.py                :: offline: a stand-in game loads the loader; hook chains, faults, reloads... ALL PASSED
cargo build --release --manifest-path mcp/Cargo.toml   :: the MCP server
python tests/mcp_smoke.py                :: the MCP server against the stand-in game
python tools/lab.py mode loader          :: the lab gets the loader (first time: python tools/deps.py)
```

Then, through MCP (`.mcp.json` registers the server as `mafia`): `game_launch` → `game_status` → `call` /
`mem_read` → edit a mod → build → `deploy` (copies and reloads the mods in the running game) → `call` again →
`log_tail`. The loader itself (`MafiaModLoader.asi`) changes only with the game closed (`game_close`, `deploy`,
`game_launch`).

**Into the game in seconds** (both builds; in the background by default: the game's window is not activated, the
user keeps working in front): `game_launch {"profile": 4, "go": "freeride", "police": 0}` (or `"go": "menu"`) skips
the logos and takes the game through its own menus; it returns once the player exists. `"police": 0` is the freeride
menu's slider: a city without patrols (the player starts with a Thompson in his hands, and patrols shoot at that).
The other freerides (`"kind": "city-night"`, `"countryside"`... with `"place": "Hoboken"`: the game has no time of day,
night is a kind), a chapter of the story (`"go": "load", "save": "Old Prison"` or its number), `"new"`, `"extreme"`.
The same from a terminal: `python tools/start.py freeride --profile 4 --police 0`. Other menus: `call menu.state`,
`call menu.command {command}`, `call menu.value {id, value}`.
**Eyes and hands**: `screenshot` returns the game's own frame as a picture (the window may be hidden or behind
others); `input` plays a timed sequence inside the game: the game's actions by name, whatever they are bound to
(`{"action": "Forwards", "ms": 2000}`, `"Accelerate"`, `"Fire"`, `"Action"`), an exact turn on foot
(`{"turn": 90}`), keys for the menus (`{"key": "Esc"}`), the menus' mouse (`{"click": [x, y]}`, `{"wheel": -3}`);
`"screenshot": true` shows the result. `call menu.click {"id": 102}` clicks a menu's control by its id (safer than
coordinates), `call input.text {"text": "Tommy"}` types. **In the world**: `spawn {"car": "Bolt-Thrower"}` or
`{"human": "Paulie", "in": "car"}` makes a car or a person of any model (the game's own functions), `teleport {actor,
to}` moves one, `give {actor, item: "Thompson 1928"}` arms by name (`call world.cars`, `world.items` list the names;
`world.seat`, `world.remove`). `script {code, act, flt, owner}` runs a
program on the game's own script machine (the missions' language: teleport, spawn copies, weapons, deaths, cars,
weather, traffic, camera; docs/SCRIPTS.md has the recipes), `script_commands {query}` tells a command's arguments. It returns the
player's state when the last step has ended. `call input.actions` lists every action and its binding.
**Watch any function** with `tap` (`"5409D0 n=4 ptr=1:32"`, let the game do the thing, `"show 0"`): the fastest way
to learn what the game does. **Wait for what happens** with `wait {"types": "MISSION_LOADED"}` (or `HUMAN_HIT` with
`"filter": {"killed": true}`, `SCRIPT_ENDED`, `MOD_FAULTED`...; `call events.since` lists what came) instead of
sleeping. **Where the time goes**: `profile {seconds}` (each mod's cost by site, the game thread sampled, named).
**Writing a mod**: `python tools/new_mod.py <id>`, then `docs/MODDING.md` (the whole way; `mods/examples/telemetry`
is a complete one). The host gives a mod the frame's drawing (text, lines, rectangles in the game's fonts), the
keys and the game's actions, hotkeys, the car by name, the mission as events, a data folder and settings, and the
game's own menus extended (an item in the pause menu, a page of its own made of the game's real controls): a mod
needs no thread, window or Windows hook of its own. Try things without a mod: `call draw.text {"x": 400, "y": 100,
"text": "hello", "size": 30, "ms": 10000}`, `call car`, `call config.set {"mod", "key", "value"}`,
`call menu.item {"menu": "gamemenu", "after": 108, "text": "Hello"}` (then `Esc` and `call menu.state`).

## Knowing the game

**Ask the knowledge base first** (`knowledge/README.md`): `kb_search` finds functions of Game.exe and LS3DF.dll
(1.2) by name, class, signature or a string they use, our own names and the notes; `kb_function` gives one function's
facts, callers, callees and its decompiled code (read from a reverse folder of full decompiles, `reverse_dir` in
`mml.config.json`; never committed). `crash_inspect` and `where` name the functions they show. Then check in the
running game (`mem_read`, `call`), and write what you learned back: `knowledge/<module>/names.csv`,
`knowledge/notes/`, addresses code uses in `game_build.h`. **Others have reversed much of the game** (multiplayer
mods for 1.2, a 1.0 reverse with 114 class documents, LS3D's interfaces): their names are in the knowledge base
(`external.csv`, source named), the scripts' commands in `knowledge/game.exe/script_commands.csv`, and
`knowledge/notes/external.md` says where to read what, by topic (`re/ext/<project>/...`): read there before
reversing a system anew.

## Map

| Path | What |
|---|---|
| `host/` | `MafiaModLoader.asi`: the never-unloaded host: game hooks → events (the mission's too), hook chains, mods, bridge, crash dumps, `tap`, the start (`start.cpp`: logos, profile, menus), input (`input.cpp`: actions, keys, the mods' keys and hotkeys), screenshots (`screenshot.cpp`), drawing (`draw.cpp`: the game's own 2D drawing for mods and `draw.*`), the game's menus extended (`menus.cpp`: items and pages for mods, `menu.item`, `menu.page`; 1.0's screens: `menus10.cpp`), a mod's data folder and settings (`config.cpp`), the game's script machine for agents and mods (`script.cpp`: `script.run`, `script_run`), the world (`world.cpp`: actors made new, moved, seated, armed; the car list and the items), the frame's drawing as events (`render.cpp`: `I3D_driver::Render`, stage 1, Present; `render.state`), the graphics bridge (`gfx.cpp`: Direct3D 11 passes of mods over the frame; `gfx.state`), the scene drawn again from mods' cameras (`view.cpp`: reflections, mirrors, a headset's eyes; `view.state`, `view.show`), the game's own geometry hidden, surveyed and captured by its textures' names (`geom.cpp`: `geom.survey`, `geom.hide`, `geom.capture`), the frame measured (`prof.cpp`: each mod's time by site, the frames, the game thread sampled; `prof.state`, `prof.sample`, MCP `profile`), a mod's own files in the game (`files.cpp`: the game's `dtaOpen` answered from the mods' `files\`; `files.list`, `files.which`, `files.read`) |
| `sdk/include/mml/mml.h` | **the C ABI** every native mod uses (C, C++, Rust, Zig...): read it whole |
| `sdk/include/mml/game/` | what code uses: `game_build.h` (every address, per build, `game::Detect`), `game.h` (layouts, calls: actors, frames, the player's car `game::Car` with its rigid body and wheels, a car's state, the mission's name), `mathx.h` |
| `knowledge/` | the knowledge base: every function of the 1.2 game (from MafiaReverse), our names, notes (`tools/kb_import.py`) |
| `mods/core/` | the core mod: the game's live state as commands (`game`, `player`, `car`, `car.save`, `car.restore`, `humans`, `actors`, `frame`, `frames.find`) |
| `mods/examples/` | `telemetry` (C++: the mod kit's complete example: an overlay, a hotkey, the car, saved files, settings), `hello_c` (plain C, the smallest), `fps_unlock` (code patched and put back, a game function fixed by a hook: the frame rate freed), `boxes` (a heavy mod: Jolt Physics from `tools/deps.py`, its own HLSL compiled by the build, a Direct3D 11 device, a clean hot reload); more languages to come (see `docs/ROADMAP.md`) |
| `mcp/` | the MCP server (Rust, `rmcp`): bridge client, game process, crash dumps, deploy, the knowledge base (`kb.rs`), screenshots (`shot.rs`) |
| `tests/` | `harness/` (the stand-in game), `mods/` (test mods: `hook_a`, `hook_b`, `kit`), `run_tests.py`, `mcp_smoke.py` |
| `docs/SCRIPTS.md` | the game's script machine: the language, the registers, tested recipes |
| `tools/` | `lab.py` (the lab's modes, install), `deps.py` (pinned downloads), `check_builds.py` (the address tables against the game's files), `kb_import.py` (the knowledge base from MafiaReverse), `kb_external.py` (other projects' names, checked), `start.py` (launch into a profile and the freeride), `new_mod.py` (a new mod: its folder, manifest, settings, CMake line), `replace.py` (exact text edits) |
| `third_party/` | MinHook, nlohmann/json (vendored); `_deps/` (downloaded: Ultimate ASI Loader, LuaJIT, Jolt Physics: the build fetches what it builds) |

## Facts that cost time to find

- **1.0's menus have no command handler**: each screen's Tick switches on the id the base `gui_menu_tick` returns.
  The loader's `menu.command` there is that tick's answer at the screen's next frame, so what it did shows in the
  next `menu.state`, not in its own reply. Screens are built from one table (`tables\menu.def`, `menu.def {name}`)
  and named by their vtables; the kinds have other tags (`docs/VERSIONS.md`).
- **Scripts: the player in a car is the car's child** (his frame is in its space): a teleport moves the car.
  `findnearactor` and `weather_setparam` take bare keywords (`Car`, `ON`), not quoted ones: quoted, they find or set
  nothing. Programs step in the mission's tick: they wait while the game is paused.
- **The real mouse reaches the game** whenever the game reads it: the user moving theirs during a test moves the
  menus' cursor (a click once missed so). In the background the loader keeps LS3D from clipping or moving the
  user's cursor; whether DirectInput still hands the game the user's moves there was not measured.

- **The intro videos** (Bink) run before the game's frame loop: no frames, so mods load and game-thread commands
  answer only after them (or once they are skipped). `where` shows `binkw32.dll` meanwhile. Bridge-thread methods
  (`ping`, `mem.read`, `log.tail`, `where`) answer at once.
- **An actor's position is in its model's parent's space**: the world in the street, a tram's frame for someone in
  a tram. Read the model frame's world matrix (`+0x10`, translation at `+0x40`) for the world position.
- **`game_build.h` is shared with mafia-ragdoll** (copied from it): after changing a table run
  `python tools/check_builds.py <1.2 Game.exe> <1.2 LS3DF.dll> <1.0 Game.exe> <1.0 LS3DF.dll>` (ALL PASSED).
- **Edit code with the editor's own tool or `tools/replace.py`**, never through string literals in a shell
  (backslashes get eaten: `\n` in C strings, `\\` in Windows paths). `replace.py` resolves paths from its own
  repository's root: run this repository's copy.
- A mod's callback that faults is unloaded at the next frame; the game goes on (`log_tail` tells which and where).
- **The game's 2D things are drawn in one callback** (0x5BD8D0, LS3D's render calls it with stage 2: the HUD, the
  open menu, the console), in pixels through `IGraph`; the HUD and the menus are laid out in 800x600 units times
  the scale at `hud + 0x40C0`. The loader's drawing for mods hangs there (`host/draw.cpp`,
  `knowledge/notes/interface.md`).
- **The game's menus are objects built from definitions** (`tables\menu\<name>.mnu`, 32-byte records, one factory
  a control kind: 38 kinds, `wind`, `stxt`, `butt`, `item`, `slid`, `pbar`, `chck`, `edit`, `wins`...); a click or
  Enter sends the control's id to the menu's vtable slot 9. `call menu.def {"name": "optionsa"}` shows any of the
  game's own definitions. A control's colour bits (0x100..0x400 of its flags) mean something else in each kind (a
  text's 0x100 is white, a button's red): read the kind's Draw before trusting a flag. In a definition `"end"`
  returns to the topmost window, not one level up; a scrolled list (`wins`) leaves its clip set for what is drawn
  after it (`knowledge/notes/interface.md`).
- **`menu.state`'s `x`, `y` are inside the parent; `screen` is where the control is.** A click at the parent-relative
  place once landed on another button and started a mission: click by id (`menu.click`).
- **A car's speed is `C_Vehicle + 0x59C`, metres a second** (the HUD's number at 0x6C3AF4 is the same in km/h; the
  dial shows mph). TrackMafia's "speed" offset (`car + 0x594`) holds the engine's idle turns in 1.2, not a speed:
  an older tool's offsets are leads, checked before use (`knowledge/notes/game.md`, "Cars").
- **The pause menu does not tick the mission; the main menu does** (its mission `00menu` runs under it): "paused"
  is a frame without the mission's tick while a menu is open.
- **`Action` in a car is three things** (0x594FA0): nothing while the car moves (speed 0.5 and more), the engine
  stopped when it runs, and only then the player gets out. Getting out of a car that was driven: wait for it to
  stop, `Action`, `Action`.
- **The freeride's player holds a Thompson** (two long weapons: `Hide Weapon` has nowhere to put it); a patrol that
  sees it shoots. Start with `"police": 0`.
- **A mouse axis is not in counts**: `Aim X` 1.0 for a frame is 400 counts, several turns of the player. Use
  `turn` (degrees), or values around 0.01 (docs/PROTOCOL.md).
- **Tool calls sent in one message may run at the same time**: a `screenshot` or a `call` beside an `input` is not
  after it (it cost two false "the hotkey did not fire" here). To see what a step did, let one tool do both
  (`input` with `"screenshot": true`; `no_wait` and then `screenshot` with `count` and `every_ms` for a key that
  is still held), or send the second call in the next message.
- **The agent's Bash tool turns every doubled backslash into a single one** before bash runs the command (measured
  2026-10-14 with `od -c`: in a quoted here-document, in single quotes, in an echo alike; one before a double quote
  survived). It corrupted files written through `cat <<'EOF'` and `python -` three times, so a hook now refuses any
  Bash command with a doubled backslash (`.claude/settings.json`, `tools/bash_guard.py`): write such text with the
  editor's tool, keep Python with escapes in a `.py` file, use single backslashes or forward slashes in commands.
- **A mod's files reach the game when it next opens them**: every mission's load opens its textures, models and
  sounds again, a menu its pictures at each opening; what the running mission has loaded stays. A texture also has
  presaved copies in the archives (`<name>_1.565`...) that LS3D takes over the picture: the loader hides them for a
  mod's picture (`knowledge/notes/rw_data.md`). `call files.which {"name"}` tells who answers a name.
- **Windows refuses a rename over an open file**, even one opened sharing deletion: delete it, then rename the new
  one into its place (the reader goes on reading the old one). `deploy` replaces files so.
- **`tools/replace.py` takes one OLD/NEW pair per `=== FILE` block**: a second `--- OLD` in the same block is taken
  as text of the first NEW.
- **The city's river** is one plane of 54 triangles at y -9.99 under the whole city, its texture `00TEST01..20.BMP`
  (animated): `call geom.survey` finds such names. A car teleported onto the river drowns Tommy (Game Over: `call
  menu.click {"id": 1000}` retries). A safe place to look at it, both builds: the city freeride (`"kind": "city"`),
  `teleport {"to": [-1598.48, -6.2, -30.8], "dir": [-0.38, 0, -0.92]}`, the embankment by Little Italy's bridge.
- **The MCP server's own binary is locked while your session runs it**: to build a new one, rename
  `mcp/target/release/mml-mcp.exe` first (Windows allows that), build, and test the new one with a client of your
  own (`tests/mcp_smoke.py` shows how); the session gets it at its next start.
