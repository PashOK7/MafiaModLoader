// ff.cpp: the host's side of the port (ff.h): patterns, patches, hooks, imports.
#include "ff.h"
#include <cstdarg>
#include <cstdio>
#include <map>

namespace ff {

const MmlHost *H;
MmlMod *M;
Settings S;
ScreenT Screen;
float fCreditsTextX, fMapPlayerWidth, fMenuOffset, fMovieAspect = 1.0f;
int nGameVersion;
int32_t *pResolutionWidth, *pResolutionHeight;

uintptr_t ExeBase() { return reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr)); }
uintptr_t Ls3dBase() { return H->module_base("LS3DF.dll"); }

void Log(MmlLogLevel level, const char *fmt, ...) {
    char line[1024];
    va_list a;
    va_start(a, fmt);
    vsnprintf(line, sizeof line, fmt, a);
    va_end(a);
    H->log(M, level, "%s", line);
}

// ---- patterns. A module is looked in as its file has it (its sections at their places in the image): the game's
// code as shipped, whatever another mod or the loader has hooked since (the loader's own hook on the HUD's
// DrawAll covers the first bytes of a pattern of the radar's). A file whose code is not what runs (a packed exe)
// finds nothing there, and the module's memory is looked in instead.
namespace {
struct Parsed {
    std::vector<uint8_t> bytes, mask;
    size_t first = 0;   // the first byte that is not ?
    // The byte looked for first (memchr): the rarest of the pattern's in the image, so that the common ones a
    // pattern of code begins with (8B, D8, E8...) do not stop the scan at every other instruction.
    size_t Anchor(const uint32_t *histogram) const {
        if (!histogram) return first;
        size_t best = first;
        for (size_t k = first; k < bytes.size(); k++)
            if (mask[k] && histogram[bytes[k]] < histogram[bytes[best]]) best = k;
        return best;
    }
};
Parsed Parse(const char *p) {
    Parsed r;
    for (const char *s = p; *s;) {
        while (*s == ' ') s++;
        if (!*s) break;
        if (*s == '?') {
            r.bytes.push_back(0);
            r.mask.push_back(0);
            s++;
            if (*s == '?') s++;
        } else {
            r.bytes.push_back(static_cast<uint8_t>(strtoul(std::string(s, 2).c_str(), nullptr, 16)));
            r.mask.push_back(1);
            s += 2;
        }
    }
    while (r.first < r.mask.size() && !r.mask[r.first]) r.first++;
    return r;
}

// Every match in [p, p + n): offsets. Under a guard (a module's memory may have pages that cannot be read).
size_t ScanRaw(const uint8_t *p, size_t n, const Parsed &pat, size_t anchor, size_t *out, size_t max) {
    size_t found = 0, len = pat.bytes.size();
    if (!len || pat.first >= len || n < len) return 0;
    __try {
        const uint8_t lead = pat.bytes[anchor];
        for (size_t i = 0; i + len <= n; i++) {
            const void *hit = memchr(p + i + anchor, lead, n - len - i + 1);
            if (!hit) break;
            i = static_cast<const uint8_t *>(hit) - p - anchor;
            size_t k = 0;
            for (; k < len; k++)
                if (pat.mask[k] && p[i + k] != pat.bytes[k]) break;
            if (k == len) {
                if (found < max) out[found] = i;
                found++;
            }
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {}
    return found < max ? found : max;
}

struct Image {
    std::vector<uint8_t> data;   // the file laid out at its RVAs (empty: not read)
    uint32_t histogram[256] = {};
    bool tried = false;
};
std::map<uintptr_t, Image> g_images;

const Image &FileImage(uintptr_t base) {
    Image &img = g_images[base];
    if (img.tried) return img;
    img.tried = true;
    wchar_t wpath[MAX_PATH];
    if (!GetModuleFileNameW(reinterpret_cast<HMODULE>(base), wpath, MAX_PATH)) return img;
    HANDLE f = CreateFileW(wpath, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, 0, nullptr);
    if (f == INVALID_HANDLE_VALUE) return img;
    LARGE_INTEGER size{};
    std::vector<uint8_t> d;
    if (GetFileSizeEx(f, &size) && size.QuadPart > 0x400 && size.QuadPart < (256 << 20)) {
        d.resize(static_cast<size_t>(size.QuadPart));
        DWORD got = 0;
        if (!ReadFile(f, d.data(), static_cast<DWORD>(d.size()), &got, nullptr) || got != d.size()) d.clear();
    }
    CloseHandle(f);
    if (d.size() < 0x400) return img;
    auto u16 = [&](size_t o) { return o + 2 <= d.size() ? *reinterpret_cast<const uint16_t *>(&d[o]) : 0; };
    auto u32 = [&](size_t o) { return o + 4 <= d.size() ? *reinterpret_cast<const uint32_t *>(&d[o]) : 0u; };
    size_t pe = u32(0x3C);
    if (u32(pe) != 0x4550) return img;
    unsigned nsec = u16(pe + 6), optsz = u16(pe + 20);
    size_t imageSize = u32(pe + 24 + 56), headers = u32(pe + 24 + 60);
    // the file must be the module that runs: the same image size as the one in memory
    auto *nt = reinterpret_cast<const IMAGE_NT_HEADERS32 *>(base + reinterpret_cast<const IMAGE_DOS_HEADER *>(base)->e_lfanew);
    if (imageSize != nt->OptionalHeader.SizeOfImage || imageSize > (256u << 20)) return img;
    img.data.assign(imageSize, 0);
    memcpy(img.data.data(), d.data(), headers < d.size() ? headers : d.size());
    size_t sec = pe + 24 + optsz;
    for (unsigned i = 0; i < nsec; i++) {
        size_t vsize = u32(sec + i * 40 + 8), va = u32(sec + i * 40 + 12), rsize = u32(sec + i * 40 + 16), raw = u32(sec + i * 40 + 20);
        size_t n = rsize < (vsize ? vsize : rsize) ? rsize : (vsize ? vsize : rsize);
        if (va >= imageSize || raw >= d.size()) continue;
        if (va + n > imageSize) n = imageSize - va;
        if (raw + n > d.size()) n = d.size() - raw;
        memcpy(&img.data[va], &d[raw], n);
    }
    for (uint8_t b : img.data) img.histogram[b]++;
    return img;
}

size_t ImageSize(uintptr_t base) {
    __try {
        auto *nt = reinterpret_cast<const IMAGE_NT_HEADERS32 *>(base + reinterpret_cast<const IMAGE_DOS_HEADER *>(base)->e_lfanew);
        return nt->OptionalHeader.SizeOfImage;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return 0; }
}
} // namespace

Pattern ScanRange(uintptr_t lo, uintptr_t hi, const char *pattern) {
    Pattern r;
    if (hi <= lo) return r;
    Parsed p = Parse(pattern);
    std::vector<size_t> at(4096);
    size_t n = ScanRaw(reinterpret_cast<const uint8_t *>(lo), hi - lo, p, p.first, at.data(), at.size());
    for (size_t i = 0; i < n; i++) r.m.push_back(lo + at[i]);
    return r;
}

// What a module's pattern found, kept for the next Apply (the module's code as shipped does not change)
std::map<std::pair<uintptr_t, std::string>, std::vector<uintptr_t>> g_found;

double g_scanMs, g_hookMs, g_patchMs;   // the time the last Apply spent looking and hooking

namespace {
struct Timer {
    double *sum;
    LARGE_INTEGER t0;
    explicit Timer(double *s) : sum(s) { QueryPerformanceCounter(&t0); }
    ~Timer() {
        LARGE_INTEGER t1, f;
        QueryPerformanceCounter(&t1);
        QueryPerformanceFrequency(&f);
        *sum += (t1.QuadPart - t0.QuadPart) * 1000.0 / f.QuadPart;
    }
};
} // namespace

Pattern ScanModule(uintptr_t base, const char *pattern) {
    Timer t(&g_scanMs);
    Pattern r;
    if (!base) return r;
    auto key = std::make_pair(base, std::string(pattern));
    auto it = g_found.find(key);
    if (it != g_found.end()) {
        r.m = it->second;
        return r;
    }
    Parsed p = Parse(pattern);
    std::vector<size_t> at(4096);
    const Image &img = FileImage(base);
    size_t n = 0;
    if (!img.data.empty()) n = ScanRaw(img.data.data(), img.data.size(), p, p.Anchor(img.histogram), at.data(), at.size());
    if (!n) n = ScanRaw(reinterpret_cast<const uint8_t *>(base), ImageSize(base), p, p.first, at.data(), at.size());
    for (size_t i = 0; i < n; i++) r.m.push_back(base + at[i]);
    g_found[key] = r.m;
    return r;
}

// ---- patches and hooks
namespace {
std::vector<uintptr_t> g_patches, g_hooks;

void MidThunk(MmlMidContext *c, void *user) { reinterpret_cast<MidFn>(user)(*c); }
} // namespace

bool Write(uintptr_t at, const void *bytes, size_t n) {
    Timer t(&g_patchMs);
    if (!H->code_patch(M, at, nullptr, bytes, n)) return false;
    g_patches.push_back(at);
    return true;
}

bool Mid(uintptr_t at, MidFn fn) {
    Timer t(&g_hookMs);
    if (!at || !H->hook_mid(M, at, MidThunk, reinterpret_cast<void *>(fn))) return false;
    g_hooks.push_back(at);
    return true;
}

bool InlineRaw(uintptr_t at, void *detour, void **original) {
    Timer t(&g_hookMs);
    if (!at || !H->hook(M, at, detour, original)) return false;
    g_hooks.push_back(at);
    return true;
}

bool ImportReplace(uintptr_t module, const char *dll, const char *name, void *fn, void **original) {
    if (!module) return false;
    __try {
        auto *dos = reinterpret_cast<const IMAGE_DOS_HEADER *>(module);
        auto *nt = reinterpret_cast<const IMAGE_NT_HEADERS32 *>(module + dos->e_lfanew);
        const IMAGE_DATA_DIRECTORY &dir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
        if (!dir.VirtualAddress) return false;
        for (auto *d = reinterpret_cast<const IMAGE_IMPORT_DESCRIPTOR *>(module + dir.VirtualAddress); d->Name; d++) {
            if (_stricmp(reinterpret_cast<const char *>(module + d->Name), dll)) continue;
            auto *names = reinterpret_cast<const IMAGE_THUNK_DATA32 *>(module + (d->OriginalFirstThunk ? d->OriginalFirstThunk : d->FirstThunk));
            auto *slots = reinterpret_cast<IMAGE_THUNK_DATA32 *>(module + d->FirstThunk);
            for (size_t i = 0; names[i].u1.AddressOfData; i++) {
                if (IMAGE_SNAP_BY_ORDINAL32(names[i].u1.Ordinal)) continue;
                auto *by = reinterpret_cast<const IMAGE_IMPORT_BY_NAME *>(module + names[i].u1.AddressOfData);
                if (strcmp(reinterpret_cast<const char *>(by->Name), name)) continue;
                uintptr_t slot = reinterpret_cast<uintptr_t>(&slots[i].u1.Function);
                if (original) *original = reinterpret_cast<void *>(slots[i].u1.Function);
                return WriteMemory(slot, reinterpret_cast<uint32_t>(fn));
            }
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {}
    return false;
}

void UndoPatches() {
    for (auto it = g_patches.rbegin(); it != g_patches.rend(); ++it) H->code_patch(M, *it, nullptr, nullptr, 0);
    g_patches.clear();
    for (uintptr_t at : g_hooks) H->unhook(M, at);   // (all the mod's links there: once is enough, twice harmless)
    g_hooks.clear();
}

size_t PatchCount() { return g_patches.size(); }
size_t HookCount() { return g_hooks.size(); }

} // namespace ff
