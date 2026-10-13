# Writing a mod

A native mod is a 32-bit DLL with three exports (`sdk/include/mml/mml.h` is the whole contract):

```c
#include "mml/mml.h"
static const MmlHost *H;

static void OnTick(const MmlEvent *ev, void *user) { /* ev->dt seconds; the game's world just moved */ }
static int Hello(const char *args_json, MmlReply *reply, void *user) {
    H->reply_append(reply, "{\"hello\":true}", (size_t)-1);
    return 0;
}

MML_EXPORT uint32_t mml_mod_api(void) { return MML_API_VERSION; }
MML_EXPORT int mml_mod_load(const MmlHost *host, MmlMod *mod) {
    H = host;
    host->subscribe(mod, MML_EVENT_MISSION_TICK, OnTick, NULL);
    host->command(mod, "hello", "says hello", Hello, NULL);   /* the bridge: "<id>.hello" */
    return 0;
}
MML_EXPORT void mml_mod_unload(MmlMod *mod) { /* save what must survive a reload: host->state_put */ }
```

and a folder `mml\mods\<id>\` with the DLL and `mod.json`:

```json
{ "id": "my_mod", "name": "My mod", "version": "0.1.0", "type": "native", "entry": "my_mod.dll",
  "enabled": true, "load_order": 100, "depends": ["core"] }
```

In this repository: put it in `mods/<id>/` and add `mml_add_mod(<id> ${CMAKE_SOURCE_DIR}/mods/<id> <sources>)` to
`CMakeLists.txt`; `cmake --build --preset x86-release` writes `build/x86/out/mml/mods/<id>/`; the MCP tool `deploy`
copies it into the game and reloads it.

## What the host gives a mod

- **Events**: every frame, the mission's tick and close, actors destroyed, humans created and hit.
- **Hooks** on any game function, chained with other mods' (`host->hook(mod, address, detour, &original)`): call
  `original` for the rest of the chain. Addresses for both builds: `mml/game/game_build.h` (`game::Detect` first).
- **Memory** that never faults (`mem_read`, `mem_write`).
- **Commands** agents call over MCP (`call` with the method `<id>.<name>`), replying JSON.
- **State** across the mod's own hot reloads (`state_put`, `state_get`).
- **Services**: publish a versioned table of functions, find others' (`service_publish`, `service_find`); re-find
  after `MML_EVENT_SERVICES_CHANGED` (the publisher may have been reloaded).
- **Posting** work to the game thread from any thread (`post`).

## Rules

- Everything runs on the game thread unless said otherwise; do not block it.
- Never keep a pointer into another mod's code or data across `MML_EVENT_SERVICES_CHANGED` / `MML_EVENT_MOD_LOADED`.
- Game objects die under you (missions close, districts stream out): re-read them every time, or listen to
  `ACTOR_DESTROYED` and `MISSION_CLOSE`. Read game memory through `mem_read` when a pointer may be stale.
- A fault in your callback unloads your mod (the game goes on); `log_tail` shows where.

Other languages: Rust (a crate over `mml.h`), LuaJIT scripts and 64-bit sidecar mods are on the roadmap
(`docs/ROADMAP.md`).
