// Crash reports and the main thread's whereabouts (see host.h).
//
// An exception nobody handles (the game would die) is logged (module and offset, the access, the registers, the call
// stack: the faulting thread's calls, stack.cpp's walk as the sampler's) and written as a minidump
// to <game>\mml\crash\Game_<pid>_<time>.dmp with every thread's stack, the modules' globals, every region's state and
// the memory the crash points into: around each register of the faulting thread and around the pointers found there
// (the objects a crash is in and what they hold: a dump of a few MB a player can send, where the heaps whole were 1 to
// 2.5 GB; MML_FULL_DUMP=1 in the game's environment writes them whole again); beside it Game_<pid>_<time>.json, what
// dies with the process: the stack with the mods
// named and its calls with their targets (`calls`: MCP's crash_triage keeps the checked ones and names them), the
// log's last lines, the taps' last calls, the last events, the mods and the mission (mcp: crash_triage,
// crash_list, crash_inspect). Exceptions caught by __try never get here (a mod's fault: mods.cpp, with its stack too).
// The game or a wrapper may install a filter later: SetUnhandledExceptionFilter is hooked, theirs goes after ours
// (CrashFilterStart; without the hook, ours is set again every frame).
#include "host.h"
#include <dbghelp.h>   // (the dump's callback types: dbghelp.dll itself is loaded when a dump is written)
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
    GatherFinal();   // (the other parts' last lines into mml.log: gather.cpp)
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

// ---- the memory a crash points into: windows around the faulting thread's registers (level 1), around the pointers
// in them (level 2: an object's members, an array) and, smaller, around the pointers in those (level 3: an array's
// elements), each clamped to its committed, readable region; static (the filter allocates nothing)
struct Range { uintptr_t base; uint32_t size; };
Range g_ranges[4096];
int g_rangeN, g_rangeAt;
uint32_t g_rangeBytes;
constexpr uint32_t kRangeCap = 16u << 20;   // (16 MB the most)

bool Readable(uintptr_t a, MEMORY_BASIC_INFORMATION *mi) {
    if (a < 0x10000 || !VirtualQuery(reinterpret_cast<const void *>(a), mi, sizeof *mi) || mi->State != MEM_COMMIT) return false;
    const DWORD p = mi->Protect & 0xFF;
    return !(mi->Protect & PAGE_GUARD) && p != PAGE_NOACCESS && p != PAGE_EXECUTE;
}

// A window [a - before, a + after) inside a's region; a module's code is left out (the dump has the modules by name)
bool AddWindow(uintptr_t a, uint32_t before, uint32_t after) {
    MEMORY_BASIC_INFORMATION mi;
    if (g_rangeN >= static_cast<int>(sizeof g_ranges / sizeof *g_ranges) || g_rangeBytes >= kRangeCap || !Readable(a, &mi)) return false;
    if (mi.Type == MEM_IMAGE && (mi.Protect & (PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY))) return false;
    const uintptr_t lo = reinterpret_cast<uintptr_t>(mi.BaseAddress), hi = lo + mi.RegionSize;
    const uintptr_t b = a - before < lo || a < before ? lo : a - before, e = a + after > hi ? hi : a + after;
    for (int i = 0; i < g_rangeN; i++)   // (inside one already kept)
        if (b >= g_ranges[i].base && e <= g_ranges[i].base + g_ranges[i].size) return true;
    g_ranges[g_rangeN++] = {b, static_cast<uint32_t>(e - b)};
    g_rangeBytes += static_cast<uint32_t>(e - b);
    return true;
}

void CollectRanges(const CONTEXT *c) {
    g_rangeN = g_rangeAt = 0;
    g_rangeBytes = 0;
    const uintptr_t regs[] = {c->Eax, c->Ebx, c->Ecx, c->Edx, c->Esi, c->Edi, c->Ebp};
    for (uintptr_t r : regs) AddWindow(r, 0x400, 0x1000);
    // the pointers the objects hold: one level more (the registers' objects first), then one more, smaller
    struct Level { uint32_t before, after; } levels[] = {{0x40, 0x400}, {0x10, 0x100}};
    int from = 0;
    for (const Level &l : levels) {
        const int to = g_rangeN;
        for (int i = from; i < to; i++) {
            const Range r = g_ranges[i];
            __try {
                for (uintptr_t a = (r.base + 3) & ~uintptr_t(3); a + 4 <= r.base + r.size; a += 4) {
                    const uintptr_t p = *reinterpret_cast<const uint32_t *>(a);
                    if (p >= 0x10000 && (p & 3) == 0) AddWindow(p, l.before, l.after);
                }
            } __except (EXCEPTION_EXECUTE_HANDLER) {}
        }
        from = to;
    }
}

// The modules whose globals a dump keeps: the game's, the loader's, the mods' (their live copies under mml\); not a
// driver's or a wrapper's (an NVIDIA driver's are 25 MB of the dump)
bool KeepsGlobals(const wchar_t *path) {
    if (!path) return false;
    const wchar_t *name = wcsrchr(path, L'\\');
    name = name ? name + 1 : path;
    for (const wchar_t *keep : {L"game.exe", L"ls3df.dll", L"rw_data.dll", L"ijoy.dll", L"mafiamodloader.asi"})
        if (!_wcsicmp(name, keep)) return true;
    for (const wchar_t *p = path; *p; p++)
        if (!_wcsnicmp(p, L"\\mml\\", 5)) return true;
    return !_wcsicmp(name, L"mml_harness.exe");   // (the offline tests' stand-in game)
}

// MiniDumpWriteDump's callback: every thread and module (a module's globals only for KeepsGlobals), and at
// MemoryCallback one range a call until there are none
BOOL CALLBACK DumpCallback(void *, const MINIDUMP_CALLBACK_INPUT *in, MINIDUMP_CALLBACK_OUTPUT *out) {
    if (!in || !out) return TRUE;
    if (in->CallbackType == ModuleCallback) {
        if (!KeepsGlobals(in->Module.FullPath)) out->ModuleWriteFlags &= ~ModuleWriteDataSeg;
        return TRUE;
    }
    if (in->CallbackType != MemoryCallback) return TRUE;
    if (g_rangeAt >= g_rangeN) return FALSE;
    out->MemoryBase = g_ranges[g_rangeAt].base;
    out->MemorySize = g_ranges[g_rangeAt].size;
    g_rangeAt++;
    return TRUE;
}

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
        StackCall calls[64];
        const int ncalls = StackCallsOf(c, true, calls, 64);
        uintptr_t pcs[32];
        const int npc = LikelyFrames(c->Eip, calls, ncalls, pcs, 32);
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
            // WithProcessThreadData 0x100 | WithFullMemoryInfo 0x800 (every region's state) | WithThreadInfo 0x1000,
            // and the ranges the callback gives; WithPrivateReadWriteMemory 0x200 (the heaps whole) on MML_FULL_DUMP
            char fullEnv[8] = "";
            const bool full = GetEnvironmentVariableA("MML_FULL_DUMP", fullEnv, sizeof fullEnv) && fullEnv[0] == '1';
            const int type = 0x1 | 0x4 | 0x20 | 0x40 | 0x100 | 0x800 | 0x1000 | (full ? 0x200 : 0);
            CollectRanges(c);
            MINIDUMP_CALLBACK_INFORMATION cb = {reinterpret_cast<MINIDUMP_CALLBACK_ROUTINE>(&DumpCallback), nullptr};
            BOOL ok = write(GetCurrentProcess(), GetCurrentProcessId(), f, type, &info, nullptr, full ? nullptr : &cb);
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
            if (ok)
                Log(MML_LOG_ERROR, "crash", "dump %s written (%s)", file,
                    lean ? "the stacks and the regions: the dump with more failed"
                    : full ? "the heaps whole: MML_FULL_DUMP" : "the stacks, the globals and the memory around the registers");
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
        // every call found, with its call's target ("frame": walked by the frame pointers, certain; null: an indirect
        // call): MCP keeps those whose call leads to the frame below and names them
        json all = json::array();
        for (int i = 0; i < ncalls; i++) {
            const uint32_t t = calls[i].target;
            all.push_back({DescribeAddress(calls[i].ret), t == kCallWalked ? json("frame") : t ? json(DescribeAddress(t)) : json(nullptr)});
        }
        rep->head["calls"] = all;
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
// the stack): the calls that led there, read from this thread's stack as a crash's report reads them (walked by the
// frame pointers from here through the system's exit to the game's code that called it). A player's game once quit
// by itself in the main menu with no dump and no word: this line names who ended it.
std::string ExitStack() {
    CONTEXT c{};
    RtlCaptureContext(&c);
    uintptr_t pcs[32];
    const int n = StackOf(&c, pcs, 32);
    std::string out;
    for (int i = 1; i < n; i++) out += (i > 1 ? ", " : "") + Frame(pcs[i]);   // (the first: this function)
    return out.empty() ? "(no calls found on the stack)" : out;
}

} // namespace host
