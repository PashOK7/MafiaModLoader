// Statistics and the profile's progress (mml.h stats_get, profile_progress, profile_saves; docs/MODDING.md
// "Statistics, the profile's progress").
//
// The game keeps no statistics of its own: the loader counts what the player does from the events every module
// sends (StatsEvent, from Dispatch: HUMAN_DIED with him as the killer or his vehicle, HUMAN_HIT and PEDESTRIAN_HIT
// he gave, SHOT, VEHICLE_ENTERED, CAR_CRASH of 5 m/s and more, HUMAN_FELL that hurt, ITEM_GAINED, MONEY_CHANGED)
// and from his place every frame (StatsFrame: the distance on foot and in vehicles, a move faster than 15 m/s on
// foot or 80 in a vehicle left out as a teleport; the time the mission's world runs). Two sets: the game being
// played (its saves keep it under the owner "mml", key "stats": put at GAME_SAVING, read back at GAME_LOADED, zero
// at a new game or a freeride) and the profile's totals (beside its .sav, written at GAME_SAVING, at the mission's
// close and every 10 s of play: the game has no exit the loader hears).
//
// The progress is the game's own: the profile manager (game_build.h loadSave, G_LoadSaveClass; the same layout in
// both builds) keeps the selected profile's header at +0x20 (its number +0x28, its name +0x30), the race-unlock
// mask +0x94, the steal mask +0x98, the freeride's bonus cars +0x9C (bit n: the letter 'A' + n), the garage's 25
// places +0xA0 ({packed car id: the database's index in the low 16 bits, the colour in the top byte; C_car*}) and
// the freeride's level word +0x1F0, all of which ProfileSave writes to the .sav. The chapters are the saves on disk
// (savegame\mafia<p>.<slot>, as SaveGameEnumFiles reads them), each named by the game's text 5000 + slot
// ("05-03 Fairplay - The Racing Circuit": the chapter is its number).
//
// Bridge methods (game thread): stats.state, profile.progress.
#include "host.h"
#include <algorithm>
#include <cmath>
#include <cstring>
#include "json.hpp"
#include "mml/game/game_build.h"
#include "mml/game/game.h"

using json = nlohmann::json;

namespace host {
namespace {

constexpr const char *kOwner = "mml", *kKey = "stats";
constexpr int kSlots = 600, kGarage = 25;
constexpr int kProfNumber = 0x28, kProfName = 0x30, kRaceMask = 0x94, kStealMask = 0x98, kFreerideMask = 0x9C,
              kGarageTable = 0xA0, kFreerideLevel = 0x1F0;

struct Counts {
    int kills = 0, deaths = 0, shots = 0, hits = 0, cars = 0, crashes = 0, falls = 0, items = 0;
    double foot = 0, vehicle = 0, played = 0;
    long long earned = 0, spent = 0;
};
Counts g_game, g_profile;
int g_profileNumber = -2;    // whose totals g_profile holds (-2: not read yet)
double g_sinceWrite;
bool g_havePos;
V3 g_lastPos{};

json ToJson(const Counts &c) {
    return {{"kills", c.kills}, {"deaths", c.deaths}, {"shots", c.shots}, {"hits", c.hits}, {"cars_driven", c.cars},
            {"crashes", c.crashes}, {"falls", c.falls}, {"items", c.items}, {"on_foot_m", std::round(c.foot * 10) / 10},
            {"in_vehicle_m", std::round(c.vehicle * 10) / 10}, {"played_s", std::round(c.played * 10) / 10},
            {"money_earned", c.earned}, {"money_spent", c.spent}};
}
Counts FromJson(const json &j) {
    Counts c;
    if (!j.is_object()) return c;
    c.kills = j.value("kills", 0); c.deaths = j.value("deaths", 0); c.shots = j.value("shots", 0); c.hits = j.value("hits", 0);
    c.cars = j.value("cars_driven", 0); c.crashes = j.value("crashes", 0); c.falls = j.value("falls", 0); c.items = j.value("items", 0);
    c.foot = j.value("on_foot_m", 0.0); c.vehicle = j.value("in_vehicle_m", 0.0); c.played = j.value("played_s", 0.0);
    c.earned = j.value("money_earned", 0LL); c.spent = j.value("money_spent", 0LL);
    return c;
}
Counts Read(int scope) {
    size_t n = SaveGetAs(kOwner, scope, kKey, nullptr, 0);
    if (!n) return Counts{};
    std::string s(n, '\0');
    SaveGetAs(kOwner, scope, kKey, &s[0], n);
    return FromJson(json::parse(s, nullptr, false));
}
void Write(int scope, const Counts &c) {
    std::string s = ToJson(c).dump(), err;
    if (!SavePutAs(kOwner, scope, kKey, s.data(), s.size(), &err) && scope == MML_SAVE_PROFILE && err != "no profile selected yet")
        HWARN("stats: %s", err.c_str());
}
// The profile's totals of the profile selected now (read when it changes).
Counts &Profile() {
    int p = SaveProfileNumber();
    if (p != g_profileNumber) {
        g_profileNumber = p;
        g_profile = p >= 0 ? Read(MML_SAVE_PROFILE) : Counts{};
    }
    return g_profile;
}
void WriteProfile() {
    if (g_profileNumber >= 0 && g_profileNumber == SaveProfileNumber()) Write(MML_SAVE_PROFILE, g_profile);
    g_sinceWrite = 0;
}

template <class F> void Both(F f) { f(g_game); f(Profile()); }

void *Player() { return game::g_exe && game::Mission() ? game::Player() : nullptr; }
void *PlayerVehicle() {
    void *p = Player();
    void *v = nullptr;
    if (p) MemRead(reinterpret_cast<uintptr_t>(p) + game::kHumanVehicle, &v, sizeof v);
    return v;
}

uintptr_t Manager() { return game::g_exe ? game::Exe().loadSave : 0; }
uint32_t R32(uintptr_t a) { uint32_t v = 0; MemRead(a, &v, 4); return v; }

std::string SaveName(int slot) {
    const char *t = GameTextOwn(5000 + static_cast<uint32_t>(slot));
    char buf[160] = {};
    if (t) MemRead(reinterpret_cast<uintptr_t>(t), buf, sizeof buf - 1);
    return buf;
}
int ChapterOf(const std::string &name) {
    return name.size() >= 2 && isdigit(static_cast<unsigned char>(name[0])) && isdigit(static_cast<unsigned char>(name[1]))
               ? (name[0] - '0') * 10 + (name[1] - '0') : 0;
}

} // namespace

void StatsEvent(const MmlEvent &ev) {
    switch (ev.type) {   // (every frame's events go by at once)
    case MML_EVENT_HUMAN_DIED: case MML_EVENT_HUMAN_HIT: case MML_EVENT_PEDESTRIAN_HIT: case MML_EVENT_SHOT:
    case MML_EVENT_VEHICLE_ENTERED: case MML_EVENT_CAR_CRASH: case MML_EVENT_HUMAN_FELL: case MML_EVENT_ITEM_GAINED:
    case MML_EVENT_MONEY_CHANGED: case MML_EVENT_GAME_SAVING: case MML_EVENT_GAME_LOADED: case MML_EVENT_MISSION_CLOSE:
        break;
    default:
        return;
    }
    if (ev.type == MML_EVENT_GAME_LOADED || ev.type == MML_EVENT_MISSION_CLOSE || ev.type == MML_EVENT_GAME_SAVING) {
        if (ev.type == MML_EVENT_GAME_SAVING) { Write(MML_SAVE_SLOT, g_game); Profile(); WriteProfile(); }
        else if (ev.type == MML_EVENT_GAME_LOADED) g_game = ev.slot >= 0 ? Read(MML_SAVE_SLOT) : Counts{};
        else WriteProfile();
        g_havePos = false;
        return;
    }
    void *p = Player();
    if (!p) return;
    void *car = PlayerVehicle();
    auto mine = [&](void *a) { return a && (a == p || (car && a == car)); };
    switch (ev.type) {
    case MML_EVENT_HUMAN_DIED:
        if (ev.actor == p) Both([](Counts &c) { c.deaths++; });
        else if (mine(ev.other)) Both([](Counts &c) { c.kills++; });
        break;
    case MML_EVENT_HUMAN_HIT: case MML_EVENT_PEDESTRIAN_HIT:
        if (ev.hit && ev.actor != p && mine(ev.hit->attacker)) Both([](Counts &c) { c.hits++; });
        break;
    case MML_EVENT_SHOT:
        if (ev.actor == p) Both([](Counts &c) { c.shots++; });
        break;
    case MML_EVENT_VEHICLE_ENTERED:
        if (ev.actor == p) Both([](Counts &c) { c.cars++; });
        break;
    case MML_EVENT_CAR_CRASH:
        if (car && ev.actor == car && ev.crash && ev.crash->speed >= 5.0f) Both([](Counts &c) { c.crashes++; });
        break;
    case MML_EVENT_HUMAN_FELL:
        if (ev.actor == p && ev.fall && ev.fall->speed >= 8.0f) Both([](Counts &c) { c.falls++; });
        break;
    case MML_EVENT_ITEM_GAINED:
        if (ev.actor == p && ev.item && ev.item->count > 0) Both([](Counts &c) { c.items++; });
        break;
    case MML_EVENT_MONEY_CHANGED:
        if (ev.change > 0) Both([&](Counts &c) { c.earned += ev.change; });
        else Both([&](Counts &c) { c.spent -= ev.change; });
        break;
    default:
        break;
    }
}

void StatsFrame(float dt, bool ticking) {
    void *p = Player();
    if (!p) { g_havePos = false; return; }
    if (ticking && dt > 0 && dt < 1) {
        Both([&](Counts &c) { c.played += dt; });
        g_sinceWrite += dt;
    }
    V3 pos{};
    bool ok = false;
    auto f = [&] { pos = game::ActorWorldPos(p); ok = true; };
    __try { f(); } __except (EXCEPTION_EXECUTE_HANDLER) { ok = false; }
    if (!ok) return;
    if (g_havePos && ticking) {
        V3 d = pos - g_lastPos;
        bool in = PlayerVehicle() != nullptr;
        if (!in) d.y = 0;   // (on foot: the ground covered, a fall is not walked)
        float m = std::sqrt(Dot(d, d));
        if (m > 0.001f && m < (in ? 80.0f : 15.0f) * dt + 0.3f) {   // (faster than a car, a runner: a teleport)
            Both([&](Counts &c) { (in ? c.vehicle : c.foot) += m; });
        }
    }
    g_lastPos = pos;
    g_havePos = true;
    if (g_sinceWrite >= 10) WriteProfile();
}

int StatsGet(int scope, MmlStats *out) {
    if (!out || out->size < offsetof(MmlStats, money_spent) || (scope != MML_SAVE_SLOT && scope != MML_SAVE_PROFILE)) return 0;
    if (scope == MML_SAVE_PROFILE && SaveProfileNumber() < 0) return 0;
    const Counts &c = scope == MML_SAVE_SLOT ? g_game : Profile();
    MmlStats s{};
    s.size = out->size;
    s.kills = c.kills; s.deaths = c.deaths; s.shots = c.shots; s.hits = c.hits; s.cars_driven = c.cars; s.crashes = c.crashes;
    s.falls = c.falls; s.items = c.items; s.on_foot_m = static_cast<float>(c.foot); s.in_vehicle_m = static_cast<float>(c.vehicle);
    s.played_s = static_cast<float>(c.played);
    s.money_earned = static_cast<int>(std::min<long long>(c.earned, INT_MAX));
    s.money_spent = static_cast<int>(std::min<long long>(c.spent, INT_MAX));
    std::memcpy(out, &s, std::min<size_t>(out->size, sizeof s));
    return 1;
}

int ProfileSaves(int *slots, int max) {
    int profile = SaveProfileNumber();
    if (profile < 0) return 0;
    int n = 0;
    for (int s = 0; s < kSlots; s++) {
        char name[64];
        snprintf(name, sizeof name, "savegame\\mafia%03d.%03d", profile, s);
        if (GetFileAttributesA((GameDir() + name).c_str()) == INVALID_FILE_ATTRIBUTES) continue;
        if (slots && n < max) slots[n] = s;
        n++;
    }
    return n;
}

int ProfileProgress(MmlProgress *out) {
    if (!out || out->size < offsetof(MmlProgress, garage) + sizeof out->garage) return 0;
    MmlProgress g{};
    g.size = out->size;
    g.profile = SaveProfileNumber();
    g.newest_slot = -1;
    for (int &c : g.garage) c = -1;
    uintptr_t m = Manager();
    if (!m || g.profile < 0) { std::memcpy(out, &g, std::min<size_t>(out->size, sizeof g)); return g.profile >= 0 ? 1 : 0; }
    MemRead(m + kProfName, g.name, sizeof g.name - 1);
    g.freeride_level = static_cast<int>(R32(m + kFreerideLevel));
    g.race_cars = R32(m + kRaceMask);
    g.steal_cars = R32(m + kStealMask);
    g.freeride_cars = R32(m + kFreerideMask);
    for (int i = 0; i < kGarage; i++) {
        uint32_t id = R32(m + kGarageTable + 8 * static_cast<uintptr_t>(i));
        g.garage[i] = id ? static_cast<int>(id & 0xFFFF) : -1;
    }
    int slots[kSlots];
    g.saves = ProfileSaves(slots, kSlots);
    for (int i = g.saves - 1; i >= 0; i--) {
        int ch = ChapterOf(SaveName(slots[i]));
        if (ch) { g.chapter = ch; g.newest_slot = slots[i]; break; }
    }
    std::memcpy(out, &g, std::min<size_t>(out->size, sizeof g));
    return 1;
}

bool StatsMethod(const std::string &method, const std::string &paramsJson, std::string *reply, bool *ok) {
    if (method != "stats.state" && method != "profile.progress") return false;
    *ok = false;
    (void)paramsJson;
    auto dump = [](const json &j) { return j.dump(-1, ' ', false, json::error_handler_t::replace); };
    auto letters = [](uint32_t mask) {
        std::string s;
        for (int b = 0; b < 26; b++) if (mask & (1u << b)) s += static_cast<char>('A' + b);
        return s;
    };
    if (method == "stats.state") {
        json out = {{"game", ToJson(g_game)}, {"profile_number", SaveProfileNumber()}};
        if (SaveProfileNumber() >= 0) out["profile"] = ToJson(Profile());
        *reply = dump(out);
    } else {
        MmlProgress g{};
        g.size = sizeof g;
        if (!ProfileProgress(&g)) { *reply = "profile.progress: no profile selected yet"; return true; }
        json garage = json::array();
        for (int c : g.garage) if (c >= 0) garage.push_back({{"index", c}, {"car", WorldCarName(c)}});
        int slots[kSlots];
        int n = std::min(ProfileSaves(slots, kSlots), kSlots);
        json saves = json::array();
        for (int i = 0; i < n; i++) saves.push_back({{"slot", slots[i]}, {"name", GameUtf8(SaveName(slots[i]))}});
        *reply = dump({{"profile", g.profile}, {"name", GameUtf8(g.name)}, {"chapter", g.chapter}, {"newest_slot", g.newest_slot},
                       {"saves_count", g.saves}, {"freeride_level", g.freeride_level}, {"race_cars", letters(g.race_cars)},
                       {"steal_cars", letters(g.steal_cars)}, {"freeride_cars", letters(g.freeride_cars)}, {"garage", garage},
                       {"saves", saves}});
    }
    *ok = true;
    return true;
}

} // namespace host
