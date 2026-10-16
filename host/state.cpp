// The game's state (mml.h money, human_health, human_items, item_ammo_set, human_take_item; docs/MODDING.md "The
// game's state: money, health, the inventory").
//
// The freeride's money is C_game +0x3640 (what the scripts' FREERIDE_SCOREGET reads), set through the game's own
// C_game::SetScore (FREERIDE_SCORESET's call), which also tells the HUD when the money is shown (+0x363E). A
// person's health is C_human +0x644 and his maximum +0x604 (the current and the maximal properties; the game's hits
// subtract from the first); the player's is told to the HUD as C_player::Hit tells it (PlayerSetLives). The inventory is the G_Inventory embedded at C_human +0x480 in both builds (MafiaOnline's
// native_inventory.h for 1.2, st0rm94's structs/inventory.md for 1.0, the scripts' HUMAN_DELWEAPON taking its
// address in both): the item in the hand, five belt slots, the coat's, four extra items, the one put away in a car,
// each 0x10 bytes {u16 id, u16, u32 rounds in it, u32 in reserve, u32}. An item is taken with the scripts' own
// human_delweapon (as world.give gives with human_addweapon).
//
// Bridge methods (game thread): state.money {set}, human.health {actor, set}, human.items {actor},
// human.ammo {actor, slot, loaded, reserve}, human.take {actor, item}.
#include "host.h"
#include <algorithm>
#include <climits>
#include <cmath>
#include <cstring>
#include <vector>
#include "json.hpp"
#include "mml/game/game_build.h"
#include "mml/game/game.h"

using json = nlohmann::json;

namespace host {
namespace {

using SetScoreFn = void(__thiscall *)(void *game, int value);
using SetLivesFn = void(__thiscall *)(void *hud, unsigned value);

constexpr int kSlotOffsets[MML_SLOT_COUNT] = {0x20, 0x30, 0x40, 0x50, 0x60, 0x70, 0x80, 0x90, 0xA0, 0xB0, 0xC0, 0xD0};
constexpr int kExtraCount = 0x08;

template <class F> DWORD Guarded(F &f) {
    __try { f(); return 0; } __except (EXCEPTION_EXECUTE_HANDLER) { return GetExceptionCode(); }
}

bool IsPerson(void *h) {
    bool ok = false;
    auto f = [&]() { int t = game::at<int>(h, game::kActorType); ok = t == 2 || t == 27; };
    return h && !Guarded(f) && ok;
}
uint8_t *Inventory(void *h) { return static_cast<uint8_t *>(h) + game::kHumanInventory; }

std::string HexOf(const void *p) {
    char b[16];
    snprintf(b, sizeof b, "0x%08X", static_cast<unsigned>(reinterpret_cast<uintptr_t>(p)));
    return b;
}

// An actor named by the bridge (default the player): a person, or why not.
bool Person(const json &p, void **h, std::string *err) {
    std::string a = p.contains("actor") && p["actor"].is_string() ? p["actor"].get<std::string>() : "player";
    if (!ParseActor(a, h, err)) return false;
    if (!IsPerson(*h)) { *err = a + " is not a person (the player, type 2, or a character, type 27)"; return false; }
    return true;
}

} // namespace

int StateMoney() {
    if (!game::g_exe) return INT_MIN;
    int v = INT_MIN;
    auto f = [&]() { if (void *g = game::Game()) v = game::at<int>(g, game::kGameScore); };
    if (Guarded(f)) return INT_MIN;
    return v;
}

bool StateMoneySet(int value, std::string *err) {
    if (!game::g_exe || !game::Exe().gameSetScore) { *err = "this build's money is not known"; return false; }
    void *g = game::Game();
    if (!g) { *err = "no mission"; return false; }
    auto f = [&]() { reinterpret_cast<SetScoreFn>(game::Exe().gameSetScore)(g, value); };
    if (Guarded(f)) { *err = "C_game::SetScore faulted"; return false; }
    return true;
}

float StateHealth(void *h, float *max) {
    if (!game::g_exe || !IsPerson(h)) return -1;
    float v = -1, m = 0;
    auto f = [&]() { v = game::at<float>(h, game::kHumanHealth); m = game::at<float>(h, game::kHumanHealthMax); };
    if (Guarded(f)) return -1;
    if (max) *max = m;
    return v;
}

bool StateHealthSet(void *h, float health, std::string *err) {
    float max = 0, now = StateHealth(h, &max);
    if (now < 0) { *err = "not a person"; return false; }
    if (now <= 0) { *err = "he is dead (the game's death stays)"; return false; }
    if (!(health > 0) || !std::isfinite(health)) { *err = "health: above 0 (a death is the game's: a hit, a script)"; return false; }
    if (max > 0 && health > max) health = max;
    auto f = [&]() { game::at<float>(h, game::kHumanHealth) = health; };
    if (Guarded(f)) { *err = "the write faulted"; return false; }
    // the player's: the HUD's number told as C_player::Hit tells it (the health of the maximum, in percent, 1 at least)
    if (h == game::Player() && game::Exe().hudSetLives && max > 0) {
        unsigned v = std::max(1u, static_cast<unsigned>(std::lround(health / max * 100.0f)));
        auto hud = [&]() { reinterpret_cast<SetLivesFn>(game::Exe().hudSetLives)(reinterpret_cast<void *>(game::Exe().hud), v); };
        if (Guarded(hud)) { *err = "the HUD's PlayerSetLives faulted (the health is written)"; return false; }
    }
    return true;
}

int StateItems(void *h, MmlItem *out, int max) {
    if (!game::g_exe || !IsPerson(h)) return 0;
    int n = 0;
    auto f = [&]() {
        uint8_t *inv = Inventory(h);
        uint32_t extra = game::at<uint32_t>(inv, kExtraCount);
        for (int s = 0; s < MML_SLOT_COUNT; s++) {
            if (s >= MML_SLOT_EXTRA && s < MML_SLOT_CAR && static_cast<uint32_t>(s - MML_SLOT_EXTRA) >= std::min(extra, 4u)) continue;
            uint8_t *it = inv + kSlotOffsets[s];
            uint16_t id = game::at<uint16_t>(it, 0);
            if (!id) continue;
            if (out && n < max) out[n] = MmlItem{s, id, game::at<int>(it, 4), game::at<int>(it, 8)};
            n++;
        }
    };
    if (Guarded(f)) return 0;
    return n;
}

bool StateAmmoSet(void *h, int slot, int loaded, int reserve, std::string *err) {
    if (!IsPerson(h)) { *err = "not a person"; return false; }
    if (slot < 0 || slot >= MML_SLOT_COUNT) { *err = "slot: 0 (the hand) .. 11 (the car's)"; return false; }
    bool empty = false;
    auto f = [&]() {
        uint8_t *it = Inventory(h) + kSlotOffsets[slot];
        if (!game::at<uint16_t>(it, 0)) { empty = true; return; }
        if (loaded >= 0) game::at<int>(it, 4) = loaded;
        if (reserve >= 0) game::at<int>(it, 8) = reserve;
    };
    if (Guarded(f)) { *err = "the write faulted"; return false; }
    if (empty) { *err = "slot " + std::to_string(slot) + " holds nothing"; return false; }
    return true;
}

bool StateTakeItem(void *h, int item, std::string *err) {
    if (!IsPerson(h)) { *err = "not a person (the player, type 2, or a character, type 27)"; return false; }
    if (item <= 0 || WorldItemName(item).empty()) { *err = "no item " + std::to_string(item) + " (world.items)"; return false; }
    char code[64];
    snprintf(code, sizeof code, "human_delweapon 0, %d", item);
    return ScriptExec(code, nullptr, &h, 1, nullptr, 0, nullptr, nullptr, err);
}

bool StateMethod(const std::string &method, const std::string &paramsJson, std::string *reply, bool *ok) {
    if (method != "state.money" && method != "human.health" && method != "human.items" && method != "human.ammo" && method != "human.take") return false;
    *ok = false;
    json p = json::parse(paramsJson.empty() ? "null" : paramsJson, nullptr, false);
    if (!p.is_object()) p = json::object();
    auto dump = [](const json &j) { return j.dump(-1, ' ', false, json::error_handler_t::replace); };
    std::string err;
    try {
        if (method == "state.money") {
            if (p.contains("set") || p.contains("add")) {
                int now = StateMoney();
                if (now == INT_MIN) { *reply = "state.money: no game"; return true; }
                long long v = p.contains("set") ? p["set"].get<long long>() : static_cast<long long>(now) + p["add"].get<long long>();
                v = std::min<long long>(std::max<long long>(v, INT_MIN + 1LL), INT_MAX);
                if (!StateMoneySet(static_cast<int>(v), &err)) { *reply = "state.money: " + err; return true; }
            }
            int m = StateMoney();
            json out = {{"money", m == INT_MIN ? json() : json(m)}};
            if (void *g = game::g_exe ? game::Game() : nullptr) out["shown"] = game::at<uint8_t>(g, game::kGameScoreShown) != 0;
            *reply = dump(out);
        } else {
            void *h = nullptr;
            if (!Person(p, &h, &err)) { *reply = method + ": " + err; return true; }
            if (method == "human.health") {
                if (p.contains("set") && !StateHealthSet(h, p["set"].get<float>(), &err)) { *reply = "human.health: " + err; return true; }
                float max = 0, v = StateHealth(h, &max);
                *reply = dump({{"actor", HexOf(h)}, {"health", v}, {"max", max}});
            } else if (method == "human.ammo") {
                int slot = p.value("slot", 0);
                if (!StateAmmoSet(h, slot, p.value("loaded", -1), p.value("reserve", -1), &err)) { *reply = "human.ammo: " + err; return true; }
            } else if (method == "human.take") {
                int item = -1;
                if (p.contains("item") && p["item"].is_number_integer()) item = p["item"].get<int>();
                else if (p.contains("item") && p["item"].is_string()) {
                    std::string name = p["item"].get<std::string>();
                    item = WorldItemId(name.c_str(), &err);
                    if (item < 0) { *reply = "item: " + err; return true; }
                }
                if (!StateTakeItem(h, item, &err)) { *reply = "human.take: " + err; return true; }
            }
            if (method != "human.health") {
                MmlItem items[MML_SLOT_COUNT];
                int n = StateItems(h, items, MML_SLOT_COUNT);
                const char *slotNames[MML_SLOT_COUNT] = {"hand", "belt 1", "belt 2", "belt 3", "belt 4", "belt 5", "coat", "extra 1", "extra 2", "extra 3", "extra 4", "car"};
                json list = json::array();
                for (int i = 0; i < n && i < MML_SLOT_COUNT; i++)
                    list.push_back({{"slot", items[i].slot}, {"where", slotNames[items[i].slot]}, {"id", items[i].id}, {"name", WorldItemName(items[i].id)},
                                    {"loaded", items[i].loaded}, {"reserve", items[i].reserve}});
                *reply = dump({{"actor", HexOf(h)}, {"items", list}});
            }
        }
        *ok = true;
    } catch (const json::exception &e) {
        *reply = e.what();
    }
    return true;
}

} // namespace host
