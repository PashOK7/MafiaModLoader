// Hook chains (see host.h, mml.h `hook`).
//
// MinHook allows one detour per function; mods (and the host) may want the same one. So each hooked function gets
// one MinHook hook whose detour is a tiny thunk `jmp [head]`, and the chain is a list of detours, outermost first:
// head points at the first detour, each detour's *original at the next, the last one's at MinHook's trampoline (the
// game's own code). Adding or removing a link only rewrites these pointers (the target's bytes are written once);
// the new pointers are stored innermost first, so a detour already running always finds a valid next step.
#include "host.h"
#include <algorithm>
#include <cstdio>
#include <mutex>
#include "MinHook.h"

namespace host {
const char *ModIdOf(const MmlMod *mod);   // mods.cpp

namespace {
enum Place { kMiddle, kOuter, kInner };   // kept outermost or innermost, or among the others
struct Link { MmlMod *owner; void *detour; void **original; Place place = kMiddle; };
struct Chain {
    uintptr_t target = 0;
    uint8_t *thunk = nullptr;          // jmp dword ptr [&head]
    void *volatile head = nullptr;
    void *trampoline = nullptr;        // MinHook's: the game's code
    std::vector<Link> links;           // outermost first
};
std::recursive_mutex g_mx;
std::vector<Chain *> g_chains;
std::vector<uint8_t *> g_freeThunks;
bool g_mhReady;
// The chains' trampolines and the functions they begin, for the stack's readers (HookedByTrampoline): rows never
// move, written under g_mx, read without it (a crash's filter on a thread that may hold it, the sampler)
struct TrampolineRow { void *volatile trampoline; volatile uintptr_t target; };
TrampolineRow g_trampolines[1024];
volatile LONG g_trampolineRows;   // the rows ever used

void TrampolineSet(void *trampoline, uintptr_t target) {
    for (LONG i = 0; i < g_trampolineRows; i++)
        if (!g_trampolines[i].trampoline) {
            g_trampolines[i].target = target;
            InterlockedExchangePointer(&g_trampolines[i].trampoline, trampoline);
            return;
        }
    if (g_trampolineRows < 1024) {
        g_trampolines[g_trampolineRows].target = target;
        g_trampolines[g_trampolineRows].trampoline = trampoline;
        InterlockedIncrement(&g_trampolineRows);
    }
}
void TrampolineClear(void *trampoline) {
    for (LONG i = 0; i < g_trampolineRows; i++)
        if (g_trampolines[i].trampoline == trampoline) InterlockedExchangePointer(&g_trampolines[i].trampoline, nullptr);
}

uint8_t *NewThunk() {
    if (g_freeThunks.empty()) {
        uint8_t *page = static_cast<uint8_t *>(VirtualAlloc(nullptr, 4096, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE));
        if (!page) return nullptr;
        for (int i = 4096 / 16 - 1; i >= 0; i--) g_freeThunks.push_back(page + i * 16);
    }
    uint8_t *t = g_freeThunks.back();
    g_freeThunks.pop_back();
    return t;
}

Chain *Find(uintptr_t target) {
    for (Chain *c : g_chains) if (c->target == target) return c;
    return nullptr;
}

void Relink(Chain *c) {
    // innermost first: every pointer a running detour may follow stays valid
    for (size_t i = c->links.size(); i-- > 0;)
        *c->links[i].original = i + 1 < c->links.size() ? c->links[i + 1].detour : c->trampoline;
    c->head = c->links.empty() ? c->trampoline : c->links[0].detour;
}

bool EnsureMinHook(std::string *err) {
    if (g_mhReady) return true;
    MH_STATUS s = MH_Initialize();
    if (s != MH_OK && s != MH_ERROR_ALREADY_INITIALIZED) { if (err) *err = MH_StatusToString(s); return false; }
    g_mhReady = true;
    return true;
}
} // namespace

namespace {
bool AddLink(MmlMod *owner, uintptr_t target, void *detour, void **original, Place place, std::string *err) {
    std::lock_guard<std::recursive_mutex> lk(g_mx);
    if (!target || !detour || !original) { if (err) *err = "null target, detour or original"; return false; }
    if (!EnsureMinHook(err)) return false;
    Chain *c = Find(target);
    if (!c) {
        c = new Chain;
        c->target = target;
        c->thunk = NewThunk();
        if (!c->thunk) { delete c; if (err) *err = "no memory for the thunk"; return false; }
        c->thunk[0] = 0xFF;   // jmp dword ptr [abs32]
        c->thunk[1] = 0x25;
        *reinterpret_cast<void *volatile **>(c->thunk + 2) = &c->head;
        FlushInstructionCache(GetCurrentProcess(), c->thunk, 6);
        MH_STATUS s = MH_CreateHook(reinterpret_cast<void *>(target), c->thunk, &c->trampoline);
        if (s != MH_OK) {
            g_freeThunks.push_back(c->thunk);
            delete c;
            if (err) *err = std::string("MinHook: ") + MH_StatusToString(s);
            return false;
        }
        c->head = c->trampoline;
        s = MH_EnableHook(reinterpret_cast<void *>(target));
        if (s != MH_OK) {
            MH_RemoveHook(reinterpret_cast<void *>(target));
            g_freeThunks.push_back(c->thunk);
            delete c;
            if (err) *err = std::string("MinHook enable: ") + MH_StatusToString(s);
            return false;
        }
        g_chains.push_back(c);
        TrampolineSet(c->trampoline, target);
    }
    for (const Link &l : c->links)
        if (l.owner == owner && l.detour == detour) { if (err) *err = "already hooked by this owner"; return false; }
    // outermost first, after the links kept outermost (an outer one goes before them all, an inner one after them)
    size_t at = 0;
    if (place == kInner) at = c->links.size();
    else if (place == kMiddle) while (at < c->links.size() && c->links[at].place == kOuter) at++;
    c->links.insert(c->links.begin() + at, Link{owner, detour, original, place});
    Relink(c);
    return true;
}
} // namespace

bool HookAdd(MmlMod *owner, uintptr_t target, void *detour, void **original, std::string *err) {
    return AddLink(owner, target, detour, original, kMiddle, err);
}
bool HookAddOuter(uintptr_t target, void *detour, void **original, std::string *err) {
    return AddLink(nullptr, target, detour, original, kOuter, err);
}
bool HookAddInner(uintptr_t target, void *detour, void **original, std::string *err) {
    return AddLink(nullptr, target, detour, original, kInner, err);
}

namespace {
bool RemoveLinks(MmlMod *owner, uintptr_t target, void *detour) {
    std::lock_guard<std::recursive_mutex> lk(g_mx);
    Chain *c = Find(target);
    if (!c) return false;
    bool removed = false;
    for (size_t i = 0; i < c->links.size();) {
        if (c->links[i].owner == owner && (!detour || c->links[i].detour == detour)) { c->links.erase(c->links.begin() + i); removed = true; }
        else i++;
    }
    Relink(c);
    if (c->links.empty()) {
        MH_DisableHook(reinterpret_cast<void *>(target));
        TrampolineClear(c->trampoline);
        MH_RemoveHook(reinterpret_cast<void *>(target));
        g_freeThunks.push_back(c->thunk);
        g_chains.erase(std::find(g_chains.begin(), g_chains.end(), c));
        delete c;
    }
    return removed;
}
} // namespace

bool HookRemove(MmlMod *owner, uintptr_t target) { return RemoveLinks(owner, target, nullptr); }
bool HookRemoveLink(MmlMod *owner, uintptr_t target, void *detour) { return detour && RemoveLinks(owner, target, detour); }

void HookRemoveAll(MmlMod *owner) {
    std::lock_guard<std::recursive_mutex> lk(g_mx);
    std::vector<uintptr_t> targets;
    for (Chain *c : g_chains)
        for (const Link &l : c->links)
            if (l.owner == owner) { targets.push_back(c->target); break; }
    for (uintptr_t t : targets) HookRemove(owner, t);
}

std::string HookList() {
    std::lock_guard<std::recursive_mutex> lk(g_mx);
    std::string out = "[";
    for (size_t i = 0; i < g_chains.size(); i++) {
        const Chain *c = g_chains[i];
        out += (i ? "," : "") + std::string("{\"target\":\"") + DescribeAddress(c->target) + "\",\"links\":[";
        for (size_t k = 0; k < c->links.size(); k++)
            out += (k ? "," : "") + std::string("{\"owner\":\"") + (c->links[k].owner ? ModIdOf(c->links[k].owner) : "host") +
                   "\",\"detour\":\"" + DescribeAddress(reinterpret_cast<uintptr_t>(c->links[k].detour)) + "\"}";
        out += "]}";
    }
    return out + "]";
}

uintptr_t HookedByTrampoline(uintptr_t at) {
    const LONG rows = g_trampolineRows;
    for (LONG i = 0; i < rows; i++)
        if (reinterpret_cast<uintptr_t>(g_trampolines[i].trampoline) == at) return g_trampolines[i].target;
    return 0;
}

} // namespace host
