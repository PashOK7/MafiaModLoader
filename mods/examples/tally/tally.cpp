// tally: what happens in the game, counted and told (MML_EVENT_SHOT, EXPLOSION, HUMAN_DIED, VEHICLE_ENTERED,
// VEHICLE_LEFT, CAR_EXPLODED, HUMAN_HIT; docs/MODDING.md "What happens"): the base an achievements mod stands on.
//
// A small panel in the top right corner counts the player's shots, the people he killed (the deaths whose killer was
// him), the people who died at all, the explosions, the cars blown up, the vehicles he took; each new thing shows a
// line under it for a few seconds ("+1 kill"). `tally.status` gives the counts, `tally.reset` starts them over; the
// counts are kept in the mod's state across reloads.
#include <windows.h>
#include <cstdio>
#include <cstring>
#include <string>
#include "mml/mml.h"

namespace {
const MmlHost *H;
MmlMod *M;

struct Counts { unsigned shots, kills, deaths, explosions, cars, rides; } C;
char g_note[96];
DWORD g_noteUntil;

void Note(const char *text) {
    snprintf(g_note, sizeof g_note, "%s", text);
    g_noteUntil = GetTickCount() + 2500;
}

void OnEvent(const MmlEvent *ev, void *) {
    void *player = H->human_player(M);
    switch (ev->type) {
    case MML_EVENT_SHOT:
        if (ev->actor == player && player) C.shots++;
        break;
    case MML_EVENT_HUMAN_DIED:
        if (ev->actor == player) { Note("you died"); break; }
        C.deaths++;
        if (MML_EVENT_HAS(ev, other) && ev->other == player && player) { C.kills++; Note("+1 kill"); }
        break;
    case MML_EVENT_EXPLOSION:
        C.explosions++;
        break;
    case MML_EVENT_CAR_EXPLODED:
        C.cars++;
        Note("a car blew up");
        break;
    case MML_EVENT_VEHICLE_ENTERED:
        if (ev->actor == player && player) { C.rides++; Note("in a vehicle"); }
        break;
    case MML_EVENT_FRAME: {
        if (H->mission_state() != MML_MISSION_RUNNING) break;
        float x = 610, y = 70;
        H->draw_rect(M, x - 6, y - 4, 186, 92, 0xA0101418u, 1);
        char line[96];
        snprintf(line, sizeof line, "shots %u   kills %u", C.shots, C.kills);
        H->draw_text(M, x, y, 15, 0xFFFFFFFFu, MML_FONT_SMALL, MML_DRAW_LEFT, line);
        snprintf(line, sizeof line, "deaths %u   rides %u", C.deaths, C.rides);
        H->draw_text(M, x, y + 20, 15, 0xFFC8C8C8u, MML_FONT_SMALL, MML_DRAW_LEFT, line);
        snprintf(line, sizeof line, "explosions %u   cars %u", C.explosions, C.cars);
        H->draw_text(M, x, y + 40, 15, 0xFFC8C8C8u, MML_FONT_SMALL, MML_DRAW_LEFT, line);
        if (GetTickCount() < g_noteUntil) H->draw_text(M, x, y + 62, 17, 0xFFFFD040u, MML_FONT_BIG, MML_DRAW_LEFT, g_note);
        break;
    }
    default: break;
    }
}

int Status(const char *, MmlReply *r, void *) {
    char out[192];
    snprintf(out, sizeof out, "{\"shots\":%u,\"kills\":%u,\"deaths\":%u,\"explosions\":%u,\"cars\":%u,\"rides\":%u}", C.shots, C.kills,
             C.deaths, C.explosions, C.cars, C.rides);
    H->reply_append(r, out, static_cast<size_t>(-1));
    return 0;
}
int Reset(const char *a, MmlReply *r, void *u) {
    C = Counts{};
    return Status(a, r, u);
}
} // namespace

MML_EXPORT int mml_mod_load(const MmlHost *host, MmlMod *mod) {
    H = host;
    M = mod;
    if (!MML_HAS(H, human_player)) return 1;   // a loader without these events
    H->state_get(M, "counts", &C, sizeof C);   // (kept across reloads)
    for (MmlEventType t : {MML_EVENT_SHOT, MML_EVENT_HUMAN_DIED, MML_EVENT_EXPLOSION, MML_EVENT_CAR_EXPLODED, MML_EVENT_VEHICLE_ENTERED,
                           MML_EVENT_FRAME})
        H->subscribe(M, t, OnEvent, nullptr);
    H->command(M, "status", "the counts", Status, nullptr);
    H->command(M, "reset", "the counts start over", Reset, nullptr);
    return 0;
}

MML_EXPORT void mml_mod_unload(MmlMod *mod) { H->state_put(mod, "counts", &C, sizeof C); }
