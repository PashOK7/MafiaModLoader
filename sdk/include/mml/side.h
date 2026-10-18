/* MafiaModLoader: the C ABI of a mod's 64-bit half, run by the sidecar (mml-sidecar64.exe).
 *
 * The game is a 32-bit process: a library made only for 64 bits (PhysX 5, NVIDIA Flow 2, a modern GPU or ML
 * runtime) cannot be loaded into it. A mod may have a 64-bit half for them: its manifest names it with "entry64"
 * (a path inside the mod's folder, e.g. "x64/flow_smoke.dll"; the folder that holds it is the half's own: its
 * libraries go beside it). The loader starts the sidecar, a 64-bit process beside the game (it ends with the game),
 * and the sidecar loads the half from a fresh copy of that folder, as the loader does a 32-bit DLL: it can be rebuilt
 * while the game runs, and `mod.reload` reloads both halves. A mod with "entry64" and no "entry" is a 64-bit half
 * alone.
 *
 * What a half has:
 * - The whole bridge, as an agent has it (`call`: "player", "world.near", "camera.state", any mod's command): each
 *   call goes to the game and waits for its answer (the game thread's methods answer at its next frame).
 * - The game's events as they come (`subscribe`): MISSION_TICK and FRAME with their dt, and every event the bridge
 *   keeps for agents (SHOT, HUMAN_HIT, CAR_CRASH, MISSION_LOADED...: docs/PROTOCOL.md "Events"), as JSON.
 * - Commands of its own on the bridge (`command`: "<mod>.<name>"), answered by the sidecar without the game thread.
 * - Shared textures (`texture_create`): Direct3D 12 textures on the adapter the game draws on, which the mod's 32-bit
 *   half reads in its graphics passes (mml.h gfx_shared_texture): how a 64-bit simulation hands the game its picture
 *   or its data without a copy through memory. Its Vulkan or Direct3D 12 code imports the texture by its handle.
 * - Channels (`channel_open`): a named block of memory both halves see, each write a whole new version (the reader
 *   always gets the latest whole one): bodies, poses, parameters, a frame's worth of anything.
 *
 * The contract:
 * - Callbacks (events, commands, load and unload) run on the sidecar's mod thread, one at a time; a half may start
 *   threads of its own (and stops them in mml_side_unload). `call` may be made from any thread.
 * - A callback that faults unloads its half (the sidecar and the other halves go on; MOD_FAULTED is told).
 * - MmlSideHost only grows (MML_SIDE_HAS); every string is UTF-8.
 *
 * A half exports:
 *   MML_EXPORT int  mml_side_load(const MmlSideHost *host, MmlSideMod *mod);   0: loaded
 *   MML_EXPORT void mml_side_unload(MmlSideMod *mod);
 *   MML_EXPORT uint32_t mml_side_api(void);                                    MML_SIDE_API_VERSION (optional)
 */
#ifndef MML_SIDE_H
#define MML_SIDE_H

#include <stddef.h>
#include <stdint.h>

#define MML_SIDE_API_VERSION 1u

#ifdef __cplusplus
#define MML_SIDE_EXTERN_C extern "C"
#else
#define MML_SIDE_EXTERN_C
#endif
#ifndef MML_EXPORT
#define MML_EXPORT MML_SIDE_EXTERN_C __declspec(dllexport)
#endif

#define MML_SIDE_HAS(table, member) ((size_t)(table)->size >= offsetof(MmlSideHost, member) + sizeof((table)->member))

#ifdef __cplusplus
extern "C" {
#endif

typedef struct MmlSideMod MmlSideMod;       /* one load of a 64-bit half */
typedef struct MmlSideReply MmlSideReply;   /* a command's reply being written */
typedef struct MmlSideChannel MmlSideChannel;

/* An event: its type ("MISSION_TICK", "FRAME", "SHOT"...) and its fields as a JSON object ({"dt": 0.016} for the
 * ticks). */
typedef void (*MmlSideEventFn)(MmlSideMod *mod, const char *type, const char *json, void *user);
/* A command: args is the bridge call's params (a JSON text); write the reply (JSON, or plain text) and return 0, or
 * reply_error and return non-zero. */
typedef int (*MmlSideCommandFn)(MmlSideMod *mod, const char *args, MmlSideReply *reply, void *user);
/* A bridge call's answer: ok 1 with the result (JSON), or 0 with the error's text. */
typedef void (*MmlSideReplyFn)(int ok, const char *text, size_t len, void *user);

typedef struct MmlSideTexture {
    uint32_t size;              /* sizeof(MmlSideTexture), set by the caller */
    uint32_t width, height;
    uint32_t format;            /* a DXGI_FORMAT: DXGI_FORMAT_R32_UINT (42), R16G16B16A16_FLOAT (10), R8G8B8A8_UNORM (28),
                                   B8G8R8A8_UNORM (87), R10G10B10A2_UNORM (24), R32_FLOAT (41): what Direct3D 11 opens */
    void *d3d12_resource;       /* ID3D12Resource* (the sidecar's Direct3D 12 device: texture_d3d12_device) */
    void *shared_handle;        /* its NT handle in this process: Vulkan imports it (VK_EXTERNAL_MEMORY_HANDLE_TYPE_D3D12_RESOURCE_BIT,
                                   a dedicated allocation), Direct3D 12 opens it (OpenSharedHandle) */
    uint32_t generation;        /* bumped when the texture is made anew */
} MmlSideTexture;

typedef struct MmlSideHost {
    uint32_t size;              /* sizeof(MmlSideHost) of the sidecar that made it */
    uint32_t version;           /* MML_SIDE_API_VERSION */

    const char *(*game_dir)(void);
    const char *(*game_build)(void);                 /* "1.2", "1.0" */
    const char *(*mod_id)(const MmlSideMod *mod);
    const char *(*mod_dir)(const MmlSideMod *mod);   /* the mod's folder in the game (not the load's copy) */
    const char *(*data_dir)(const MmlSideMod *mod);  /* the mod's data folder (mml.h mod_data_dir: the same one) */
    void (*log)(const MmlSideMod *mod, int level, const char *fmt, ...);   /* level: mml.h MmlLogLevel */

    /* Any bridge method or mod command, as an agent calls it (method, params as JSON or null), from any thread;
     * fn gets the answer before call returns. 1: answered ok, 0: an error (the text tells), -1: no answer in time. */
    int (*call)(MmlSideMod *mod, const char *method, const char *params, MmlSideReplyFn fn, void *user, uint32_t timeout_ms);

    int (*subscribe)(MmlSideMod *mod, const char *type, MmlSideEventFn fn, void *user);   /* "*": every event */
    int (*command)(MmlSideMod *mod, const char *name, const char *help, MmlSideCommandFn fn, void *user);
    void (*reply_append)(MmlSideReply *reply, const char *text, size_t len);
    void (*reply_error)(MmlSideReply *reply, const char *message);

    /* Shared textures. The adapter the game draws on is known once its graphics bridge runs (a mod with "gfx": true
     * on at the start); until then texture_create fails and adapter_luid returns 0. */
    int (*adapter_luid)(uint32_t luid[2]);
    void *(*texture_d3d12_device)(void);             /* ID3D12Device* on that adapter (the sidecar's; not to be released) */
    int (*texture_create)(MmlSideMod *mod, const char *name, uint32_t width, uint32_t height, uint32_t format, MmlSideTexture *out);
    void (*texture_remove)(MmlSideMod *mod, const char *name);

    /* Channels: "<name>" of this mod; both halves open it with the same size. A write is a whole new version (at
     * most size bytes); a read copies the latest whole version (its length), its version number in *version (0:
     * nothing written yet). */
    MmlSideChannel *(*channel_open)(MmlSideMod *mod, const char *name, uint32_t size);
    int (*channel_write)(MmlSideChannel *ch, const void *data, uint32_t bytes);
    uint32_t (*channel_read)(MmlSideChannel *ch, void *data, uint32_t cap, uint64_t *version);
} MmlSideHost;

typedef int (*MmlSideLoadFn)(const MmlSideHost *host, MmlSideMod *mod);
typedef void (*MmlSideUnloadFn)(MmlSideMod *mod);
typedef uint32_t (*MmlSideApiFn)(void);

#ifdef __cplusplus
}
#endif

#endif
