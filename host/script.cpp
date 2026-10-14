// The game's own script machine for agents and mods: programs in the mission scripts' language, compiled and run by
// the game's interpreter (C_program), as the missions run theirs (mml.h script_*, docs/SCRIPTS.md).
//
// A program is a standalone C_program of the loader's: the game's operator new, its constructor (the standalone
// vtable, whose Process is the full opcode switch), the source compiled by the game's own Compile (each line by the
// vtable's DecodeInstruction and DecodeParams; the loader stops at the first line that does not compile and says
// which), its registers given before it starts (256 floats, 64 actors, 64 frames: allocated with the game's new, as
// its dim_* would), then CallProcess once a mission tick, after the mission's own tick: what the game's entities do
// with theirs in their AI. It ends at END (the game leaves it standing on it) or after its last instruction; a fault
// in a command stops it (the guard says where); the mission's close stops every program (their actors go); an
// actor deleted is taken out of every program's act[] (the game's own InvalidateActorReg). A finished program is
// freed by the game's Done and deleting destructor.
//
// Init is not called: it frees the registers and compiles again; the loader does its work itself (the counters a
// constructed program has, the line before set to -1, the compile).
//
// Bridge methods (game thread): script.run {code, owner, act: {"0": "player" | "car" | "0x..."}, flt: {"0": 1.5}, timeout_ms},
// script.state {id}, script.stop {id | "all"}, script.list.
#include "host.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <memory>
#include "json.hpp"
#include "mml/game/game.h"
#include "mml/game/game_build.h"

using json = nlohmann::json;

namespace host {
namespace {

enum { kProgSize = 0x80, kInstrBegin = 0x10, kInstrEnd = 0x14, kPc = 0x1C, kLineBefore = 0x20, kCallHead = 0x40, kOwner = 0x44,
       kFlt = 0x48, kFltCount = 0x4C, kFrm = 0x50, kFrmCount = 0x54, kAct = 0x58, kActCount = 0x5C };
enum { kOpEnd = 6, kFlts = 256, kActs = 64, kFrms = 64 };
enum { kSlotDecodeInstruction = 6 };

using ThisFn = void(__fastcall *)(uintptr_t self, void *edx);
using SetSourceFn = void(__fastcall *)(uintptr_t self, void *edx, const char *text);
using CompileFn = int(__fastcall *)(uintptr_t self, void *edx, char *text, int stop, int *at, int *len);
using ProcessFn = char(__fastcall *)(uintptr_t self, void *edx, int ms);
using ActorFn = void(__fastcall *)(uintptr_t self, void *edx, uintptr_t actor);
using DeleteFn = void(__fastcall *)(uintptr_t self, void *edx, int flags);
using DecodeFn = int(__fastcall *)(uintptr_t self, void *edx, const char *line, int *len);
using NewFn = void *(__cdecl *)(size_t size);

uint32_t R32(uintptr_t a) { uint32_t v = 0; MemRead(a, &v, 4); return v; }
void W32(uintptr_t a, uint32_t v) { MemWrite(a, &v, 4); }

uintptr_t g_faultAt;
int Note(EXCEPTION_POINTERS *e) {
    g_faultAt = reinterpret_cast<uintptr_t>(e->ExceptionRecord->ExceptionAddress);
    return EXCEPTION_EXECUTE_HANDLER;
}
template <class F> DWORD Guarded(F &f) {
    __try { f(); return 0; } __except (Note(GetExceptionInformation())) { return GetExceptionCode(); }
}

const game::ExeBuild &E() { return game::Exe(); }
bool Known() { return game::g_exe && E().progCtor && E().gameNew; }

struct Script {
    int id;
    MmlMod *mod = nullptr;      // null: the bridge's
    MmlScriptFn fn = nullptr;
    void *user = nullptr;
    uintptr_t prog = 0;
    std::vector<int> lines;     // the source line (1-based) of each instruction
    int state = MML_SCRIPT_RUNNING;
    std::string error;
    int line = 0;
    DWORD started = 0, timeout = 30000, ended = 0;
    unsigned ticks = 0;
    json result;                // the registers as it ended
};
std::vector<std::unique_ptr<Script>> g_scripts;   // running, and the last finished ones (for script.state)
int g_nextId = 1;

unsigned Instructions(uintptr_t prog) {
    uint32_t b = R32(prog + kInstrBegin), e = R32(prog + kInstrEnd);
    return b && e >= b ? (e - b) / 8 : 0;
}

void *GameNew(size_t n) {
    void *p = nullptr;
    auto call = [&] { p = reinterpret_cast<NewFn>(E().gameNew)(n); };
    if (Guarded(call) || !p) return nullptr;
    memset(p, 0, n);
    return p;
}

std::string Hex(uintptr_t v) {
    char b[16];
    snprintf(b, sizeof b, "0x%08X", static_cast<unsigned>(v));
    return b;
}
std::string FrameName(uintptr_t frame) {
    char buf[96] = {};
    uintptr_t p = R32(frame + game::kFrameName);
    if (!p) return "";
    for (size_t i = 0; i + 1 < sizeof buf; i++)
        if (!MemRead(p + i, buf + i, 1) || !buf[i]) { buf[i] = 0; break; }
    return buf;
}

// The registers now: the floats up to the last that is not 0, the actors and frames that are set.
json Registers(uintptr_t prog) {
    json out = {{"flt", json::array()}, {"act", json::array()}, {"frm", json::array()}};
    uint32_t nf = std::min<uint32_t>(R32(prog + kFltCount), 4096), na = std::min<uint32_t>(R32(prog + kActCount), 4096),
             nr = std::min<uint32_t>(R32(prog + kFrmCount), 4096);
    std::vector<float> f(nf);
    if (nf && MemRead(R32(prog + kFlt), f.data(), nf * 4)) {
        size_t last = nf;
        while (last > 0 && f[last - 1] == 0.0f) last--;
        for (size_t i = 0; i < last; i++) out["flt"].push_back(std::round(f[i] * 1e5) / 1e5);
    }
    for (uint32_t i = 0; i < na; i++) {
        uintptr_t a = R32(R32(prog + kAct) + 4 * i);
        if (!a) continue;
        uintptr_t frame = R32(a + game::kActorFrame);
        out["act"].push_back({{"i", i}, {"actor", Hex(a)}, {"type", R32(a + game::kActorType)}, {"model", frame ? FrameName(frame) : ""}});
    }
    for (uint32_t i = 0; i < nr; i++) {
        uintptr_t fr = R32(R32(prog + kFrm) + 4 * i);
        if (fr) out["frm"].push_back({{"i", i}, {"frame", Hex(fr)}, {"name", FrameName(fr)}});
    }
    return out;
}

void Free(Script *s) {
    if (!s->prog) return;
    uintptr_t prog = s->prog;
    s->prog = 0;
    auto call = [&] {
        reinterpret_cast<ThisFn>(E().progDone)(prog, nullptr);
        reinterpret_cast<DeleteFn>(R32(E().progVt))(prog, nullptr, 1);
    };
    if (DWORD ex = Guarded(call)) HWARN("script %d: freeing it faulted (%08X at %s)", s->id, static_cast<unsigned>(ex), DescribeAddress(g_faultAt).c_str());
}

struct CallArgs { MmlScriptFn fn; const MmlScriptResult *r; void *user; };
void CallTramp(void *p) {
    CallArgs *a = static_cast<CallArgs *>(p);
    a->fn(a->r, a->user);
}

// A program ends: its registers kept, its mod told (behind its guard), the game's program freed.
void End(Script *s, int state, const std::string &why, bool tell) {
    if (s->state != MML_SCRIPT_RUNNING) return;
    s->state = state;
    s->error = why;
    s->ended = GetTickCount();
    std::vector<float> flt;
    std::vector<void *> act, frm;
    if (s->prog) {
        s->result = Registers(s->prog);
        uint32_t nf = std::min<uint32_t>(R32(s->prog + kFltCount), 4096), na = std::min<uint32_t>(R32(s->prog + kActCount), 4096),
                 nr = std::min<uint32_t>(R32(s->prog + kFrmCount), 4096);
        flt.resize(nf);
        act.resize(na);
        frm.resize(nr);
        if (nf) MemRead(R32(s->prog + kFlt), flt.data(), nf * 4);
        if (na) MemRead(R32(s->prog + kAct), act.data(), na * 4);
        if (nr) MemRead(R32(s->prog + kFrm), frm.data(), nr * 4);
    }
    Free(s);
    const char *names[] = {"running", "done", "failed", "stopped"};
    if (state == MML_SCRIPT_DONE) HLOG("script %d of %s: done (%u ticks)", s->id, ModIdOf(s->mod), s->ticks);
    else HWARN("script %d of %s: %s: %s", s->id, ModIdOf(s->mod), names[state], why.c_str());
    if (!tell || !s->mod || !s->fn || !ModAlive(s->mod)) return;
    MmlScriptResult r{};
    r.size = sizeof r;
    r.handle = s->id;
    r.state = state;
    r.error = s->error.c_str();
    r.line = s->line;
    r.flt = flt.data();
    r.flt_count = static_cast<int>(flt.size());
    r.act = act.data();
    r.act_count = static_cast<int>(act.size());
    r.frm = frm.data();
    r.frm_count = static_cast<int>(frm.size());
    CallArgs args{s->fn, &r, s->user};
    ModCall(s->mod, CallTramp, &args, "a script's callback");
}

// Finished programs are kept for script.state: the last 32.
void Trim() {
    size_t done = 0;
    for (auto &s : g_scripts) done += s->state != MML_SCRIPT_RUNNING;
    for (size_t i = 0; i < g_scripts.size() && done > 32;) {
        if (g_scripts[i]->state != MML_SCRIPT_RUNNING) { g_scripts.erase(g_scripts.begin() + i); done--; }
        else i++;
    }
}

Script *Find(int id) {
    for (auto &s : g_scripts) if (s->id == id) return s.get();
    return nullptr;
}
// The programs' ids now (a callback may run new programs: the vector changes under a loop over it).
std::vector<int> RunningIds() {
    std::vector<int> ids;
    for (auto &s : g_scripts) if (s->state == MML_SCRIPT_RUNNING) ids.push_back(s->id);
    return ids;
}

// Registers of the game's kind (allocated with its new: its Done frees them, its dim_* replaces them).
bool GiveRegisters(uintptr_t prog, void *const *acts, int nacts, const float *flts, int nflts) {
    int na = std::max(static_cast<int>(kActs), nacts), nf = std::max(static_cast<int>(kFlts), nflts);
    void *a = GameNew(4 * static_cast<size_t>(na)), *f = GameNew(4 * static_cast<size_t>(nf)), *r = GameNew(4 * kFrms);
    if (!a || !f || !r) return false;
    if (acts) for (int i = 0; i < nacts; i++) reinterpret_cast<void **>(a)[i] = acts[i];
    if (flts) for (int i = 0; i < nflts; i++) reinterpret_cast<float *>(f)[i] = flts[i];
    W32(prog + kAct, reinterpret_cast<uintptr_t>(a));
    W32(prog + kActCount, static_cast<uint32_t>(na));
    W32(prog + kFlt, reinterpret_cast<uintptr_t>(f));
    W32(prog + kFltCount, static_cast<uint32_t>(nf));
    W32(prog + kFrm, reinterpret_cast<uintptr_t>(r));
    W32(prog + kFrmCount, kFrms);
    return true;
}

} // namespace

int ScriptRun(MmlMod *mod, const char *code, void *owner, void *const *acts, int nacts, const float *flts, int nflts, uint32_t timeout,
              MmlScriptFn fn, void *user, std::string *err) {
    if (!Known()) { *err = "the script machine is not known for this build"; return 0; }
    if (!code || !*code) { *err = "code: the program's text"; return 0; }
    if (nacts < 0 || nacts > 4096 || nflts < 0 || nflts > 4096) { *err = "acts, flts: at most 4096"; return 0; }
    if (MissionState() == MML_MISSION_NONE || !game::Mission()) { *err = "no mission: programs run in the mission's tick"; return 0; }
    // The text as the game's compiler wants it: one command a line, no empty lines (it takes them as errors), no
    // leading blanks or tabs; each kept line's number remembered.
    auto s = std::make_unique<Script>();
    std::string text;
    int number = 0;
    const char *p = code;
    while (*p) {
        const char *e = p;
        while (*e && *e != '\n' && *e != '\r') e++;
        number++;
        std::string line(p, e);
        size_t a = line.find_first_not_of(" \t"), b = line.find_last_not_of(" \t");
        line = a == std::string::npos ? "" : line.substr(a, b - a + 1);
        for (char &c : line) if (c == '\t') c = ' ';
        if (!line.empty() && line.compare(0, 2, "//") != 0) {
            text += line + "\n";
            s->lines.push_back(number);
        }
        if (*e == '\r' && e[1] == '\n') e++;
        p = *e ? e + 1 : e;
    }
    if (s->lines.empty()) { *err = "code: no command in it"; return 0; }
    if (!text.empty()) text.pop_back();
    const game::ExeBuild &x = E();
    uintptr_t prog = reinterpret_cast<uintptr_t>(GameNew(kProgSize));
    if (!prog) { *err = "the game's new failed"; return 0; }
    int errors = 0, at = -1, len = 0;
    std::vector<char> buf(text.begin(), text.end());
    buf.push_back(0);
    auto make = [&] {
        reinterpret_cast<ThisFn>(x.progCtor)(prog, nullptr);
        reinterpret_cast<SetSourceFn>(x.progSetSource)(prog, nullptr, text.c_str());
        errors = reinterpret_cast<CompileFn>(x.progCompile)(prog, nullptr, buf.data(), 1, &at, &len);
    };
    s->id = g_nextId++;
    s->prog = prog;
    if (DWORD ex = Guarded(make)) {
        *err = "compiling faulted (" + Hex(ex) + " at " + DescribeAddress(g_faultAt) + ")";
        return 0;   // (the half-made program is left as it is)
    }
    if (errors) {
        // the line the compiler stopped at: by its offset in the text it was given
        int index = 0;
        for (int i = 0; i < at && i < static_cast<int>(text.size()); i++) index += text[i] == '\n';
        std::string bad = at >= 0 && at < static_cast<int>(text.size()) ? text.substr(static_cast<size_t>(at), static_cast<size_t>(std::max(len, 0))) : "";
        int known = -1, nameLen = 0;
        auto decode = [&] { known = reinterpret_cast<DecodeFn>(R32(R32(prog) + 4 * kSlotDecodeInstruction))(prog, nullptr, bad.c_str(), &nameLen); };
        Guarded(decode);
        int src = index < static_cast<int>(s->lines.size()) ? s->lines[static_cast<size_t>(index)] : 0;
        *err = "line " + std::to_string(src) + ": \"" + bad + "\": " + (known < 0 ? "not a command of the game's" : "its arguments are not understood");
        Free(s.get());
        if (mod) Log(MML_LOG_WARN, ModIdOf(mod), "script_run: %s", err->c_str());
        return 0;
    }
    W32(prog + kLineBefore, 0xFFFFFFFF);   // (what Init sets)
    W32(prog + kOwner, reinterpret_cast<uintptr_t>(owner));
    if (!GiveRegisters(prog, acts, nacts, flts, nflts)) { Free(s.get()); *err = "the game's new failed for the registers"; return 0; }
    s->mod = mod;
    s->fn = fn;
    s->user = user;
    s->started = GetTickCount();
    s->timeout = timeout ? timeout : 30000;
    int id = s->id;
    HLOG("script %d of %s: %u instructions compiled", id, ModIdOf(mod), Instructions(prog));
    g_scripts.push_back(std::move(s));
    Trim();
    return id;
}

// Each mission tick (after the mission's own): every running program one step, as the game's entities run theirs.
void ScriptTick(int ms) {
    if (g_scripts.empty()) return;
    for (int id : RunningIds()) {
        Script *s = Find(id);   // (a callback may run or stop others)
        if (!s || s->state != MML_SCRIPT_RUNNING || !s->prog) continue;
        if (GetTickCount() - s->started > s->timeout) { End(s, MML_SCRIPT_STOPPED, "its time ran out (" + std::to_string(s->timeout) + " ms)", true); continue; }
        uintptr_t prog = s->prog;
        unsigned pc = R32(prog + kPc);
        auto step = [&] { reinterpret_cast<ProcessFn>(E().progCallProcess)(prog, nullptr, ms); };
        DWORD ex = Guarded(step);
        s->ticks++;
        if (ex) {
            s->line = pc < s->lines.size() ? s->lines[pc] : 0;
            End(s, MML_SCRIPT_FAILED, "line " + std::to_string(s->line) + " faulted (" + Hex(ex) + " at " + DescribeAddress(g_faultAt) + ")", true);
            continue;
        }
        unsigned n = Instructions(prog);
        pc = R32(prog + kPc);
        bool atEnd = pc < n && R32(R32(prog + kInstrBegin) + 8 * pc) == kOpEnd && !R32(prog + kCallHead);
        if (pc >= n || atEnd) End(s, MML_SCRIPT_DONE, "", true);
    }
    Trim();
}

void ScriptMissionClose() {
    for (int id : RunningIds())
        if (Script *s = Find(id)) End(s, MML_SCRIPT_STOPPED, "the mission closed", true);
}

void ScriptActorGone(void *actor) {
    for (auto &s : g_scripts) {
        if (s->state != MML_SCRIPT_RUNNING || !s->prog) continue;
        uintptr_t prog = s->prog;
        auto call = [&] { reinterpret_cast<ActorFn>(E().progInvalidateActor)(prog, nullptr, reinterpret_cast<uintptr_t>(actor)); };
        Guarded(call);
    }
}

void ScriptStop(MmlMod *mod, int handle) {
    Script *s = Find(handle);
    if (s && s->mod == mod) End(s, MML_SCRIPT_STOPPED, "stopped by its owner", true);
}

int ScriptState(MmlMod *mod, int handle) {
    Script *s = Find(handle);
    return s && s->mod == mod ? s->state : -1;
}

void ScriptsRemove(MmlMod *mod) {
    for (int id : RunningIds())
        if (Script *s = Find(id); s && s->mod == mod) End(s, MML_SCRIPT_STOPPED, "its mod unloaded", false);
}

namespace {
// An actor named by the bridge: "player", "car" (the car the player sits in, else the one driven last), an address.
bool ActorOf(const json &v, void **out, std::string *err) {
    if (v.is_string()) {
        std::string s = v.get<std::string>();
        if (s == "player") { *out = game::Player(); if (!*out) { *err = "no player"; return false; } return true; }
        if (s == "car") {
            *out = game::PlayerCar();
            if (!*out) *out = game::LastCar();
            if (!*out) { *err = "no car: the player has not driven one"; return false; }
            return true;
        }
        uintptr_t a = ParseAddress(s);
        if (!a) { *err = "an actor: \"player\", \"car\" or an address (\"0x...\"), not \"" + s + "\""; return false; }
        *out = reinterpret_cast<void *>(a);
    } else if (v.is_number_unsigned() || v.is_number_integer()) {
        *out = reinterpret_cast<void *>(static_cast<uintptr_t>(v.get<uint64_t>()));
    } else {
        *err = "an actor: \"player\", \"car\" or an address";
        return false;
    }
    // an actor is an object whose vtable is in Game.exe's .rdata
    uint32_t vt = R32(reinterpret_cast<uintptr_t>(*out));
    if (!game::InExeSection(".rdata", vt)) { *err = Hex(reinterpret_cast<uintptr_t>(*out)) + " is not an actor (no vtable of the game's)"; return false; }
    return true;
}

json StateOf(Script *s) {
    const char *names[] = {"running", "done", "failed", "stopped"};
    json j = {{"id", s->id}, {"owner", s->mod ? ModIdOf(s->mod) : "bridge"}, {"state", names[s->state]}, {"ticks", s->ticks},
              {"ms", (s->state == MML_SCRIPT_RUNNING ? GetTickCount() : s->ended) - s->started}};
    if (!s->error.empty()) j["error"] = s->error;
    if (s->line) j["line"] = s->line;
    if (s->state == MML_SCRIPT_RUNNING && s->prog) {
        unsigned pc = R32(s->prog + kPc);
        j["at_line"] = pc < s->lines.size() ? s->lines[pc] : 0;
        j.update(Registers(s->prog));
    } else if (s->result.is_object()) {
        j.update(s->result);
    }
    return j;
}
} // namespace

bool ScriptMethod(const std::string &method, const std::string &paramsJson, std::string *reply, bool *ok) {
    if (method != "script.run" && method != "script.state" && method != "script.stop" && method != "script.list") return false;
    *ok = false;
    json p;
    try { p = json::parse(paramsJson.empty() || paramsJson == "null" ? "{}" : paramsJson); } catch (...) {}
    if (!p.is_object()) p = json::object();
    auto dump = [](const json &j) { return j.dump(-1, ' ', false, json::error_handler_t::replace); };
    try {
        if (method == "script.run") {
            std::string code = p.value("code", "");
            std::vector<void *> acts;
            std::vector<float> flts;
            if (p.contains("act") && p["act"].is_object())
                for (auto &kv : p["act"].items()) {
                    int i = std::atoi(kv.key().c_str());
                    if (i < 0 || i >= 4096) { *reply = "act: indexes 0..4095"; return true; }
                    void *a = nullptr;
                    std::string err;
                    if (!ActorOf(kv.value(), &a, &err)) { *reply = "act[" + kv.key() + "]: " + err; return true; }
                    if (acts.size() <= static_cast<size_t>(i)) acts.resize(static_cast<size_t>(i) + 1);
                    acts[static_cast<size_t>(i)] = a;
                }
            if (p.contains("flt") && p["flt"].is_object())
                for (auto &kv : p["flt"].items()) {
                    int i = std::atoi(kv.key().c_str());
                    if (i < 0 || i >= 4096 || !kv.value().is_number()) { *reply = "flt: {index 0..4095: a number}"; return true; }
                    if (flts.size() <= static_cast<size_t>(i)) flts.resize(static_cast<size_t>(i) + 1);
                    flts[static_cast<size_t>(i)] = kv.value().get<float>();
                }
            std::string err;
            void *owner = nullptr;
            if (p.contains("owner") && !p["owner"].is_null() && !ActorOf(p["owner"], &owner, &err)) { *reply = "owner: " + err; return true; }
            int id = ScriptRun(nullptr, code.c_str(), owner, acts.data(), static_cast<int>(acts.size()), flts.data(), static_cast<int>(flts.size()),
                               p.value("timeout_ms", 30000u), nullptr, nullptr, &err);
            if (!id) { *reply = err; return true; }
            *reply = dump({{"id", id}, {"state", "running"}, {"instructions", Instructions(Find(id)->prog)},
                           {"note", "it runs from the next mission tick: script.state {id} tells how it goes and its registers"}});
        } else if (method == "script.state") {
            Script *s = Find(p.value("id", 0));
            if (!s) { *reply = "id: a program's (script.list lists them; the last 32 finished ones are kept)"; return true; }
            *reply = dump(StateOf(s));
        } else if (method == "script.stop") {
            bool all = p.contains("id") && p["id"].is_string() && p["id"].get<std::string>() == "all";
            int want = p.contains("id") && p["id"].is_number_integer() ? p["id"].get<int>() : 0, n = 0;
            if (!all && !want) { *reply = "id: a program's, or \"all\""; return true; }
            for (int id : RunningIds())
                if (Script *s = Find(id); s && (all || id == want)) {
                    End(s, MML_SCRIPT_STOPPED, "stopped from the bridge", true);
                    n++;
                }
            *reply = dump({{"stopped", n}});
        } else {
            json list = json::array();
            for (auto &s : g_scripts) list.push_back({{"id", s->id}, {"owner", s->mod ? ModIdOf(s->mod) : "bridge"},
                                                      {"state", StateOf(s.get())["state"]}});
            *reply = dump({{"known", Known()}, {"scripts", list}});
        }
        *ok = true;
    } catch (const json::exception &e) {
        *reply = e.what();
    }
    return true;
}

} // namespace host
