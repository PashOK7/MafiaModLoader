// Test mod: hooks the harness's harness_add, adds 10 to what the rest of the chain returns. Command `hook_a.call
// {"a": 2, "b": 3}` calls harness_add through every hook on it.
#include <windows.h>
#include <string>
#include "mml/mml.h"

namespace {
const MmlHost *H;
using AddFn = int (*)(int, int);
AddFn g_original;
AddFn g_target;

int Detour(int a, int b) { return g_original(a, b) + 10; }

int Call(const char *args, MmlReply *r, void *) {
    int a = 0, b = 0;
    sscanf_s(args, "{\"a\":%d,\"b\":%d}", &a, &b);
    std::string out = std::to_string(g_target(a, b));
    H->reply_append(r, out.c_str(), out.size());
    return 0;
}
// the service hook_b publishes (test.math v1), found by name
int UseMath(const char *, MmlReply *r, void *) {
    struct Math { uint32_t size; int (*mul)(int, int); };
    uint32_t version = 0;
    auto m = static_cast<const Math *>(H->service_find("test.math", 1, &version));
    if (!m) { H->reply_error(r, "no test.math service"); return 1; }
    std::string out = std::to_string(m->mul(6, 7));
    H->reply_append(r, out.c_str(), out.size());
    return 0;
}
} // namespace

MML_EXPORT int mml_mod_load(const MmlHost *host, MmlMod *mod) {
    H = host;
    g_target = reinterpret_cast<AddFn>(GetProcAddress(GetModuleHandleA(nullptr), "harness_add"));
    if (!g_target) return 1;
    if (!host->hook(mod, reinterpret_cast<uintptr_t>(g_target), reinterpret_cast<void *>(Detour), reinterpret_cast<void **>(&g_original)))
        return 2;
    host->command(mod, "call", "harness_add(a, b) through the hook chain", Call, nullptr);
    host->command(mod, "math", "6 * 7 through the test.math service", UseMath, nullptr);
    return 0;
}

MML_EXPORT void mml_mod_unload(MmlMod *) {}
