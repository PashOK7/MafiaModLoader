/* MafiaModLoader: the C ABI between the loader and its mods.
 *
 * A native mod is a 32-bit DLL (C, C++, Rust, Zig... anything that exports C functions) in its own folder under
 * <game>\mml\mods\<id>\ with a mod.json manifest. The loader copies the DLL to a fresh file before loading it, so the
 * original can be rebuilt while the game runs; `mod.reload <id>` (bridge, MCP) unloads and loads it again.
 *
 * The contract:
 * - Every call into a mod happens on the game thread, unless a function says otherwise.
 * - MmlHost only grows: new members are added at its end; `size` tells which exist (MML_HAS(host, member)).
 * - What a mod registers (hooks, event handlers, commands, services) is removed by the loader when the mod unloads:
 *   a mod never leaves a pointer into its own code behind.
 * - Memory functions never fault: they return 0 when an address cannot be read or written.
 *
 * A mod exports:
 *   MML_EXPORT int  mml_mod_load(const MmlHost *host, MmlMod *mod);   0: loaded; else it is unloaded again
 *   MML_EXPORT void mml_mod_unload(MmlMod *mod);                       before its DLL is freed (reload, exit)
 *   MML_EXPORT uint32_t mml_mod_api(void);                             return MML_API_VERSION (optional)
 */
#ifndef MML_H
#define MML_H

#include <stddef.h>
#include <stdint.h>

#define MML_API_VERSION 1u

#ifdef __cplusplus
#define MML_EXTERN_C extern "C"
#else
#define MML_EXTERN_C
#endif
#define MML_EXPORT MML_EXTERN_C __declspec(dllexport)

/* Whether a host table has a member (the table only grows; an older loader has a smaller one). */
#define MML_HAS(table, member) ((size_t)(table)->size >= offsetof(MmlHost, member) + sizeof((table)->member))

#ifdef __cplusplus
extern "C" {
#endif

typedef struct MmlMod MmlMod;       /* one loaded mod (a reload gives a new one) */
typedef struct MmlReply MmlReply;   /* a command's reply being written */

typedef enum MmlLogLevel { MML_LOG_DEBUG = 0, MML_LOG_INFO = 1, MML_LOG_WARN = 2, MML_LOG_ERROR = 3 } MmlLogLevel;

typedef struct MmlVec3 { float x, y, z; } MmlVec3;

typedef enum MmlEventType {
    MML_EVENT_FRAME = 1,             /* every frame in every game state (intro, menus, loading, missions); dt */
    MML_EVENT_MISSION_TICK = 2,      /* right after the mission's tick (the game's world moved); dt */
    MML_EVENT_MISSION_CLOSE = 3,     /* the mission's world is about to go (a mission change, a restart, the exit) */
    MML_EVENT_ACTOR_DESTROYED = 4,   /* an actor is about to be destroyed: actor */
    MML_EVENT_HUMAN_CREATED = 5,     /* a C_human (the player, NPCs, pooled traffic people) was constructed: actor */
    MML_EVENT_HUMAN_HIT = 6,         /* a C_human took a hit (after the game's Hit): actor, hit */
    MML_EVENT_SERVICES_CHANGED = 7,  /* a service was published or went away (its mod unloaded): name */
    MML_EVENT_MOD_LOADED = 8,        /* a mod was loaded or reloaded: name (its id) */
    MML_EVENT_COUNT_
} MmlEventType;

typedef struct MmlHumanHit {
    int type;               /* the game's hit type (0 bullet, 2 tram/plane, 3 explosion, 5 car, 6 fall, 8 car...) */
    MmlVec3 dir, pos;       /* as the game passed them (may be zero) */
    float damage;
    void *attacker;         /* the actor that caused it, or null */
    int was_alive, alive;   /* the human's alive flag before and after the game's Hit */
} MmlHumanHit;

typedef struct MmlEvent {
    uint32_t size;          /* sizeof(MmlEvent) as the loader built it */
    MmlEventType type;
    float dt;               /* FRAME, MISSION_TICK: seconds */
    void *actor;            /* ACTOR_DESTROYED, HUMAN_CREATED, HUMAN_HIT */
    const MmlHumanHit *hit; /* HUMAN_HIT */
    const char *name;       /* SERVICES_CHANGED, MOD_LOADED */
} MmlEvent;

typedef void (*MmlEventFn)(const MmlEvent *ev, void *user);

/* A command, reachable through the bridge (and so by agents over MCP) as "<mod id>.<name>" or, for the core mod,
 * as "<name>". args: JSON text (an object, or "null"). Write the reply with reply_append (JSON text) or reply_error.
 * Returns 0 on success. Commands run on the game thread. */
typedef int (*MmlCommandFn)(const char *args_json, MmlReply *reply, void *user);

typedef struct MmlHost {
    uint32_t size;          /* sizeof(MmlHost) as the loader built it */
    uint32_t api_version;   /* MML_API_VERSION of the loader */

    /* ---- identity */
    const char *(*game_build)(void);             /* "1.2", "1.0", or "unknown" (the loader still runs: no game hooks) */
    const char *(*game_dir)(void);               /* the game's folder, with a trailing backslash */
    const char *(*mod_id)(const MmlMod *mod);
    const char *(*mod_dir)(const MmlMod *mod);   /* the mod's folder (its manifest, its files), trailing backslash */

    /* ---- log: <game>\mml\logs\mml.log (and the bridge's log tail) */
    void (*log)(const MmlMod *mod, MmlLogLevel level, const char *fmt, ...);

    /* ---- memory (any thread; never faults) */
    int (*mem_read)(uintptr_t addr, void *dst, size_t n);
    int (*mem_write)(uintptr_t addr, const void *src, size_t n);   /* code pages are unprotected for the write */
    uintptr_t (*module_base)(const char *module);                   /* "Game.exe", "LS3DF.dll"...; 0: not loaded */

    /* ---- hooks: chained. Several mods may hook one function; each detour calls *original for the rest of the chain
     * (the loader keeps *original right as hooks come and go). Removed when the mod unloads. 0 on failure. */
    int (*hook)(MmlMod *mod, uintptr_t target, void *detour, void **original);
    int (*unhook)(MmlMod *mod, uintptr_t target);

    /* ---- events */
    int (*subscribe)(MmlMod *mod, MmlEventType type, MmlEventFn fn, void *user);

    /* ---- commands (bridge, MCP) */
    int (*command)(MmlMod *mod, const char *name, const char *help, MmlCommandFn fn, void *user);
    void (*reply_append)(MmlReply *reply, const char *text, size_t len);   /* len (size_t)-1: up to the NUL */
    void (*reply_error)(MmlReply *reply, const char *message);

    /* ---- state kept across the mod's own reloads (blobs by key; gone when the game exits) */
    int (*state_put)(MmlMod *mod, const char *key, const void *data, size_t n);
    size_t (*state_get)(MmlMod *mod, const char *key, void *data, size_t cap);   /* the blob's size; 0: none */

    /* ---- services: a mod offers a versioned table of functions under a name; other mods find it. A table lives as
     * long as the mod that published it: re-find it after MML_EVENT_SERVICES_CHANGED. */
    int (*service_publish)(MmlMod *mod, const char *name, uint32_t version, const void *table);
    const void *(*service_find)(const char *name, uint32_t min_version, uint32_t *version);

    /* ---- run fn(user) on the game thread at the next frame (callable from any thread) */
    void (*post)(MmlMod *mod, void (*fn)(void *user), void *user);
} MmlHost;

typedef int (*MmlModLoadFn)(const MmlHost *host, MmlMod *mod);
typedef void (*MmlModUnloadFn)(MmlMod *mod);
typedef uint32_t (*MmlModApiFn)(void);

#ifdef __cplusplus
}
#endif

#endif /* MML_H */
