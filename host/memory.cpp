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
        std::string path = ModulePath(m);
        const char *name = path.c_str() + path.find_last_of('\\') + 1;
        snprintf(out, sizeof out, "%s+0x%X", name, static_cast<unsigned>(addr - reinterpret_cast<uintptr_t>(m)));
    } else {
        snprintf(out, sizeof out, "0x%08X", static_cast<unsigned>(addr));
    }
    return out;
}

// One function of a module's imports pointed at another (its import table's slot), the old one in *old.
bool PatchImport(HMODULE module, const char *dll, const char *fn, void *replacement, void **old) {
    auto base = reinterpret_cast<uint8_t *>(module);
    auto dos = reinterpret_cast<IMAGE_DOS_HEADER *>(base);
    auto nt = reinterpret_cast<IMAGE_NT_HEADERS *>(base + dos->e_lfanew);
    const IMAGE_DATA_DIRECTORY &dir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    if (!dir.VirtualAddress) return false;
    for (auto d = reinterpret_cast<IMAGE_IMPORT_DESCRIPTOR *>(base + dir.VirtualAddress); d->Name; ++d) {
        if (_stricmp(reinterpret_cast<const char *>(base + d->Name), dll) != 0 || !d->OriginalFirstThunk) continue;
        auto names = reinterpret_cast<IMAGE_THUNK_DATA *>(base + d->OriginalFirstThunk);
        auto slots = reinterpret_cast<IMAGE_THUNK_DATA *>(base + d->FirstThunk);
        for (; names->u1.AddressOfData; ++names, ++slots) {
            if (IMAGE_SNAP_BY_ORDINAL(names->u1.Ordinal)) continue;
            auto ibn = reinterpret_cast<IMAGE_IMPORT_BY_NAME *>(base + names->u1.AddressOfData);
            if (strcmp(reinterpret_cast<const char *>(ibn->Name), fn) != 0) continue;
            *old = reinterpret_cast<void *>(slots->u1.Function);
            uintptr_t v = reinterpret_cast<uintptr_t>(replacement);
            return MemWrite(reinterpret_cast<uintptr_t>(&slots->u1.Function), &v, sizeof v);
        }
    }
    return false;
}

} // namespace host
