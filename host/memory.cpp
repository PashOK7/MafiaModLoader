// Memory access for mods and the bridge (see host.h).
#include "host.h"
#include <cctype>
#include <cstring>

namespace host {
namespace {
bool CopySafe(void *dst, const void *src, size_t n) {
    __try { memcpy(dst, src, n); return true; } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
constexpr DWORD kWritable = PAGE_READWRITE | PAGE_WRITECOPY | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY;
constexpr DWORD kExec = PAGE_EXECUTE | PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY;
} // namespace

bool MemRead(uintptr_t addr, void *dst, size_t n) {
    if (!addr || !dst) return false;
    return CopySafe(dst, reinterpret_cast<const void *>(addr), n);
}

bool MemWrite(uintptr_t addr, const void *src, size_t n) {
    if (!addr || !src) return false;
    // page by page: a write may cross from data into code
    uintptr_t at = addr, end = addr + n;
    const uint8_t *from = static_cast<const uint8_t *>(src);
    bool code = false;
    while (at < end) {
        MEMORY_BASIC_INFORMATION mi;
        if (!VirtualQuery(reinterpret_cast<const void *>(at), &mi, sizeof mi) || mi.State != MEM_COMMIT) return false;
        uintptr_t regionEnd = reinterpret_cast<uintptr_t>(mi.BaseAddress) + mi.RegionSize;
        size_t len = static_cast<size_t>((end < regionEnd ? end : regionEnd) - at);
        DWORD old = 0;
        bool changed = false;
        if (!(mi.Protect & kWritable) || (mi.Protect & PAGE_GUARD)) {
            if (!VirtualProtect(reinterpret_cast<void *>(at), len, PAGE_EXECUTE_READWRITE, &old)) return false;
            changed = true;
        }
        code = code || (mi.Protect & kExec);
        bool ok = CopySafe(reinterpret_cast<void *>(at), from + (at - addr), len);
        if (changed) VirtualProtect(reinterpret_cast<void *>(at), len, old, &old);
        if (!ok) return false;
        at += len;
    }
    if (code) FlushInstructionCache(GetCurrentProcess(), reinterpret_cast<const void *>(addr), n);
    return true;
}

uintptr_t ModuleBase(const char *name) {
    return reinterpret_cast<uintptr_t>(GetModuleHandleA(name));
}

uintptr_t ParseAddress(const std::string &text) {
    std::string s;
    for (char c : text) if (!isspace(static_cast<unsigned char>(c))) s += c;
    if (s.empty()) return 0;
    uintptr_t base = 0;
    size_t plus = s.find('+');
    std::string off = s;
    if (plus != std::string::npos) {
        std::string mod = s.substr(0, plus);
        off = s.substr(plus + 1);
        if (!_stricmp(mod.c_str(), "game") || !_stricmp(mod.c_str(), "exe")) base = ModuleBase(nullptr);
        else if (!_stricmp(mod.c_str(), "ls3d") || !_stricmp(mod.c_str(), "ls3df")) base = ModuleBase("LS3DF.dll");
        else base = ModuleBase(mod.c_str());
        if (!base) return 0;
    }
    char *end = nullptr;
    unsigned long long v = strtoull(off.c_str(), &end, 16);   // hex with or without 0x
    if (!end || *end) return 0;
    return base + static_cast<uintptr_t>(v);
}

std::string DescribeAddress(uintptr_t addr) {
    char out[160];
    HMODULE m = nullptr;
    if (addr && GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                                   reinterpret_cast<const char *>(addr), &m) && m) {
        char path[MAX_PATH] = "";
        GetModuleFileNameA(m, path, MAX_PATH);
        const char *name = strrchr(path, '\\') ? strrchr(path, '\\') + 1 : path;
        snprintf(out, sizeof out, "%s+0x%X", name, static_cast<unsigned>(addr - reinterpret_cast<uintptr_t>(m)));
    } else {
        snprintf(out, sizeof out, "0x%08X", static_cast<unsigned>(addr));
    }
    return out;
}

} // namespace host
