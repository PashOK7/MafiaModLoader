// Interaction (mml.h zone_add, use_point_add, timer_add and their removes; docs/MODDING.md "Interaction").
//
// Zones: spheres in the world (or following a frame) the player enters and leaves, on foot or in his car (his
// model's world position: in a car it is the car's); the mod's function is told of each crossing, the bridge's
// zones go to events.since (ZONE_ENTERED, ZONE_LEFT).
// Use-points: a zone the player stands in on foot, with a prompt: the game's own help line (G_IndicatorsClass::
// MissionHelpAdd: the objectives' lines at the top, in the game's font), shown with the loader's own text id in the
// game's text database, again while he stays once it has gone; while he is in one, the game's "Action" is kept from
// the game (input.cpp's capture: he does not also enter a car or open a door) and its press calls the mod (USED).
// Timers: the mission's clock (MML_EVENT_MISSION_TICK's time: they stand while the game is paused or loads), once
// or repeated; a mod's go with its unload, every zone, use-point and timer with the mission.
//
// Bridge methods (game thread): interact.list, zone.add {at | on, bone, pos, radius}, use.add {at, radius, prompt},
// timer.add {ms, repeat}, interact.remove {id | all}.
#include "host.h"
#include <algorithm>
#include <cmath>
#include <string>
#include <vector>
#include "json.hpp"
#include "mml/game/game_build.h"
#include "mml/game/game.h"

using json = nlohmann::json;

namespace host {
namespace {

enum Kind { kZone, kUse, kTimer };
struct Item {
    int id;
    Kind kind;
    MmlMod *mod;
    void *frame;              // followed (null: the world)
    V3 pos;                   // the world's, or in the frame's space
    float radius;
    std::string prompt;       // a use-point's (UTF-8)
    uint32_t textId;
    MmlZoneFn zoneFn;
    MmlUseFn useFn;
    MmlTimerFn timerFn;
    void *user;
    bool inside;
    DWORD promptUntil;        // a use-point's help line shown until then
    int ms, left;             // a timer's period and what is left of it (mission ms)
    bool repeat;
    int fired;
};
std::vector<Item> g_items;
int g_nextId;
int g_actionSlot = -1;        // "Action"'s primary slot (its secondary is the next)
bool g_actionKept;

const game::ExeBuild &E() { return game::Exe(); }

template <class F> DWORD Guarded(F &f) {
    __try { f(); return 0; } __except (EXCEPTION_EXECUTE_HANDLER) { return GetExceptionCode(); }
}

bool PlayerPlace(V3 *out, bool *onFoot) {
    void *pl = game::g_exe ? game::Player() : nullptr;
    void *model = pl ? ActorFrame(pl) : nullptr;
    float w[16];
    if (!model || !FrameWorld(model, w)) return false;
    *out = {w[12], w[13], w[14]};
    bool foot = true;
    auto g = [&] { foot = game::at<void *>(pl, game::kHumanVehicle) == nullptr; };
    Guarded(g);
    *onFoot = foot;
    return true;
}

bool Centre(const Item &it, V3 *out) {
    if (!it.frame) { *out = it.pos; return true; }
    float w[16];
    if (!FrameWorld(it.frame, w)) return false;
    *out = {w[12] + it.pos.x * w[0] + it.pos.y * w[4] + it.pos.z * w[8], w[13] + it.pos.x * w[1] + it.pos.y * w[5] + it.pos.z * w[9],
            w[14] + it.pos.x * w[2] + it.pos.y * w[6] + it.pos.z * w[10]};
    return true;
}

struct ZoneCall { Item *it; int inside; void *actor; };
void CallZone(void *p) { auto *c = static_cast<ZoneCall *>(p); c->it->zoneFn(c->it->id, c->inside, c->actor, c->it->user); }
struct UseCall { Item *it; };
void CallUse(void *p) { auto *c = static_cast<UseCall *>(p); c->it->useFn(c->it->id, c->it->user); }
void CallTimer(void *p) { auto *c = static_cast<UseCall *>(p); c->it->timerFn(c->it->id, c->it->user); }

void Tell(Item &it, const char *type, const char *what) {
    json d = {{"id", it.id}, {"by", it.mod ? ModIdOf(it.mod) : "bridge"}};
    if (it.kind == kUse) d["prompt"] = it.prompt;
    EventsRecord(type, d.dump());
    (void)what;
}

void KeepAction(bool keep) {
    if (keep == g_actionKept || g_actionSlot < 0) return;
    ActionCapture(kLoaderOwner, g_actionSlot, keep);
    g_actionKept = keep;
}

} // namespace

int HudHelpLine(const std::string &utf8) {
    if (!game::g_exe || !E().hudHelpAdd || !E().hud) return 0;
    uint32_t id = GameTextId(utf8);
    if (!id) return 0;
    using HelpFn = int(__fastcall *)(uintptr_t hud, void *edx, uint32_t id);
    int ms = 0;
    auto f = [&] { ms = reinterpret_cast<HelpFn>(E().hudHelpAdd)(E().hud, nullptr, id); };
    if (Guarded(f)) return 0;
    return ms;
}

int ZoneAdd(MmlMod *mod, const float *pos, float radius, void *frame, MmlZoneFn fn, void *user, std::string *err) {
    if (!game::g_exe) { *err = "no game"; return 0; }
    if (!(radius > 0)) { *err = "radius: more than 0"; return 0; }
    Item it{};
    it.id = ++g_nextId;
    it.kind = kZone;
    it.mod = mod;
    it.frame = frame;
    it.pos = pos ? V3{pos[0], pos[1], pos[2]} : V3{0, 0, 0};
    it.radius = radius;
    it.zoneFn = fn;
    it.user = user;
    g_items.push_back(it);
    return it.id;
}

int UseAdd(MmlMod *mod, const float *pos, float radius, void *frame, const char *prompt, MmlUseFn fn, void *user, std::string *err) {
    if (!game::g_exe) { *err = "no game"; return 0; }
    if (!(radius > 0)) { *err = "radius: more than 0"; return 0; }
    if (g_actionSlot < 0) g_actionSlot = ActionSlotOf("Action");
    Item it{};
    it.id = ++g_nextId;
    it.kind = kUse;
    it.mod = mod;
    it.frame = frame;
    it.pos = pos ? V3{pos[0], pos[1], pos[2]} : V3{0, 0, 0};
    it.radius = radius;
    it.prompt = prompt ? prompt : "";
    it.useFn = fn;
    it.user = user;
    g_items.push_back(it);
    return it.id;
}

int TimerAdd(MmlMod *mod, int ms, bool repeat, MmlTimerFn fn, void *user, std::string *err) {
    if (ms <= 0) { *err = "ms: more than 0"; return 0; }
    Item it{};
    it.id = ++g_nextId;
    it.kind = kTimer;
    it.mod = mod;
    it.ms = it.left = ms;
    it.repeat = repeat;
    it.timerFn = fn;
    it.user = user;
    g_items.push_back(it);
    return it.id;
}

bool InteractRemove(MmlMod *mod, int id) {
    auto it = std::find_if(g_items.begin(), g_items.end(), [&](const Item &i) { return i.id == id && (!mod || i.mod == mod); });
    if (it == g_items.end()) return false;
    g_items.erase(it);
    return true;
}

// Every frame of a loaded mission: the crossings and the use-points' prompts and presses.
void InteractFrame() {
    if (g_items.empty()) { KeepAction(false); return; }
    V3 p;
    bool foot = false;
    bool have = PlayerPlace(&p, &foot);
    bool inUse = false;
    DWORD now = GetTickCount();
    for (size_t i = 0; i < g_items.size(); i++) {
        Item &it = g_items[i];
        if (it.kind == kTimer) continue;
        V3 c;
        bool in = have && Centre(it, &c) && Len(p - c) <= it.radius && (it.kind == kZone || foot);
        if (in != it.inside) {
            it.inside = in;
            int id = it.id;
            if (it.kind == kZone) {
                Tell(it, in ? "ZONE_ENTERED" : "ZONE_LEFT", "");
                if (it.zoneFn && (!it.mod || ModAlive(it.mod))) {
                    ZoneCall zc{&it, in ? 1 : 0, game::Player()};
                    ModCall(it.mod, CallZone, &zc, "its zone function");
                }
            } else if (in) {
                it.promptUntil = 0;
            }
            if (i >= g_items.size() || g_items[i].id != id) { i--; continue; }   // (removed by its own function)
        }
        if (it.kind == kUse && it.inside) {
            inUse = true;
            if (!it.prompt.empty() && now >= it.promptUntil) {
                int ms = HudHelpLine(it.prompt);
                it.promptUntil = now + static_cast<DWORD>(ms > 0 ? ms + 500 : 3000);
            }
            if (g_actionKept && g_actionSlot >= 0 && (ActionValue(g_actionSlot, true) >= 0.25f || ActionValue(g_actionSlot + 1, true) >= 0.25f)) {
                Tell(it, "USED", "");
                if (it.useFn && (!it.mod || ModAlive(it.mod))) {
                    UseCall uc{&it};
                    ModCall(it.mod, CallUse, &uc, "its use-point function");
                }
                break;   // (one use a press; the list may have changed)
            }
        }
    }
    KeepAction(inUse);
}

// The mission's tick (its time: the pause and the loading stand still).
void InteractTick(int ms) {
    for (size_t i = 0; i < g_items.size();) {
        Item &it = g_items[i];
        if (it.kind != kTimer) { i++; continue; }
        it.left -= ms;
        if (it.left > 0) { i++; continue; }
        it.fired++;
        int id = it.id;
        Tell(it, "TIMER", "");
        if (it.timerFn && (!it.mod || ModAlive(it.mod))) {
            UseCall uc{&it};
            ModCall(it.mod, CallTimer, &uc, "its timer function");
        }
        auto again = std::find_if(g_items.begin(), g_items.end(), [&](const Item &x) { return x.id == id; });
        if (again == g_items.end()) continue;   // (removed by its own function)
        i = static_cast<size_t>(again - g_items.begin());
        if (again->repeat) { again->left += again->ms; if (again->left <= 0) again->left = again->ms; i++; }
        else g_items.erase(again);
    }
}

void InteractModGone(MmlMod *mod) {
    g_items.erase(std::remove_if(g_items.begin(), g_items.end(), [&](const Item &i) { return i.mod == mod; }), g_items.end());
}
void InteractMissionClose() {
    g_items.clear();
    KeepAction(false);
}

bool InteractMethod(const std::string &method, const std::string &paramsJson, std::string *reply, bool *ok) {
    static const char *kMethods[] = {"interact.list", "zone.add", "use.add", "timer.add", "interact.remove"};
    if (std::none_of(std::begin(kMethods), std::end(kMethods), [&](const char *m) { return method == m; })) return false;
    *ok = false;
    json p = json::parse(paramsJson.empty() ? "null" : paramsJson, nullptr, false);
    if (!p.is_object()) p = json::object();
    auto dump = [](const json &j) { return j.dump(-1, ' ', false, json::error_handler_t::replace); };
    auto r3 = [](const V3 &v) { return json::array({std::round(v.x * 100) / 100, std::round(v.y * 100) / 100, std::round(v.z * 100) / 100}); };
    std::string err;
    try {
        if (method == "interact.list") {
            json list = json::array();
            for (const Item &it : g_items) {
                json j = {{"id", it.id}, {"kind", it.kind == kZone ? "zone" : it.kind == kUse ? "use" : "timer"}, {"by", it.mod ? ModIdOf(it.mod) : "bridge"}};
                if (it.kind == kTimer) { j["ms"] = it.ms; j["left"] = it.left; j["repeat"] = it.repeat; j["fired"] = it.fired; }
                else {
                    V3 c;
                    if (Centre(it, &c)) j["at"] = r3(c);
                    j["radius"] = it.radius;
                    j["inside"] = it.inside;
                    if (it.kind == kUse) j["prompt"] = it.prompt;
                }
                list.push_back(j);
            }
            V3 pl;
            bool foot = false;
            json out = {{"items", list}, {"action_kept", g_actionKept}};
            if (PlayerPlace(&pl, &foot)) { out["player"] = r3(pl); out["on_foot"] = foot; }
            *reply = dump(out);
            *ok = true;
            return true;
        }
        if (method == "interact.remove") {
            if (p.value("all", false)) {
                size_t n = 0;
                for (size_t i = 0; i < g_items.size();) if (!g_items[i].mod) { g_items.erase(g_items.begin() + i); n++; } else i++;
                *reply = dump({{"removed", n}});
            } else {
                if (!InteractRemove(nullptr, p.value("id", 0))) { *reply = "interact.remove: id: one of the bridge's (interact.list)"; return true; }
                *reply = dump({{"removed", 1}});
            }
            *ok = true;
            return true;
        }
        if (method == "timer.add") {
            int id = TimerAdd(nullptr, p.value("ms", 0), p.value("repeat", false), nullptr, nullptr, &err);
            if (!id) { *reply = "timer.add: " + err; return true; }
            *reply = dump({{"id", id}});
            *ok = true;
            return true;
        }
        // zone.add, use.add: a place (`at`, or `on` an actor with `bone` and `pos`), a radius
        void *frame = nullptr;
        V3 pos{0, 0, 0};
        if (p.contains("on") && p["on"].is_string()) {
            void *actor = nullptr;
            if (!ParseActor(p["on"].get<std::string>(), &actor, &err)) { *reply = method + ": on: " + err; return true; }
            frame = ActorFrame(actor);
            if (!frame) { *reply = method + ": on: the actor has no model"; return true; }
            if (p.contains("bone") && p["bone"].is_string()) {
                void *b = game::FindChild(frame, p["bone"].get<std::string>().c_str());
                if (!b) { *reply = method + ": bone: not in its model"; return true; }
                frame = b;
            }
            if (p.contains("pos") && p["pos"].is_array() && p["pos"].size() == 3) pos = {p["pos"][0].get<float>(), p["pos"][1].get<float>(), p["pos"][2].get<float>()};
        } else if (p.contains("at") && p["at"].is_array() && p["at"].size() == 3) {
            pos = {p["at"][0].get<float>(), p["at"][1].get<float>(), p["at"][2].get<float>()};
        } else {
            bool foot;
            if (!PlayerPlace(&pos, &foot)) { *reply = method + ": at: [x, y, z] (or no player to put it at)"; return true; }
        }
        const float f[3] = {pos.x, pos.y, pos.z};
        int id = method == "zone.add" ? ZoneAdd(nullptr, f, p.value("radius", 3.0f), frame, nullptr, nullptr, &err)
                                      : UseAdd(nullptr, f, p.value("radius", 2.0f), frame, p.value("prompt", "").c_str(), nullptr, nullptr, &err);
        if (!id) { *reply = method + ": " + err; return true; }
        *reply = dump({{"id", id}, {"at", r3(pos)}});
        *ok = true;
    } catch (const json::exception &e) {
        *reply = e.what();
    }
    return true;
}

} // namespace host
