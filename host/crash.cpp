// Crash reports and the main thread's whereabouts (see host.h).
//
// An exception nobody handles (the game would die) is logged (module and offset, the access, the registers, the code
// addresses on the faulting thread's stack: return addresses and leftovers, a hint rather than an exact call stack)
// and written as a minidump to <game>\mml\crash\Game_<pid>_<time>.dmp with the heaps and every region's state, so an
// object a crash points into can be looked at afterwards (mcp: crash_list, crash_inspect). Exceptions caught by __try
// never get here. The game or a wrapper may install a filter later: EnsureCrashFilter, every frame, puts ours back in
// front and chains to theirs.
#include "host.h"
#include <cstdio>
#include <cstring>

namespace host {
namespace {
LPTOP_LEVEL_EXCEPTION_FILTER g_prevFilter;
volatile LONG g_crashing;
DWORD g_mainTid;

bool ModuleOf(const void *addr, char *name, int cap, uintptr_t *rva) {
    HMODULE m = nullptr;
    if (!GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                            static_cast<const char *>(addr), &m) || !m)
        return false;
    char path[MAX_PATH];
    if (!GetModuleFileNameA(m, path, MAX_PATH)) return false;
    const char *base = strrchr(path, '\\');
    snprintf(name, cap, "%s", base ? base + 1 : path);
    *rva = reinterpret_cast<uintptr_t>(addr) - reinterpret_cast<uintptr_t>(m);
    return true;
}

bool IsCode(uintptr_t addr) {
    MEMORY_BASIC_INFORMATION mi;
    if (!VirtualQuery(reinterpret_cast<const void *>(addr), &mi, sizeof mi) || mi.State != MEM_COMMIT || mi.Type != MEM_IMAGE)
        return false;
    return (mi.Protect & (PAGE_EXECUTE | PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY)) != 0;
}

void LogStackScan(const CONTEXT *c) {
    const uintptr_t *sp = reinterpret_cast<const uintptr_t *>(c->Esp);
    const uintptr_t *top = reinterpret_cast<const uintptr_t *>(reinterpret_cast<NT_TIB *>(NtCurrentTeb())->StackBase);
    for (int found = 0; sp < top && found < 48; sp++) {
        char name[64];
        uintptr_t rva;
        if (!IsCode(*sp) || !ModuleOf(reinterpret_cast<const void *>(*sp), name, sizeof name, &rva)) continue;
        Log(MML_LOG_ERROR, "crash", "  [esp+%04X] %s+0x%X", static_cast<unsigned>(reinterpret_cast<uintptr_t>(sp) - c->Esp),
            name, static_cast<unsigned>(rva));
        found++;
    }
}

typedef BOOL(WINAPI *MiniDumpWriteDumpFn)(HANDLE, DWORD, HANDLE, int, void *, void *, void *);

LONG WINAPI CrashFilter(EXCEPTION_POINTERS *ep) {
    // a filter we chain to may chain back to us (it saw ours as its previous one): stop the loop there
    static thread_local bool inside;
    if (inside) return EXCEPTION_CONTINUE_SEARCH;
    inside = true;
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
        LogStackScan(c);
        std::string dir = MmlDir() + "crash\\";
        CreateDirectoryA(dir.c_str(), nullptr);
        SYSTEMTIME t;
        GetLocalTime(&t);
        char file[MAX_PATH];
        snprintf(file, sizeof file, "%sGame_%u_%04d%02d%02d_%02d%02d%02d.dmp", dir.c_str(), static_cast<unsigned>(GetCurrentProcessId()),
                 t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute, t.wSecond);
        HMODULE dbghelp = LoadLibraryA("dbghelp.dll");
        auto write = dbghelp ? reinterpret_cast<MiniDumpWriteDumpFn>(GetProcAddress(dbghelp, "MiniDumpWriteDump")) : nullptr;
        HANDLE f = CreateFileA(file, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (write && f != INVALID_HANDLE_VALUE) {
            struct { DWORD tid; EXCEPTION_POINTERS *ep; BOOL client; } info = {GetCurrentThreadId(), ep, FALSE};
            // WithDataSegs 0x1 | WithHandleData 0x4 | WithUnloadedModules 0x20 | WithIndirectlyReferencedMemory 0x40 |
            // WithProcessThreadData 0x100 | WithPrivateReadWriteMemory 0x200 (the heaps) | WithFullMemoryInfo 0x800
            // (every region's state) | WithThreadInfo 0x1000
            const int type = 0x1 | 0x4 | 0x20 | 0x40 | 0x100 | 0x200 | 0x800 | 0x1000;
            BOOL ok = write(GetCurrentProcess(), GetCurrentProcessId(), f, type, &info, nullptr, nullptr);
            Log(MML_LOG_ERROR, "crash", "dump %s %s", file, ok ? "written" : "failed");
        } else {
            Log(MML_LOG_ERROR, "crash", "no dump (MiniDumpWriteDump %s, file %s)", write ? "found" : "missing",
                f != INVALID_HANDLE_VALUE ? "open" : "not open");
        }
        if (f != INVALID_HANDLE_VALUE) CloseHandle(f);
    }
    LONG r = g_prevFilter ? g_prevFilter(ep) : EXCEPTION_CONTINUE_SEARCH;
    inside = false;
    return r;
}
} // namespace

void EnsureCrashFilter() {
    LPTOP_LEVEL_EXCEPTION_FILTER prev = SetUnhandledExceptionFilter(CrashFilter);
    if (prev == CrashFilter) return;
    if (prev) HLOG("crash filter: another one (%p) was installed; ours runs first and chains to it", reinterpret_cast<void *>(prev));
    g_prevFilter = prev;
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

} // namespace host
