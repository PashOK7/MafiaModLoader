# Architecture

```
   agent (Claude Code, Codex, ...)                          the game process (32-bit)
   ───────────────────────────────                          ───────────────────────────────────────────
   MCP (stdio)                                              Game.exe, LS3DF.dll
      │                                                       │  Ultimate ASI Loader (dinput8.dll)
   mml-mcp.exe (Rust, 64-bit) ──── TCP 127.0.0.1:47100 ────►  MafiaModLoader.asi  (the host: never unloaded)
      │  game process: launch, close (exact pid)               ├─ game hooks → events
      │  log on disk, crash dumps (minidump)                   ├─ hook chains (MinHook once per function)
      │  deploy (build → game folder → mod.reload)             ├─ bridge (NDJSON, token)
      │                                                         ├─ crash filter (full dumps), `where`
      │                                                         └─ mods: mml\mods\<id>\ (each hot-reloadable)
      │                                                              ├─ core   (the game's live state)
      │                                                              ├─ hello_c, your mods (C, C++, Rust...)
      │                                                              └─ (to come) the LuaJIT runtime, the sidecar link
   (to come) mml-sidecar64.exe: 64-bit mods, past the 32-bit address space, shared memory with the game
```

## The host (`host/`)

`MafiaModLoader.asi` is the one part that is never unloaded. Ultimate ASI Loader loads it early, on the game's main
thread, under the loader lock: there it only sets up (paths, log, crash filter, MinHook writes, the bridge's thread).
Mods load at the **first frame** (the game's frame limiter, LS3DF `0x72100` in 1.2: called once a frame in every state
except the intro videos), on the game thread, where `LoadLibrary` is safe.

- **Events.** The game's hooks (`game_hooks.cpp`) become `MmlEvent`s: `FRAME` (every frame), `MISSION_TICK`,
  `MISSION_CLOSE`, `ACTOR_DESTROYED`, `HUMAN_CREATED`, `HUMAN_HIT`; and the loader's own `SERVICES_CHANGED`,
  `MOD_LOADED`. Addresses come from `sdk/include/mml/game/game_build.h` (per build; `game::Detect` picks the running
  one by its code). An unknown build: no game events, everything else runs.
- **Hook chains** (`hooks.cpp`). MinHook allows one detour per function; mods may want the same function. Each hooked
  function gets one MinHook hook whose detour is a 6-byte thunk `jmp [head]`; the chain is a list of detours, the last
  one hooked outermost. Adding or removing a link only rewrites pointers (innermost first), so the game's bytes are
  written once and a running detour always has a valid next step. Tested in every load and unload order
  (`tests/run_tests.py`).
- **Mods** (`mods.cpp`). A mod's DLL is copied to `mml\live\mod_<id>_<pid>_<n>.dll` and loaded from there: the
  original can be rebuilt while the game runs. Unloading removes everything it registered (hooks, handlers, commands,
  services, posted calls): no pointer into its code is left behind. Every call into a mod is behind `__try`: a fault
  marks it, it is unloaded at the next frame, the game goes on. `state_put`/`state_get` keep blobs across a mod's own
  reloads.
- **Services**: a mod publishes a versioned table of functions under a name; others find it. This is how separate
  mods share features at run time (the ragdoll mod's physics for a VR mod) without linking to each other.
- **The bridge** (`bridge.cpp`): see `PROTOCOL.md`. Memory and log methods answer on the bridge's thread (even while
  the game thread hangs); mod commands wait for the game thread's next frame.
- **Crashes** (`crash.cpp`): an unhandled exception is logged (module+offset, registers, a stack scan) and written as
  a minidump with the heaps and every region's state (`mml\crash\`). The game's own filter is chained, and ours is put
  back in front every frame.

## The MCP server (`mcp/`)

A separate 64-bit process, so it outlives the game: it launches and closes the game, reads the loader's log file
when the game is gone, opens crash dumps (`minidump`, Mozilla's), and deploys builds. It talks to the host through
the bridge only. Tools: `game_status`, `game_launch`, `game_close`, `call` (any method), `commands_list`, `mem_read`,
`mem_write`, `log_tail`, `mods_list`, `mod_reload`, `where`, `crash_list`, `crash_inspect`, `deploy`.

## Files in the game folder

```
<game>\MafiaModLoader.asi           the host
<game>\dinput8.dll                  Ultimate ASI Loader (pinned, tools/deps.py)
<game>\mml\config.json              optional: {"bridge_port": 47100}
<game>\mml\bridge.json              written at start: port, token (new every run), pid
<game>\mml\mods\<id>\mod.json, ...  the mods
<game>\mml\live\                    the loaded copies of the mods' DLLs
<game>\mml\logs\mml.log             this run's log (mml.prev.log: the previous run's)
<game>\mml\crash\*.dmp              crash dumps
```
