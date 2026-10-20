// Crash reports and the main thread's whereabouts (see host.h).
//
// An exception nobody handles (the game would die) is logged (module and offset, the access, the registers, the call
// stack: the faulting thread's likely return addresses, code with a call right before it) and written as a minidump
// to <game>\mml\crash\Game_<pid>_<time>.dmp with the heaps and every region's state, so an object a crash points into
// can be looked at afterwards; beside it Game_<pid>_<time>.json, what dies with the process: the stack with the mods
// named, the log's last lines, the taps' last calls, the last events, the mods and the mission (mcp: crash_triage,
// crash_list, crash_inspect). Exceptions caught by __try never get here (a mod's fault: mods.cpp, with its stack too).
// The game or a wrapper may install a filter later: SetUnhandledExceptionFilter is hooked, theirs goes after ours
// (CrashFilterStart; without the hook, ours is set again every frame).
#include "host.h"
#include <cstdio>
#include <cstring>
#include "json.hpp"

using json = nlohmann::json;

namespace host {
json EventsSince(const json &p, std::string *err);   // (events.cpp)

namespace {
LPTOP_LEVEL_EXCEPTION_FILTER g_prevFilter;
volatile LONG g_crashing;
DWORD g_mainTid;

bool ModuleOf(const void *addr, char *name, int cap, uintptr_t *rva) {
    HMODULE m = nullptr;
    if (!GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                            static_cast<const char *>(addr), &m) || !m)
        return false;
    std::string path = ModulePath(m);
    if (path.empty()) return false;
    snprintf(name, cap, "%s", path.c_str() + path.find_last_of('\\') + 1);
    *rva = reinterpret_cast<uintptr_t>(addr) - reinterpret_cast<uintptr_t>(m);
    return true;
}

bool IsCode(uintptr_t addr) {
    MEMORY_BASIC_INFORMATION mi;
    if (!VirtualQuery(reinterpret_cast<const void *>(addr), &mi, sizeof mi) || mi.State != MEM_COMMIT || mi.Type != MEM_IMAGE)
        return false;
    return (mi.Protect & (PAGE_EXECUTE | PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY)) != 0;
}

// A likely return address: code whose bytes right before it are a call (E8 rel32, or FF /2: a register, [reg],
// [reg+d8], [reg+d32], [abs32], with or without an index byte). Leftovers of finished calls pass too: a stack scan is
// close to the calls, not exact (the game's code keeps no frame pointers).
bool AfterCall(uintptr_t v) {
    if (v < 0x10000 || !IsCode(v) || !IsCode(v - 7)) return false;
    const uint8_t *p = reinterpret_cast<const uint8_t *>(v);
    auto reg2 = [](uint8_t modrm) { return (modrm & 0x38) == 0x10; };   // the /2 of FF
    if (p[-5] == 0xE8) return true;
    if (p[-2] == 0xFF && reg2(p[-1]) && (p[-1] >> 6 == 3 || (p[-1] >> 6 == 0 && (p[-1] & 7) != 4 && (p[-1] & 7) != 5))) return true;
    if (p[-3] == 0xFF && reg2(p[-2]) && ((p[-2] >> 6 == 1 && (p[-2] & 7) != 4) || (p[-2] >> 6 == 0 && (p[-2] & 7) == 4))) return true;
    if (p[-4] == 0xFF && reg2(p[-3]) && p[-3] >> 6 == 1 && (p[-3] & 7) == 4) return true;
    if (p[-6] == 0xFF && reg2(p[-5]) && ((p[-5] >> 6 == 2 && (p[-5] & 7) != 4) || p[-5] == 0x15)) return true;
    if (p[-7] == 0xFF && reg2(p[-6]) && p[-6] >> 6 == 2 && (p[-6] & 7) == 4) return true;
    return false;
}

std::string Frame(uintptr_t pc) {
    std::string mod = ModAt(pc);
    return DescribeAddress(pc) + (mod.empty() ? "" : " (mod " + mod + ")");
}

// What dies with the process, written beside the dump on a thread of its own: the crashing thread may hold the lock
// of the log, the events or a tap, so the report gives up after a few seconds rather than hang the exit.
struct Report {
    std::string file;
    json head;
};
DWORD WINAPI WriteReport(void *arg) {
    Report *r = static_cast<Report *>(arg);
    json j = r->head;
    j["build"] = GameBuild();
    j["frames"] = Frames();
    j["mission"] = {{"name", MissionName()}, {"state", MissionState()}};
    j["mods"] = json::parse(ModsList(), nullptr, false);
    j["log"] = LogTail(60);
    std::string taps = tap::Command("list");
    json shown = json::array();
    for (int i = 0; i < 16; i++) {
        if (taps.find("tap " + std::to_string(i) + " ") != std::string::npos) shown.push_back(tap::Command("show " + std::to_string(i) + " 8"));
    }
    j["taps"] = shown;
    std::string err;
    uint64_t last = EventsSince(json{{"seq", 0}, {"limit", 1}}, &err).value("latest", static_cast<uint64_t>(0));
    j["events"] = EventsSince(json{{"seq", last > 40 ? last - 40 : 0}}, &err)["events"];
    std::string text = j.dump(1, ' ', false, json::error_handler_t::replace);
    HANDLE f = PathCreateFile(r->file, GENERIC_WRITE, 0, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL);
    if (f != INVALID_HANDLE_VALUE) {
        DWORD wrote = 0;
        WriteFile(f, text.data(), static_cast<DWORD>(text.size()), &wrote, nullptr);
        CloseHandle(f);
    }
    return 0;
}

typedef BOOL(WINAPI *MiniDumpWriteDumpFn)(HANDLE, DWORD, HANDLE, int, void *, void *, void *);

// The threads inside the filter now, by id: a filter we chain to may chain back to us (it saw ours as its previous
// one), and that loop stops at the second entry. Not thread_local: the filter runs on whichever thread faults, and a
// thread without a TLS vector (made with SkipThreadAttach: a player's game had one) faulted again on reading it, so
// the game died with neither a log line nor a dump.
volatile LONG g_inside[16];

LONG WINAPI CrashFilter(EXCEPTION_POINTERS *ep) {
    const LONG me = static_cast<LONG>(GetCurrentThreadId());
    int slot = -1;
    for (int i = 0; i < 16; i++) if (g_inside[i] == me) return EXCEPTION_CONTINUE_SEARCH;
    for (int i = 0; i < 16 && slot < 0; i++) if (InterlockedCompareExchange(&g_inside[i], me, 0) == 0) slot = i;
    // (more than 16 threads faulting at once: the rest go on unguarded; the dump is written once anyway)
    if (InterlockedExchange(&g_crashing, 1) == 0) {
        const EXCEPTION_RECORD *er = ep->ExceptionRecord;
        const bool av = er->ExceptionCode == EXCEPTION_ACCESS_VIOLATION && er->NumberParameters >= 2;
        char name[64] = "?";
        uintptr_t rva = 0;
        ModuleOf(er->ExceptionAddress, name, sizeof name, &rva);
        Log(MML_LOG_ERROR, "crash", "exception %08X at %p (%s+0x%X) on thread %u%s", static_cast<unsigned>(er->ExceptionCode),
            er->ExceptionAddress, name, static_cast<unsigned>(rva), static_cast<unsigned>(GetCurrentThreadId()),
            av ? (er->ExceptionInformation[0] ? ", write" : ", read") : "");
        if (av) Log(MML_LOG_ERROR, "crash", "address %p", reinterpret_cast<void *>(er->ExceptionInformation[1]));
        const CONTEXT *c = ep->ContextRecord;
        Log(MML_LOG_ERROR, "crash", "eax %08X ebx %08X ecx %08X edx %08X esi %08X edi %08X ebp %08X esp %08X",
            static_cast<unsigned>(c->Eax), static_cast<unsigned>(c->Ebx), static_cast<unsigned>(c->Ecx),
            static_cast<unsigned>(c->Edx), static_cast<unsigned>(c->Esi), static_cast<unsigned>(c->Edi),
            static_cast<unsigned>(c->Ebp), static_cast<unsigned>(c->Esp));
        // (an allocation that failed for want of address space shows here as little free and a small largest piece)
        AddressSpace as = AddressSpaceNow();
        Log(MML_LOG_ERROR, "crash", "address space: %u MB free of %u (the largest piece %u MB)%s", as.freeMb, as.totalMb, as.largestMb,
            as.largeAddressAware ? "" : ", Game.exe not Large Address Aware");
        uintptr_t pcs[32];
        int npc = StackOf(c, pcs, 32);
        for (int i = 0; i < npc; i++) Log(MML_LOG_ERROR, "crash", "  %s %s", i ? "from" : "at  ", Frame(pcs[i]).c_str());
        std::string dir = MmlDir() + "crash\\";
        MakeDir(dir);
        SYSTEMTIME t;
        GetLocalTime(&t);
        char file[1024];   // (UTF-8: a folder of MAX_PATH characters takes up to three bytes each)
        snprintf(file, sizeof file, "%sGame_%u_%04d%02d%02d_%02d%02d%02d.dmp", dir.c_str(), static_cast<unsigned>(GetCurrentProcessId()),
                 t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute, t.wSecond);
        HMODULE dbghelp = LoadLibraryA("dbghelp.dll");
        auto write = dbghelp ? reinterpret_cast<MiniDumpWriteDumpFn>(GetProcAddress(dbghelp, "MiniDumpWriteDump")) : nullptr;
        HANDLE f = PathCreateFile(file, GENERIC_WRITE, 0, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL);
        if (write && f != INVALID_HANDLE_VALUE) {
            struct { DWORD tid; EXCEPTION_POINTERS *ep; BOOL client; } info = {GetCurrentThreadId(), ep, FALSE};
            // WithDataSegs 0x1 | WithHandleData 0x4 | WithUnloadedModules 0x20 | WithIndirectlyReferencedMemory 0x40 |
            // WithProcessThreadData 0x100 | WithPrivateReadWriteMemory 0x200 (the heaps) | WithFullMemoryInfo 0x800
            // (every region's state) | WithThreadInfo 0x1000
            const int type = 0x1 | 0x4 | 0x20 | 0x40 | 0x100 | 0x200 | 0x800 | 0x1000;
            BOOL ok = write(GetCurrentProcess(), GetCurrentProcessId(), f, type, &info, nullptr, nullptr);
            DWORD why = ok ? 0 : GetLastError();
            bool lean = false;
            if (!ok) {
                // a region that cannot be read whole (0x8007012B, a partial copy: seen in the stand-in game) fails the
                // whole dump: again without the heaps and what they point to, the stacks, modules and regions kept
                SetFilePointer(f, 0, nullptr, FILE_BEGIN);
                SetEndOfFile(f);
                ok = write(GetCurrentProcess(), GetCurrentProcessId(), f, 0x1 | 0x4 | 0x20 | 0x100 | 0x800 | 0x1000, &info, nullptr, nullptr);
                lean = ok != FALSE;
            }
            if (ok) Log(MML_LOG_ERROR, "crash", "dump %s written%s", file, lean ? " without the heaps (the full dump failed)" : "");
            else Log(MML_LOG_ERROR, "crash", "dump %s failed (error 0x%08X)", file, static_cast<unsigned>(why));
        } else {
            Log(MML_LOG_ERROR, "crash", "no dump (MiniDumpWriteDump %s, file %s)", write ? "found" : "missing",
                f != INVALID_HANDLE_VALUE ? "open" : "not open");
        }
        if (f != INVALID_HANDLE_VALUE) CloseHandle(f);
        // the report beside the dump (kept to the end of the process: the thread may outlive the wait)
        Report *rep = new Report;
        rep->file = std::string(file, strlen(file) - 4) + ".json";
        char code[16];
        snprintf(code, sizeof code, "%08X", static_cast<unsigned>(er->ExceptionCode));
        rep->head = {{"exception", code}, {"at", Frame(reinterpret_cast<uintptr_t>(er->ExceptionAddress))},
                     {"thread", GetCurrentThreadId()}, {"main_thread", GetCurrentThreadId() == g_mainTid},
                     {"address_space", {{"total_mb", as.totalMb}, {"free_mb", as.freeMb}, {"largest_mb", as.largestMb},
                                        {"large_address_aware", as.largeAddressAware}}}};
        if (av) rep->head["access"] = {{"kind", er->ExceptionInformation[0] == 1 ? "write" : er->ExceptionInformation[0] == 8 ? "execute" : "read"},
                                       {"address", DescribeAddress(er->ExceptionInformation[1])}};
        json stack = json::array();
        for (int i = 0; i < npc; i++) stack.push_back(Frame(pcs[i]));
        rep->head["stack"] = stack;
        HANDLE th = CreateThread(nullptr, 0, WriteReport, rep, 0, nullptr);
        if (th) {
            DWORD w = WaitForSingleObject(th, 4000);
            Log(MML_LOG_ERROR, "crash", "report %s %s", rep->file.c_str(), w == WAIT_OBJECT_0 ? "written" : "not finished (a lock held by the crashing thread?)");
            CloseHandle(th);
        }
    }
    LONG r = g_prevFilter ? g_prevFilter(ep) : EXCEPTION_CONTINUE_SEARCH;
    if (slot >= 0) InterlockedExchange(&g_inside[slot], 0);
    return r;
}

// Ours kept first. The game's runtime, a wrapper or an overlay may set a filter of its own at any time; ours must run
// first and chain to it. SetUnhandledExceptionFilter is hooked (KernelBase's: kernel32's jumps there): a filter set by
// another goes after ours, and its caller is given what came before it (what it would chain to); ours stays set.
// The hook runs on whichever thread calls it: no TLS, no log (the frame logs what it counted). Without the hook (it
// could not be made), ours is set again every frame, as it once always was: KernelBase's SetUnhandledExceptionFilter
// does far more than swap a pointer on Windows 10, 1.2 to 2.2 % of a frame in the sampler (crash.filter
// {every_frame: true} brings that back, for a comparison).
using SetFilterFn = LPTOP_LEVEL_EXCEPTION_FILTER(WINAPI *)(LPTOP_LEVEL_EXCEPTION_FILTER);
SetFilterFn oSetFilter;
volatile LONG g_othersSet, g_othersLogged;
bool g_filterHooked, g_everyFrame;
std::string g_filterWhy;

LPTOP_LEVEL_EXCEPTION_FILTER WINAPI hkSetFilter(LPTOP_LEVEL_EXCEPTION_FILTER f) {
    if (f == CrashFilter) return oSetFilter(f);
    auto before = static_cast<LPTOP_LEVEL_EXCEPTION_FILTER>(
        InterlockedExchangePointer(reinterpret_cast<PVOID volatile *>(&g_prevFilter), reinterpret_cast<PVOID>(f)));
    InterlockedIncrement(&g_othersSet);
    return before;
}
} // namespace

void EnsureCrashFilter() {
    LPTOP_LEVEL_EXCEPTION_FILTER prev = SetUnhandledExceptionFilter(CrashFilter);
    if (prev == CrashFilter) return;
    if (prev) HLOG("crash filter: another one (%p) was installed; ours runs first and chains to it", reinterpret_cast<void *>(prev));
    g_prevFilter = prev;
}

void CrashFilterStart() {
    EnsureCrashFilter();
    HMODULE kb = GetModuleHandleA("KernelBase.dll");
    FARPROC at = kb ? GetProcAddress(kb, "SetUnhandledExceptionFilter") : nullptr;
    if (!at) at = GetProcAddress(GetModuleHandleA("kernel32.dll"), "SetUnhandledExceptionFilter");
    std::string err = at ? "" : "SetUnhandledExceptionFilter not found";
    if (at && HookAdd(nullptr, reinterpret_cast<uintptr_t>(at), reinterpret_cast<void *>(&hkSetFilter),
                      reinterpret_cast<void **>(&oSetFilter), &err)) {
        g_filterHooked = true;
        HLOG("crash filter: SetUnhandledExceptionFilter hooked at %s (another's filter goes after ours)",
             DescribeAddress(reinterpret_cast<uintptr_t>(at)).c_str());
    } else {
        g_everyFrame = true;
        g_filterWhy = err;
        HWARN("crash filter: SetUnhandledExceptionFilter not hooked (%s): ours set again every frame", err.c_str());
    }
}

void CrashFrame() {
    if (g_everyFrame) EnsureCrashFilter();
    const LONG n = g_othersSet;
    if (n != g_othersLogged) {
        g_othersLogged = n;
        HLOG("crash filter: another one was set (%s); ours stays first and chains to it",
             DescribeAddress(reinterpret_cast<uintptr_t>(g_prevFilter)).c_str());
    }
}

bool CrashMethod(const std::string &method, const std::string &paramsJson, std::string *reply, bool *ok) {
    if (method != "crash.filter") return false;
    json p = json::parse(paramsJson.empty() ? "null" : paramsJson, nullptr, false);
    if (p.is_object() && p.contains("every_frame")) {
        bool every = p["every_frame"].is_boolean() && p["every_frame"].get<bool>();
        if (!every && !g_filterHooked) {
            *ok = false;
            *reply = "crash.filter: SetUnhandledExceptionFilter is not hooked (" + g_filterWhy + "): ours is set every frame";
            return true;
        }
        g_everyFrame = every;
    }
    *ok = true;
    json r = {{"hooked", g_filterHooked}, {"every_frame", g_everyFrame}, {"others_set", static_cast<long>(g_othersSet)},
              {"chained_to", g_prevFilter ? json(DescribeAddress(reinterpret_cast<uintptr_t>(g_prevFilter))) : json(nullptr)}};
    if (!g_filterWhy.empty()) r["why"] = g_filterWhy;
    *reply = r.dump(-1, ' ', false, json::error_handler_t::replace);
    return true;
}

namespace {
uint32_t Read32(uintptr_t a) {
    uint32_t v = 0;
    return MemRead(a, &v, 4) ? v : 0;
}
// Where a direct call goes (E8 rel32, FF 15 [abs32] read): its target and the jumps it starts with followed (a
// hooked function's jmp to its detour, an import's thunk), up to four places; 0 places: an indirect call.
int CallTargets(uintptr_t ret, uintptr_t *t) {
    const uint8_t *p = reinterpret_cast<const uint8_t *>(ret);
    uintptr_t g = 0;
    if (p[-5] == 0xE8) g = ret + static_cast<int32_t>(Read32(ret - 4));
    else if (p[-6] == 0xFF && p[-5] == 0x15) g = Read32(Read32(ret - 4));
    int n = 0;
    while (g && IsCode(g) && n < 4) {
        t[n++] = g;
        uint8_t b0 = 0, b1 = 0;
        if (!MemRead(g, &b0, 1)) break;
        MemRead(g + 1, &b1, 1);
        if (b0 == 0xE9) g = g + 5 + static_cast<int32_t>(Read32(g + 1));
        else if (b0 == 0xEB) g = g + 2 + static_cast<int8_t>(b1);
        else if (b0 == 0xFF && b1 == 0x25) g = Read32(Read32(g + 2));
        else break;
    }
    return n;
}
} // namespace

// The scan keeps a code address with a call before it only when the call can lead to the frames above it: a direct
// call's target (its jumps followed) must hold one of the last frames kept (within 32 KiB of its start: the game's
// functions are shorter); an indirect call cannot be checked and is kept. What a finished call left on the stack is
// dropped so, mostly.
int StackOf(const CONTEXT *c, uintptr_t *out, int cap) {
    int n = 0;
    if (cap > 0) out[n++] = c->Eip;
    const uintptr_t *sp = reinterpret_cast<const uintptr_t *>(c->Esp);
    const uintptr_t *top = reinterpret_cast<const uintptr_t *>(reinterpret_cast<NT_TIB *>(NtCurrentTeb())->StackBase);
    if (sp >= top || top - sp > 0x40000) return n;   // not this thread's stack
    for (; sp < top && n < cap; sp++) {
        uintptr_t v = *sp;
        if (!AfterCall(v) || v == out[n - 1]) continue;
        uintptr_t t[4];
        int nt = CallTargets(v, t);
        bool leads = nt == 0;
        for (int k = 0; k < nt && !leads; k++)
            for (int f = n - 1; f >= 0 && f >= n - 4 && !leads; f--) leads = out[f] >= t[k] && out[f] < t[k] + 0x8000;
        if (leads) out[n++] = v;
    }
    return n;
}

void SetMainThread(DWORD tid) { g_mainTid = tid; }
DWORD MainThread() { return g_mainTid; }

// Samples the main thread a few times (suspend, read its registers, resume); the last sample's raw stack values that
// point into loaded code follow. Nothing that could need a lock the suspended thread may hold (the log, the loader
// lock) runs while it is suspended; addresses are turned into module+offset afterwards.
std::string Where() {
    HANDLE t = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_QUERY_INFORMATION, FALSE, g_mainTid);
    if (!t) return "where: cannot open the main thread";
    std::string out;
    std::vector<uintptr_t> stack;
    for (int sample = 0; sample < 5; sample++) {
        if (sample) Sleep(100);
        if (SuspendThread(t) == static_cast<DWORD>(-1)) { CloseHandle(t); return "where: SuspendThread failed"; }
        CONTEXT c{};
        c.ContextFlags = CONTEXT_FULL;
        BOOL ok = GetThreadContext(t, &c);
        if (ok && sample == 4) {
            uintptr_t sp = c.Esp;
            MEMORY_BASIC_INFORMATION mi;
            if (VirtualQuery(reinterpret_cast<const void *>(sp), &mi, sizeof mi) && mi.State == MEM_COMMIT) {
                uintptr_t end = reinterpret_cast<uintptr_t>(mi.BaseAddress) + mi.RegionSize;
                for (; sp + 4 <= end && stack.size() < 4096; sp += 4) stack.push_back(*reinterpret_cast<const uintptr_t *>(sp));
            }
        }
        ResumeThread(t);
        if (!ok) { CloseHandle(t); return "where: GetThreadContext failed"; }
        char name[64] = "?";
        uintptr_t rva = 0;
        ModuleOf(reinterpret_cast<const void *>(c.Eip), name, sizeof name, &rva);
        char line[160];
        snprintf(line, sizeof line, "sample %d: eip %s+0x%X esp %08X\n", sample, name, static_cast<unsigned>(rva), static_cast<unsigned>(c.Esp));
        out += line;
    }
    CloseHandle(t);
    int found = 0;
    for (size_t i = 0; i < stack.size() && found < 120; i++) {
        char name[64];
        uintptr_t rva;
        if (!IsCode(stack[i]) || !ModuleOf(reinterpret_cast<const void *>(stack[i]), name, sizeof name, &rva)) continue;
        bool own = !_stricmp(name, "Game.exe") || !_stricmp(name, "LS3DF.dll") || !_stricmp(name, "MafiaModLoader.asi") ||
                   !_strnicmp(name, "mod_", 4);
        if (found >= 24 && !own) continue;
        char line[160];
        snprintf(line, sizeof line, "  [esp+%04X] %s+0x%X\n", static_cast<unsigned>(i * 4), name, static_cast<unsigned>(rva));
        out += line;
        found++;
    }
    return out;
}

// The process ending without a fault (ExitProcess: DllMain's detach runs on the thread that called it, under it on
// the stack): the calls that led there, found by a scan of this thread's stack as a crash's report finds them. A
// player's game once quit by itself in the main menu with no dump and no word: this line names who ended it.
std::string ExitStack() {
    uintptr_t sp = reinterpret_cast<uintptr_t>(_AddressOfReturnAddress());
    uintptr_t top = __readfsdword(4);   // NT_TIB's StackBase
    std::string out;
    int found = 0;
    for (; sp + 4 <= top && found < 32; sp += 4) {
        uintptr_t v = *reinterpret_cast<const uintptr_t *>(sp);
        if (!AfterCall(v)) continue;
        out += (found ? ", " : "") + Frame(v);
        found++;
    }
    return out.empty() ? "(no calls found on the stack)" : out;
}

} // namespace host
