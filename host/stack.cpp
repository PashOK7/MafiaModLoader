// A thread's calls read from its stack (see host.h): the sampler's (prof.cpp) and the crash report's (crash.cpp) one
// walk, so a crash's stack is as exact as a profile's.
//
// The game's code keeps no frame pointers; Windows' own x86 code and the loader's (built /Oy-) do. So a thread in the
// system's, a driver's or our code is walked up by its frame pointers to the first frame of the game's (certain), and
// from there the stack is scanned: every return address into the game's or our code (right after a call instruction),
// each with its call's target when the call can be read (a direct call, a call through a pointer at a fixed place),
// followed through jumps, the host's hook chains and MinHook's trampolines to the function that runs. A return address
// whose call leads elsewhere than the frame below is stale (left in a frame's unwritten locals by a call that
// returned); MCP checks that against the functions' starts (mcp/src/stacks.rs), the host only roughly (LikelyFrames).
#include "host.h"
#include <cwchar>
#include <cwctype>

extern "C" IMAGE_DOS_HEADER __ImageBase;

namespace host {

// A return address: code right after a call (E8 rel32, or FF /2: a register, [reg], [reg+d8], [reg+d32], [abs32],
// with or without an index byte). Leftovers of finished calls pass too.
bool AfterCallAt(uintptr_t a) {
    if (a < 0x10000) return false;
    __try {
        const uint8_t *p = reinterpret_cast<const uint8_t *>(a);
        auto reg2 = [](uint8_t modrm) { return (modrm & 0x38) == 0x10; };   // the /2 of FF
        if (p[-5] == 0xE8) return true;
        if (p[-2] == 0xFF && reg2(p[-1]) && (p[-1] >> 6 == 3 || (p[-1] >> 6 == 0 && (p[-1] & 7) != 4 && (p[-1] & 7) != 5))) return true;
        if (p[-3] == 0xFF && reg2(p[-2]) && ((p[-2] >> 6 == 1 && (p[-2] & 7) != 4) || (p[-2] >> 6 == 0 && (p[-2] & 7) == 4))) return true;
        if (p[-4] == 0xFF && reg2(p[-3]) && p[-3] >> 6 == 1 && (p[-3] & 7) == 4) return true;
        if (p[-6] == 0xFF && reg2(p[-5]) && ((p[-5] >> 6 == 2 && (p[-5] & 7) != 4) || p[-5] == 0x15)) return true;
        // [reg+d32] with an index byte, or [index*n+abs32] (a table of functions)
        if (p[-7] == 0xFF && reg2(p[-6]) && ((p[-6] >> 6 == 2 && (p[-6] & 7) == 4) || (p[-6] >> 6 == 0 && (p[-6] & 7) == 4 && (p[-5] & 7) == 5)))
            return true;
        return false;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

namespace {
// The length of an instruction that may stand between `mov reg, [abs]` and `call reg` without changing reg: a push,
// or a load into another register (arguments, `this`); 0 for anything else.
int Harmless(const uint8_t *i, int reg) {
    const int r2 = (i[1] >> 3) & 7;
    if (i[0] >= 0x50 && i[0] <= 0x57) return 1;                                      // push r
    if (i[0] == 0x6A) return 2;                                                      // push imm8
    if (i[0] == 0x68) return 5;                                                      // push imm32
    if (i[0] == 0xFF && i[1] == 0x75) return 3;                                      // push [ebp+d8]
    if (i[0] == 0xFF && i[1] == 0x74 && i[2] == 0x24) return 4;                      // push [esp+d8]
    if (i[0] == 0xFF && i[1] == 0x35) return 6;                                      // push [abs]
    if (i[0] >= 0xB8 && i[0] <= 0xBF) return (i[0] & 7) != reg ? 5 : 0;              // mov r2, imm32
    if (i[0] != 0x8B && i[0] != 0x8D) return 0;                                      // mov / lea r2, ...
    if (r2 == reg) return 0;
    if (i[1] >> 6 == 3) return i[0] == 0x8B ? 2 : 0;                                 // mov r2, r3
    if (i[1] >> 6 == 1 && (i[1] & 7) == 5) return 3;                                 // [ebp+d8]
    if (i[1] >> 6 == 1 && (i[1] & 7) == 4 && i[2] == 0x24) return 4;                 // [esp+d8]
    if (i[1] >> 6 == 0 && (i[1] & 7) == 5) return 6;                                 // [abs]
    return 0;
}
} // namespace

// Where the call before a return address went, when it is a direct one (`call rel32`), through a pointer at a fixed
// place (`call [abs]`: an import, a hook's original), or through a register loaded from one just before it (`mov eax,
// [abs]` ... `call eax`: today's compiler calls a hook's original so): its target now, followed through jumps (an
// import's thunk, MinHook's patch and the host's chain's `jmp [head]`) to the code that runs; a MinHook trampoline (a
// detour calling the original) is the hooked function itself. 0 for a call through an object (a vtable's) or a
// register loaded otherwise.
uint32_t CallTarget(uint32_t ret) {
    __try {
        const uint8_t *p = reinterpret_cast<const uint8_t *>(static_cast<uintptr_t>(ret));
        uint32_t t = 0;
        if (p[-5] == 0xE8) t = ret + *reinterpret_cast<const int32_t *>(p - 4);
        else if (p[-6] == 0xFF && p[-5] == 0x15) t = **reinterpret_cast<uint32_t *const *>(p - 4);   // call [abs]: an import, a pointer
        else if (p[-2] == 0xFF && (p[-1] & 0xF8) == 0xD0) {   // call reg: the load before it, read forward from each place
            const int reg = p[-1] & 7;
            const uint8_t *call = p - 2;
            for (int back = 5; back <= 24 && !t; back++) {
                const uint8_t *q = call - back;
                const uint8_t *abs = nullptr;
                if (reg == 0 && q[0] == 0xA1) abs = q + 1;   // mov eax, [abs]
                else if (q[0] == 0x8B && (q[1] & 0xC7) == 0x05 && ((q[1] >> 3) & 7) == reg) abs = q + 2;
                if (!abs) continue;
                const uint8_t *i = abs + 4;
                for (int n; i < call && (n = Harmless(i, reg)) != 0;) i += n;
                if (i == call) t = **reinterpret_cast<uint32_t *const *>(abs);
            }
            if (!t) return 0;
        } else {
            return 0;
        }
        for (int hop = 0; hop < 4; hop++) {
            if (uintptr_t hooked = HookedByTrampoline(t)) return static_cast<uint32_t>(hooked);
            const uint8_t *q = reinterpret_cast<const uint8_t *>(static_cast<uintptr_t>(t));
            if (q[0] == 0xE9) t = t + 5 + *reinterpret_cast<const int32_t *>(q + 1);
            else if (q[0] == 0xEB) t = t + 2 + static_cast<int8_t>(q[1]);
            else if (q[0] == 0xFF && q[1] == 0x25) t = **reinterpret_cast<uint32_t *const *>(q + 2);
            else break;
        }
        return t;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return 0;
    }
}

// The walk by frame pointers from a thread not in the game's code: each frame's return address at [EBP + 4], the next
// frame at [EBP], up to the first return address into the game's code (its index in the stack, or SIZE_MAX: the
// thread is in the game's code, or a frame pointer does not lead up the stack: a function that uses EBP otherwise, a
// mod built without frame pointers). Each return address on the way must follow a call.
size_t AnchorOf(const StackView &v, CodeKindFn kind, void *ctx) {
    if (kind(v.eip, ctx) == kCodeGame) return SIZE_MAX;
    uint32_t ebp = v.ebp;
    for (int depth = 0; depth < 64; depth++) {
        if (ebp < v.esp || ebp & 3) return SIZE_MAX;
        const size_t i = (ebp - v.esp) / 4;
        if (i + 1 >= v.n) return SIZE_MAX;
        const uint32_t ret = v.stack[i + 1], next = v.stack[i];
        const CodeKind k = kind(ret, ctx);
        if (k == kCodeNone || !AfterCallAt(ret)) return SIZE_MAX;
        if (k == kCodeGame) return i + 1;
        if (next <= ebp) return SIZE_MAX;
        ebp = next;
    }
    return SIZE_MAX;
}

int StackCalls(const StackView &v, CodeKindFn kind, void *ctx, bool others, StackCall *out, int cap) {
    int n = 0;
    const size_t anchor = AnchorOf(v, kind, ctx);
    // (walked by its frame pointers: the frames on the way, read from the walk itself, certain; the system's only when
    // asked for: the sampler adds the time by the game's and our functions)
    if (anchor != SIZE_MAX) {
        uint32_t e = v.ebp;
        while (e >= v.esp && (e - v.esp) / 4 + 1 < anchor && n < cap) {
            const size_t i = (e - v.esp) / 4;
            const uint32_t ret = v.stack[i + 1];
            const CodeKind k = kind(ret, ctx);
            if (k == kCodeOurs || (others && k == kCodeOther)) out[n++] = {ret, kCallWalked};
            e = v.stack[i];
        }
    }
    for (size_t i = anchor == SIZE_MAX ? 0 : anchor; i < v.n && n < cap; i++) {
        const uint32_t r = v.stack[i];
        const CodeKind k = kind(r, ctx);
        if ((k != kCodeGame && k != kCodeOurs) || !AfterCallAt(r)) continue;
        if (n && out[n - 1].ret == r) continue;
        out[n++] = {r, i == anchor ? kCallWalked : CallTarget(r)};
    }
    return n;
}

// The host's own rough check (the log's lines, a mod's fault): a return address whose direct call's target is not
// within 32 KiB below one of the last few frames kept is dropped; an indirect call's and a walked one are kept.
int LikelyFrames(uint32_t eip, const StackCall *calls, int n, uintptr_t *out, int cap) {
    int k = 0;
    if (cap > 0) out[k++] = eip;
    for (int i = 0; i < n && k < cap; i++) {
        const uint32_t t = calls[i].target;
        bool leads = t == 0 || t == kCallWalked;
        for (int f = k - 1; f >= 0 && f >= k - 4 && !leads; f--) leads = out[f] >= t && out[f] < t + 0x8000;
        if (leads) out[k++] = calls[i].ret;
    }
    return k;
}

// ---- whose code an address is, on any thread at any time (a crash's filter, a mod's fault): no snapshot of the
// modules, no TLS. Each region asked (VirtualQuery) is remembered: a stack's values fall in a few of them.
namespace {
struct Region { uintptr_t base, end; CodeKind kind; };
struct Regions { Region r[64]; int n, next; };

CodeKind KindOfModule(uintptr_t base) {
    if (base == reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr))) return kCodeGame;   // Game.exe (the stand-in's own)
    if (base == reinterpret_cast<uintptr_t>(GetModuleHandleW(L"LS3DF.dll"))) return kCodeGame;
    if (base == reinterpret_cast<uintptr_t>(&__ImageBase)) return kCodeOurs;
    wchar_t path[MAX_PATH];
    DWORD len = GetModuleFileNameW(reinterpret_cast<HMODULE>(base), path, MAX_PATH);
    for (DWORD i = 0; i < len; i++) path[i] = static_cast<wchar_t>(towlower(path[i]));
    return len && wcsstr(path, L"\\mml\\live\\") ? kCodeOurs : kCodeOther;   // (a mod as the loader runs it)
}

CodeKind LiveKind(uintptr_t a, void *ctx) {
    Regions &rs = *static_cast<Regions *>(ctx);
    for (int i = 0; i < rs.n; i++) if (a >= rs.r[i].base && a < rs.r[i].end) return rs.r[i].kind;
    MEMORY_BASIC_INFORMATION mi;
    if (!VirtualQuery(reinterpret_cast<const void *>(a), &mi, sizeof mi)) return kCodeNone;
    const uintptr_t base = reinterpret_cast<uintptr_t>(mi.BaseAddress), module = reinterpret_cast<uintptr_t>(mi.AllocationBase);
    const bool code = mi.State == MEM_COMMIT && mi.Type == MEM_IMAGE &&
                      (mi.Protect & (PAGE_EXECUTE | PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY));
    Region g = {base, base + mi.RegionSize, code ? KindOfModule(module) : kCodeNone};
    if (rs.n < 64) rs.r[rs.n++] = g;
    else { rs.r[rs.next] = g; rs.next = (rs.next + 1) % 64; }
    return g.kind;
}
} // namespace

int StackCallsOf(const CONTEXT *c, bool others, StackCall *out, int cap) {
    const NT_TIB *tib = reinterpret_cast<const NT_TIB *>(NtCurrentTeb());
    const uintptr_t top = reinterpret_cast<uintptr_t>(tib->StackBase), low = reinterpret_cast<uintptr_t>(tib->StackLimit);
    if (c->Esp < low || c->Esp >= top) return 0;   // not this thread's stack
    StackView v = {c->Eip, c->Esp, c->Ebp, reinterpret_cast<const uint32_t *>(static_cast<uintptr_t>(c->Esp)),
                   (top - c->Esp) / 4};
    if (v.n > 0x10000) v.n = 0x10000;   // (256 KB up from the fault)
    Regions rs;
    rs.n = rs.next = 0;
    return StackCalls(v, LiveKind, &rs, others, out, cap);
}

int StackOf(const CONTEXT *c, uintptr_t *out, int cap) {
    StackCall calls[64];
    const int n = StackCallsOf(c, true, calls, 64);
    return LikelyFrames(c->Eip, calls, n, out, cap);
}

} // namespace host
