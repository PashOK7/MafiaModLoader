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
// A drive (human.drive) is ENEMY_CAR_MOVETO: the chase manager (C_sledovacka at C_game +0x2B18) makes a record for
// the car with a C_driver and its way by the road network, the car's own place its first node. Its Tick drives
// forward only: started facing away from its way, the car turned on full lock across the pavement into what was
// there, stood, and after 2 s the manager's unsticking (record flag 0x100000, 15 s, the driver's +0x208 at 1) backed
// it up and then drove it with no limit of its own (22 m/s in town). So once the record is there the loader looks at
// the way's first leg: facing more than 90 degrees from it, the car is given that same unsticking at once, with the
// driver's reverse (+0x19C bit 0: the game's gear -1, at most 3 m/s, the wheels turned until the car faces within
// 0.34 rad of where it steers, or something is behind it) and the road's speeds kept (+0x298), as Tick sets them;
// once the game has ended the reverse, the unsticking is ended as the game ends it, and the drive goes on as the
// game drives it (`start` "turn"). "place" puts the car on its way's first road node facing the next instead (a
// jump: for a car nobody sees), "as_is" leaves it to the game. `speed` caps the way's nodes' speeds (+0x48, m/s),
// what the driver's limit and its corners' speeds come from (FillStartValues, MinV).
//
// Bridge methods (game thread): human.order {actor, code, act, flt, at}, human.order_state {actor}, human.go {actor,
// to, run, radius}, human.follow {actor, target, distance}, human.attack {actor, target}, human.flee {actor, from,
// distance}, human.enter {actor, car, seat}, human.drive {actor, car, to, stop, radius, start, speed}, human.stop
// {actor}.
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

// The chase manager (C_sledovacka, the same in both builds: knowledge/notes/game.md "Drives and the road network"):
// at C_game +0x2B18, its records a vector at +0x14 (end +0x18) of 0x44 bytes: +0x00 the car, +0x04 its C_driver,
// +0x1C flags (0x100000 unsticking), +0x28 the unsticking's time (ms; also how long the car has stood before it).
constexpr int kGameChase = 0x2B18, kChaseRecords = 0x14, kChaseRecordsEnd = 0x18, kChaseRecordSize = 0x44;
constexpr int kChaseCar = 0x00, kChaseDriver = 0x04, kChaseFlags = 0x1C, kChaseUnstickMs = 0x28;
constexpr uint32_t kChaseUnstick = 0x100000;
// C_driver (both builds): its way a vector at +0x164 (end +0x168) of 0x70-byte nodes, the position first and the
// speed at +0x48 (m/s); the node it drives to +0x170, the one an unsticking goes back to +0x78 (Tick puts the cursor
// there when it finds the car stuck; the unsticking's Tick sets the cursor back to it, and the manager takes a
// cursor of 0 for an arrival); flags +0x19C (bit 0: backing up); +0x298 (a byte): the road's speeds kept while
// unsticking.
constexpr int kDriverPath = 0x164, kDriverPathEnd = 0x168, kDriverNodeSize = 0x70, kDriverNodeSpeed = 0x48;
constexpr int kDriverCursor = 0x170, kDriverResume = 0x78;
constexpr int kDriverFlags = 0x19C, kDriverRoadSpeeds = 0x298;
constexpr uint32_t kDriverReverse = 1;
constexpr float kTurnAbove = 1.5707964f;   // the way's first leg further than this from the car's forward: turned first
constexpr float kCalmMetres = 6.0f;         // the game's own unsticking ended once the car is this far from where it stood
constexpr int kCalmMs = 6000;               // ... or after this long

// What a named order is for, looked at when the game says it is done (the program on the loader's wait): the game
// ends a drive it gives up (a car stuck, its way lost) as it ends one that arrived, so the outcome is measured.
enum GoalKind { kGoalNone, kGoalPlace, kGoalSeat, kGoalDrive };
enum DriveStart { kStartTurn, kStartPlace, kStartAsIs };   // a drive started facing away from its way
struct Goal {
    int kind = kGoalNone;
    float to[3] = {};      // the place (go, drive)
    float roadEnd[3] = {}; // drive: the road node nearest the place, where the game's way ends
    bool hasRoadEnd = false;
    float radius = 0;      // within it: arrived
    void *car = nullptr;   // enter, drive
    bool stop = true;      // drive: stop there
    int start = kStartTurn;   // drive
    float speed = 0;       // drive: the way's speeds at most this (m/s); 0 the road's
    bool calm = true;      // drive: the game's own unsticking kept short
};

struct Order {
    int id = 0;
    MmlMod *mod = nullptr;      // null: the bridge's
    void *human = nullptr;
    std::string code;           // as asked (a drive's is made anew each time it is given: kGoalDrive)
    std::vector<void *> acts;   // act[]
    std::vector<float> flts;    // flt[]
    std::vector<int> lines;     // each instruction's line in the order's code (0: the loader's first line)
    unsigned tail = 0;          // the instruction kIdleTail starts at: the order is done there
    std::vector<void *> frames; // the points' dummy frames (frm[])
    std::string what;           // the order as asked, for the log and the state
    Goal goal;
    int retries = 0, retried = 0, retryMs = 5000;   // given again when it ends short of its goal
    int retryIn = -1;           // ms of the mission's time until it is given again; -1 none pending
    bool ended = false;
    // a drive's start, each time it is given: its record looked for until the game has planned the way
    bool planned = false, turning = false;
    uintptr_t driver = 0;       // the C_driver it was planned with
    int turnMs = 0;
    std::string started;        // what was done at the start ("turned", "placed", "on its way"...)
    float startAngle = -1;      // degrees between the car's forward and its way's first leg
    bool calming = false;       // in the game's own unsticking (calm)
    V3 calmFrom{};
    int calmMs = 0, unstuck = 0;   // its time; how many times the game unstuck the car in this order
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

json Arr3(const float *v) { return json::array({std::round(v[0] * 100) / 100, std::round(v[1] * 100) / 100, std::round(v[2] * 100) / 100}); }
V3 V(const float *v) { return V3{v[0], v[1], v[2]}; }

// Where an order stands against its goal, now: arrived (go, drive: within the radius of the place or, a drive, of
// the road's end), seated (enter), and the distance left.
json Outcome(const Order &o) {
    json j;
    const Goal &g = o.goal;
    if (g.kind == kGoalPlace || g.kind == kGoalDrive) {
        void *who = g.kind == kGoalDrive && g.car ? g.car : o.human;
        V3 at = game::ActorWorldPos(who);
        float d = Len(at - V(g.to));
        float dEnd = g.hasRoadEnd ? Len(at - V(g.roadEnd)) : d;
        j["arrived"] = d <= g.radius || dEnd <= g.radius;
        j["distance"] = std::round(d * 100) / 100;
        if (g.kind == kGoalDrive && g.hasRoadEnd) {
            j["road_end"] = Arr3(g.roadEnd);
            j["road_end_distance"] = std::round(dEnd * 100) / 100;
        }
    } else if (g.kind == kGoalSeat) {
        j["seated"] = R32(reinterpret_cast<uintptr_t>(o.human) + game::kHumanVehicle) == reinterpret_cast<uintptr_t>(g.car);
    }
    if (g.kind == kGoalDrive && !o.started.empty()) {
        j["start"] = o.started;
        if (o.startAngle >= 0) j["start_angle"] = std::round(o.startAngle);
        if (o.unstuck) j["unstuck"] = o.unstuck;
    }
    if (o.retries) j["retried"] = o.retried;
    return j;
}
bool Reached(const Order &o) {
    if (o.goal.kind == kGoalNone) return true;
    json j = Outcome(o);
    return j.value("arrived", false) || j.value("seated", false);
}

json StateJson(const MmlOrderState &s, const Order *o) {
    json j = {{"id", s.id}, {"running", s.running != 0}, {"line", s.line}, {"script_drives", s.script_drives != 0},
              {"ai_state", s.ai_state}, {"action", s.action != 0}};
    if (o) {
        j["what"] = o->what;
        json out = Outcome(*o);   // (kept: items() of a temporary would dangle)
        for (auto &kv : out.items()) j[kv.key()] = kv.value();
        if (o->retryIn >= 0) j["retry_in_ms"] = o->retryIn;
    }
    return j;
}

// A drive's code, made anew each time it is given: into the car first unless he sits in it already (MOVE_TO_CAR to
// the driver's door, USECAR in), then CAR_MOVETO, which takes only someone sitting in that car (C_entity +0x98).
std::string DriveCode(const Order &o) {
    bool in = R32(reinterpret_cast<uintptr_t>(o.human) + game::kHumanVehicle) == reinterpret_cast<uintptr_t>(o.goal.car);
    std::string code = in ? "" : "enemy_move_to_car 0, 0\nenemy_usecar 0, 0\n";
    return code + "enemy_car_moveto 0, 0" + (o.goal.stop ? ", stop" : "");
}

void W32(uintptr_t a, uint32_t v) { MemWrite(a, &v, 4); }
float F32(uintptr_t a) { float v = 0; MemRead(a, &v, 4); return v; }

// The chase manager's record of a car (ENEMY_CAR_MOVETO's), or 0: looked up anew each time (its vector moves).
uintptr_t ChaseRecord(void *car) {
    void *g = game::Game();
    if (!g || !car) return 0;
    uintptr_t m = reinterpret_cast<uintptr_t>(g) + kGameChase;
    uint32_t b = R32(m + kChaseRecords), e = R32(m + kChaseRecordsEnd);
    if (!b || e <= b || (e - b) % kChaseRecordSize || (e - b) / kChaseRecordSize > 256) return 0;
    for (uint32_t r = b; r < e; r += kChaseRecordSize)
        if (R32(r + kChaseCar) == reinterpret_cast<uintptr_t>(car)) return r;
    return 0;
}

// An unsticking ended as the game ends its own (Tick: the flag, +0x298, the time), and the reverse with it; not
// when the record is another drive's now.
void EndUnstick(Order &o) {
    uintptr_t rec = ChaseRecord(o.goal.car);
    if (!rec || R32(rec + kChaseDriver) != o.driver) return;
    W32(rec + kChaseFlags, R32(rec + kChaseFlags) & ~kChaseUnstick);
    uint8_t zero = 0;
    MemWrite(o.driver + kDriverRoadSpeeds, &zero, 1);
    W32(rec + kChaseUnstickMs, 0);
    W32(o.driver + kDriverFlags, R32(o.driver + kDriverFlags) & ~kDriverReverse);
}
void EndTurn(Order &o, const char *how) {
    if (!o.turning) return;
    o.turning = false;
    EndUnstick(o);
    o.started = std::string("turned (") + how + ")";
    HLOG("order %d: the car turned to its way in %d ms (%s)", o.id, o.turnMs, how);
}

// Each mission tick of a drive: its start looked at once the game has planned it (its record there), a turn
// followed, and the game's own unsticking kept short (`calm`).
void DriveTick(Order &o, int ms) {
    Goal &g = o.goal;
    if (g.kind != kGoalDrive || o.ended || o.retryIn >= 0 || !g.car) return;
    if (o.turning) {
        o.turnMs += std::max(ms, 0);
        uintptr_t rec = ChaseRecord(g.car);
        if (!rec || R32(rec + kChaseDriver) != o.driver) { o.turning = false; o.started = "turning (the drive ended)"; return; }
        bool backing = (R32(o.driver + kDriverFlags) & kDriverReverse) != 0;
        bool unsticking = (R32(rec + kChaseFlags) & kChaseUnstick) != 0;
        if (!backing) EndTurn(o, "it faces its way");
        else if (!unsticking) EndTurn(o, "the game's 15 s of unsticking over");
        return;
    }
    if (o.planned) {
        // the game's own unsticking (the car stood 2 s): 15 s with no limit of its own unless ended; ended here once
        // the car has got away (kCalmMetres from where it stood) or after kCalmMs, the reverse it may be in let end
        uintptr_t rec = g.calm ? ChaseRecord(g.car) : 0;
        if (!rec || R32(rec + kChaseDriver) != o.driver) return;
        bool unsticking = (R32(rec + kChaseFlags) & kChaseUnstick) != 0;
        if (!unsticking) { o.calming = false; return; }
        V3 at = game::ActorWorldPos(g.car);
        if (!o.calming) {
            o.calming = true;
            o.calmFrom = at;
            o.calmMs = 0;
            o.unstuck++;
            HLOG("order %d: the game's unsticking began (%d: the car stood)", o.id, o.unstuck);
            return;
        }
        o.calmMs += std::max(ms, 0);
        bool backing = (R32(o.driver + kDriverFlags) & kDriverReverse) != 0;
        if (!backing && (Len(at - o.calmFrom) >= kCalmMetres || o.calmMs >= kCalmMs)) {
            EndUnstick(o);
            o.calming = false;
            HLOG("order %d: the game's unsticking ended after %d ms, %.1f m: the road's speeds again", o.id, o.calmMs, Len(at - o.calmFrom));
        }
        return;
    }
    if (o.planned) return;
    uintptr_t rec = ChaseRecord(g.car);
    if (!rec) return;   // not driving yet (walking to the car, getting in)
    uintptr_t drv = R32(rec + kChaseDriver);
    uint32_t pb = drv ? R32(drv + kDriverPath) : 0, pe = drv ? R32(drv + kDriverPathEnd) : 0;
    if (!pb || pe <= pb || (pe - pb) % kDriverNodeSize) return;
    unsigned n = (pe - pb) / kDriverNodeSize;
    o.planned = true;
    o.driver = drv;
    o.started = "on its way";
    o.startAngle = -1;
    if (g.speed > 0)
        for (unsigned i = 0; i < n; i++) {
            uintptr_t s = pb + i * kDriverNodeSize + kDriverNodeSpeed;
            if (F32(s) > g.speed) MemWrite(s, &g.speed, 4);
        }
    if (n < 2) return;
    // the way's first leg: from the car (the way's node 0) to its first road node, against the car's forward
    V3 at = game::ActorWorldPos(g.car), first{}, second{};
    MemRead(pb + kDriverNodeSize, &first, 12);
    if (n > 2) MemRead(pb + 2 * kDriverNodeSize, &second, 12);
    void *frame = game::at<void *>(g.car, game::kActorFrame);
    const float *w = frame ? game::WorldMatrix(frame) : nullptr;
    if (!w) return;
    V3 fwd{w[8], 0, w[10]}, leg{first.x - at.x, 0, first.z - at.z};
    float lf = Len(fwd), ll = Len(leg);
    if (lf < 1e-3f || ll < 1e-3f) return;
    float c = (fwd.x * leg.x + fwd.z * leg.z) / (lf * ll);
    float angle = std::acos(std::max(-1.0f, std::min(1.0f, c)));
    o.startAngle = angle * 57.29578f;
    if (angle <= kTurnAbove || g.start == kStartAsIs) return;
    if (g.start == kStartPlace) {
        // on its first road node (the ground under it), facing the next (or along the leg)
        V3 face = n > 2 ? V3{second.x - first.x, 0, second.z - first.z} : leg;
        V3 hit{}, nrm{};
        uint32_t prim = 0;
        if (game::TestLine(first + V3{0, 3, 0}, first - V3{0, 30, 0}, &hit, &nrm, &prim) && !game::IsWaterSurface(prim)) first.y = hit.y + 0.5f;
        MmlVec3 p{first.x, first.y, first.z}, d{face.x, 0, face.z};
        std::string err;
        if (WorldTeleport(g.car, p, &d, false, &err)) o.started = "placed on its way";
        else HWARN("order %d: the car not put on its way: %s", o.id, err.c_str());
        HLOG("order %d: the car faced %.0f degrees from its way: put on its first node", o.id, o.startAngle);
        return;
    }
    // the game's unsticking with its reverse, now (what Tick does after the car has stood 2 s)
    W32(drv + kDriverResume, R32(drv + kDriverCursor));
    W32(drv + kDriverFlags, R32(drv + kDriverFlags) | kDriverReverse);
    W32(rec + kChaseFlags, R32(rec + kChaseFlags) | kChaseUnstick);
    W32(rec + kChaseUnstickMs, 0);
    uint8_t one = 1;
    MemWrite(drv + kDriverRoadSpeeds, &one, 1);
    o.turning = true;
    o.turnMs = 0;
    o.started = "turning";
    o.calming = false;
    HLOG("order %d: the car faces %.0f degrees from its way: turned first (the game's reverse)", o.id, o.startAngle);
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

namespace {

// An order given to him: its code (a drive's made now) into his program, then ForceScript. Again for a retry.
bool Issue(Order &o, std::string *err) {
    uintptr_t e = reinterpret_cast<uintptr_t>(o.human);
    if (o.goal.kind == kGoalDrive) {
        uintptr_t v = R32(e + game::kHumanVehicle);
        if (v && v != reinterpret_cast<uintptr_t>(o.goal.car)) {
            *err = "he sits in another car (" + Hex(v) + "): out first (human.order {code: \"enemy_usecar\"})";
            return false;
        }
        o.code = DriveCode(o);
    }
    o.planned = o.turning = o.calming = false;   // (a drive's start looked at again: the game plans its way anew)
    o.driver = 0;
    std::string text = EndsToIdle(o.code.c_str()) + "\n" + kIdleTail;
    o.lines.clear();   // (ScriptCompileInto adds to it: a retry's would follow the first's)
    if (!ScriptCompileInto(e + kProgram, kStartEvent, text.c_str(), o.acts.data(), static_cast<int>(o.acts.size()), o.flts.data(),
                           static_cast<int>(o.flts.size()), o.frames.data(), static_cast<int>(o.frames.size()), &o.lines, err))
        return false;
    o.tail = static_cast<unsigned>(o.lines.size()) - kIdleTailLines;
    for (unsigned i = o.tail; i < o.lines.size(); i++) o.lines[i] = 0;   // the loader's lines, not the code's
    unsigned pc0 = 0, count = 0;
    bool atEnd0 = false;
    ScriptWhere(e + kProgram, &pc0, &count, &atEnd0);
    if (count != o.lines.size()) HWARN("order: %u instructions compiled for %u lines", count, static_cast<unsigned>(o.lines.size()));
    char forced = 0;
    auto force = [&] { forced = reinterpret_cast<ForceFn>(game::Exe().entityForceScript)(o.human, nullptr, 0); };
    if (DWORD ex = Guarded(force)) HWARN("order: ForceScript faulted (%08X)", static_cast<unsigned>(ex));
    bool drives = (R32(e + kFlags) & kScriptDrives) != 0;
    std::string again = o.retried ? " (again: " + std::to_string(o.retried) + " of " + std::to_string(o.retries) + ")" : "";
    HLOG("order %d of %s for %s%s: %u instructions, %s", o.id, ModIdOf(o.mod), Hex(e).c_str(), again.c_str(),
         static_cast<unsigned>(o.lines.size()),
         drives ? "his script drives him" : forced ? "handed to his script" : "his AI kept him (ForceScript refused: a civilian in panic?)");
    o.ended = false;
    return true;
}

static int HumanOrderGoal(MmlMod *mod, void *human, const char *code, void *const *acts, int nacts, const float *flts, int nflts,
                          const float *points, int npoints, const Goal *goal, int retries, int retryMs, std::string *err);

} // namespace

int HumanOrder(MmlMod *mod, void *human, const char *code, void *const *acts, int nacts, const float *flts, int nflts,
               const float *points, int npoints, std::string *err) {
    return HumanOrderGoal(mod, human, code, acts, nacts, flts, nflts, points, npoints, nullptr, 0, 0, err);
}

// An order with a goal (the bridge's human.go, enter, drive: what ORDER_ENDED's outcome measures) and its retries.
namespace {
static int HumanOrderGoal(MmlMod *mod, void *human, const char *code, void *const *acts, int nacts, const float *flts, int nflts,
                          const float *points, int npoints, const Goal *goal, int retries, int retryMs, std::string *err) {
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
    if (nacts < 0 || nflts < 0) { *err = "acts, flts: counts"; return 0; }
    if ((!code || !*code) && !(goal && goal->kind == kGoalDrive)) { *err = "code: the order's text"; return 0; }
    Order o;
    o.id = g_nextId;
    o.mod = mod;
    o.human = human;
    o.code = code ? code : "";
    if (acts) o.acts.assign(acts, acts + nacts);
    if (flts) o.flts.assign(flts, flts + nflts);
    if (goal) o.goal = *goal;
    o.retries = std::max(0, retries);
    if (retryMs > 0) o.retryMs = retryMs;
    for (int i = 0; i < npoints; i++) {
        void *f = DummyFrameAt(points + 3 * i);
        if (!f) { ReleaseFrames(o); *err = "a frame for the point " + std::to_string(i) + " could not be made"; return 0; }
        o.frames.push_back(f);
    }
    if (!Issue(o, err)) { ReleaseFrames(o); return 0; }
    g_nextId++;
    o.what = o.code;
    if (o.what.size() > 300) o.what = o.what.substr(0, 300) + "...";
    // his program is ours now: the frames of an order before it are free
    for (size_t i = 0; i < g_orders.size(); i++)
        if (g_orders[i].human == human) {
            EndTurn(g_orders[i], "another order given");
            ReleaseFrames(g_orders[i]);
            g_orders.erase(g_orders.begin() + static_cast<long>(i));
            break;
        }
    g_orders.push_back(std::move(o));
    return g_orders.back().id;
}
} // namespace

// Each mission tick (ms: its time): an order the game has done is measured against its goal; short of it, with
// retries left, it is given again after retryMs (a drive the game gave up: a car pushed off its way, a lost path);
// else ORDER_ENDED, with the outcome.
void OrdersTick(int ms) {
    for (Order &o : g_orders) {
        DriveTick(o, ms);
        if (o.retryIn >= 0) {
            o.retryIn -= std::max(ms, 0);
            if (o.retryIn > 0) continue;
            o.retryIn = -1;
            o.retried++;
            std::string err;
            if (Issue(o, &err)) continue;
            HWARN("order %d: not given again: %s", o.id, err.c_str());
            o.retries = o.retried;   // (no more: measured and told below, as it stands)
            o.ended = false;
        }
        if (o.ended) continue;
        MmlOrderState s{sizeof s};
        if (!HumanOrderState(o.human, &s) || s.running) continue;
        o.ended = true;
        if (!Reached(o) && o.retried < o.retries) {
            o.retryIn = o.retryMs;
            HLOG("order %d: ended short of its goal (%s); given again in %d ms", o.id, Outcome(o).dump(-1, ' ', false, json::error_handler_t::replace).c_str(), o.retryMs);
            continue;
        }
        json j = {{"id", o.id}, {"actor", Hex(reinterpret_cast<uintptr_t>(o.human))}, {"owner", ModIdOf(o.mod)}};
        json out = Outcome(o);
        for (auto &kv : out.items()) j[kv.key()] = kv.value();
        std::string data = j.dump(-1, ' ', false, json::error_handler_t::replace);
        EventsRecord("ORDER_ENDED", data);
        HLOG("order %d: done %s", o.id, data.c_str());
    }
}

void OrdersActorGone(void *actor) {
    // a car an order is for: forgotten (the outcome measured at him, no more retries: nothing to drive)
    for (Order &o : g_orders) {
        if (o.goal.car != actor) continue;
        o.goal.car = nullptr;
        o.turning = false;
        o.retries = o.retried;
        for (void *&a : o.acts) if (a == actor) a = nullptr;
    }
    for (size_t i = 0; i < g_orders.size(); i++)
        if (g_orders[i].human == actor) {
            EndTurn(g_orders[i], "its driver gone");
            ReleaseFrames(g_orders[i]);
            g_orders.erase(g_orders.begin() + static_cast<long>(i));
            return;
        }
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
                                  "human.enter", "human.drive", "human.stop"};
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
    Goal goal;
    int retries = p.value("retries", 0);
    // a place: [x, y, z] or an actor (where it is now)
    auto place = [&](const char *key, float out[3]) -> bool {
        if (V3Of(p.value(key, json()), out)) return true;
        void *a = nullptr;
        if (!p.contains(key) || !p[key].is_string() || !ParseActor(p[key].get<std::string>(), &a, &err)) {
            err = std::string(key) + ": [x, y, z] or an actor";
            return false;
        }
        V3 w = game::ActorWorldPos(a);
        out[0] = w.x, out[1] = w.y, out[2] = w.z;
        return true;
    };
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
        if (!place("to", to)) { *reply = "human.go: " + err; return true; }
        points.assign(to, to + 3);
        double radius = p.value("radius", 1.0);
        code = "enemy_move_to_frame 0, " + Num(radius) + (p.value("run", false) ? ", run" : "");
        goal.kind = kGoalPlace;
        std::copy(to, to + 3, goal.to);
        goal.radius = static_cast<float>(radius) + 0.5f;   // (he stops at the radius: a step's margin)
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
        goal.kind = kGoalSeat;
        goal.car = acts[0];
    } else if (method == "human.drive") {
        // into the car (default: the one he sits in) as its driver, then the game's drive by the road network to
        // `to`; the outcome measured at the end (within `radius` of the place or of the road's end), retried
        void *car = nullptr;
        if (p.contains("car")) {
            if (!ActorParam(p, "car", &car, &err)) { *reply = method + ": " + err; return true; }
        } else {
            car = reinterpret_cast<void *>(R32(reinterpret_cast<uintptr_t>(human) + game::kHumanVehicle));
            if (!car) { *reply = "human.drive: car: he sits in none (car: the car he is to drive)"; return true; }
        }
        if (R32(reinterpret_cast<uintptr_t>(car) + game::kActorType) != MML_ACTOR_CAR) { *reply = "human.drive: car: not a car"; return true; }
        float to[3];
        if (!place("to", to)) { *reply = "human.drive: " + err; return true; }
        acts.push_back(car);
        points.assign(to, to + 3);
        goal.kind = kGoalDrive;
        goal.car = car;
        std::copy(to, to + 3, goal.to);
        goal.radius = p.value("radius", 25.0f);
        goal.stop = p.value("stop", true);
        // facing away from its way: "turn" (the game's reverse first), "place" (put on its way), "as_is"
        std::string start = p.value("start", "turn");
        if (start == "turn") goal.start = kStartTurn;
        else if (start == "place") goal.start = kStartPlace;
        else if (start == "as_is") goal.start = kStartAsIs;
        else { *reply = "human.drive: start: \"turn\", \"place\" or \"as_is\""; return true; }
        goal.speed = p.value("speed", 0.0f);
        goal.calm = p.value("calm", true);
        if (!(goal.speed >= 0 && goal.speed < 100)) { *reply = "human.drive: speed: metres a second (0: the road's)"; return true; }
        goal.hasRoadEnd = RoadNear(to, goal.roadEnd) >= 0;
        retries = p.value("retries", 2);
    } else {   // human.stop
        code = "enemy_stop";
    }
    int id = HumanOrderGoal(nullptr, human, code.c_str(), acts.data(), static_cast<int>(acts.size()), flts.data(),
                            static_cast<int>(flts.size()), points.data(), static_cast<int>(points.size() / 3),
                            goal.kind ? &goal : nullptr, retries, p.value("retry_ms", 5000), &err);
    if (!id) { *reply = method + ": " + err; return true; }
    MmlOrderState s{sizeof s};
    HumanOrderState(human, &s);
    *ok = true;
    *reply = StateJson(s, Find(human)).dump(-1, ' ', false, json::error_handler_t::replace);
    return true;
}

} // namespace host
