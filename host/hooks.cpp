// Hook chains (see host.h, mml.h `hook`, `hook_mid`, `code_patch`).
//
// MinHook allows one detour per function; mods (and the host) may want the same one. So each hooked function gets
// one MinHook hook whose detour is a tiny thunk `jmp [head]`, and the chain is a list of detours, outermost first:
// head points at the first detour, each detour's *original at the next, the last one's at MinHook's trampoline (the
// game's own code). Adding or removing a link only rewrites these pointers (the target's bytes are written once);
// the new pointers are stored innermost first, so a detour already running always finds a valid next step.
//
// A hook in the middle of a function (hook_mid) is a link of the same chains: MinHook relocates the instructions it
// overwrites at any address, not only at a function's entry, and the link's detour is a stub of the host's that keeps
// every register, the flags and the whole x87 and SSE state (FXSAVE), calls the mod's function with the registers as
// they were (MmlMidContext: written back when it returns), and jumps on to the rest of the chain. So mid hooks and
// entry hooks of several mods share one address, in the chain's order.
//
// A patch of the game's code (code_patch) writes bytes the mod says it expects there and puts back what was there
// when the mod unloads (unless something else changed them since: then they are left, and the log says so). A patch
// and a hook may not overlap: a hook's relocated bytes would carry the patch away, a patch over a hook's jump would
// break it.
#include "host.h"
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <mutex>
#include "MinHook.h"

namespace host {
const char *ModIdOf(const MmlMod *mod);   // mods.cpp
bool ModCallAnyThread(MmlMod *mod, void (*fn)(void *), void *user, const char *where);   // mods.cpp

namespace {
enum Place { kMiddle, kOuter, kInner };   // kept outermost or innermost, or among the others
struct MidEntry;
struct Link { MmlMod *owner; void *detour; void **original; Place place = kMiddle; MidEntry *mid = nullptr; };
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

// ---- the patches of the game's code (code_patch)
struct Patch { MmlMod *owner; uintptr_t at; std::vector<uint8_t> orig, mine; };
std::vector<Patch> g_patches;
constexpr size_t kHookBytes = 5;   // what MinHook writes at a target (a jmp rel32)
bool Overlaps(uintptr_t a, size_t n, uintptr_t b, size_t m) { return a < b + m && b < a + n; }
std::string Hex(const uint8_t *p, size_t n) {
    std::string s;
    char b[4];
    for (size_t i = 0; i < n && i < 32; i++) { snprintf(b, sizeof b, "%02X", p[i]); s += (i ? " " : "") + std::string(b); }
    return n > 32 ? s + " ..." : s;
}

// ---- the stubs of the hooks in the middle of functions (hook_mid): one a link, in pages of the host's. An unlinked
// one is reused only seconds later (a thread may still be inside it).
struct MidEntry {
    MmlMod *mod = nullptr;
    MmlMidFn fn = nullptr;
    void *user = nullptr;
    void *volatile next = nullptr;   // the chain's next step (the link's *original)
    uint8_t *code = nullptr;         // the stub
    ULONGLONG freedAt = 0;
};
std::vector<MidEntry *> g_freeMids;
constexpr size_t kMidStub = 64;

struct MidCall { MidEntry *e; MmlMidContext *c; };
void CallMid(void *p) {
    auto *m = static_cast<MidCall *>(p);
    m->e->fn(m->c, m->e->user);
}
// The stub's call: the registers as PUSHAD left them, the flags, and the slot the stub returns through (MmlMidContext's
// eip: the hooked address going in; the next step of the chain, or where the mod sends the code, going out). PUSHAD's
// ESP is the one after the slot and PUSHFD: the hooked code's is 8 above.
void __stdcall MidDispatch(MidEntry *e, MmlMidContext *c, uintptr_t at) {
    c->esp += 8;
    c->eip = static_cast<uint32_t>(at);
    MidCall call{e, c};
    if (e->mod) ModCallAnyThread(e->mod, CallMid, &call, "its hook in the middle of a function");
    else CallMid(&call);   // a host-owned mid hook has no mod lifetime/fault guard (as host-owned entry hooks)
    c->eip = c->eip != at ? c->eip : static_cast<uint32_t>(reinterpret_cast<uintptr_t>(e->next));
}

// sub esp, 4 (the slot it returns through); pushfd; pushad; mov ebx, esp (the context); sub esp, 0x210; and esp, -16;
// fxsave [esp]; fninit (an empty x87 stack for the mod's code, whatever the game left on it); push at; push ebx; push
// entry; call MidDispatch (stdcall: it pops all three); fxrstor [esp]; mov esp, ebx; popad; popfd; ret (to the slot)
void WriteMidStub(MidEntry *e, uintptr_t at) {
    uint8_t *p = e->code;
    size_t i = 0;
    auto put = [&](std::initializer_list<uint8_t> bytes) { for (uint8_t b : bytes) p[i++] = b; };
    auto dword = [&](uint32_t v) { memcpy(p + i, &v, 4); i += 4; };
    put({0x83, 0xEC, 0x04, 0x9C, 0x60, 0x8B, 0xDC, 0x81, 0xEC});
    dword(0x210);
    put({0x83, 0xE4, 0xF0, 0x0F, 0xAE, 0x04, 0x24, 0xDB, 0xE3, 0x68});
    dword(static_cast<uint32_t>(at));
    put({0x53, 0x68});
    dword(static_cast<uint32_t>(reinterpret_cast<uintptr_t>(e)));
    put({0xE8});
    dword(static_cast<uint32_t>(reinterpret_cast<uintptr_t>(&MidDispatch) - reinterpret_cast<uintptr_t>(p + i + 4)));
    put({0x0F, 0xAE, 0x0C, 0x24, 0x8B, 0xE3, 0x61, 0x9D, 0xC3});
    FlushInstructionCache(GetCurrentProcess(), p, i);
}

MidEntry *NewMid() {
    const ULONGLONG now = GetTickCount64();
    for (size_t i = 0; i < g_freeMids.size(); i++)
        if (now - g_freeMids[i]->freedAt > 2000) {
            MidEntry *e = g_freeMids[i];
            g_freeMids.erase(g_freeMids.begin() + i);
            return e;
        }
    uint8_t *page = static_cast<uint8_t *>(VirtualAlloc(nullptr, 4096, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE));
    if (!page) return nullptr;
    MidEntry *first = nullptr;
    for (size_t k = 0; k < 4096 / kMidStub; k++) {
        auto *e = new MidEntry;
        e->code = page + k * kMidStub;
        if (!first) first = e;
        else g_freeMids.push_back(e);   // (freedAt 0: free at once)
    }
    return first;
}
} // namespace

namespace {
bool AddLink(MmlMod *owner, uintptr_t target, void *detour, void **original, Place place, std::string *err, MidEntry *mid = nullptr) {
    std::lock_guard<std::recursive_mutex> lk(g_mx);
    if (!target || !detour || !original) { if (err) *err = "null target, detour or original"; return false; }
    if (!EnsureMinHook(err)) return false;
    Chain *c = Find(target);
    if (!c) {
        for (const Patch &p : g_patches)
            if (Overlaps(target, kHookBytes, p.at, p.mine.size())) {
                if (err) *err = std::string("the bytes there are patched (code_patch by ") + (p.owner ? ModIdOf(p.owner) : "the host") + ")";
                return false;
            }
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
    c->links.insert(c->links.begin() + at, Link{owner, detour, original, place, mid});
    Relink(c);
    return true;
}
} // namespace

bool HookMid(MmlMod *owner, uintptr_t target, MmlMidFn fn, void *user, std::string *err) {
    std::lock_guard<std::recursive_mutex> lk(g_mx);
    if (!fn) { if (err) *err = "no function"; return false; }
    MidEntry *e = NewMid();
    if (!e) { if (err) *err = "no memory for the stub"; return false; }
    e->mod = owner, e->fn = fn, e->user = user, e->next = nullptr;
    WriteMidStub(e, target);
    if (!AddLink(owner, target, e->code, const_cast<void **>(&e->next), kMiddle, err, e)) {
        e->freedAt = GetTickCount64();
        g_freeMids.push_back(e);
        return false;
    }
    return true;
}

bool PatchAdd(MmlMod *owner, uintptr_t at, const void *expect, const void *bytes, size_t n, std::string *err) {
    std::lock_guard<std::recursive_mutex> lk(g_mx);
    if (!at || !bytes || !n || n > 4096) { *err = "no address, no bytes, or more than 4096"; return false; }
    for (const Chain *c : g_chains)
        if (Overlaps(at, n, c->target, kHookBytes)) { *err = "a hook is there (" + DescribeAddress(c->target) + ")"; return false; }
    for (const Patch &p : g_patches)
        if (Overlaps(at, n, p.at, p.mine.size())) {
            *err = std::string("patched already by ") + (p.owner ? ModIdOf(p.owner) : "the host") + " at " + DescribeAddress(p.at);
            return false;
        }
    Patch p{owner, at, std::vector<uint8_t>(n), std::vector<uint8_t>(static_cast<const uint8_t *>(bytes), static_cast<const uint8_t *>(bytes) + n)};
    if (!MemRead(at, p.orig.data(), n)) { *err = "the address cannot be read"; return false; }
    if (expect && memcmp(p.orig.data(), expect, n) != 0) {
        *err = "the bytes there are " + Hex(p.orig.data(), n) + ", not " + Hex(static_cast<const uint8_t *>(expect), n) +
               " (another build, or another mod or ASI changed them)";
        return false;
    }
    if (!MemWrite(at, bytes, n)) { *err = "the code could not be written"; return false; }
    FlushInstructionCache(GetCurrentProcess(), reinterpret_cast<void *>(at), n);
    g_patches.push_back(std::move(p));
    return true;
}

namespace {
void PatchUndo(const Patch &p) {
    std::vector<uint8_t> now(p.mine.size());
    if (!MemRead(p.at, now.data(), now.size()) || now != p.mine) {
        Log(MML_LOG_WARN, p.owner ? ModIdOf(p.owner) : "host", "code_patch at %s: changed since it was written (%s): left as it is",
            DescribeAddress(p.at).c_str(), Hex(now.data(), now.size()).c_str());
        return;
    }
    MemWrite(p.at, p.orig.data(), p.orig.size());
    FlushInstructionCache(GetCurrentProcess(), reinterpret_cast<void *>(p.at), p.orig.size());
}
} // namespace

bool PatchRemove(MmlMod *owner, uintptr_t at) {
    std::lock_guard<std::recursive_mutex> lk(g_mx);
    for (size_t i = g_patches.size(); i-- > 0;)
        if (g_patches[i].owner == owner && g_patches[i].at == at) {
            PatchUndo(g_patches[i]);
            g_patches.erase(g_patches.begin() + i);
            return true;
        }
    return false;
}

void PatchRemoveAll(MmlMod *owner) {
    std::lock_guard<std::recursive_mutex> lk(g_mx);
    for (size_t i = g_patches.size(); i-- > 0;)   // (the last written first: a patch over the bytes of another's undone in order)
        if (g_patches[i].owner == owner) {
            PatchUndo(g_patches[i]);
            g_patches.erase(g_patches.begin() + i);
        }
}

std::string PatchList() {
    std::lock_guard<std::recursive_mutex> lk(g_mx);
    std::string out = "[";
    for (size_t i = 0; i < g_patches.size(); i++)
        out += (i ? "," : "") + std::string("{\"at\":\"") + DescribeAddress(g_patches[i].at) + "\",\"owner\":\"" +
               (g_patches[i].owner ? ModIdOf(g_patches[i].owner) : "host") + "\",\"bytes\":\"" + Hex(g_patches[i].mine.data(), g_patches[i].mine.size()) + "\"}";
    return out + "]";
}

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
    std::vector<MidEntry *> mids;
    for (size_t i = 0; i < c->links.size();) {
        if (c->links[i].owner == owner && (!detour || c->links[i].detour == detour)) {
            if (c->links[i].mid) mids.push_back(c->links[i].mid);
            c->links.erase(c->links.begin() + i);
            removed = true;
        } else {
            i++;
        }
    }
    for (MidEntry *e : mids) {   // (after the relink below no new call enters it; one running finishes in its own time)
        e->freedAt = GetTickCount64();
        g_freeMids.push_back(e);
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
                   "\",\"detour\":\"" +
                   DescribeAddress(c->links[k].mid ? reinterpret_cast<uintptr_t>(c->links[k].mid->fn) : reinterpret_cast<uintptr_t>(c->links[k].detour)) +
                   (c->links[k].mid ? "\",\"mid\":true}" : "\"}");
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
