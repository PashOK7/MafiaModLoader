# MafiaModLoader: guide for AI agents

A mod loader for **Mafia: The City of Lost Heaven** (2002; builds 1.2 and 1.0) made for agents first: an MCP server
gives you the live game (its memory, its objects, the loader's log, its crashes), every mod hot-reloads, and the
game can be started, queried and closed without a human at the keyboard. Read this file, then
`docs/ARCHITECTURE.md`, `docs/PROTOCOL.md`, `docs/MODDING.md`, `docs/ROADMAP.md` (what is next, what is known to be
missing) and the end of `docs/JOURNAL.md`. Keep the roadmap and the journal current as you work.

## Rules

- **Only the game folder in `mml.config.json`** (`game_dir`). Never search the disk for other installs.
- **The lab may be shared** with other projects (mafia-ragdoll's developer host is the game's `IJoy.dll` there).
  `python tools/lab.py status` tells which mode it is in; `mode loader` / `mode ragdoll` switch, keeping what they
  take out in `<game>\mml_lab\` and the saves in `<game>-backup\savegame-<date>`. Never delete another project's
  files; put the lab back in the mode the owner uses when you are done.
- **The game may be driven by agents, from inside**: launch and close it (`game_launch`, `game_close`, by its exact
  pid), query and change its state through the bridge. Never send the owner's real mouse or keyboard input.
- **Never commit game files, extracted assets, dumps or decompiles.** `re/` and `*.dmp` are ignored.
- **Correct over quick**: use the engine's own functions and hook points, verify every address in the disassembly,
  say plainly what is a compromise.
- **Language**: everything in the repository is English. Talk to the owner in the language they use.
- **Ask the owner before**: pushing, publishing, opening PRs, anything outside the game folder and this repository.

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

**Into the game in seconds**: `game_launch {"profile": 4, "go": "freeride"}` (or `"go": "menu"`) skips the logos and
takes the game through its own menus; it returns once the player exists. The owner's shortcut:
`python tools/start.py freeride --profile 4`. Other menus: `call menu.state`, `call menu.command {command}`.
**Watch any function** with `tap` (`"5409D0 n=4 ptr=1:32"`, let the game do the thing, `"show 0"`): the fastest way
to learn what the game does.

## Knowing the game

**Ask the knowledge base first** (`knowledge/README.md`): `kb_search` finds functions of Game.exe and LS3DF.dll
(1.2) by name, class, signature or a string they use, our own names and the notes; `kb_function` gives one function's
facts, callers, callees and its decompiled code (read from the MafiaReverse folder, `reverse_dir` in
`mml.config.json`; never committed). `crash_inspect` and `where` name the functions they show. Then check in the
running game (`mem_read`, `call`), and write what you learned back: `knowledge/<module>/names.csv`,
`knowledge/notes/`, addresses code uses in `game_build.h`.

## Map

| Path | What |
|---|---|
| `host/` | `MafiaModLoader.asi`: the never-unloaded host: game hooks → events, hook chains, mods, bridge, crash dumps, `tap`, the start (`start.cpp`: logos, profile, menus) |
| `sdk/include/mml/mml.h` | **the C ABI** every native mod uses (C, C++, Rust, Zig...): read it whole |
| `sdk/include/mml/game/` | what code uses: `game_build.h` (every address, per build, `game::Detect`), `game.h` (layouts, calls), `mathx.h` |
| `knowledge/` | the knowledge base: every function of the 1.2 game (from MafiaReverse), our names, notes (`tools/kb_import.py`) |
| `mods/core/` | the core mod: the game's live state as commands (`game`, `player`, `humans`, `actors`, `frame`, `frames.find`) |
| `mods/examples/` | `hello_c` (plain C); more languages to come (see `docs/ROADMAP.md`) |
| `mcp/` | the MCP server (Rust, `rmcp`): bridge client, game process, crash dumps, deploy, the knowledge base (`kb.rs`) |
| `tests/` | `harness/` (the stand-in game), `mods/` (test mods), `run_tests.py`, `mcp_smoke.py` |
| `tools/` | `lab.py` (the lab's modes, install), `deps.py` (pinned downloads), `check_builds.py` (the address tables against the game's files), `kb_import.py` (the knowledge base from MafiaReverse), `start.py` (launch into a profile and the freeride), `replace.py` (exact text edits) |
| `third_party/` | MinHook, nlohmann/json (vendored); `_deps/` (downloaded: Ultimate ASI Loader, LuaJIT) |

## Facts that cost time to find

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
