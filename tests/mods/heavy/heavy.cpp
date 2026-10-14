// Test mod: a heavy mod's lifecycle (host/lifecycle.cpp). It ships two libraries in its folder: heavylib.dll, which
// it imports, and heavyrt.dll, which it loads by its bare name in mml_mod_load (as PhysX or OpenVR loaders do). Its
// commands start a thread of its own (stopped in mml_mod_unload, or left running on purpose), leak address space and
// handles, and leave heavyrt.dll loaded, so the tests can see what the loader finds when it unloads.
#include <windows.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include "mml/mml.h"

extern "C" __declspec(dllimport) int heavylib_value(void);

namespace {
const MmlHost *H;
HMODULE g_rt;
bool g_leakRt;
HANDLE g_thread;
volatile LONG g_stop;
struct ThreadArgs { DWORD ms; };

int IntArg(const char *args, const char *key, int def) {
    char pat[64];
    snprintf(pat, sizeof pat, "\"%s\":", key);
    const char *p = args ? strstr(args, pat) : nullptr;
    if (!p) return def;
    p += strlen(pat);
    if (!strncmp(p, "true", 4)) return 1;
    if (!strncmp(p, "false", 5)) return 0;
    return atoi(p);
}

void PathOf(HMODULE m, char *out, size_t cap) {
    out[0] = 0;
    if (m) GetModuleFileNameA(m, out, static_cast<DWORD>(cap));
    for (char *c = out; *c; c++) if (*c == '\\') *c = '/';   // (no escaping needed in the reply)
}

int Status(const char *, MmlReply *r, void *) {
    char self[MAX_PATH], lib[MAX_PATH], rt[MAX_PATH], out[4 * MAX_PATH];
    HMODULE me = nullptr;
    GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                       reinterpret_cast<LPCSTR>(&Status), &me);
    PathOf(me, self, sizeof self);
    PathOf(GetModuleHandleA("heavylib.dll"), lib, sizeof lib);
    PathOf(g_rt, rt, sizeof rt);
    auto rtValue = g_rt ? reinterpret_cast<int (*)()>(GetProcAddress(g_rt, "heavyrt_value")) : nullptr;
    snprintf(out, sizeof out, "{\"self\":\"%s\",\"lib\":%d,\"lib_path\":\"%s\",\"rt\":%d,\"rt_path\":\"%s\"}", self,
             heavylib_value(), lib, rtValue ? rtValue() : -1, rt);
    H->reply_append(r, out, static_cast<size_t>(-1));
    return 0;
}

DWORD WINAPI Worker(void *p) {   // runs in this DLL's code until stopped or its time is up
    DWORD ms = static_cast<ThreadArgs *>(p)->ms;
    delete static_cast<ThreadArgs *>(p);
    DWORD start = GetTickCount();
    while (!g_stop && GetTickCount() - start < ms) Sleep(10);
    return 0;
}

int Thread(const char *args, MmlReply *r, void *) {
    DWORD ms = static_cast<DWORD>(IntArg(args, "ms", 60000));
    bool leave = IntArg(args, "leave", 0) != 0;
    HANDLE t = CreateThread(nullptr, 0, Worker, new ThreadArgs{ms}, 0, nullptr);
    if (!t) { H->reply_error(r, "CreateThread failed"); return 1; }
    if (leave) CloseHandle(t);   // not stopped at the unload: the loader must keep the DLL
    else g_thread = t;
    H->reply_append(r, "true", static_cast<size_t>(-1));
    return 0;
}

DWORD WINAPI PoolWork(void *p) {   // on a thread of the system's pool: begun elsewhere, in this DLL's code now
    Sleep(static_cast<DWORD>(reinterpret_cast<uintptr_t>(p)));
    return 0;
}

int Pool(const char *args, MmlReply *r, void *) {   // never waited for at the unload
    DWORD ms = static_cast<DWORD>(IntArg(args, "ms", 2500));
    if (!QueueUserWorkItem(PoolWork, reinterpret_cast<void *>(static_cast<uintptr_t>(ms)), WT_EXECUTEDEFAULT)) {
        H->reply_error(r, "QueueUserWorkItem failed");
        return 1;
    }
    H->reply_append(r, "true", static_cast<size_t>(-1));
    return 0;
}

int Leak(const char *args, MmlReply *r, void *) {
    int mb = IntArg(args, "mb", 0), handles = IntArg(args, "handles", 0);
    if (mb > 0 && !VirtualAlloc(nullptr, static_cast<SIZE_T>(mb) << 20, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE)) {
        H->reply_error(r, "VirtualAlloc failed");
        return 1;
    }
    for (int i = 0; i < handles; i++) CreateEventA(nullptr, FALSE, FALSE, nullptr);
    H->reply_append(r, "true", static_cast<size_t>(-1));
    return 0;
}

int LeakRt(const char *, MmlReply *r, void *) {
    g_leakRt = true;   // heavyrt.dll not freed at the unload
    H->reply_append(r, "true", static_cast<size_t>(-1));
    return 0;
}
} // namespace

MML_EXPORT int mml_mod_load(const MmlHost *host, MmlMod *mod) {
    H = host;
    g_rt = LoadLibraryA("heavyrt.dll");   // a bare name: found in the load's folder while mml_mod_load runs
    if (!g_rt) {
        host->log(mod, MML_LOG_ERROR, "heavyrt.dll not found (%lu)", GetLastError());
        return 1;
    }
    host->command(mod, "status", "the libraries' values and where each was loaded from", Status, nullptr);
    host->command(mod, "thread", "{ms, leave}: a thread in this DLL's code (leave: not stopped at the unload)", Thread, nullptr);
    host->command(mod, "pool", "{ms}: a work item of the system's thread pool sleeping in this DLL's code", Pool, nullptr);
    host->command(mod, "leak","{mb, handles}: address space and handles never given back", Leak, nullptr);
    host->command(mod, "leak_rt", "heavyrt.dll not freed at the unload", LeakRt, nullptr);
    return 0;
}

MML_EXPORT void mml_mod_unload(MmlMod *) {
    if (g_thread) {
        InterlockedExchange(&g_stop, 1);
        WaitForSingleObject(g_thread, 5000);
        CloseHandle(g_thread);
        g_thread = nullptr;
    }
    if (g_rt && !g_leakRt) FreeLibrary(g_rt);
    g_rt = nullptr;
}
