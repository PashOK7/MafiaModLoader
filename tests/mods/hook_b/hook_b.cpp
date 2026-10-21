// Test mod: hooks the harness's harness_add too (after hook_a: the outer link), doubles what the rest returns. It
// also faults on `hook_b.crash` (the loader must unload it and go on) and publishes a service, `test.math` v1.
#include <windows.h>
#include <cstdio>
#include "mml/mml.h"

namespace {
const MmlHost *H;
using AddFn = int (*)(int, int);
AddFn g_original;

int Detour(int a, int b) {
    if (a == -2) *static_cast<volatile int *>(nullptr) = b;   // (the harness's `crash_mod`: a crash in a mod's hook)
    return g_original(a, b) * 2;
}

int Crash(const char *, MmlReply *, void *) {
    volatile int *p = nullptr;
    return *p;   // an access violation inside a mod's command
}

int Call(const char *args, MmlReply *r, void *) {
    int a = 0, b = 0;
    sscanf_s(args, "{\"a\":%d,\"b\":%d}", &a, &b);
    auto target = reinterpret_cast<int (*)(int, int)>(GetProcAddress(GetModuleHandleA(nullptr), "harness_add"));
    char out[32];
    snprintf(out, sizeof out, "%d", target(a, b));
    H->reply_append(r, out, (size_t)-1);
    return 0;
}

struct Math { uint32_t size; int (*mul)(int, int); };
int Mul(int a, int b) { return a * b; }
const Math kMath = {sizeof(Math), Mul};
} // namespace

MML_EXPORT int mml_mod_load(const MmlHost *host, MmlMod *mod) {
    H = host;
    auto target = reinterpret_cast<uintptr_t>(GetProcAddress(GetModuleHandleA(nullptr), "harness_add"));
    if (!target || !host->hook(mod, target, reinterpret_cast<void *>(Detour), reinterpret_cast<void **>(&g_original))) return 1;
    host->command(mod, "crash", "fault on purpose (the loader unloads this mod)", Crash, nullptr);
    host->command(mod, "call", "harness_add(a, b) through the hook chain", Call, nullptr);
    host->service_publish(mod, "test.math", 1, &kMath);
    return 0;
}

MML_EXPORT void mml_mod_unload(MmlMod *) {}
