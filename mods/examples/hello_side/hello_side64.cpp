// hello_side: the smallest 64-bit half (sdk/include/mml/side.h; docs/MODDING.md "64-bit halves"). It runs in the
// sidecar, a 64-bit process beside the game: it counts the game's frames, asks the bridge where the player is, and
// answers two commands of its own.
//   call hello_side.status        -> {"frames": ..., "ticks": ..., "pointer_bits": 64}
//   call hello_side.player        -> the bridge's "player", asked from the 64-bit side
#include <cstdio>
#include <string>
#include "mml/side.h"

namespace {
const MmlSideHost *H;
unsigned long long g_frames, g_ticks;

void OnEvent(MmlSideMod *, const char *type, const char *, void *) {
    if (type[0] == 'F') g_frames++;
    else g_ticks++;
}

int Status(MmlSideMod *, const char *, MmlSideReply *r, void *) {
    char b[160];
    snprintf(b, sizeof b, "{\"frames\": %llu, \"ticks\": %llu, \"pointer_bits\": %u}", g_frames, g_ticks,
             static_cast<unsigned>(sizeof(void *) * 8));
    H->reply_append(r, b, strlen(b));
    return 0;
}

int Player(MmlSideMod *m, const char *, MmlSideReply *r, void *) {
    struct Got { std::string text; int ok; } got{std::string(), 0};
    int rc = H->call(m, "player", nullptr, [](int ok, const char *t, size_t n, void *u) {
        auto *g = static_cast<Got *>(u);
        g->ok = ok;
        g->text.assign(t, n);
    }, &got, 3000);
    if (rc != 1) { H->reply_error(r, got.text.c_str()); return 1; }
    H->reply_append(r, got.text.data(), got.text.size());
    return 0;
}
} // namespace

MML_EXPORT uint32_t mml_side_api(void) { return MML_SIDE_API_VERSION; }

MML_EXPORT int mml_side_load(const MmlSideHost *host, MmlSideMod *mod) {
    H = host;
    H->subscribe(mod, "FRAME", OnEvent, nullptr);
    H->subscribe(mod, "MISSION_TICK", OnEvent, nullptr);
    H->command(mod, "status", "{} -> the frames and ticks seen from the 64-bit side", Status, nullptr);
    H->command(mod, "player", "{} -> the bridge's \"player\", asked from the 64-bit side", Player, nullptr);
    H->log(mod, 1, "hello from 64 bits (the game %s, %s)", H->game_build(), H->game_dir());
    return 0;
}

MML_EXPORT void mml_side_unload(MmlSideMod *mod) { H->log(mod, 1, "goodbye after %llu frames", g_frames); }
