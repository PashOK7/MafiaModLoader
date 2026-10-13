/* hello_c: the smallest complete native mod, in plain C.
 *
 * It counts frames and mission ticks, keeps the counts across its own hot reloads (state_put/state_get), and answers
 * two commands over the bridge: `hello_c.status` and `hello_c.say {"text": "..."}`. Rebuild it while the game runs
 * and call `mod.reload {"id": "hello_c"}`: the counts go on from where they were. */
#include <stdio.h>
#include <string.h>
#include "mml/mml.h"

static const MmlHost *H;
static struct { unsigned frames, ticks, loads; } S;   /* what survives a reload */

static void OnFrame(const MmlEvent *ev, void *user) { (void)ev; (void)user; S.frames++; }
static void OnTick(const MmlEvent *ev, void *user) { (void)ev; (void)user; S.ticks++; }

static int Status(const char *args, MmlReply *reply, void *user) {
    char out[160];
    (void)args; (void)user;
    snprintf(out, sizeof out, "{\"frames\":%u,\"mission_ticks\":%u,\"loads\":%u,\"game\":\"%s\"}", S.frames, S.ticks, S.loads,
             H->game_build());
    H->reply_append(reply, out, (size_t)-1);
    return 0;
}

static int Say(const char *args, MmlReply *reply, void *user) {
    (void)user;
    H->log(NULL, MML_LOG_INFO, "hello_c says: %s", args);   /* args is JSON text: {"text": "..."} */
    H->reply_append(reply, "true", (size_t)-1);
    return 0;
}

MML_EXPORT uint32_t mml_mod_api(void) { return MML_API_VERSION; }

MML_EXPORT int mml_mod_load(const MmlHost *host, MmlMod *mod) {
    H = host;
    host->state_get(mod, "counts", &S, sizeof S);   /* nothing the first time */
    S.loads++;
    host->subscribe(mod, MML_EVENT_FRAME, OnFrame, NULL);
    host->subscribe(mod, MML_EVENT_MISSION_TICK, OnTick, NULL);
    host->command(mod, "status", "frames and mission ticks seen, loads of this mod", Status, NULL);
    host->command(mod, "say", "log a line (params: {\"text\": \"...\"})", Say, NULL);
    host->log(mod, MML_LOG_INFO, "hello from C (load %u, game %s, mod folder %s)", S.loads, host->game_build(), host->mod_dir(mod));
    return 0;
}

MML_EXPORT void mml_mod_unload(MmlMod *mod) {
    H->state_put(mod, "counts", &S, sizeof S);
}
