// tap: watch calls to any function of the running game without writing a hook for it (from mafia-ragdoll's host).
//
//   <at> [n=4] [ret|fret] [ptr=<src>[+off][:len],...] [from=<ret addr>] [log] [max=N]   start watching
//   list | show <id> [k=8] | clear <id> | off <id>|all
//
// <at>: a hex address (Game.exe 5409D0), ls3d+RVA, game+RVA or <module>+RVA. Every call records the return address
// (the caller), ECX and EDX, the first n stack arguments (n <= 8), and up to four snapshots of memory: <src> is an
// argument (1..8), c (ECX), d (EDX) or r (the return value, with ret), +off a hex offset, :len bytes (<= 64, default
// 12: a vector); a snapshot that starts with text shows it as a string too (`ptr=1:32` on a `const char *`).
// `from=` keeps only the calls from one call site (its return address). `ret` also records EAX on return, `fret`
// ST(0) too. `show` gives the callers and the values each argument took (counts), then the last k calls in full.
// `log` writes every call to the log (at most max of them, else 2000).
//
// Each tap is a link of the host's hook chain (owner: the host) whose detour is a small generated thunk: pushad,
// pushfd, TapEntry(index, saved registers), popfd, popad, jmp to the rest of the chain. TapEntry reads the caller's
// return address and the stack arguments through the saved ESP and books them into fixed tables (no allocation: a
// tapped function may run inside the heap's own locks). With `ret`, TapEntry swaps the return address for the return
// thunk and keeps the real one on a per-thread shadow stack; TapReturn books EAX (and ST(0)) and gives the real
// address back to the thunk's `ret`. The host is never unloaded, so the thunks and the swapped addresses stay valid.
//
// Taps in `<game>\mml\config.json` ("taps": ["5409D0 n=4 ptr=1:32 log", ...]) start with the game, before its first
// frame: for what happens during the start (the intro, the menu's first steps).
#include "host.h"
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace host {
namespace tap {
namespace {

constexpr int kMaxTaps = 16, kMaxArgs = 8, kMaxSnaps = 4, kSnapMax = 64, kRing = 64, kCallers = 32, kVals = 8;
constexpr int kShadow = 256;
constexpr int kEcx = kMaxArgs, kEax = kMaxArgs + 1;  // value tables after the arguments'
constexpr long kLogCap = 2000;                         // lines a tap may log without max=

struct Snap { char src; uint32_t off; int len; };  // src: '1'..'8' an argument, 'c' ECX, 'd' EDX, 'r' the result
struct Rec {
    uint32_t ret, ecx, edx, eax;
    float st0;
    uint32_t args[kMaxArgs];
    uint8_t snap[kMaxSnaps][kSnapMax];
    uint8_t snapOk;  // a bit per snapshot that could be read
    bool returned;
    DWORD tid, tick;
};
struct Count { uint32_t v; long n; };
struct Tap {
    bool used;
    unsigned gen;
    uintptr_t target;
    uint32_t from;  // only calls returning here (0: every call)
    void *orig;
    uint8_t *thunk;
    int nargs;
    bool ret, fret, log;
    long max;
    Snap snaps[kMaxSnaps];
    int nsnaps;
    volatile LONG lock;
    long hits, logged;
    DWORD t0;
    Rec ring[kRing];
    long ringN;
    Count callers[kCallers];
    long callersOther;
    Count vals[kMaxArgs + 2][kVals];
    long valsOther[kMaxArgs + 2];
};
struct Shadow { uint32_t ret, esp; int tap; unsigned gen; long rec; };

Tap g_taps[kMaxTaps];
uint8_t *g_code;      // one RWX page: the return thunk, then one entry thunk per tap
uint8_t *g_retThunk;
__declspec(thread) Shadow t_shadow[kShadow];
__declspec(thread) int t_depth;

void Lock(Tap &t) { while (InterlockedExchange(&t.lock, 1)) YieldProcessor(); }
void Unlock(Tap &t) { InterlockedExchange(&t.lock, 0); }

bool ReadMem(const void *p, void *dst, int len) {
    __try { memcpy(dst, p, len); return true; } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

void Bump(Count *c, int n, long *other, uint32_t v) {
    for (int i = 0; i < n; i++) if (c[i].n && c[i].v == v) { c[i].n++; return; }
    for (int i = 0; i < n; i++) if (!c[i].n) { c[i].v = v; c[i].n = 1; return; }
    (*other)++;
}

// An address as the knowledge base writes it: Game.exe absolute, LS3DF.dll as ls3d+RVA, other modules as name+RVA.
std::string At(uint32_t a) {
    char buf[96];
    HMODULE m = nullptr;
    if (a && GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                                (LPCSTR)(uintptr_t)a, &m) && m) {
        std::string path = ModulePath(m);
        const char *name = path.c_str() + path.find_last_of('\\') + 1;
        uint32_t rva = a - (uint32_t)(uintptr_t)m;
        if (m == GetModuleHandleA(nullptr)) snprintf(buf, sizeof buf, "%X", a);
        else if (!_stricmp(name, "LS3DF.dll")) snprintf(buf, sizeof buf, "ls3d+%X", rva);
        else snprintf(buf, sizeof buf, "%s+%X", name, rva);
    } else {
        snprintf(buf, sizeof buf, "%08X", a);
    }
    return buf;
}

// A dword, with its float reading when that looks like a real number (pointers and small integers do not).
std::string Val(uint32_t v) {
    char buf[48];
    float f;
    memcpy(&f, &v, 4);
    float a = fabsf(f);
    if (v && a >= 1e-4f && a <= 1e7f) snprintf(buf, sizeof buf, "%X (%.5g)", v, f);
    else snprintf(buf, sizeof buf, "%X", v);
    return buf;
}

// The text a snapshot starts with: at least 3 printable characters, up to a zero or the snapshot's end.
std::string Text(const uint8_t *b, int len) {
    int n = 0;
    while (n < len && b[n] >= 0x20 && b[n] < 0x7F) n++;
    if (n < 3 || (n < len && b[n] != 0)) return "";
    return std::string(" \"") + std::string(reinterpret_cast<const char *>(b), n) + (n == len ? "...\"" : "\"");
}

std::string SnapText(const Tap &t, const Rec &r, int i) {
    const Snap &s = t.snaps[i];
    char head[32];
    if (s.src == 'c' || s.src == 'd' || s.src == 'r') snprintf(head, sizeof head, "%c+%X:", s.src, s.off);
    else snprintf(head, sizeof head, "a%c+%X:", s.src, s.off);
    std::string out = head;
    if (!(r.snapOk & (1 << i))) return out + " (unreadable)";
    std::string text = Text(r.snap[i], s.len);
    if (!text.empty()) return out + text;
    for (int k = 0; k + 4 <= s.len; k += 4) {
        uint32_t v;
        memcpy(&v, r.snap[i] + k, 4);
        out += " " + Val(v);
    }
    for (int k = s.len & ~3; k < s.len; k++) {
        char b[8];
        snprintf(b, sizeof b, " %02X", r.snap[i][k]);
        out += b;
    }
    return out;
}

void TakeSnaps(const Tap &t, Rec &r, const uint32_t *f, const uint32_t *args, bool atReturn) {
    for (int i = 0; i < t.nsnaps; i++) {
        const Snap &s = t.snaps[i];
        if ((s.src == 'r') != atReturn) continue;
        uint32_t base = s.src == 'c' ? f[7] : s.src == 'd' ? f[6] : s.src == 'r' ? f[8] : args[s.src - '1'];
        if (base && ReadMem((const void *)(uintptr_t)(base + s.off), r.snap[i], s.len)) r.snapOk |= 1 << i;
    }
}

std::string RecText(const Tap &t, const Rec &r, long n) {
    char head[96];
    snprintf(head, sizeof head, "#%ld +%lums%s from ", n, (unsigned long)(r.tick - t.t0),
             r.tid == MainThread() ? "" : " (other thread)");
    std::string s = head + At(r.ret) + " ecx=" + Val(r.ecx);
    for (int i = 0; i < t.nargs; i++) s += " a" + std::to_string(i + 1) + "=" + Val(r.args[i]);
    for (int i = 0; i < t.nsnaps; i++) s += " | " + SnapText(t, r, i);
    if (t.ret || t.fret) {
        if (!r.returned) s += " -> (not returned yet)";
        else {
            s += " -> eax=" + Val(r.eax);
            if (t.fret) { char b[32]; snprintf(b, sizeof b, " st0=%.6g", r.st0); s += b; }
        }
    }
    return s;
}

void LogRec(Tap &t, int idx, const Rec &r, long n) {
    if (!t.log) return;
    if (InterlockedIncrement(&t.logged) > (t.max > 0 ? t.max : kLogCap)) return;
    Log(MML_LOG_INFO, "tap", "tap %d %s: %s", idx, At((uint32_t)t.target).c_str(), RecText(t, r, n).c_str());
}

} // namespace

extern "C" void __cdecl TapEntry(int idx, uint32_t *f) {
    // f: [0] EFLAGS, then pushad's EDI ESI EBP ESP EBX EDX ECX EAX; ESP points at the return address
    Tap &t = g_taps[idx];
    if (!t.used) return;
    uint32_t *sp = (uint32_t *)(uintptr_t)f[4];
    if (t.from && sp[0] != t.from) return;
    uint32_t args[kMaxArgs] = {};
    ReadMem(sp + 1, args, 4 * t.nargs);
    Lock(t);
    long n = ++t.hits;
    bool record = t.max <= 0 || n <= t.max;
    long slot = -1;
    Rec copy;
    if (record) {
        slot = t.ringN++;
        Rec &r = t.ring[slot % kRing];
        memset(&r, 0, sizeof r);
        r.ret = sp[0];
        r.ecx = f[7];
        r.edx = f[6];
        memcpy(r.args, args, sizeof args);
        r.tid = GetCurrentThreadId();
        r.tick = GetTickCount();
        TakeSnaps(t, r, f, args, false);
        Bump(t.callers, kCallers, &t.callersOther, r.ret);
        for (int i = 0; i < t.nargs; i++) Bump(t.vals[i], kVals, &t.valsOther[i], args[i]);
        Bump(t.vals[kEcx], kVals, &t.valsOther[kEcx], r.ecx);
        copy = r;
    }
    Unlock(t);
    if (!record) return;
    // a thread without a TLS vector (made with SkipThreadAttach) has no shadow stack: its calls are logged at entry
    if (!ThreadHasTls()) { LogRec(t, idx, copy, n); return; }
    // entries at or below this call's stack pointer belong to frames already gone (an exception unwound them)
    while (t_depth > 0 && t_shadow[t_depth - 1].esp <= f[4]) t_depth--;
    if ((t.ret || t.fret) && t_depth < kShadow) {
        t_shadow[t_depth++] = {sp[0], f[4], idx, t.gen, slot};
        sp[0] = (uint32_t)(uintptr_t)g_retThunk;
    } else {
        LogRec(t, idx, copy, n);
    }
}

extern "C" void __cdecl TapReturn(uint32_t *f) {
    // f as in TapEntry, then [9]: the slot the thunk's `ret` takes the real return address from
    float st0 = 0;
    __asm fst st0  // the function's float result, if it has one (reads ST(0) without popping it)
    uint32_t cur = f[4];
    // The entry this return belongs to: the deepest one at or below the current stack pointer. Deeper entries
    // were left by frames an exception unwound; they are dropped.
    int best = -1;
    for (int i = t_depth - 1; i >= 0; i--) {
        if (t_shadow[i].esp <= cur && (best < 0 || t_shadow[i].esp > t_shadow[best].esp)) best = i;
    }
    if (best < 0) best = t_depth - 1;  // cannot happen while the stack is sound
    Shadow s = t_shadow[best];
    t_depth = best;
    f[9] = s.ret;
    Tap &t = g_taps[s.tap];
    if (!t.used || t.gen != s.gen) return;
    Lock(t);
    bool live = s.rec >= 0 && t.ringN - s.rec <= kRing;
    Rec copy;
    if (live) {
        Rec &r = t.ring[s.rec % kRing];
        r.eax = f[8];
        r.st0 = st0;
        r.returned = true;
        TakeSnaps(t, r, f, r.args, true);
        Bump(t.vals[kEax], kVals, &t.valsOther[kEax], r.eax);
        copy = r;
    }
    Unlock(t);
    if (live) LogRec(t, s.tap, copy, s.rec + 1);
}

namespace {

uint8_t *MakeEntryThunk(int idx, void **origSlot) {
    uint8_t *p = g_code + 64 * (idx + 1);
    int k = 0;
    p[k++] = 0x60;                         // pushad
    p[k++] = 0x9C;                         // pushfd
    p[k++] = 0x54;                         // push esp
    p[k++] = 0x68;                         // push idx
    *(int32_t *)(p + k) = idx; k += 4;
    p[k++] = 0xE8;                         // call TapEntry
    *(int32_t *)(p + k) = (int32_t)((uintptr_t)&TapEntry - (uintptr_t)(p + k + 4)); k += 4;
    p[k++] = 0x83; p[k++] = 0xC4; p[k++] = 0x08;  // add esp, 8
    p[k++] = 0x9D;                         // popfd
    p[k++] = 0x61;                         // popad
    p[k++] = 0xFF; p[k++] = 0x25;          // jmp [origSlot]: the rest of the chain
    *(uint32_t *)(p + k) = (uint32_t)(uintptr_t)origSlot; k += 4;
    FlushInstructionCache(GetCurrentProcess(), p, k);
    return p;
}

void MakeReturnThunk() {
    uint8_t *p = g_code;
    int k = 0;
    p[k++] = 0x83; p[k++] = 0xEC; p[k++] = 0x04;  // sub esp, 4: the slot for the real return address
    p[k++] = 0x60;                                 // pushad
    p[k++] = 0x9C;                                 // pushfd
    p[k++] = 0x54;                                 // push esp
    p[k++] = 0xE8;                                 // call TapReturn
    *(int32_t *)(p + k) = (int32_t)((uintptr_t)&TapReturn - (uintptr_t)(p + k + 4)); k += 4;
    p[k++] = 0x83; p[k++] = 0xC4; p[k++] = 0x04;  // add esp, 4
    p[k++] = 0x9D;                                 // popfd
    p[k++] = 0x61;                                 // popad
    p[k++] = 0xC3;                                 // ret: to the real return address
    FlushInstructionCache(GetCurrentProcess(), p, k);
    g_retThunk = p;
}

bool IsExec(uintptr_t a) {
    MEMORY_BASIC_INFORMATION mi;
    if (!VirtualQuery((const void *)a, &mi, sizeof mi) || mi.State != MEM_COMMIT) return false;
    return (mi.Protect & (PAGE_EXECUTE | PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY)) != 0;
}

std::string Off(int idx) {
    Tap &t = g_taps[idx];
    if (!t.used) return "no tap " + std::to_string(idx);
    HookRemoveLink(nullptr, t.target, t.thunk);
    t.used = false;  // the thunk and the record stay: a return in flight still finds the return thunk
    return "tap " + std::to_string(idx) + " off";
}

std::string Start(char **tok, int n) {
    uintptr_t at = ParseAddress(tok[0]);
    if (!at || !IsExec(at)) return std::string("error: not code: ") + tok[0];
    for (int i = 0; i < kMaxTaps; i++) if (g_taps[i].used && g_taps[i].target == at) return "error: tap " + std::to_string(i) + " already watches it";
    int idx = -1;
    for (int i = 0; i < kMaxTaps; i++) if (!g_taps[i].used) { idx = i; break; }
    if (idx < 0) return "error: all taps in use (off <id>)";
    Tap &t = g_taps[idx];
    unsigned gen = t.gen + 1;
    memset(&t, 0, sizeof t);
    t.gen = gen;
    t.target = at;
    t.nargs = 4;
    for (int i = 1; i < n; i++) {
        const char *a = tok[i];
        if (!strncmp(a, "n=", 2)) t.nargs = atoi(a + 2) < 0 ? 0 : atoi(a + 2) > kMaxArgs ? kMaxArgs : atoi(a + 2);
        else if (!strcmp(a, "ret")) t.ret = true;
        else if (!strcmp(a, "fret")) t.ret = t.fret = true;
        else if (!strcmp(a, "log")) t.log = true;
        else if (!strncmp(a, "max=", 4)) t.max = atol(a + 4);
        else if (!strncmp(a, "from=", 5)) {
            t.from = (uint32_t)ParseAddress(a + 5);
            if (!t.from) return std::string("error: from= ") + (a + 5);
        }
        else if (!strncmp(a, "ptr=", 4)) {
            char spec[128];
            strncpy(spec, a + 4, sizeof spec - 1);
            spec[sizeof spec - 1] = 0;
            char *ctx = nullptr;
            for (char *p = strtok_s(spec, ",", &ctx); p && t.nsnaps < kMaxSnaps; p = strtok_s(nullptr, ",", &ctx)) {
                Snap s{p[0], 0, 12};
                if (!strchr("12345678cdr", s.src)) return std::string("error: ptr source ") + p;
                const char *q = p + 1;
                if (*q == '+') s.off = strtoul(q + 1, (char **)&q, 16);
                if (*q == ':') s.len = atoi(q + 1);
                if (s.len < 1 || s.len > kSnapMax) return std::string("error: ptr length ") + p;
                if (s.src >= '1' && s.src <= '8' && s.src - '0' > t.nargs) t.nargs = s.src - '0';
                t.snaps[t.nsnaps++] = s;
            }
        } else return std::string("error: unknown option ") + a;
    }
    t.thunk = MakeEntryThunk(idx, &t.orig);
    t.t0 = GetTickCount();
    t.used = true;  // before the hook goes live: TapEntry ignores unused taps
    std::string err;
    if (!HookAdd(nullptr, at, t.thunk, &t.orig, &err)) {
        t.used = false;
        return "error: " + err;
    }
    return "tap " + std::to_string(idx) + " on " + At((uint32_t)at);
}

std::string Show(int idx, int k) {
    Tap &t = g_taps[idx];
    if (!t.gen) return "no tap " + std::to_string(idx);
    Lock(t);
    Tap *s = new Tap(t);  // a copy to format outside the lock (too big for the stack of the bridge's threads)
    Unlock(t);
    char head[160];
    snprintf(head, sizeof head, "tap %d %s%s: %ld calls, %ld recorded%s", idx, At((uint32_t)s->target).c_str(),
             s->used ? "" : " (off)", s->hits, s->ringN, s->max > 0 && s->hits > s->max ? " (max reached)" : "");
    std::string out = head;
    if (s->from) out += " (only from " + At(s->from) + ")";
    out += "\n";
    auto table = [&](const char *name, const Count *c, int n, long other, bool where) {
        // the counts, largest first
        int order[kCallers];
        int m = 0;
        for (int i = 0; i < n; i++) if (c[i].n) order[m++] = i;
        for (int i = 0; i < m; i++) for (int j = i + 1; j < m; j++) if (c[order[j]].n > c[order[i]].n) std::swap(order[i], order[j]);
        if (!m) return;
        out += std::string("  ") + name + ":";
        for (int i = 0; i < m; i++)
            out += (i ? ", " : " ") + (where ? At(c[order[i]].v) : Val(c[order[i]].v)) + " x" + std::to_string(c[order[i]].n);
        if (other) out += ", " + std::to_string(other) + " calls with other values";
        out += "\n";
    };
    table("callers", s->callers, kCallers, s->callersOther, true);
    table("ecx", s->vals[kEcx], kVals, s->valsOther[kEcx], false);
    for (int i = 0; i < s->nargs; i++) table(("a" + std::to_string(i + 1)).c_str(), s->vals[i], kVals, s->valsOther[i], false);
    if (s->ret) table("eax", s->vals[kEax], kVals, s->valsOther[kEax], false);
    long first = s->ringN - k < 0 ? 0 : s->ringN - k;
    if (s->ringN - first > kRing) first = s->ringN - kRing;
    for (long i = first; i < s->ringN; i++) out += "  " + RecText(*s, s->ring[i % kRing], i + 1) + "\n";
    delete s;
    return out;
}

} // namespace

void Init() {
    if (!g_code) {
        g_code = (uint8_t *)VirtualAlloc(nullptr, 4096, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
        if (g_code) MakeReturnThunk();
    }
}

std::string Command(const std::string &line) {
    if (!g_code) return "error: tap has no code page";
    char buf[512];
    strncpy(buf, line.c_str(), sizeof buf - 1);
    buf[sizeof buf - 1] = 0;
    char *tok[24];
    int n = 0;
    char *ctx = nullptr;
    for (char *p = strtok_s(buf, " \t", &ctx); p && n < 24; p = strtok_s(nullptr, " \t", &ctx)) tok[n++] = p;
    if (n < 1) return "<at> [n=4] [ret|fret] [ptr=...] [from=...] [log] [max=N] | list | show <id> [k] | clear <id> | off <id>|all";
    std::string c = tok[0];
    if (c == "list") {
        std::string out;
        for (int i = 0; i < kMaxTaps; i++) {
            const Tap &t = g_taps[i];
            if (t.used) out += "tap " + std::to_string(i) + " " + At((uint32_t)t.target) + ": " + std::to_string(t.hits) + " calls\n";
        }
        return out.empty() ? "no taps" : out;
    }
    if (c == "off") {
        if (n < 2) return "off <id>|all";
        if (!strcmp(tok[1], "all")) {
            std::string out;
            for (int i = 0; i < kMaxTaps; i++) if (g_taps[i].used) out += Off(i) + "\n";
            return out.empty() ? "no taps" : out;
        }
        return Off(atoi(tok[1]));
    }
    if (c == "show" || c == "clear") {
        if (n < 2) return c + " <id>";
        int idx = atoi(tok[1]);
        if (idx < 0 || idx >= kMaxTaps) return "no tap";
        if (c == "show") return Show(idx, n > 2 ? atoi(tok[2]) : 8);
        Tap &t = g_taps[idx];
        Lock(t);
        t.hits = t.ringN = t.logged = t.callersOther = 0;
        memset(t.callers, 0, sizeof t.callers);
        memset(t.vals, 0, sizeof t.vals);
        memset(t.valsOther, 0, sizeof t.valsOther);
        t.t0 = GetTickCount();
        Unlock(t);
        return "tap " + std::to_string(idx) + " cleared";
    }
    return Start(tok, n);
}

} // namespace tap
} // namespace host
