# Journal

What was done, found and tried, in order (newest last). Agents: read the end first, append as you go.

## 2026-10-13: the base

- The host (`MafiaModLoader.asi` through Ultimate ASI Loader 9.7.4), the C ABI (`mml.h`), hook chains, native mods
  with hot reload, the bridge, crash dumps, the core mod, the Rust MCP server, offline tests (`run_tests.py`,
  `mcp_smoke.py`). Committed as the first commit.
- First live run in the lab (1.2): `game_launch` started the game, the loader detected 1.2 and hooked. While the Bink
  intros play there are no frames (`where` showed `binkw32.dll`); the mods loaded with the first frame of the menu.
  `game` answered (mission `00menu`, the scene); `player` is none in the menu.

## 2026-10-13: the knowledge base

- The owner's MafiaReverse folder (full Hex-Rays decompiles of the 1.2 Game.exe and LS3DF.dll with indexes) brought
  in as `knowledge/` (`tools/kb_import.py`): 4833 + 1599 functions, the call graph, string sites; the decompiled
  code stays out of the repository and is read at run time from `reverse_dir`. Our own names merged in
  (`names.csv`: mafia-ragdoll's Ghidra project, AntiCulling's report, `game_build.h`), and notes
  (`knowledge/notes/`: the DTA archives and rw_data.dll, IJoy.dll, findings about the game).
- MCP: `kb_search`, `kb_function`; `crash_inspect` and `where` name the functions of the addresses they print.
- The bridge took a fixed port: a test harness started while the game ran could not listen and the MCP server read
  the harness's token against the game's bridge ("bad token"). Now the bridge takes the next free port and writes
  `bridge.json` only once it listens (a stale one is deleted at start).

## 2026-10-13: tap, and the start

- `tap` (from mafia-ragdoll's host) moved into the loader's host as a link of the hook chain: the bridge method
  `tap`, the MCP tool `tap` (callers named from the knowledge base), startup taps from `mml\config.json`. Snapshots
  that start with text show it as a string.
- Recorded the way into the freeride: 14 startup taps (menus, profiles, saves, missions) while the owner launched
  the game, took a profile and clicked Freeride. Then read in the decompile: the menus loop in
  `GM_Menu::ExecuteMenu` until `MenuCloseAll` sets the result; a click calls the menu's command handler (vtable
  `+0x24`). Details: `knowledge/notes/game.md`.
- The start (`host/start.cpp`): `mml\start.json` asks for it once; the logo videos are answered "played", then in
  each menu's frame the loader sends the command a click would (the profile's 100 + i, the main menu's 990105, the
  freeride menu's 201). First try in the lab: from the launch to the freeride in 2.6 s; through MCP
  (`game_launch {"profile": 4, "go": "freeride"}`) 5.9 s until the player exists. The game's default freeride is
  `freeitaly` (code 28); the decompile's case labels for 28..31 were guesses.
