// ASI mods that write the game's code or constants without VirtualProtect (see host.h).
//
// Some ASI mods of the 2010s patch Game.exe by writing into its image as it is: libMafiaCountrysideMap 2.0 (2016, the
// countryside on the radar) writes a call into C_game::Init's end, the radar's scale for the countryside into .rdata
// and its picture's name over the game's, all from a timer, with no VirtualProtect. .text and .rdata are not writable
// (the image's own protections, in every layout: with the loader or without it, Ultimate ASI Loader 9 or 10), so
// each write faulted; a wrapper's handler went on and the plugin did half its work, the map never shown.
//
// The loader's vectored handler, first in the process's list, lets such a write through: an access violation writing
// into Game.exe's or LS3DF.dll's image, on a committed page that is only not writable (no guard, not no-access), by
// an instruction of an .asi other than the loader's own. The page is made writable (its execute kept) and the
// instruction runs again. Every other fault goes on as before: the game's own writes, the loader's, its mods', a
// wrapper's, an ASI's anywhere else (a bug still dies as one, with its dump). The page stays writable for the run:
// such a plugin may write it again later (its next timer, the next mission). Each page opened is logged once, with
// the plugin, the place and the section.
//
// Any thread may run the handler (a plugin's timer, a thread made without TLS): no TLS, no allocation, no CRT locale.
// The pages opened are kept in a table; a thread with TLS logs its own at once, the frame logs the others.
#include "host.h"
#include <cstdio>

extern "C" IMAGE_DOS_HEADER __ImageBase;

namespace host {
namespace {
struct Image { uintptr_t base, end; const char *name; };
Image g_images[2];   // Game.exe, LS3DF.dll (as they were at the start: both are loaded before the loader)
int g_nImages;

// A page opened: state 0 free, 1 being filled, 2 to be logged, 3 logged.
struct Opened { uintptr_t page, at, eip, by; DWORD from, to; bool tls; volatile LONG state; };
constexpr int kMaxOpened = 64;
Opened g_opened[kMaxOpened];
volatile LONG g_nOpened, g_pending, g_more;   // (g_more: pages past the table's end, opened but not logged)

uintptr_t ImageEnd(uintptr_t base) {
    auto dos = reinterpret_cast<const IMAGE_DOS_HEADER *>(base);
    auto nt = reinterpret_cast<const IMAGE_NT_HEADERS *>(base + dos->e_lfanew);
    return base + nt->OptionalHeader.SizeOfImage;
}

const Image *ImageOf(uintptr_t a) {
    for (int i = 0; i < g_nImages; i++)
        if (a >= g_images[i].base && a < g_images[i].end) return &g_images[i];
    return nullptr;
}

// Whether a module is an .asi (Ultimate ASI Loader's plugins) other than the loader. (No CRT: a thread without TLS.)
bool IsForeignAsi(uintptr_t base) {
    if (base == reinterpret_cast<uintptr_t>(&__ImageBase)) return false;
    wchar_t path[MAX_PATH];
    DWORD len = GetModuleFileNameW(reinterpret_cast<HMODULE>(base), path, MAX_PATH);
    if (len < 5 || len >= MAX_PATH) return false;
    const wchar_t *ext = path + len - 4;
    return ext[0] == L'.' && (ext[1] | 0x20) == L'a' && (ext[2] | 0x20) == L's' && (ext[3] | 0x20) == L'i';
}

// The section of an image an address falls in (".text", ".rdata"...), into `out` (9 bytes).
void SectionOf(uintptr_t base, uintptr_t a, char *out) {
    auto dos = reinterpret_cast<const IMAGE_DOS_HEADER *>(base);
    auto nt = reinterpret_cast<const IMAGE_NT_HEADERS *>(base + dos->e_lfanew);
    const IMAGE_SECTION_HEADER *s = IMAGE_FIRST_SECTION(nt);
    const uintptr_t rva = a - base;
    snprintf(out, 9, "headers");
    for (int i = 0; i < nt->FileHeader.NumberOfSections; i++, s++) {
        const DWORD size = s->Misc.VirtualSize ? s->Misc.VirtualSize : s->SizeOfRawData;
        if (rva >= s->VirtualAddress && rva < s->VirtualAddress + size) {
            memcpy(out, s->Name, 8);
            out[8] = 0;
            return;
        }
    }
}

const char *ProtectName(DWORD p) {
    switch (p & 0xFF) {
    case PAGE_READONLY: return "read-only";
    case PAGE_READWRITE: return "read-write";
    case PAGE_EXECUTE: return "execute";
    case PAGE_EXECUTE_READ: return "execute-read";
    case PAGE_EXECUTE_READWRITE: return "execute-read-write";
    default: return "other";
    }
}

void LogOpened(int i) {
    Opened &o = g_opened[i];
    if (InterlockedCompareExchange(&o.state, 3, 2) != 2) return;   // (logged by the other side already)
    InterlockedDecrement(&g_pending);
    const Image *img = ImageOf(o.at);
    std::string by = ModulePath(reinterpret_cast<HMODULE>(o.by));
    by = by.substr(by.find_last_of("\\/") + 1);
    char section[9];
    SectionOf(img->base, o.at, section);
    HLOG("asi compat: %s (+0x%X) wrote %s+0x%X (%s, %s) without VirtualProtect: the page 0x%08X made %s, the write "
         "run again%s", by.c_str(), static_cast<unsigned>(o.eip - o.by), img->name, static_cast<unsigned>(o.at - img->base),
         section, ProtectName(o.from), static_cast<unsigned>(o.page), ProtectName(o.to),
         o.tls ? "" : " (on a thread without TLS: logged at the frame)");
}

LONG CALLBACK OnFault(EXCEPTION_POINTERS *e) {
    const EXCEPTION_RECORD *r = e->ExceptionRecord;
    if (r->ExceptionCode != EXCEPTION_ACCESS_VIOLATION || r->NumberParameters < 2 || r->ExceptionInformation[0] != 1)
        return EXCEPTION_CONTINUE_SEARCH;   // (not a write)
    const uintptr_t at = r->ExceptionInformation[1];
    if (!ImageOf(at)) return EXCEPTION_CONTINUE_SEARCH;
    const uintptr_t eip = e->ContextRecord->Eip;
    MEMORY_BASIC_INFORMATION code, page;
    if (!VirtualQuery(reinterpret_cast<const void *>(eip), &code, sizeof code) || code.Type != MEM_IMAGE)
        return EXCEPTION_CONTINUE_SEARCH;
    const uintptr_t by = reinterpret_cast<uintptr_t>(code.AllocationBase);
    if (ImageOf(by) || !IsForeignAsi(by)) return EXCEPTION_CONTINUE_SEARCH;
    if (!VirtualQuery(reinterpret_cast<const void *>(at), &page, sizeof page) || page.State != MEM_COMMIT ||
        page.Type != MEM_IMAGE || (page.Protect & (PAGE_GUARD | PAGE_NOACCESS)))
        return EXCEPTION_CONTINUE_SEARCH;
    const uintptr_t base = at & ~static_cast<uintptr_t>(0xFFF);
    DWORD to;
    switch (page.Protect & 0xFF) {
    case PAGE_READONLY: to = PAGE_READWRITE; break;
    case PAGE_EXECUTE: case PAGE_EXECUTE_READ: to = PAGE_EXECUTE_READWRITE; break;
    default: {   // writable already: another thread opened it between the fault and now
        const LONG n = g_nOpened < kMaxOpened ? g_nOpened : kMaxOpened;
        for (LONG i = 0; i < n; i++)
            if (g_opened[i].page == base && g_opened[i].state >= 2) return EXCEPTION_CONTINUE_EXECUTION;
        return EXCEPTION_CONTINUE_SEARCH;
    }
    }
    DWORD old = 0;
    if (!VirtualProtect(reinterpret_cast<void *>(base), 0x1000, to, &old)) return EXCEPTION_CONTINUE_SEARCH;
    const LONG i = InterlockedIncrement(&g_nOpened) - 1;
    if (i < kMaxOpened) {
        Opened &o = g_opened[i];
        o.state = 1;
        o.page = base, o.at = at, o.eip = eip, o.by = by, o.from = page.Protect, o.to = to, o.tls = ThreadHasTls();
        InterlockedIncrement(&g_pending);
        InterlockedExchange(&o.state, 2);
        if (o.tls) LogOpened(i);
    } else {
        InterlockedIncrement(&g_more);
    }
    return EXCEPTION_CONTINUE_EXECUTION;
}
} // namespace

void AsiCompatStart() {
    const struct { HMODULE m; const char *name; } images[] = {{GetModuleHandleW(nullptr), "Game.exe"},
                                                             {GetModuleHandleW(L"LS3DF.dll"), "LS3DF.dll"}};
    for (const auto &m : images)
        if (m.m) g_images[g_nImages++] = {reinterpret_cast<uintptr_t>(m.m), ImageEnd(reinterpret_cast<uintptr_t>(m.m)), m.name};
    if (AddVectoredExceptionHandler(1, OnFault))
        HLOG("asi compat: an ASI mod's write into Game.exe's or LS3DF.dll's code or constants without VirtualProtect "
             "is let through (the page made writable, logged)");
    else
        HWARN("asi compat: the vectored handler not added (error %lu): such ASI mods' writes fault", GetLastError());
}

void AsiCompatFrame() {
    if (!g_pending && !g_more) return;
    const LONG n = g_nOpened < kMaxOpened ? g_nOpened : kMaxOpened;
    for (LONG i = 0; i < n; i++)
        if (g_opened[i].state == 2) LogOpened(i);
    if (const LONG more = InterlockedExchange(&g_more, 0))
        HWARN("asi compat: %ld more pages opened for ASI mods' writes (past the table's %d: not named)", more, kMaxOpened);
}
} // namespace host
