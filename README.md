# MafiaModLoader

A mod loader for **Mafia: The City of Lost Heaven** (2002; the Steam/GOG 1.2 and the original 1.0), made for AI agents
first and for people who write mods with them. An agent connected over MCP can start the game straight into a
profile and the freeride, read and write its memory, see its live objects, watch any of its functions, rebuild and
hot-reload mods while it runs, and open its crash dumps, all without anyone at the keyboard.

A mod written for it needs no thread, no window and no Windows hook of its own: the loader gives it drawing in the
game's frame (the game's own fonts and 2D drawing), the keys and the game's actions, hotkeys, the car and the
mission by name, a data folder and settings an agent can change over MCP, and the game's own menus extended: an
item in the pause menu and pages of the mod's own, made of the game's real controls (an achievements list, a mod's
settings), not an overlay. `python tools/new_mod.py <id>` starts
one; [docs/MODDING.md](docs/MODDING.md) is the whole way.

Status: in development, before its first release (2026-10). Everything listed here works in both builds, 1.2 and
1.0, each part seen in both games before it was called done; [docs/ROADMAP.md](docs/ROADMAP.md) has what is next
and what is known to be missing.

## What a mod gets

One C header, `sdk/include/mml/mml.h`, every function of it in both builds:

- **The frame**: text, lines and rectangles in the game's own fonts; the 3D world's drawing points (before the
  world, after it, before Present); a Direct3D 11 graphics bridge over the game's Direct3D 9 (the frame's colour,
  depth, camera, fog and sun, mods' passes in order, GPU timers, the 2D layer as its own target).
- **The scene from other cameras**: LS3D draws the mission again for a mirror, a reflection or a headset's two eyes
  (asymmetric frusta, a clip plane, the right room); the frame itself drawn from a mod's camera; a world point to
  the screen and back.
- **The game's own geometry**: its meshes hidden, surveyed and captured by their textures' names (the river's water
  replaced by a mod's).
- **Questions to the world**, answered by the game's collision tests: a ray, a sphere's sweep, the ground under a
  point, the actors near it, the collision's triangles, boxes and spheres in a box.
- **People**: every person, their skeletons by bone, poses written for the frame's drawing.
- **What happens, as events**: the mission's life, hits, shots, explosions, deaths with their killer, cars entered
  and left, cars blown up, mods loaded or faulted.
- **The game's state**: the freeride's money, health, the inventory slot by slot and its rounds; the scene's lights
  (the sun, the ambient light, every lamp) read and changed through LS3D's own setters.
- **The game's effects**: the car exhaust read or replaced, `particles.def`'s records, every particle's move told
  (and changed), the rain's parameters.
- **The world without a program**: cars and people of any model made, moved, seated, armed; props (any model, not
  an actor) placed in the world or attached to a bone, a car or another prop; sounds in 2D or 3D (on a car, at a
  place) with their pitch and volume changed live, music streamed, the city's music paused; keys and actions
  kept from the game, the player's control locked, a car's physics replaced by a mod's; the game's own script machine (every
  command the missions have).
- **Input and menus**: the game's actions by name, keys, hotkeys; items and whole pages in the game's own menus,
  made of its real controls.
- **Files and settings**: a mod's own textures, models, sounds and tables answered to the game by name; a data
  folder; settings an agent can change while the game runs.
- **Measuring**: each mod's time by site, the frame's hitches with what took them, the game thread sampled.
- **A heavy mod's life**: its own libraries and threads, a Direct3D 11 device, shaders built by CMake, and a clean
  hot reload (checked: 20 reloads in a row lose no thread and no address space).

## What is in it

| Part | What it is |
|---|---|
| `MafiaModLoader.asi` (`host/`) | A 32-bit DLL loaded into the game by [Ultimate ASI Loader](https://github.com/ThirteenAG/Ultimate-ASI-Loader). It is never unloaded: it turns the game's functions into events, keeps the hook chains mods share, loads the mods, isolates their faults, writes crash dumps, runs the bridge (TCP on 127.0.0.1) and the start. |
| Mods (`mml\mods\<id>\`) | DLLs with a `mod.json`, written against one C header (`sdk/include/mml/mml.h`): C, C++, Rust, anything with a C ABI. Each is rebuilt and reloaded while the game runs. `core` gives the game's live state (player, car, people, actors, frames); `mods/examples/telemetry` (a speed and lap-time overlay) is the complete example of a mod; `mods/examples/fps_unlock` frees the frame rate (the game's 62.5 fps limit taken away, what breaks above it fixed); `mods/examples/boxes` is a heavy mod (Jolt Physics, a Direct3D 11 device of its own, shaders compiled in its build) that hot-reloads cleanly. One example a service: `rear_mirror` (mirrors, reflections, a headset's eyes), `river_water` (the river drawn anew), `depth_pulse`, `hud_panel`, `puppet`, `tally`, `orbit_cam`, `probe`, `bounty`, `dusk`, `exhaust_fx`, `wind`, `storm`, `props_demo`, `engine_note`, `hover` (most are off by default: `call mod.load {"id"}`). |
| `mml-mcp.exe` (`mcp/`) | The MCP server (Rust, 64-bit), outside the game: it launches and closes the game, talks to the bridge, reads the log and the crash dumps, deploys builds, and answers questions about the game's code from the knowledge base. |
| `knowledge/` | What is known about the game's code: every function of the 1.2 `Game.exe` and `LS3DF.dll` (names, classes, signatures, calls), our own names, notes. Imported from the MafiaReverse decompiles (facts only; the code itself is never committed). |

## Quick start

Needs Windows, Visual Studio 2022 (C++ x86 tools), CMake 3.24+, Rust (stable), Python 3.10+, a Mafia install (1.2 or 1.0; `mml.config.json` names it, and a lab folder for each build you test in).

```bat
python tools/deps.py                                  :: pinned downloads: Ultimate ASI Loader, LuaJIT, Jolt (hashes checked; the build fetches Jolt by itself)
copy mml.config.example.json mml.config.json          :: then set game_dir (and reverse_dir, if you have MafiaReverse)
cmake --preset x86
cmake --build --preset x86-release                    :: the loader and the mods
cargo build --release --manifest-path mcp/Cargo.toml  :: the MCP server
python tests/run_tests.py                             :: offline tests (a stand-in game): ALL PASSED
python tests/mcp_smoke.py                             :: the MCP server against it: ALL PASSED
python tools/lab.py mode loader                       :: install into the game (dinput8.dll, the .asi, the mods)
python tools/start.py freeride --profile 4            :: the game, straight into profile 4's freeride (--police 0: no patrols)
```

`.mcp.json` registers the server as `mafia` for Claude Code (and any MCP client). Agents start with
[AGENTS.md](AGENTS.md).

## For agents (MCP tools)

| Tool | What |
|---|---|
| `game_launch` | start the game; with `{"profile": 4, "go": "freeride"}` straight into the freeride (returns once the player exists); `"police": 0` for a city without patrols |
| `screenshot` | see the game: its own frame as a picture, whether or not the window is visible |
| `input` | act in the game: a timed sequence of its own actions (walk, drive, fire, turn by an angle) and keys, injected inside the game |
| `game_status`, `game_close` | is it running (and the mods); close it (its window, then its exact pid) |
| `call`, `commands_list` | any bridge method or mod command (`player`, `car`, `world.ray`, `human.items`, `lights.list`, `camera.set`, `menu.state`...); `commands_list {"query": "world."}` gives each method's params and result (made from docs/PROTOCOL.md) |
| `spawn`, `teleport`, `give`, `script`, `script_commands` | the world without a program: cars and people made, moved, armed; the game's own script machine |
| `wait` | wait for what happens (a mission loaded, a person dead, a mod faulted) instead of sleeping |
| `profile` | where the frame's time goes: each mod by site, the game thread sampled, named |
| `watch` | a hardware watchpoint: which code writes (or reads) a field, with its callers and the values |
| `mem_read`, `mem_write` | the game's memory (`"game+0x1762A0"`, `"ls3d+1AAE0"`), never faulting |
| `tap` | watch any function: callers, arguments, memory behind them, the result |
| `where` | what the game's main thread is doing (a hang, a loading screen) |
| `log_tail` | the loader's log, live or from disk |
| `mods_list`, `mod_reload`, `deploy` | the mods; copy a build into the game and reload it |
| `crash_list`, `crash_inspect` | the crash dumps, read: the exception, the memory, the stack, named |
| `kb_search`, `kb_function` | the knowledge base: find a function, read its facts, calls and decompiled code |

## Documents

- [AGENTS.md](AGENTS.md): the guide for agents (rules, the work loop, the map, facts that cost time).
- [docs/ARCHITECTURE.md](docs/ARCHITECTURE.md): how it works. [docs/PROTOCOL.md](docs/PROTOCOL.md): the bridge.
- [docs/MODDING.md](docs/MODDING.md): writing a mod, from `tools/new_mod.py` to the game (what the host gives:
  drawing, keys, the car, the mission, files and settings). [docs/SCRIPTS.md](docs/SCRIPTS.md): the game's own
  script machine for agents and mods (every command the missions have), and the world without a program (a car or
  a person of any model made new, moved, seated, armed by name). [docs/VERSIONS.md](docs/VERSIONS.md): the
  two builds. [knowledge/README.md](knowledge/README.md): the
  knowledge base.
- [docs/ROADMAP.md](docs/ROADMAP.md): what is done, next, later, and the known gaps.
- [docs/JOURNAL.md](docs/JOURNAL.md): what was done and found, in order.

## Third-party

MinHook 1.3.4 and nlohmann/json 3.12.0 (vendored, `third_party/README.md`), Ultimate ASI Loader 9.7.4, LuaJIT 2.1 and
Jolt Physics 5.6.0 (downloaded and hash-checked by `tools/deps.py`), Rust crates (`mcp/Cargo.lock`). No game files are in the
repository.
