# MafiaModLoader

A mod loader for **Mafia: The City of Lost Heaven** (2002; Steam/GOG 1.2, and 1.0 in part), made for AI agents
first and for people who write mods with them. An agent connected over MCP can start the game straight into a
profile and the freeride, read and write its memory, see its live objects, watch any of its functions, rebuild and
hot-reload mods while it runs, and open its crash dumps, all without anyone at the keyboard.

A mod written for it needs no thread, no window and no Windows hook of its own: the loader gives it drawing in the
game's frame (the game's own fonts and 2D drawing), the keys and the game's actions, hotkeys, the car and the
mission by name, a data folder and settings an agent can change over MCP, and the game's own menus extended: an
item in the pause menu and pages of the mod's own, made of the game's real controls (an achievements list, a mod's
settings), not an overlay. `python tools/new_mod.py <id>` starts
one; [docs/MODDING.md](docs/MODDING.md) is the whole way.

Status: private, early (2026-10). The base, the knowledge base, the start, eyes and hands for agents and the mod
author's kit work in the 1.2 game; see [docs/ROADMAP.md](docs/ROADMAP.md) for what is next and what is known to be
missing.

## What is in it

| Part | What it is |
|---|---|
| `MafiaModLoader.asi` (`host/`) | A 32-bit DLL loaded into the game by [Ultimate ASI Loader](https://github.com/ThirteenAG/Ultimate-ASI-Loader). It is never unloaded: it turns the game's functions into events, keeps the hook chains mods share, loads the mods, isolates their faults, writes crash dumps, runs the bridge (TCP on 127.0.0.1) and the start. |
| Mods (`mml\mods\<id>\`) | DLLs with a `mod.json`, written against one C header (`sdk/include/mml/mml.h`): C, C++, Rust, anything with a C ABI. Each is rebuilt and reloaded while the game runs. `core` gives the game's live state (player, car, people, actors, frames); `mods/examples/telemetry` (a speed and lap-time overlay) is the complete example of a mod. |
| `mml-mcp.exe` (`mcp/`) | The MCP server (Rust, 64-bit), outside the game: it launches and closes the game, talks to the bridge, reads the log and the crash dumps, deploys builds, and answers questions about the game's code from the knowledge base. |
| `knowledge/` | What is known about the game's code: every function of the 1.2 `Game.exe` and `LS3DF.dll` (names, classes, signatures, calls), our own names, notes. Imported from the MafiaReverse decompiles (facts only; the code itself is never committed). |

## Quick start

Needs Windows, Visual Studio 2022 (C++ x86 tools), CMake 3.24+, Rust (stable), Python 3.10+, a Mafia 1.2 install.

```bat
python tools/deps.py                                  :: pinned downloads: Ultimate ASI Loader, LuaJIT (hashes checked)
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
| `call`, `commands_list` | any bridge method or mod command (`player`, `car`, `humans`, `menu.state`, `telemetry.status`...); drawing without a mod (`draw.text`, `draw.rect`, `draw.line`), a mod's settings (`config.get`, `config.set`) |
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
  drawing, keys, the car, the mission, files and settings). [knowledge/README.md](knowledge/README.md): the
  knowledge base.
- [docs/ROADMAP.md](docs/ROADMAP.md): what is done, next, later, and the known gaps.
- [docs/JOURNAL.md](docs/JOURNAL.md): what was done and found, in order.

## Third-party

MinHook 1.3.4 and nlohmann/json 3.12.0 (vendored, `third_party/README.md`), Ultimate ASI Loader 9.7.4 and LuaJIT 2.1
(downloaded and hash-checked by `tools/deps.py`), Rust crates (`mcp/Cargo.lock`). No game files are in the
repository.
