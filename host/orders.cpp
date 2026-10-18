// Orders for people (mml.h human_order, human_order_state; docs/MODDING.md "Orders for people", docs/SCRIPTS.md
// "Orders").
//
// A person with a script, a C_entity (actor type 0x1B: actor_spawn's people, the missions' NPCs; st0rm94's
// structs/entity.md), carries a program of his own at +0xA9C, a C_program whose vtable is the entity's: its
// DecodeInstruction knows the people's own commands (ENEMY_MOVE_TO_FRAME, ENEMY_ACTION_FOLLOW, ENEMY_ACTION_FIRE...,
// opcodes 0x1000..0x103F) besides the language's, and C_entity::AI runs it while the flag 0x200 of +0xF08 is set
// ("the script drives"). An order is that program made anew: its Done, our code compiled into it by its own vtable
// (script.cpp ScriptCompileInto), its registers given (act[], flt[], frm[]: a point of the world is a dummy frame of
// the loader's), then C_entity::ForceScript(entity, false) as the scripts' ENEMY_FORCESCRIPT does it: SetEvent(its
// program, 0xEA63) jumps the program to its `event 60003` (the loader puts that line first) and the program's
// callback sets 0x200; the AI state is 0 again and its queued actions cleared. From there the game runs it: a
// command that queues an action (a move, a follow, a fire) waits for it: the action's AI phase hands him back to the
// script once his queue is empty (C_entity::ScriptSwitch, the program's +0x60 against +0xF58: script.cpp).
// An order never reaches END: END's C_program::ClearAfterEnd puts its owner's act state at 2, which takes him out
// of C_game's lists of actors to tick (ChangeActState), for good (SetActState never changes a 2; C_actor::Rebirth
// makes it 0 again but puts him into no list): he would stand frozen, his AI and his animations stopped. So the
// loader ends each order with a wait of its own (kIdleTail), as a mission's NPC waits in his script, the code's own
// `end` lines become a jump there, and the order is done once the program stands there. A mission's NPC whose own
// script did end (act state 2) is refused: the game ticks him no more. Each mission tick the loader looks at where
// each order stands and tells ORDER_ENDED once it is done.
//
// Bridge methods (game thread): human.order {actor, code, act, flt, at}, human.order_state {actor}, human.go {actor,
// to, run, radius}, human.follow {actor, target, distance}, human.attack {actor, target}, human.flee {actor, from,
// distance}, human.enter {actor, car, seat}, human.stop {actor}.
#include "host.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include "json.hpp"
#include "mml/game/game.h"
#include "mml/game/game_build.h"

using json = nlohmann::json;

namespace host {
namespace {

constexpr int kProgram = 0xA9C;            // C_entity: its C_program
constexpr int kFlags = 0xF08;              // its script / AI flags
constexpr int kAiState = 0xF40;
constexpr uint32_t kScriptDrives = 0x200, kActionRuns = 0x800;
constexpr const char *kStartEvent = "event 60003";   // what ForceScript's SetEvent(0xEA63) jumps to
// the order's end: he waits there in his script, ticked (the label is the loader's: an order's code keeps off 60003)
constexpr const char *kIdleTail = "label 60003\nwait 1000\ngoto 60003";
constexpr unsigned kIdleTailLines = 3;
constexpr const char *kToIdle = "goto 60003";   // what the code's own `end` becomes

using ForceFn = char(__fastcall *)(void *self, void *edx, char keep);

struct Order {
    int id = 0;
    MmlMod *mod = nullptr;      // null: the bridge's
    void *human = nullptr;
    std::vector<int> lines;     // each instruction's line in the order's code (0: the loader's first line)
    unsigned tail = 0;          // the instruction kIdleTail starts at: the order is done there
    std::vector<void *> frames; // the points' dummy frames (frm[])
    std::string what;           // the order as asked, for the log and the state
    bool ended = false;
};
std::vector<Order> g_orders;   // one a person: his last
int g_nextId = 1;

uint32_t R32(uintptr_t a) { uint32_t v = 0; MemRead(a, &v, 4); return v; }
std::string Hex(uintptr_t v) {
    char b[16];
    snprintf(b, sizeof b, "0x%08X", static_cast<unsigned>(v));
    return b;
}
template <class F> DWORD Guarded(F &f) {
    __try { f(); return 0; } __except (EXCEPTION_EXECUTE_HANDLER) { return GetExceptionCode(); }
}

Order *Find(void *human) {
    for (Order &o : g_orders) if (o.human == human) return &o;
    return nullptr;
}
void ReleaseFrames(Order &o) {
    for (void *f : o.frames) FrameRelease(f);
    o.frames.clear();
}

bool IsEntity(void *human) {
    if (!human || !game::g_exe) return false;
    return R32(reinterpret_cast<uintptr_t>(human) + game::kActorType) == game::kActorTypeEntity;
}

// The code's own `end` lines (the command alone, a comment after it allowed) made a jump to the loader's wait: the
// program never reaches END (see the top), and the line numbers stay the code's.
std::string EndsToIdle(const char *code) {
    std::string out, line;
    auto flush = [&](bool nl) {
        size_t a = line.find_first_not_of(" \t");
        bool isEnd = a != std::string::npos && line.size() >= a + 3 && _strnicmp(line.c_str() + a, "end", 3) == 0;
        if (isEnd) {
            size_t b = line.find_first_not_of(" \t\r", a + 3);
            isEnd = b == std::string::npos || line.compare(b, 2, "//") == 0;
            if (isEnd) line = line.substr(0, a) + kToIdle + (b == std::string::npos ? "" : " " + line.substr(b));
        }
        out += line;
        if (nl) out += '\n';
        line.clear();
    };
    for (const char *p = code; *p; p++) {
        if (*p == '\n') flush(true);
        else line += *p;
    }
    flush(false);
    return out;
}

json StateJson(const MmlOrderState &s, const Order *o) {
    json j = {{"id", s.id}, {"running", s.running != 0}, {"line", s.line}, {"script_drives", s.script_drives != 0},
              {"ai_state", s.ai_state}, {"action", s.action != 0}};
    if (o) j["what"] = o->what;
    return j;
}

} // namespace

int HumanOrderState(void *human, MmlOrderState *out) {
    if (!out || out->size < sizeof(uint32_t)) return 0;
    MmlOrderState s{};
    s.size = std::min<uint32_t>(out->size, sizeof s);
    Order *o = Find(human);
    if (!o || !IsEntity(human)) { memcpy(out, &s, s.size); return 0; }
    uintptr_t e = reinterpret_cast<uintptr_t>(human);
    unsigned pc = 0, count = 0;
    bool atEnd = false;
    ScriptWhere(e + kProgram, &pc, &count, &atEnd);
    uint32_t flags = R32(e + kFlags);
    s.id = o->id;
    s.running = !(atEnd || pc >= count || pc >= o->tail);
    s.line = s.running && pc < o->lines.size() ? o->lines[pc] : 0;
    s.script_drives = (flags & kScriptDrives) != 0;
    s.ai_state = static_cast<int>(R32(e + kAiState));
    s.action = (flags & kActionRuns) != 0;
    memcpy(out, &s, s.size);
    return 1;
}

int HumanOrder(MmlMod *mod, void *human, const char *code, void *const *acts, int nacts, const float *flts, int nflts,
               const float *points, int npoints, std::string *err) {
    if (!game::g_exe || !game::Exe().entityForceScript) { *err = "orders are not known for this build"; return 0; }
    if (MissionState() == MML_MISSION_NONE || !game::Mission()) { *err = "no mission"; return 0; }
    if (!IsEntity(human)) {
        *err = Hex(reinterpret_cast<uintptr_t>(human)) + " is not a person with a script of his own (a C_entity, actor type 0x1B: "
               "actor_spawn's people, the missions' NPCs; not the player, not the traffic's pedestrians)";
        return 0;
    }
    uintptr_t e = reinterpret_cast<uintptr_t>(human);
    uint8_t alive = 0;
    MemRead(e + game::kActorAlive, &alive, 1);
    if (!alive) { *err = "he is dead"; return 0; }
    if (R32(e + game::kActorActState) == game::kActStateEnded) {
        *err = "his own script has ended (act state 2): the game ticks him no more, so no order can move him";
        return 0;
    }
    if (npoints < 0 || npoints > 64) { *err = "points: at most 64"; return 0; }
    Order o;
    for (int i = 0; i < npoints; i++) {
        void *f = DummyFrameAt(points + 3 * i);
        if (!f) { ReleaseFrames(o); *err = "a frame for the point " + std::to_string(i) + " could not be made"; return 0; }
        o.frames.push_back(f);
    }
    if (!code || !*code) { ReleaseFrames(o); *err = "code: the order's text"; return 0; }
    std::string text = EndsToIdle(code) + "\n" + kIdleTail;
    if (!ScriptCompileInto(e + kProgram, kStartEvent, text.c_str(), acts, nacts, flts, nflts, o.frames.data(),
                           static_cast<int>(o.frames.size()), &o.lines, err)) {
        ReleaseFrames(o);
        return 0;
    }
    o.tail = static_cast<unsigned>(o.lines.size()) - kIdleTailLines;
    for (unsigned i = o.tail; i < o.lines.size(); i++) o.lines[i] = 0;   // the loader's lines, not the code's
    unsigned pc0 = 0, count = 0;
    bool atEnd0 = false;
    ScriptWhere(e + kProgram, &pc0, &count, &atEnd0);
    if (count != o.lines.size()) HWARN("order: %u instructions compiled for %u lines", count, static_cast<unsigned>(o.lines.size()));
    // his program is ours now: the frames of an order before it are free
    for (size_t i = 0; i < g_orders.size(); i++)
        if (g_orders[i].human == human) { ReleaseFrames(g_orders[i]); g_orders.erase(g_orders.begin() + static_cast<long>(i)); break; }
    char forced = 0;
    auto force = [&] { forced = reinterpret_cast<ForceFn>(game::Exe().entityForceScript)(human, nullptr, 0); };
    if (DWORD ex = Guarded(force)) HWARN("order: ForceScript faulted (%08X)", static_cast<unsigned>(ex));
    o.id = g_nextId++;
    o.mod = mod;
    o.human = human;
    o.what = code;
    if (o.what.size() > 300) o.what = o.what.substr(0, 300) + "...";
    bool drives = (R32(e + kFlags) & kScriptDrives) != 0;
    HLOG("order %d of %s for %s: %u instructions, %s", o.id, ModIdOf(mod), Hex(e).c_str(), static_cast<unsigned>(o.lines.size()),
         drives ? "his script drives him" : forced ? "handed to his script" : "his AI kept him (ForceScript refused: a civilian in panic?)");
    g_orders.push_back(std::move(o));
    return g_orders.back().id;
}

void OrdersTick() {
    for (Order &o : g_orders) {
        if (o.ended) continue;
        MmlOrderState s{sizeof s};
        if (!HumanOrderState(o.human, &s) || s.running) continue;
        o.ended = true;
        EventsRecord("ORDER_ENDED", json({{"id", o.id}, {"actor", Hex(reinterpret_cast<uintptr_t>(o.human))}, {"owner", ModIdOf(o.mod)}})
                                        .dump(-1, ' ', false, json::error_handler_t::replace));
        HLOG("order %d: done", o.id);
    }
}

void OrdersActorGone(void *actor) {
    for (size_t i = 0; i < g_orders.size(); i++)
        if (g_orders[i].human == actor) { ReleaseFrames(g_orders[i]); g_orders.erase(g_orders.begin() + static_cast<long>(i)); return; }
}

void OrdersMissionClose() {
    for (Order &o : g_orders) ReleaseFrames(o);
    g_orders.clear();
}

namespace {

bool V3Of(const json &v, float out[3]) {
    if (!v.is_array() || v.size() != 3) return false;
    for (int i = 0; i < 3; i++) {
        if (!v[i].is_number()) return false;
        out[i] = v[i].get<float>();
    }
    return true;
}

bool ActorParam(const json &p, const char *key, void **out, std::string *err) {
    if (!p.contains(key)) { *err = std::string(key) + ": an actor (\"player\", \"car\", an address)"; return false; }
    const json &v = p[key];
    if (v.is_string()) return ParseActor(v.get<std::string>(), out, err);
    *err = std::string(key) + ": an actor (\"player\", \"car\", an address)";
    return false;
}

std::string Num(double v) {
    char b[32];
    snprintf(b, sizeof b, "%.3f", v);
    return b;
}

} // namespace

bool OrdersMethod(const std::string &method, const std::string &paramsJson, std::string *reply, bool *ok) {
    static const char *names[] = {"human.order", "human.order_state", "human.go", "human.follow", "human.attack", "human.flee",
                                  "human.enter", "human.stop"};
    if (std::find_if(std::begin(names), std::end(names), [&](const char *n) { return method == n; }) == std::end(names)) return false;
    *ok = false;
    json p = json::parse(paramsJson.empty() ? "null" : paramsJson, nullptr, false);
    if (!p.is_object()) p = json::object();
    std::string err;
    void *human = nullptr;
    if (!ActorParam(p, "actor", &human, &err)) { *reply = method + ": " + err; return true; }
    if (method == "human.order_state") {
        MmlOrderState s{sizeof s};
        int known = HumanOrderState(human, &s);
        *ok = true;
        *reply = known ? StateJson(s, Find(human)).dump(-1, ' ', false, json::error_handler_t::replace) : "null";
        return true;
    }
    std::string code;
    std::vector<void *> acts;
    std::vector<float> flts, points;
    auto act = [&](const char *key) -> bool {
        void *a = nullptr;
        if (!ActorParam(p, key, &a, &err)) return false;
        acts.push_back(a);
        return true;
    };
    if (method == "human.order") {
        code = p.value("code", "");
        // act {"0": "player"}, flt {"0": 1.5}, at [[x, y, z], ...] (frm[0], frm[1]...)
        if (p.contains("act") && p["act"].is_object())
            for (auto it = p["act"].begin(); it != p["act"].end(); ++it) {
                int i = std::atoi(it.key().c_str());
                void *a = nullptr;
                if (i < 0 || i > 255 || !it.value().is_string() || !ParseActor(it.value().get<std::string>(), &a, &err)) {
                    *reply = "human.order: act[" + it.key() + "]: " + (err.empty() ? "an actor" : err);
                    return true;
                }
                if (acts.size() <= static_cast<size_t>(i)) acts.resize(static_cast<size_t>(i) + 1, nullptr);
                acts[static_cast<size_t>(i)] = a;
            }
        if (p.contains("flt") && p["flt"].is_object())
            for (auto it = p["flt"].begin(); it != p["flt"].end(); ++it) {
                int i = std::atoi(it.key().c_str());
                if (i < 0 || i > 4095 || !it.value().is_number()) { *reply = "human.order: flt[" + it.key() + "]: a number"; return true; }
                if (flts.size() <= static_cast<size_t>(i)) flts.resize(static_cast<size_t>(i) + 1, 0.0f);
                flts[static_cast<size_t>(i)] = it.value().get<float>();
            }
        if (p.contains("at") && p["at"].is_array())
            for (const json &v : p["at"]) {
                float x[3];
                if (!V3Of(v, x)) { *reply = "human.order: at: points [x, y, z]"; return true; }
                points.insert(points.end(), x, x + 3);
            }
    } else if (method == "human.go") {
        float to[3];
        if (!V3Of(p.value("to", json()), to)) { *reply = "human.go: to [x, y, z]"; return true; }
        points.assign(to, to + 3);
        code = "enemy_move_to_frame 0, " + Num(p.value("radius", 1.0)) + (p.value("run", false) ? ", run" : "");
    } else if (method == "human.follow") {
        if (!act("target")) { *reply = method + ": " + err; return true; }
        code = "enemy_action_follow 0, " + Num(p.value("distance", 2.0));
    } else if (method == "human.attack") {
        if (!act("target")) { *reply = method + ": " + err; return true; }
        code = "enemy_action_fire 0";
    } else if (method == "human.flee") {
        if (!act("from")) { *reply = method + ": " + err; return true; }
        code = "enemy_action_runaway 0, " + Num(p.value("distance", 30.0));
    } else if (method == "human.enter") {
        if (!act("car")) { *reply = method + ": " + err; return true; }
        // MOVE_TO_CAR walks him to the seat's door; USECAR gets him in (a free seat, the car still, within 3 m)
        std::string seat = std::to_string(p.value("seat", 0));
        code = "enemy_move_to_car 0, " + seat + (p.value("run", false) ? ", run" : "") + "\nenemy_usecar 0, " + seat;
    } else {   // human.stop
        code = "enemy_stop";
    }
    int id = HumanOrder(nullptr, human, code.c_str(), acts.data(), static_cast<int>(acts.size()), flts.data(), static_cast<int>(flts.size()),
                        points.data(), static_cast<int>(points.size() / 3), &err);
    if (!id) { *reply = method + ": " + err; return true; }
    MmlOrderState s{sizeof s};
    HumanOrderState(human, &s);
    *ok = true;
    *reply = StateJson(s, Find(human)).dump(-1, ' ', false, json::error_handler_t::replace);
    return true;
}

} // namespace host
