// Hardware watchpoints on the game thread (docs/PROTOCOL.md "watch"): who writes, or reads, a field. After
// mafia-ragdoll's hwwatch.cpp, for up to four at once (the debug registers DR0..DR3) and from the bridge.
//
// A watch traps on the game thread's every access of its 1, 2 or 4 bytes (write: writes only; access: reads and
// writes). The vectored handler records each piece of code that did it once (its address, how many times, the first
// game-module return addresses on the stack, and for writes the value before and after), and lets the thread go
// on. It never reads the watched memory in access mode (a read there traps again: a recursion once crashed the game)
// and allocates nothing: the records are fixed arrays, formatted when asked. Other threads are not watched.
//
// Bridge methods (game thread): watch.set {addr, mode, len} -> {id}, watch.show {id}, watch.clear {id | all}.
#include "host.h"
#include <cstring>
#include "json.hpp"

using json = nlohmann::json;

namespace host {
namespace {

constexpr int kSlots = 4, kSites = 48, kStack = 10;
struct Site { uint32_t eip, count, before, after; uint32_t stack[kStack]; };
struct Watch {
    bool on;
    int id;
    uintptr_t addr;
    int len;                // 1, 2, 4
    bool access;
    volatile uint32_t last; // write mode: the value before the next write
    Site sites[kSites];
    volatile LONG nsites;
    volatile LONG hits, dropped;
};
Watch g_w[kSlots];
int g_nextId = 1;
PVOID g_veh;
uintptr_t g_selfBase, g_selfEnd;
struct Range { uintptr_t base, end; };
Range g_game[2];   // Game.exe, LS3DF.dll: the return addresses kept

uint32_t ReadLen(uintptr_t a, int len) {
    uint32_t v = 0;
    memcpy(&v, reinterpret_cast<const void *>(a), static_cast<size_t>(len));
    return v;
}
bool IsGame(uint32_t a) {
    for (const Range &r : g_game) if (a >= r.base && a < r.end) return true;
    return false;
}

LONG CALLBACK Handler(EXCEPTION_POINTERS *ep) {
    if (ep->ExceptionRecord->ExceptionCode != EXCEPTION_SINGLE_STEP) return EXCEPTION_CONTINUE_SEARCH;
    DWORD dr6 = ep->ContextRecord->Dr6;
    if (!(dr6 & 0xF)) return EXCEPTION_CONTINUE_SEARCH;   // (a single step not ours)
    ep->ContextRecord->Dr6 = 0;
    uint32_t eip = ep->ContextRecord->Eip;
    bool ours = eip >= g_selfBase && eip < g_selfEnd;
    for (int i = 0; i < kSlots; i++) {
        if (!(dr6 & (1u << i)) || !g_w[i].on) continue;
        Watch &w = g_w[i];
        if (ours) continue;   // (the loader's own reads, e.g. memory shown to an agent)
        uint32_t now = 0;
        if (!w.access) {
            now = ReadLen(w.addr, w.len);
            if (now == w.last) continue;   // (written with the same value)
        }
        InterlockedIncrement(&w.hits);
        LONG n = w.nsites;
        Site *s = nullptr;
        for (LONG k = 0; k < n; k++) if (w.sites[k].eip == eip) { s = &w.sites[k]; break; }
        if (!s) {
            if (n >= kSites) { InterlockedIncrement(&w.dropped); if (!w.access) w.last = now; continue; }
            s = &w.sites[n];
            memset(s, 0, sizeof *s);
            s->eip = eip;
            const uint32_t *sp = reinterpret_cast<const uint32_t *>(static_cast<uintptr_t>(ep->ContextRecord->Esp));
            int k = 0;
            for (int j = 0; j < 512 && k < kStack; j++) {
                uint32_t v = 0;
                if (IsBadReadPtr(sp + j, 4)) break;
                v = sp[j];
                if (IsGame(v)) s->stack[k++] = v;
            }
            InterlockedIncrement(&w.nsites);
        }
        s->count++;
        if (!w.access) { s->before = w.last; s->after = now; w.last = now; }
    }
    return EXCEPTION_CONTINUE_EXECUTION;
}

struct SetRegs { DWORD tid; };
DWORD WINAPI Setter(void *p) {
    auto *s = static_cast<SetRegs *>(p);
    HANDLE t = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_SET_CONTEXT, FALSE, s->tid);
    if (!t) return 1;
    SuspendThread(t);
    CONTEXT c{};
    c.ContextFlags = CONTEXT_DEBUG_REGISTERS;
    GetThreadContext(t, &c);
    DWORD dr7 = 0;
    DWORD *dr[kSlots] = {&c.Dr0, &c.Dr1, &c.Dr2, &c.Dr3};
    for (int i = 0; i < kSlots; i++) {
        const Watch &w = g_w[i];
        *dr[i] = w.on ? static_cast<DWORD>(w.addr) : 0;
        if (!w.on) continue;
        DWORD rw = w.access ? 3u : 1u, ln = w.len == 1 ? 0u : w.len == 2 ? 1u : 3u;
        dr7 |= (1u << (i * 2)) | (rw << (16 + i * 4)) | (ln << (18 + i * 4));
    }
    c.Dr7 = dr7;
    c.Dr6 = 0;
    SetThreadContext(t, &c);
    ResumeThread(t);
    CloseHandle(t);
    return 0;
}
void Apply() {
    SetRegs s{GetCurrentThreadId()};
    HANDLE h = CreateThread(nullptr, 0, Setter, &s, 0, nullptr);
    if (h) { WaitForSingleObject(h, 2000); CloseHandle(h); }
}

std::string Hex8(uint32_t v) {
    char b[16];
    snprintf(b, sizeof b, "0x%08X", v);
    return b;
}

} // namespace

bool WatchMethod(const std::string &method, const std::string &paramsJson, std::string *reply, bool *ok) {
    if (method != "watch.set" && method != "watch.show" && method != "watch.clear") return false;
    *ok = false;
    json p = json::parse(paramsJson.empty() ? "null" : paramsJson, nullptr, false);
    if (!p.is_object()) p = json::object();
    if (!g_selfBase) {
        HMODULE self = nullptr;
        GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           reinterpret_cast<LPCSTR>(&Handler), &self);
        if (self) {
            auto *sdos = reinterpret_cast<IMAGE_DOS_HEADER *>(self);
            auto *snt = reinterpret_cast<IMAGE_NT_HEADERS *>(reinterpret_cast<uint8_t *>(self) + sdos->e_lfanew);
            g_selfBase = reinterpret_cast<uintptr_t>(self);
            g_selfEnd = g_selfBase + snt->OptionalHeader.SizeOfImage;
        }
        const char *mods[2] = {nullptr, "LS3DF.dll"};
        for (int i = 0; i < 2; i++) {
            HMODULE m = GetModuleHandleA(mods[i]);
            if (!m) continue;
            auto *dos = reinterpret_cast<IMAGE_DOS_HEADER *>(m);
            auto *nt = reinterpret_cast<IMAGE_NT_HEADERS *>(reinterpret_cast<uint8_t *>(m) + dos->e_lfanew);
            g_game[i] = {reinterpret_cast<uintptr_t>(m), reinterpret_cast<uintptr_t>(m) + nt->OptionalHeader.SizeOfImage};
        }
    }
    if (method == "watch.set") {
        std::string a = p.value("addr", "");
        uintptr_t addr = ParseAddress(a);
        int len = p.value("len", 4);
        std::string mode = p.value("mode", "write");
        if (!addr) { *reply = "watch.set: addr (\"0x...\", \"game+0x...\")"; return true; }
        if (len != 1 && len != 2 && len != 4) { *reply = "watch.set: len 1, 2 or 4"; return true; }
        if (addr % static_cast<uintptr_t>(len)) { *reply = "watch.set: the address must be aligned to len (the debug registers' rule)"; return true; }
        if (mode != "write" && mode != "access") { *reply = "watch.set: mode write or access"; return true; }
        uint32_t v = 0;
        if (!MemRead(addr, &v, static_cast<size_t>(len))) { *reply = "watch.set: the address is not readable"; return true; }
        int slot = -1;
        for (int i = 0; i < kSlots; i++) if (!g_w[i].on) { slot = i; break; }
        if (slot < 0) { *reply = "watch.set: four watches already (watch.clear one)"; return true; }
        if (!g_veh) g_veh = AddVectoredExceptionHandler(1, Handler);
        Watch &w = g_w[slot];
        memset(&w, 0, sizeof w);
        w.id = g_nextId++;
        w.addr = addr;
        w.len = len;
        w.access = mode == "access";
        w.last = v;
        w.on = true;
        Apply();
        *ok = true;
        *reply = json({{"id", w.id}, {"addr", Hex8(static_cast<uint32_t>(addr))}, {"where", DescribeAddress(addr)}, {"mode", mode}, {"len", len},
                       {"value", Hex8(v)}, {"next", "let the game run, then watch.show {id}"}}).dump(-1, ' ', false, json::error_handler_t::replace);
        return true;
    }
    if (method == "watch.clear") {
        bool all = p.value("all", false) || !p.contains("id");
        int id = p.value("id", 0);
        int n = 0;
        for (Watch &w : g_w) if (w.on && (all || w.id == id)) { w.on = false; n++; }
        Apply();
        *ok = true;
        *reply = json({{"cleared", n}}).dump(-1, ' ', false, json::error_handler_t::replace);
        return true;
    }
    int id = p.value("id", 0);
    for (Watch &w : g_w) {
        if (w.id != id || !w.id) continue;
        json sites = json::array();
        LONG n = w.nsites;
        for (LONG k = 0; k < n; k++) {
            const Site &s = w.sites[k];
            json st = json::array();
            for (uint32_t r : s.stack) if (r) st.push_back(DescribeAddress(r));
            json j = {{"at", DescribeAddress(s.eip)}, {"count", s.count}, {"stack", st}};
            if (!w.access) { j["before"] = Hex8(s.before); j["after"] = Hex8(s.after); }
            sites.push_back(j);
        }
        uint32_t now = 0;
        bool readable = !w.access && MemRead(w.addr, &now, static_cast<size_t>(w.len));
        json out = {{"id", w.id}, {"on", w.on}, {"addr", Hex8(static_cast<uint32_t>(w.addr))}, {"mode", w.access ? "access" : "write"},
                    {"hits", w.hits}, {"sites", sites}, {"dropped", w.dropped}};
        if (readable) out["value"] = Hex8(now);
        *reply = out.dump(-1, ' ', false, json::error_handler_t::replace);
        *ok = true;
        return true;
    }
    *reply = "watch.show: no such watch (its id from watch.set; a cleared one's records go with its slot's reuse)";
    return true;
}

} // namespace host
