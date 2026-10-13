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
- The repository is English; it will be a private GitHub repository (not pushed yet: ask the owner first).
- The lab (`W:\MafiaLAB`) is shared with mafia-ragdoll (`tools/lab.py mode loader|ragdoll`).

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

## Next, in order

1. **Eyes and hands for agents.**
   - Screenshots of the game's own frame (the back buffer, after the frame is drawn), returned as MCP image content;
     optionally a small sequence (a few frames apart) for motion.
   - Input injected from inside: the game's own DirectInput reads (keyboard, mouse) given keys held for N frames,
     mouse moves, clicks; the owner's real devices and the window's focus untouched. MCP tools `input` (a list of
     timed actions) and `screenshot`.
   - Together: an agent plays a scenario itself and looks at the result.
2. **The engine's own commands from MCP.** The mission scripts' interpreter (`C_program::DecodeInstruction`
   0x463960, `C_program::Process` 0x46D3B0) knows hundreds of commands (`HUMAN_*`, `PLAYER_*`, `CAR_*`,
   `ACTOR_SETPOS`, weather, time). Run one at a time from the bridge (a small program given to the interpreter, or
   the command handlers called directly): teleport, spawn a car or a person, give a weapon, kill or knock someone
   down, set the time of day. A developer console built on the game's own mechanisms.
3. **LuaJIT script mods** (`type: "lua"`): a runtime mod embedding LuaJIT (pinned by `tools/deps.py`), one Lua state
   per script mod, hot reload on change; the C API through LuaJIT's FFI (`ffi.cdef` of `mml.h`), so scripts read and
   write memory, describe the game's structures and call its functions; `lua.eval` for agents (a hypothesis tested
   in five lines, without building a DLL).
4. **Scenarios: tests in the real game.** A file of steps (start, teleport, spawn, input, wait, check a value or an
   event, screenshot) run by the loader; a pass/fail report through MCP. First user: mafia-ragdoll's cases (a car
   hits a person getting up, a body thrown into a tram).
5. **The start, completed.** Missions and saves through the load menu (the main menu's 990104, then the chapter or
   save chosen: `SaveGameSelect` / `SaveGameLoad`); the other freerides (`freeride`, `freeridenoc`, `freekrajina`,
   `extreme`: the freeride menu's kind menu (command 100) and the spawn menu its start opens for kinds other than
   28); a new game (990103). The friend's reverse of the game's interface is expected to help here.
6. **Structures by name.** `inspect <addr> as C_human` (and other classes): fields named and typed from `game.h`
   and the decompile's layouts, kept in `knowledge/` as tables of offsets.

## Later

- **Events to MCP as they happen**: deaths, hits, missions opened and closed, mods faulting, as a stream an agent
  can wait on ("wait until a person dies", "wait until the mission is loaded").
- **Hardware watchpoints** (`hwwatch` from mafia-ragdoll): who writes or reads a field, on the bridge and MCP. Its
  handler must never read the watched address (a recursion crashed the game once).
- **Ghidra behind MCP**: the query server `ghq` (mafia-ragdoll's `tools/ghq.py`) as MCP tools: decompile, xrefs,
  rename and comment (findings written into the project), against the Ghidra projects of both builds.
- **State snapshots for experiments**: the game's own save and load used as a restore point, to repeat an
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
- **1.0**: the start's and the menus' addresses for 1.0 (`game_build.h` has zeros there now), everything checked on
  the 1.0 lab copy (`W:\Mafia10LAB`); the knowledge base for 1.0 (only 1.2's decompile is imported).
- **Publishing**: the private GitHub repository (when the owner says so), CI for the offline tests.

## Known gaps

- **The start covers the default freeride and the main menu only** (see Next, 5). Without `profile` the profile
  menu waits for the player. The start runs only for 1.2.
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
