# Roadmap

Agreed with the owner on 2026-10-13. Done:

- **Stage 1: the base.** The host (`MafiaModLoader.asi` via Ultimate ASI Loader): game events, hook chains, native
  mods with hot reload, fault isolation, services, state across reloads, the bridge, crash dumps. The core mod (the
  game's live state). The MCP server (Rust): status, launch/close, any call, memory, log, mods, `where`, crash dumps,
  deploy. Offline tests (a stand-in game) and an MCP smoke test. The lab's modes (shared with mafia-ragdoll).

Next, in order:

1. **LuaJIT script mods** (`type: "lua"`): a runtime mod embedding LuaJIT (pinned, `tools/deps.py`), one Lua state
   per script mod, hot reload on change; the C API through LuaJIT's FFI (`ffi.cdef` of `mml.h`), so scripts can read
   and write memory, describe the game's structures and call its functions; a `lua.eval` command for agents.
2. **Rust mods**: a crate `mml` over `mml.h` (types, a safe wrapper, a `#[mml_mod]` entry macro), an example, built for
   `i686-pc-windows-msvc`.
3. **Driving the game from inside**: input injected into the game's own DirectInput reads (keys, mouse, held for N
   frames), screenshots of the game's back buffer (the bridge returns PNG for MCP image content), saves and missions
   loaded by the game's own functions, the intro videos skipped.
4. **The 64-bit sidecar**: `mml-sidecar64.exe`, started by the host, loads 64-bit mods (heavy logic past the 32-bit
   address space); shared-memory channels (lock-free rings, bulk blocks for poses and meshes) between the game and it;
   services that cross the process boundary.
5. **Knowledge tools for agents**: the Ghidra query server (ghq, from mafia-ragdoll) and runtime `tap` (watch any
   function's calls) behind MCP tools; the game knowledge base (`game_build.h`) grown with every finding.
6. **mafia-ragdoll on the loader**: its host's duties taken by the loader, the ragdoll a mod; the physics published as
   a service (for the VR mod).
7. **1.0**: everything checked on the 1.0 lab copy; addresses for both builds in `game_build.h`.
