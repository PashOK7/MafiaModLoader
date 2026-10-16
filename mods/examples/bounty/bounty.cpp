// bounty: the game's state read and changed (money, money_set, human_health, human_items, item_ammo_set;
// docs/MODDING.md "The game's state: money, health, the inventory").
//
// In the freeride: each person the player kills pays `reward` dollars (the game's own HUD shows the money: it is the
// freeride's own, set through the game's setter); his health comes back `regen` points a second once nothing has hit
// him for `regen_after` seconds; with `endless_ammo` the weapon in his hand never runs out of reserve rounds.
#include <windows.h>
#include <cstdio>
#include <cstring>
#include <initializer_list>
#include "mml/mml.h"

namespace {
const MmlHost *H;
MmlMod *M;
int g_reward = 50, g_kills, g_paid;
float g_regen = 2, g_regenAfter = 8;
bool g_endless;
DWORD g_lastHit, g_lastTick;
double g_carry;   // the health owed between frames (whole points are written)

void Settings() {
    g_reward = static_cast<int>(H->config_number(M, "reward", 50));
    g_regen = static_cast<float>(H->config_number(M, "regen", 2));
    g_regenAfter = static_cast<float>(H->config_number(M, "regen_after", 8));
    g_endless = H->config_number(M, "endless_ammo", 0) != 0;
}

void Frame() {
    DWORD now = GetTickCount();
    float dt = (now - g_lastTick) / 1000.0f;
    g_lastTick = now;
    void *p = H->human_player(M);
    if (!p || H->mission_state() != MML_MISSION_RUNNING || dt <= 0 || dt > 0.5f) return;
    float max = 0, hp = H->human_health(M, p, &max);
    if (hp > 0 && hp < max && g_regen > 0 && now - g_lastHit > static_cast<DWORD>(g_regenAfter * 1000)) {
        g_carry += g_regen * dt;
        if (g_carry >= 1) {
            float add = static_cast<float>(static_cast<int>(g_carry));
            g_carry -= add;
            H->human_health_set(M, p, hp + add > max ? max : hp + add);
        }
    }
    if (g_endless) {
        MmlItem items[MML_SLOT_COUNT];
        int n = H->human_items(M, p, items, MML_SLOT_COUNT);
        for (int i = 0; i < n; i++)
            if (items[i].slot == MML_SLOT_HAND && items[i].reserve < 100) H->item_ammo_set(M, p, MML_SLOT_HAND, -1, 100);
    }
}

void OnEvent(const MmlEvent *ev, void *) {
    switch (ev->type) {
    case MML_EVENT_CONFIG_CHANGED: Settings(); break;
    case MML_EVENT_FRAME: Frame(); break;
    case MML_EVENT_HUMAN_HIT:
        if (ev->actor && ev->actor == H->human_player(M)) { g_lastHit = GetTickCount(); g_carry = 0; }
        break;
    case MML_EVENT_HUMAN_DIED: {
        void *p = H->human_player(M);
        if (!p || ev->other != p || ev->actor == p) break;
        g_kills++;
        int money = H->money(M);
        if (money != INT32_MIN && g_reward && H->money_set(M, money + g_reward)) g_paid += g_reward;
        break;
    }
    default: break;
    }
}

int Status(const char *, MmlReply *r, void *) {
    void *p = H->human_player(M);
    float max = 0, hp = p ? H->human_health(M, p, &max) : -1;
    MmlItem items[MML_SLOT_COUNT];
    int n = p ? H->human_items(M, p, items, MML_SLOT_COUNT) : 0;
    char out[640];
    int len = snprintf(out, sizeof out, "{\"money\":%d,\"kills\":%d,\"paid\":%d,\"health\":%.1f,\"max\":%.1f,\"items\":[", H->money(M), g_kills,
                       g_paid, hp, max);
    for (int i = 0; i < n && len < 560; i++)
        len += snprintf(out + len, sizeof out - len, "%s{\"slot\":%d,\"id\":%d,\"loaded\":%d,\"reserve\":%d}", i ? "," : "", items[i].slot, items[i].id,
                        items[i].loaded, items[i].reserve);
    snprintf(out + len, sizeof out - len, "]}");
    H->reply_append(r, out, static_cast<size_t>(-1));
    return 0;
}
} // namespace

MML_EXPORT int mml_mod_load(const MmlHost *host, MmlMod *mod) {
    H = host;
    M = mod;
    if (!MML_HAS(H, human_take_item)) return 1;   // a loader without the game's state
    Settings();
    g_lastTick = g_lastHit = GetTickCount();
    for (MmlEventType t : {MML_EVENT_FRAME, MML_EVENT_CONFIG_CHANGED, MML_EVENT_HUMAN_HIT, MML_EVENT_HUMAN_DIED}) H->subscribe(M, t, OnEvent, nullptr);
    H->command(M, "status", "the money, the kills paid, the player's health and inventory", Status, nullptr);
    return 0;
}

MML_EXPORT void mml_mod_unload(MmlMod *) {}
