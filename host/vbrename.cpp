// The game's vertex and index buffers renamed: the CPU never waits for the GPU to write them (ROADMAP 31i; docs/JOURNAL.md
// 2026-10-26, "where the menu's 1.2 ms go").
//
// LS3D keeps its vertices in pools (C_VBuffer: 128 KB and more each, D3DPOOL_DEFAULT) and its indices alike, and writes
// into a pool by locking it whole with no flags (1.2 lod::SetUV, C_VBuffer's own locks in 1.0: `Lock(0, 0, &data, 0)`;
// NOSYSLOCK in 1.0), only the vertices that changed (an env-mapped object's uv every frame, a lit object's colours,
// the particles), the rest kept; C_VBManager::m_UnlockAllVBuffers unlocks every pool before the frame is drawn. A lock
// with no flags of a buffer the GPU still draws from waits until it has: the CPU never runs ahead of the GPU (the
// menu's frame: 0.32 of 1.2 ms in 12 locks; far: 2.6 ms in 221).
//
// So, as a driver renames a buffer locked with DISCARD, but keeping what was not written: each such buffer the game
// makes (D3DPOOL_DEFAULT, 64 KB and more, not the host's or a mod's) gets a copy of its contents in the process's own
// memory, allocated with MEM_WRITE_WATCH, and Direct3D buffers of its kind of ours (dynamic: three, more for one
// written many times a frame, 8 MB of them at most: 1.0's IGraph::DrawIndexedPrimitiveList locks its one buffer with
// NOSYSLOCK alone for each person's shadow). The game's Lock is given that memory at once; at its Unlock the pages
// written (GetWriteWatch) go into a copy the GPU no longer reads (an event query issued when it stopped being the one
// drawn), which is drawn from then on: SetStreamSource and SetIndices given the game's buffer bind the copy drawn now,
// GetStreamSource and GetIndices give the game's buffer back, a state block's Apply and a new copy while the buffer
// is bound are followed. Every copy keeps the pages changed since it was last written and is brought up to date when
// its turn comes. All copies busy or the copies' budget spent: the copy drawn now is written in place, waiting as the
// game did. The game's own promises kept: a lock with NOOVERWRITE or DISCARD alone is written into the copy drawn now
// with the same flag (a discarded copy written whole: a plain lock may draw it later); a buffer written only so (1.2's
// shadows, the particles: rings) is given back to the game after 8 such unlocks, nothing of ours left on it.
//
// The hooks are innermost in the host's chains on the device the game draws with (a wrapper's Direct3D 9: dxwrapper's,
// DXVK's): the counters (d3dstats.cpp) and the state followed (d3dmirror.cpp) see the game's own buffers. Only the
// game's thread binds them (its lookups take no lock; another thread's take one); the game locks its pools on its own
// thread. `vb.check` reads every copy drawn back against the memory copy. `vb.set {rename: false}`: the game's own buffer locked at its Lock as the game locks it (its wait where it
// was), written from the memory copy at the Unlock and drawn: the A of an A/B; `vb.state`; `vb.selftest` (a device of
// its own: the offline tests).
#include "host.h"
#include <windows.h>
#include <d3d9.h>
#include <intrin.h>
#include <algorithm>
#include <atomic>
#include <string>
#include <vector>
#include "json.hpp"

namespace host {
namespace {
using json = nlohmann::json;

// IDirect3DDevice9's slots; a buffer's (vertices' and indices' alike); a state block's
enum : int { kVtReset = 16, kVtPresent = 17, kVtCreateVb = 26, kVtCreateIb = 27, kVtSetStream = 100, kVtGetStream = 101,
             kVtSetIndices = 104, kVtGetIndices = 105 };
enum : int { kVtRelease = 2, kVtLock = 11, kVtUnlock = 12 };
enum : int { kVtSbApply = 5 };
constexpr int kStreams = 16;
constexpr int kMinCopies = 4;                    // the game's buffer and three more: the GPU three frames behind
constexpr int kMaxCopies = 64;                   // a buffer written many times a frame (1.0's DrawIndexedPrimitiveList:
constexpr UINT kCopiesBytes = 8u << 20;          // each person's shadow) gets more, up to 8 MB of them
constexpr UINT kMinBytes = 0x10000;              // LS3D's pools are 128 KB and more; smaller buffers left alone
constexpr uint64_t kCopyBudget = 192ull << 20;   // the copies' bytes, every buffer's (a 32-bit process's address space)
constexpr size_t kPage = 4096;
constexpr DWORD kPlain = 1;              // a lock with neither NOOVERWRITE nor DISCARD: the one that waits for the GPU
constexpr int kRingGiveBack = 8;         // a buffer written only so this many times, never renamed: the game's again
constexpr uint32_t kSlotBits = 14, kSlots = 1u << kSlotBits;

struct Copy {
    IUnknown *buf = nullptr;            // IDirect3DVertexBuffer9 or IDirect3DIndexBuffer9 (the first: the game's own)
    IDirect3DQuery9 *done = nullptr;    // issued when it stopped being the one drawn
    bool issued = false, dynamic = false;
    std::vector<uint32_t> pending;      // the pages changed since it was last written
    std::vector<uint32_t> held;         // the pages it holds as the game wrote them (none after a DISCARD: vb.check)
};
struct Entry {
    int kind = 0;                       // 0 vertices, 1 indices
    void *game = nullptr;               // the buffer the game has
    IDirect3DDevice9 *dev = nullptr;
    UINT bytes = 0;
    DWORD usage = 0, format = 0;        // (a vertex buffer's FVF, an index buffer's format)
    uint8_t *shadow = nullptr;
    size_t pages = 0;
    std::vector<uint32_t> ever;         // the pages ever written: a new copy's first contents
    std::vector<Copy> copies;
    int current = 0;
    int locks = 0;
    void *direct = nullptr;             // renaming off: the game's own buffer, locked as the game asked
    DWORD seen = 0;                     // the game's locks since the last Unlock: kPlain, D3DLOCK_NOOVERWRITE, D3DLOCK_DISCARD
    bool plainEver = false;             // a lock that waits seen once: the buffer is kept
    int ringCommits = 0;                // unlocks after NOOVERWRITE or DISCARD alone
    std::vector<void *> watch;          // GetWriteWatch's answer
};
struct Slot { std::atomic<void *> key{nullptr}; Entry *e = nullptr; int copy = 0; };
void *const kGone = reinterpret_cast<void *>(1);

using LockFn = HRESULT(__stdcall *)(void *, UINT, UINT, void **, DWORD);
using UnlockFn = HRESULT(__stdcall *)(void *);
using ReleaseFn = ULONG(__stdcall *)(void *);
using CreateVbFn = HRESULT(__stdcall *)(IDirect3DDevice9 *, UINT, DWORD, DWORD, D3DPOOL, IDirect3DVertexBuffer9 **, HANDLE *);
using CreateIbFn = HRESULT(__stdcall *)(IDirect3DDevice9 *, UINT, DWORD, D3DFORMAT, D3DPOOL, IDirect3DIndexBuffer9 **, HANDLE *);
using SetStreamFn = HRESULT(__stdcall *)(IDirect3DDevice9 *, UINT, IDirect3DVertexBuffer9 *, UINT, UINT);
using GetStreamFn = HRESULT(__stdcall *)(IDirect3DDevice9 *, UINT, IDirect3DVertexBuffer9 **, UINT *, UINT *);
using SetIndicesFn = HRESULT(__stdcall *)(IDirect3DDevice9 *, IDirect3DIndexBuffer9 *);
using GetIndicesFn = HRESULT(__stdcall *)(IDirect3DDevice9 *, IDirect3DIndexBuffer9 **);
using ResetFn = HRESULT(__stdcall *)(IDirect3DDevice9 *, D3DPRESENT_PARAMETERS *);
using PresentFn = HRESULT(__stdcall *)(IDirect3DDevice9 *, const RECT *, const RECT *, HWND, const RGNDATA *);
using ApplyFn = HRESULT(__stdcall *)(IDirect3DStateBlock9 *);

LockFn oLock[2];
UnlockFn oUnlock[2];
ReleaseFn oRelease[2];
CreateVbFn oCreateVb;
CreateIbFn oCreateIb;
SetStreamFn oSetStream;
GetStreamFn oGetStream;
SetIndicesFn oSetIndices;
GetIndicesFn oGetIndices;
ResetFn oReset;
PresentFn oPresent;
ApplyFn oApply;

SRWLOCK g_srw = SRWLOCK_INIT;           // the table's and the entries' changes; another thread's lookups
Slot g_slots[kSlots];
uint32_t g_used, g_gone;                // slots holding a key, slots left by one taken out
std::vector<Entry *> g_graveyard;       // entries taken out, freed on the game's thread (its lookups take no lock)
IDirect3DDevice9 *g_dev;                // the device hooked
void **g_devVt;
bool g_installed, g_rename = true, g_test;
std::string g_why = "not installed (no Direct3D 9 device made by the game yet)";
uintptr_t g_hostBase, g_hostEnd;

struct Bound { Entry *e = nullptr; UINT offset = 0, stride = 0; };
Bound g_bound[kStreams];                // what the game bound (its buffers), the game's thread's
Entry *g_boundIb;

struct Stats {
    uint64_t adopted = 0, adoptedBytes = 0, released = 0, copiesMade = 0, copyBytes = 0, refusedBudget = 0,
             locks = 0, commits = 0, renames = 0, inPlace = 0, appends = 0, discards = 0, givenBack = 0, waits = 0, failures = 0,
             pagesWritten = 0, bytesWritten = 0;
    double waitMs = 0, commitMs = 0;
} g_st;
uint64_t g_tsc;                         // rdtsc ticks a millisecond

inline bool GameThread() { return GetCurrentThreadId() == MainThread(); }
inline uint32_t Hash(void *p) { return (static_cast<uint32_t>(reinterpret_cast<uintptr_t>(p) >> 3) * 2654435761u) >> (32 - kSlotBits); }

struct Hit { Entry *e = nullptr; int copy = 0; };
// The lookup: lock-free on the game's thread (entries are freed there), under the lock elsewhere
Hit FindRaw(void *p) {
    for (uint32_t i = Hash(p), n = 0; n < kSlots; i = (i + 1) & (kSlots - 1), n++) {
        void *k = g_slots[i].key.load(std::memory_order_acquire);
        if (!k) return {};
        if (k == p) return {g_slots[i].e, g_slots[i].copy};
    }
    return {};
}
Hit Find(void *p) {
    if (!p || !g_used) return {};
    if (GameThread()) return FindRaw(p);
    AcquireSRWLockShared(&g_srw);
    Hit h = FindRaw(p);
    ReleaseSRWLockShared(&g_srw);
    return h;
}
// (under the lock)
bool Insert(void *p, Entry *e, int copy) {
    if ((g_used + 1) * 4 > kSlots * 3) return false;
    for (uint32_t i = Hash(p);; i = (i + 1) & (kSlots - 1)) {
        void *k = g_slots[i].key.load(std::memory_order_relaxed);
        if (k && k != kGone) continue;
        if (k == kGone) g_gone--;
        g_slots[i].e = e;
        g_slots[i].copy = copy;
        g_slots[i].key.store(p, std::memory_order_release);
        g_used++;
        return true;
    }
}
void Remove(void *p) {
    for (uint32_t i = Hash(p), n = 0; n < kSlots; i = (i + 1) & (kSlots - 1), n++) {
        void *k = g_slots[i].key.load(std::memory_order_relaxed);
        if (!k) return;
        if (k != p) continue;
        g_slots[i].key.store(kGone, std::memory_order_release);
        g_used--, g_gone++;
        return;
    }
}
// The table made again without the slots left by entries taken out (the game's thread, under the lock)
void Rehash() {
    std::vector<std::pair<void *, std::pair<Entry *, int>>> keep;
    for (Slot &s : g_slots) {
        void *k = s.key.load(std::memory_order_relaxed);
        if (k && k != kGone) keep.push_back({k, {s.e, s.copy}});
        s.key.store(nullptr, std::memory_order_relaxed);
    }
    g_used = g_gone = 0;
    for (auto &[k, v] : keep) Insert(k, v.first, v.second);
}

bool OursCaller(void *ra) {
    const auto a = reinterpret_cast<uintptr_t>(ra);
    if (a >= g_hostBase && a < g_hostEnd) return true;
    return !ModAt(a).empty();
}

void SetBit(std::vector<uint32_t> &v, size_t i) { v[i >> 5] |= 1u << (i & 31); }
inline bool Bit(const std::vector<uint32_t> &v, size_t i) { return (v[i >> 5] >> (i & 31)) & 1; }

// ---- a buffer the game made: its copy in memory and its first Direct3D buffer (its own)
void Adopt(IDirect3DDevice9 *d, int kind, void *buf, UINT bytes, DWORD usage, DWORD format) {
    auto *e = new Entry;
    e->kind = kind, e->game = buf, e->dev = d, e->bytes = bytes, e->usage = usage, e->format = format;
    e->pages = (bytes + kPage - 1) / kPage;
    e->shadow = static_cast<uint8_t *>(VirtualAlloc(nullptr, e->pages * kPage, MEM_RESERVE | MEM_COMMIT | MEM_WRITE_WATCH, PAGE_READWRITE));
    IDirect3DQuery9 *q = nullptr;
    if (!e->shadow || FAILED(d->CreateQuery(D3DQUERYTYPE_EVENT, &q)) || !q) {
        if (e->shadow) VirtualFree(e->shadow, 0, MEM_RELEASE);
        delete e;
        g_st.failures++;
        return;
    }
    e->ever.assign((e->pages + 31) / 32, 0);
    e->watch.resize(e->pages);
    Copy c;
    c.buf = static_cast<IUnknown *>(buf);
    c.done = q;
    c.dynamic = (usage & D3DUSAGE_DYNAMIC) != 0;
    c.pending.assign(e->ever.size(), 0);
    c.held.assign(e->ever.size(), 0);
    e->copies.push_back(std::move(c));
    AcquireSRWLockExclusive(&g_srw);
    const bool in = Insert(buf, e, 0);
    ReleaseSRWLockExclusive(&g_srw);
    if (!in) {
        q->Release();
        VirtualFree(e->shadow, 0, MEM_RELEASE);
        delete e;
        g_st.failures++;
        return;
    }
    g_st.adopted++;
    g_st.adoptedBytes += bytes;
}

int AddCopy(Entry &e) {
    const int cap = std::max(kMinCopies, std::min<int>(kMaxCopies, static_cast<int>(kCopiesBytes / std::max(e.bytes, 1u))));
    if (static_cast<int>(e.copies.size()) >= cap) return -1;
    if (g_st.copyBytes + e.bytes > kCopyBudget) { g_st.refusedBudget++; return -1; }
    const DWORD usage = (e.usage & ~D3DUSAGE_WRITEONLY) | D3DUSAGE_DYNAMIC | (g_test ? 0 : D3DUSAGE_WRITEONLY);
    IUnknown *b = nullptr;
    HRESULT hr;
    if (e.kind == 0) hr = oCreateVb(e.dev, e.bytes, usage, e.format, D3DPOOL_DEFAULT, reinterpret_cast<IDirect3DVertexBuffer9 **>(&b), nullptr);
    else hr = oCreateIb(e.dev, e.bytes, usage, static_cast<D3DFORMAT>(e.format), D3DPOOL_DEFAULT, reinterpret_cast<IDirect3DIndexBuffer9 **>(&b), nullptr);
    IDirect3DQuery9 *q = nullptr;
    if (FAILED(hr) || !b || FAILED(e.dev->CreateQuery(D3DQUERYTYPE_EVENT, &q)) || !q) {
        if (b) oRelease[e.kind](b);
        g_st.failures++;
        return -1;
    }
    AcquireSRWLockExclusive(&g_srw);
    const bool in = Insert(b, &e, static_cast<int>(e.copies.size()));
    ReleaseSRWLockExclusive(&g_srw);
    if (!in) {
        q->Release();
        oRelease[e.kind](b);
        g_st.failures++;
        return -1;
    }
    Copy c;
    c.buf = b;
    c.done = q;
    c.dynamic = true;
    c.pending = e.ever;   // (everything written so far)
    c.held.assign(e.ever.size(), 0);
    e.copies.push_back(std::move(c));
    g_st.copiesMade++;
    g_st.copyBytes += e.bytes;
    return static_cast<int>(e.copies.size()) - 1;
}

bool Idle(Copy &c) {
    if (!c.issued) return true;
    const HRESULT hr = c.done->GetData(nullptr, 0, 0);
    if (hr == S_FALSE) return false;
    c.issued = false;   // (done, or the device lost: nothing waits on it)
    return true;
}

void Rebind(Entry &e) {
    IUnknown *now = e.copies[e.current].buf;
    if (e.kind == 0) {
        for (int s = 0; s < kStreams; s++)
            if (g_bound[s].e == &e) oSetStream(e.dev, s, static_cast<IDirect3DVertexBuffer9 *>(static_cast<void *>(now)), g_bound[s].offset, g_bound[s].stride);
    } else if (g_boundIb == &e) {
        oSetIndices(e.dev, static_cast<IDirect3DIndexBuffer9 *>(static_cast<void *>(now)));
    }
}

// A copy's pending pages from the memory copy into its locked contents
void WriteInto(Entry &e, Copy &c, void *p) {
    for (size_t w = 0; w < c.pending.size(); w++) {
        unsigned long low = 0;
        while (_BitScanForward(&low, c.pending[w])) {
            const size_t first = w * 32 + low;
            size_t last = first;   // (a run of pages: one copy)
            while (last + 1 < e.pages && Bit(c.pending, last + 1)) last++;
            const size_t from = first * kPage, to = std::min<size_t>((last + 1) * kPage, e.bytes);
            if (to > from) memcpy(static_cast<uint8_t *>(p) + from, e.shadow + from, to - from);
            g_st.pagesWritten += last - first + 1;
            g_st.bytesWritten += to > from ? to - from : 0;
            for (size_t q = first; q <= last; q++) c.pending[q >> 5] &= ~(1u << (q & 31)), c.held[q >> 5] |= 1u << (q & 31);
        }
    }
}

void SwitchTo(Entry &e, int t) {
    const int cur = e.current;
    if (t == cur) { g_st.inPlace++; return; }
    Copy &old = e.copies[cur];
    old.done->Issue(D3DISSUE_END);   // (every draw that may read it is before this)
    old.issued = true;
    e.current = t;
    Rebind(e);
    g_st.renames++;
}

void Forget(void *buf);
void Unadopt(Entry &e);

// The game's Unlock: the pages it wrote into a copy the GPU does not read, drawn from then on
void Commit(Entry &e) {
    ULONG_PTR n = e.watch.size();
    DWORD gran = 0;
    if (GetWriteWatch(WRITE_WATCH_FLAG_RESET, e.shadow, e.pages * kPage, e.watch.data(), &n, &gran) != 0) n = 0;
    const uint64_t t0 = __rdtsc();
    for (ULONG_PTR i = 0; i < n; i++) {
        const size_t page = (static_cast<uint8_t *>(e.watch[i]) - e.shadow) / kPage;
        if (page >= e.pages) continue;
        SetBit(e.ever, page);
        for (Copy &c : e.copies) SetBit(c.pending, page);
    }
    if (e.direct) {   // (renaming off: the game's own buffer, locked at the game's Lock, written and drawn)
        e.seen = 0;
        if (n) g_st.commits++;
        WriteInto(e, e.copies[0], e.direct);
        oUnlock[e.kind](e.game);
        e.direct = nullptr;
        SwitchTo(e, 0);
        g_st.commitMs += static_cast<double>(__rdtsc() - t0) / g_tsc;
        return;
    }
    if (!n) return;
    g_st.commits++;
    // The game's own promises kept: appended with NOOVERWRITE (the GPU reads none of what it wrote; LS3D's rings, the
    // shadows' and the particles' buffers) or rewritten after a DISCARD: into the copy drawn now, with the same flag
    // (the driver renames a discarded one). Whole pages are written: the bytes of a page the game did not write are
    // the ones the copy already holds.
    if (!(e.seen & kPlain) && (e.seen & (D3DLOCK_NOOVERWRITE | D3DLOCK_DISCARD))) {
        Copy &c = e.copies[e.current];
        const DWORD how = e.seen & D3DLOCK_DISCARD && c.dynamic ? D3DLOCK_DISCARD : c.dynamic ? D3DLOCK_NOOVERWRITE : 0;
        void *p = nullptr;
        e.seen = 0;
        if (FAILED(oLock[e.kind](c.buf, 0, 0, &p, how)) || !p) { g_st.failures++; return; }
        if (how == D3DLOCK_DISCARD) c.pending = e.ever;   // (a discarded copy written whole: it may be drawn again after
        WriteInto(e, c, p);                                // a plain lock, which keeps what was not written)
        oUnlock[e.kind](c.buf);
        how == D3DLOCK_DISCARD ? g_st.discards++ : g_st.appends++;
        g_st.commitMs += static_cast<double>(__rdtsc() - t0) / g_tsc;
        // a ring that never waited and was never renamed (its own buffer drawn, up to date): given back to the game
        if (!e.plainEver && e.current == 0 && e.copies.size() == 1 && ++e.ringCommits >= kRingGiveBack) Unadopt(e);
        return;
    }
    e.seen = 0;
    e.plainEver = true;
    // the copies of ours (from 1): the game's own buffer is drawn only while it has none, so that it is never bound
    // on the device and goes when the game releases it
    const int cur = e.current, n1 = static_cast<int>(e.copies.size()) - 1;
    int t = -1;
    for (int k = 1; k <= n1 && t < 0; k++) {
        const int i = 1 + (std::max(cur - 1, 0) + k) % n1;
        if (i != cur && Idle(e.copies[i])) t = i;
    }
    if (t < 0) t = AddCopy(e);
    const bool wait = t < 0;
    if (wait) t = cur;   // (all busy, or the copies' budget spent: written in place, the GPU waited for as the game did)
    Copy &c = e.copies[t];
    void *p = nullptr;
    const uint64_t w0 = __rdtsc();
    if (FAILED(oLock[e.kind](c.buf, 0, 0, &p, wait || !c.dynamic ? 0 : D3DLOCK_NOOVERWRITE)) || !p) {
        g_st.failures++;   // (its pages stay pending: written at its next turn)
        return;
    }
    if (wait) {
        g_st.waits++;
        g_st.waitMs += static_cast<double>(__rdtsc() - w0) / g_tsc;
    }
    WriteInto(e, c, p);
    oUnlock[e.kind](c.buf);
    SwitchTo(e, t);
    g_st.commitMs += static_cast<double>(__rdtsc() - t0) / g_tsc;
}

// The game's buffer gone: its copies, its memory
void Forget(void *buf) {
    AcquireSRWLockExclusive(&g_srw);
    Hit h = FindRaw(buf);
    if (!h.e || h.copy != 0) { ReleaseSRWLockExclusive(&g_srw); return; }
    Entry *e = h.e;
    for (Copy &c : e->copies) Remove(c.buf);
    g_graveyard.push_back(e);
    ReleaseSRWLockExclusive(&g_srw);
    for (int s = 0; s < kStreams; s++) if (g_bound[s].e == e) g_bound[s] = Bound{};
    if (g_boundIb == e) g_boundIb = nullptr;
    for (size_t i = 0; i < e->copies.size(); i++) {
        Copy &c = e->copies[i];
        if (i > 0) {   // (a copy still bound on the device lives until it is unbound)
            oRelease[e->kind](c.buf);
            g_st.copyBytes -= e->bytes;
        }
        if (c.done) c.done->Release();
        c.buf = nullptr, c.done = nullptr;
    }
    VirtualFree(e->shadow, 0, MEM_RELEASE);
    e->shadow = nullptr;
    g_st.released++;
    g_st.adoptedBytes -= e->bytes;
    g_st.adopted--;
}

// A buffer the game writes only as a ring, drawn from its own Direct3D buffer and that up to date: its locks go
// straight to it again (nothing of ours left on it)
void Unadopt(Entry &e) {
    AcquireSRWLockExclusive(&g_srw);
    Remove(e.game);
    g_graveyard.push_back(&e);
    ReleaseSRWLockExclusive(&g_srw);
    for (int s = 0; s < kStreams; s++) if (g_bound[s].e == &e) g_bound[s] = Bound{};
    if (g_boundIb == &e) g_boundIb = nullptr;
    if (e.copies[0].done) e.copies[0].done->Release();
    e.copies[0].done = nullptr;
    VirtualFree(e.shadow, 0, MEM_RELEASE);
    e.shadow = nullptr;
    g_st.givenBack++;
    g_st.adoptedBytes -= e.bytes;
    g_st.adopted--;
}

void Sweep() {
    if (g_graveyard.empty() && g_gone * 8 < kSlots) return;
    AcquireSRWLockExclusive(&g_srw);
    for (Entry *e : g_graveyard) delete e;
    g_graveyard.clear();
    if (g_gone * 8 >= kSlots) Rehash();
    ReleaseSRWLockExclusive(&g_srw);
}

// ---- the hooks
template <int K> HRESULT __stdcall hkLock(void *b, UINT off, UINT size, void **pp, DWORD flags) {
    Hit h = Find(b);
    if (!h.e || h.copy != 0 || !pp) return oLock[K](b, off, size, pp, flags);   // (a copy: the host's own lock)
    Entry &e = *h.e;
    if (off >= e.bytes) return D3DERR_INVALIDCALL;
    if (!(flags & D3DLOCK_READONLY)) e.seen |= flags & (D3DLOCK_NOOVERWRITE | D3DLOCK_DISCARD) ? flags & (D3DLOCK_NOOVERWRITE | D3DLOCK_DISCARD) : kPlain;
    if (!g_rename && !e.direct && !(flags & D3DLOCK_READONLY)) {   // (off: the game's own lock with its flags, its wait where it was)
        const uint64_t w0 = __rdtsc();
        if (FAILED(oLock[K](b, 0, 0, &e.direct, flags))) e.direct = nullptr;
        else if (flags & D3DLOCK_DISCARD) e.copies[0].pending = e.ever;   // (written whole at the Unlock)
        if (!(flags & (D3DLOCK_NOOVERWRITE | D3DLOCK_DISCARD))) {
            g_st.waits++;
            g_st.waitMs += static_cast<double>(__rdtsc() - w0) / g_tsc;
        }
    }
    *pp = e.shadow + off;
    e.locks++;
    g_st.locks++;
    return D3D_OK;
}
template <int K> HRESULT __stdcall hkUnlock(void *b) {
    Hit h = Find(b);
    if (!h.e || h.copy != 0) return oUnlock[K](b);
    Entry &e = *h.e;
    if (e.locks > 0 && --e.locks > 0) return D3D_OK;
    Commit(e);
    return D3D_OK;
}
template <int K> ULONG __stdcall hkRelease(void *b) {
    const ULONG r = oRelease[K](b);
    if (!r && g_used) Forget(b);   // (only the pointer is used: the object is gone)
    return r;
}

HRESULT __stdcall hkCreateVb(IDirect3DDevice9 *d, UINT len, DWORD usage, DWORD fvf, D3DPOOL pool, IDirect3DVertexBuffer9 **pp, HANDLE *shared) {
    const HRESULT hr = oCreateVb(d, len, usage, fvf, pool, pp, shared);
    if (SUCCEEDED(hr) && pp && *pp && pool == D3DPOOL_DEFAULT && !shared && len >= kMinBytes && (g_test || !OursCaller(_ReturnAddress())))
        Adopt(d, 0, *pp, len, usage, fvf);
    return hr;
}
HRESULT __stdcall hkCreateIb(IDirect3DDevice9 *d, UINT len, DWORD usage, D3DFORMAT fmt, D3DPOOL pool, IDirect3DIndexBuffer9 **pp, HANDLE *shared) {
    const HRESULT hr = oCreateIb(d, len, usage, fmt, pool, pp, shared);
    if (SUCCEEDED(hr) && pp && *pp && pool == D3DPOOL_DEFAULT && !shared && len >= kMinBytes && (g_test || !OursCaller(_ReturnAddress())))
        Adopt(d, 1, *pp, len, usage, fmt);
    return hr;
}

HRESULT __stdcall hkSetStream(IDirect3DDevice9 *d, UINT s, IDirect3DVertexBuffer9 *b, UINT off, UINT stride) {
    const Hit h = Find(b);
    if (s < kStreams && GameThread()) g_bound[s] = Bound{h.e, off, stride};
    if (h.e) b = static_cast<IDirect3DVertexBuffer9 *>(static_cast<void *>(h.e->copies[h.e->current].buf));
    return oSetStream(d, s, b, off, stride);
}
HRESULT __stdcall hkGetStream(IDirect3DDevice9 *d, UINT s, IDirect3DVertexBuffer9 **pp, UINT *off, UINT *stride) {
    const HRESULT hr = oGetStream(d, s, pp, off, stride);
    if (SUCCEEDED(hr) && pp && *pp) {
        const Hit h = Find(*pp);
        if (h.e && h.copy != 0) {   // (the game's buffer, as it set it)
            static_cast<IUnknown *>(h.e->game)->AddRef();
            (*pp)->Release();
            *pp = static_cast<IDirect3DVertexBuffer9 *>(h.e->game);
        }
    }
    return hr;
}
HRESULT __stdcall hkSetIndices(IDirect3DDevice9 *d, IDirect3DIndexBuffer9 *b) {
    const Hit h = Find(b);
    if (GameThread()) g_boundIb = h.e;
    if (h.e) b = static_cast<IDirect3DIndexBuffer9 *>(static_cast<void *>(h.e->copies[h.e->current].buf));
    return oSetIndices(d, b);
}
HRESULT __stdcall hkGetIndices(IDirect3DDevice9 *d, IDirect3DIndexBuffer9 **pp) {
    const HRESULT hr = oGetIndices(d, pp);
    if (SUCCEEDED(hr) && pp && *pp) {
        const Hit h = Find(*pp);
        if (h.e && h.copy != 0) {
            static_cast<IUnknown *>(h.e->game)->AddRef();
            (*pp)->Release();
            *pp = static_cast<IDirect3DIndexBuffer9 *>(h.e->game);
        }
    }
    return hr;
}
// A state block's Apply may bind a copy no longer drawn (it kept the one bound when it was captured): the one drawn now
HRESULT __stdcall hkApply(IDirect3DStateBlock9 *sb) {
    const HRESULT hr = oApply(sb);
    if (FAILED(hr) || !g_used || !g_dev || !GameThread()) return hr;
    IDirect3DDevice9 *d = nullptr;
    if (FAILED(sb->GetDevice(&d)) || !d) return hr;
    d->Release();
    if (d != g_dev) return hr;
    for (UINT s = 0; s < kStreams; s++) {
        IDirect3DVertexBuffer9 *b = nullptr;
        UINT off = 0, stride = 0;
        if (FAILED(oGetStream(d, s, &b, &off, &stride))) continue;
        const Hit h = Find(b);
        g_bound[s] = Bound{h.e, off, stride};
        if (h.e && b != static_cast<void *>(h.e->copies[h.e->current].buf))
            oSetStream(d, s, static_cast<IDirect3DVertexBuffer9 *>(static_cast<void *>(h.e->copies[h.e->current].buf)), off, stride);
        if (b) b->Release();
    }
    IDirect3DIndexBuffer9 *ib = nullptr;
    if (SUCCEEDED(oGetIndices(d, &ib))) {
        const Hit h = Find(ib);
        g_boundIb = h.e;
        if (h.e && ib != static_cast<void *>(h.e->copies[h.e->current].buf))
            oSetIndices(d, static_cast<IDirect3DIndexBuffer9 *>(static_cast<void *>(h.e->copies[h.e->current].buf)));
        if (ib) ib->Release();
    }
    return hr;
}
HRESULT __stdcall hkReset(IDirect3DDevice9 *d, D3DPRESENT_PARAMETERS *pp) {
    if (d == g_dev) { for (Bound &b : g_bound) b = Bound{}; g_boundIb = nullptr; }
    return oReset(d, pp);
}
HRESULT __stdcall hkPresent(IDirect3DDevice9 *d, const RECT *src, const RECT *dst, HWND wnd, const RGNDATA *dirty) {
    if (d == g_dev && GameThread()) Sweep();
    return oPresent(d, src, dst, wnd, dirty);
}

bool Install(IDirect3DDevice9 *d, std::string *why) {
    if (g_installed) {
        if (*reinterpret_cast<void ***>(d) == g_devVt) { g_dev = d; return true; }
        *why = "already installed on a device of another kind";
        return false;
    }
    if (!g_hostBase) {
        HMODULE me = nullptr;
        GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           reinterpret_cast<const char *>(&Install), &me);
        auto *dos = reinterpret_cast<const IMAGE_DOS_HEADER *>(me);
        auto *nt = reinterpret_cast<const IMAGE_NT_HEADERS *>(reinterpret_cast<const uint8_t *>(me) + dos->e_lfanew);
        g_hostBase = reinterpret_cast<uintptr_t>(me);
        g_hostEnd = g_hostBase + nt->OptionalHeader.SizeOfImage;
        LARGE_INTEGER f;
        QueryPerformanceFrequency(&f);
        const uint64_t c0 = __rdtsc();
        LARGE_INTEGER q0, q1;
        QueryPerformanceCounter(&q0);
        do QueryPerformanceCounter(&q1); while (q1.QuadPart - q0.QuadPart < f.QuadPart / 50);
        g_tsc = (__rdtsc() - c0) * f.QuadPart / (q1.QuadPart - q0.QuadPart) / 1000;
        if (!g_tsc) g_tsc = 1;
    }
    // the classes' vtables: a buffer of each kind and a state block made for the purpose (the game's are the same)
    IDirect3DVertexBuffer9 *vb = nullptr;
    IDirect3DIndexBuffer9 *ib = nullptr;
    IDirect3DStateBlock9 *sb = nullptr;
    d->CreateVertexBuffer(64, D3DUSAGE_DYNAMIC | D3DUSAGE_WRITEONLY, 0, D3DPOOL_DEFAULT, &vb, nullptr);
    d->CreateIndexBuffer(64, D3DUSAGE_DYNAMIC | D3DUSAGE_WRITEONLY, D3DFMT_INDEX16, D3DPOOL_DEFAULT, &ib, nullptr);
    d->CreateStateBlock(D3DSBT_VERTEXSTATE, &sb);
    if (!vb || !ib || !sb) {
        if (vb) vb->Release();
        if (ib) ib->Release();
        if (sb) sb->Release();
        *why = "the device made no buffer or state block to find their classes";
        return false;
    }
    void **dv = *reinterpret_cast<void ***>(d), **vv = *reinterpret_cast<void ***>(vb), **iv = *reinterpret_cast<void ***>(ib),
         **sv = *reinterpret_cast<void ***>(sb);
    struct S { void *at; void *detour; void **orig; };
    std::vector<S> sites = {
        {dv[kVtCreateVb], reinterpret_cast<void *>(&hkCreateVb), reinterpret_cast<void **>(&oCreateVb)},
        {dv[kVtCreateIb], reinterpret_cast<void *>(&hkCreateIb), reinterpret_cast<void **>(&oCreateIb)},
        {dv[kVtSetStream], reinterpret_cast<void *>(&hkSetStream), reinterpret_cast<void **>(&oSetStream)},
        {dv[kVtGetStream], reinterpret_cast<void *>(&hkGetStream), reinterpret_cast<void **>(&oGetStream)},
        {dv[kVtSetIndices], reinterpret_cast<void *>(&hkSetIndices), reinterpret_cast<void **>(&oSetIndices)},
        {dv[kVtGetIndices], reinterpret_cast<void *>(&hkGetIndices), reinterpret_cast<void **>(&oGetIndices)},
        {dv[kVtReset], reinterpret_cast<void *>(&hkReset), reinterpret_cast<void **>(&oReset)},
        {dv[kVtPresent], reinterpret_cast<void *>(&hkPresent), reinterpret_cast<void **>(&oPresent)},
        {sv[kVtSbApply], reinterpret_cast<void *>(&hkApply), reinterpret_cast<void **>(&oApply)},
        {vv[kVtLock], reinterpret_cast<void *>(&hkLock<0>), reinterpret_cast<void **>(&oLock[0])},
        {vv[kVtUnlock], reinterpret_cast<void *>(&hkUnlock<0>), reinterpret_cast<void **>(&oUnlock[0])},
        {vv[kVtRelease], reinterpret_cast<void *>(&hkRelease<0>), reinterpret_cast<void **>(&oRelease[0])},
    };
    // the index buffer's: its own functions, or the vertex buffer's (one function behind both: hooked once, both kinds' lookups)
    struct Same { int slot; void **orig0, **orig1; void *detour1; };
    const Same both[] = {{kVtLock, reinterpret_cast<void **>(&oLock[0]), reinterpret_cast<void **>(&oLock[1]), reinterpret_cast<void *>(&hkLock<1>)},
                         {kVtUnlock, reinterpret_cast<void **>(&oUnlock[0]), reinterpret_cast<void **>(&oUnlock[1]), reinterpret_cast<void *>(&hkUnlock<1>)},
                         {kVtRelease, reinterpret_cast<void **>(&oRelease[0]), reinterpret_cast<void **>(&oRelease[1]), reinterpret_cast<void *>(&hkRelease<1>)}};
    std::vector<std::pair<int, int>> shared;   // (index buffer slot, index into both)
    for (int i = 0; i < 3; i++) {
        if (iv[both[i].slot] == vv[both[i].slot]) shared.push_back({both[i].slot, i});
        else sites.push_back({iv[both[i].slot], both[i].detour1, both[i].orig1});
    }
    vb->Release();
    ib->Release();
    sb->Release();
    std::vector<std::pair<uintptr_t, void *>> done;
    for (const S &s : sites) {
        std::string err;
        const auto at = reinterpret_cast<uintptr_t>(s.at);
        // (the creations outermost: the caller's own return address tells the game's buffers from the host's and the mods')
        const bool outer = s.detour == reinterpret_cast<void *>(&hkCreateVb) || s.detour == reinterpret_cast<void *>(&hkCreateIb);
        if (!(outer ? HookAddOuter(at, s.detour, s.orig, &err) : HookAddInner(at, s.detour, s.orig, &err))) {
            for (auto &[a, h] : done) HookRemoveLink(nullptr, a, h);
            *why = "not hooked: " + err;
            return false;
        }
        done.push_back({at, s.detour});
    }
    for (auto &[slot, i] : shared) *both[i].orig1 = *both[i].orig0;
    g_dev = d;
    g_devVt = dv;
    g_installed = true;
    g_why = "on";
    return true;
}

json StateJson() {
    json j = {{"installed", g_installed}, {"why", g_why}, {"rename", g_rename}, {"buffers", g_st.adopted},
              {"buffers_mb", g_st.adoptedBytes / 1048576.0}, {"copies", g_st.copiesMade}, {"copies_mb", g_st.copyBytes / 1048576.0},
              {"copies_budget_mb", kCopyBudget >> 20}, {"refused_budget", g_st.refusedBudget}, {"released", g_st.released},
              {"locks", g_st.locks}, {"commits", g_st.commits}, {"renames", g_st.renames}, {"in_place", g_st.inPlace}, {"appends", g_st.appends}, {"discards", g_st.discards}, {"given_back", g_st.givenBack},
              {"waits", g_st.waits}, {"wait_ms", g_st.waitMs}, {"commit_ms", g_st.commitMs}, {"pages_written", g_st.pagesWritten},
              {"mb_written", g_st.bytesWritten / 1048576.0}, {"failures", g_st.failures}, {"slots_used", g_used}};
    return j;
}

// ---- vb.check: the copy drawn now of every buffer read back and compared with the memory copy, page by page (a
// write-only buffer read: slow, a check only)
json Check() {
    json bad = json::array();
    uint64_t buffers = 0, pages = 0, differ = 0, unread = 0;
    for (Slot &s : g_slots) {
        void *k = s.key.load(std::memory_order_acquire);
        if (!k || k == kGone || s.copy != 0 || !s.e) continue;
        Entry &e = *s.e;
        if (e.locks) continue;
        buffers++;
        Copy &c = e.copies[e.current];
        void *p = nullptr;
        if (FAILED(oLock[e.kind](c.buf, 0, 0, &p, D3DLOCK_READONLY)) || !p) { unread++; continue; }
        int n = 0;
        for (size_t i = 0; i < e.pages; i++) {
            if (!Bit(e.ever, i) || Bit(c.pending, i) || !Bit(c.held, i)) continue;
            pages++;
            const size_t from = i * kPage, to = std::min<size_t>(from + kPage, e.bytes);
            if (memcmp(static_cast<uint8_t *>(p) + from, e.shadow + from, to - from) != 0) { differ++; n++; }
        }
        oUnlock[e.kind](c.buf);
        char at[16];
        snprintf(at, sizeof at, "0x%08X", static_cast<unsigned>(reinterpret_cast<uintptr_t>(e.game)));
        if (n && bad.size() < 20)
            bad.push_back({{"buffer", at}, {"kind", e.kind ? "indices" : "vertices"}, {"copy", e.current},
                           {"pages_differ", n}, {"pages", e.pages}, {"plain_ever", e.plainEver}});
    }
    return {{"buffers", buffers}, {"pages", pages}, {"pages_differ", differ}, {"unread", unread}, {"differing", bad}};
}

// ---- vb.selftest: a device of its own (a hidden window), buffers made as the game's, written, drawn, checked
std::string SelfTest() {
    json r = json::object();
    auto fail = [&](const std::string &why) { r["ok"] = false; r["error"] = why; return r.dump(-1, ' ', false, json::error_handler_t::replace); };
    HMODULE lib = LoadLibraryW(L"d3d9.dll");
    using Create9Fn = IDirect3D9 *(WINAPI *)(UINT);
    auto create9 = lib ? reinterpret_cast<Create9Fn>(GetProcAddress(lib, "Direct3DCreate9")) : nullptr;
    IDirect3D9 *d3d = create9 ? create9(D3D_SDK_VERSION) : nullptr;
    if (!d3d) return fail("no Direct3D 9");
    WNDCLASSW wc{};
    wc.lpfnWndProc = DefWindowProcW;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpszClassName = L"mml_vbrename_test";
    RegisterClassW(&wc);
    HWND wnd = CreateWindowW(wc.lpszClassName, L"", WS_POPUP, 0, 0, 64, 64, nullptr, nullptr, wc.hInstance, nullptr);
    D3DPRESENT_PARAMETERS pp{};
    pp.Windowed = TRUE;
    pp.SwapEffect = D3DSWAPEFFECT_DISCARD;
    pp.BackBufferFormat = D3DFMT_UNKNOWN;
    pp.BackBufferWidth = 64, pp.BackBufferHeight = 64;
    pp.hDeviceWindow = wnd;
    pp.PresentationInterval = D3DPRESENT_INTERVAL_IMMEDIATE;
    IDirect3DDevice9 *d = nullptr;
    HRESULT hr = d3d->CreateDevice(D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, wnd, D3DCREATE_HARDWARE_VERTEXPROCESSING, &pp, &d);
    if (FAILED(hr)) hr = d3d->CreateDevice(D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, wnd, D3DCREATE_SOFTWARE_VERTEXPROCESSING, &pp, &d);
    auto done = [&] {
        if (d) d->Release();
        d3d->Release();
        DestroyWindow(wnd);
    };
    if (FAILED(hr) || !d) { done(); return fail("no device (" + std::to_string(static_cast<unsigned>(hr)) + ")"); }
    IDirect3DDevice9 *gameDev = g_dev;
    void **gameVt = g_devVt;
    const bool wasInstalled = g_installed;
    std::string why;
    if (g_installed && *reinterpret_cast<void ***>(d) != g_devVt) { done(); return fail("installed on the game's device: a test device of another kind cannot be hooked"); }
    if (!Install(d, &why)) { done(); return fail(why); }
    g_test = true;
    const Stats before = g_st;
    const bool rename = g_rename;
    g_rename = true;
    std::vector<std::string> bad;
    // the game's pool: 256 KB of vertices, written a few pages a frame, bound and drawn each frame
    const UINT bytes = 256 * 1024, stride = 16;
    IDirect3DVertexBuffer9 *vb = nullptr;
    d->CreateVertexBuffer(bytes, D3DUSAGE_DYNAMIC, D3DFVF_XYZRHW, D3DPOOL_DEFAULT, &vb, nullptr);
    const Hit h = Find(vb);
    if (!h.e) bad.push_back("the buffer was not taken");
    std::vector<uint8_t> want(bytes, 0);
    bool waited = false;
    for (int f = 0; f < 24 && h.e; f++) {
        void *p = nullptr;
        const uint64_t t0 = __rdtsc();
        vb->Lock(0, 0, &p, 0);
        if ((__rdtsc() - t0) / g_tsc > 2) waited = true;
        if (p != h.e->shadow) bad.push_back("frame " + std::to_string(f) + ": the lock did not give the buffer's copy in memory");
        // a few pages (the first frame: all of it), a byte pattern of the frame's
        for (UINT o = 0; o < bytes; o += 4) {
            const UINT page = o / kPage;
            if (f == 0 || page % 7 == static_cast<UINT>(f % 7)) {
                const uint32_t v = (f * 2654435761u) ^ o;
                memcpy(static_cast<uint8_t *>(p) + o, &v, 4);
                memcpy(want.data() + o, &v, 4);
            }
        }
        vb->Unlock();
        // the copy drawn now holds everything written
        IUnknown *now = h.e->copies[h.e->current].buf;
        void *q = nullptr;
        if (SUCCEEDED(oLock[0](now, 0, 0, &q, D3DLOCK_READONLY)) && q) {
            if (memcmp(q, want.data(), bytes) != 0) bad.push_back("frame " + std::to_string(f) + ": the copy drawn differs from what was written");
            oUnlock[0](now);
        } else {
            bad.push_back("frame " + std::to_string(f) + ": the copy drawn could not be read");
        }
        d->SetStreamSource(0, vb, 0, stride);
        IDirect3DVertexBuffer9 *got = nullptr;
        UINT o = 0, s = 0;
        d->GetStreamSource(0, &got, &o, &s);
        if (got != vb) bad.push_back("frame " + std::to_string(f) + ": GetStreamSource did not give the game's buffer");
        if (got) got->Release();
        IDirect3DVertexBuffer9 *raw = nullptr;
        oGetStream(d, 0, &raw, &o, &s);
        if (raw != static_cast<void *>(now)) bad.push_back("frame " + std::to_string(f) + ": the device does not draw from the copy drawn now");
        if (raw) raw->Release();
        d->BeginScene();
        d->SetFVF(D3DFVF_XYZRHW);
        d->DrawPrimitive(D3DPT_POINTLIST, 0, bytes / stride);
        d->EndScene();
        d->Present(nullptr, nullptr, nullptr, nullptr);
    }
    // a state block captured with an older copy bound: its Apply binds the one drawn now
    if (h.e) {
        d->SetStreamSource(0, vb, 0, stride);
        IDirect3DStateBlock9 *sb = nullptr;
        d->CreateStateBlock(D3DSBT_ALL, &sb);
        void *p = nullptr;
        vb->Lock(0, 0, &p, 0);
        static_cast<uint8_t *>(p)[0] ^= 1;
        want[0] ^= 1;
        vb->Unlock();
        d->SetStreamSource(0, nullptr, 0, 0);
        if (sb) { sb->Apply(); sb->Release(); }
        IDirect3DVertexBuffer9 *raw = nullptr;
        UINT o = 0, s = 0;
        oGetStream(d, 0, &raw, &o, &s);
        if (raw != static_cast<void *>(h.e->copies[h.e->current].buf)) bad.push_back("a state block's Apply left a copy no longer drawn bound");
        if (raw) raw->Release();
    }
    // a DISCARD between plain locks: whatever copy is drawn after it holds everything written (a plain lock keeps
    // what it did not write)
    for (int f = 0; f < 6 && h.e; f++) {
        void *p = nullptr;
        const DWORD how = f == 2 ? D3DLOCK_DISCARD : 0;
        vb->Lock(0, 0, &p, how);
        const UINT o = static_cast<UINT>(f) * 3 * kPage;
        memset(static_cast<uint8_t *>(p) + o, 0x40 + f, kPage);
        memset(want.data() + o, 0x40 + f, kPage);
        vb->Unlock();
        IUnknown *now = h.e->copies[h.e->current].buf;
        void *q = nullptr;
        if (SUCCEEDED(oLock[0](now, 0, 0, &q, D3DLOCK_READONLY)) && q) {
            if (memcmp(q, want.data(), bytes) != 0) bad.push_back("after a DISCARD, lock " + std::to_string(f) + ": the copy drawn lost what was written before");
            oUnlock[0](now);
        }
        d->SetStreamSource(0, vb, 0, stride);
        d->BeginScene();
        d->DrawPrimitive(D3DPT_POINTLIST, 0, bytes / stride);
        d->EndScene();
        d->Present(nullptr, nullptr, nullptr, nullptr);
    }
    // a ring (NOOVERWRITE appends, a DISCARD at its start): written into the buffer drawn, never renamed, given back
    IDirect3DVertexBuffer9 *ring = nullptr;
    d->CreateVertexBuffer(bytes, D3DUSAGE_DYNAMIC | D3DUSAGE_WRITEONLY, D3DFVF_XYZRHW, D3DPOOL_DEFAULT, &ring, nullptr);
    if (!Find(ring).e) bad.push_back("the ring was not taken");
    const uint64_t renamesBefore = g_st.renames;
    for (int i = 0; i < kRingGiveBack + 2 && ring; i++) {
        void *p = nullptr;
        ring->Lock(static_cast<UINT>(i % 8) * 4096, 4096, &p, i % 8 == 0 ? D3DLOCK_DISCARD : D3DLOCK_NOOVERWRITE);
        if (p) memset(p, i, 4096);
        ring->Unlock();
    }
    r["ring_given_back"] = ring && !Find(ring).e;
    if (g_st.renames != renamesBefore) bad.push_back("the ring was renamed");
    if (!r["ring_given_back"].get<bool>()) bad.push_back("the ring was not given back to the game");
    if (ring) ring->Release();
    r["copies"] = h.e ? h.e->copies.size() : 0;
    r["renames"] = g_st.renames - before.renames;
    r["waits"] = g_st.waits - before.waits;
    r["lock_waited"] = waited;
    d->SetStreamSource(0, nullptr, 0, 0);
    if (vb) vb->Release();
    r["forgotten"] = !Find(vb).e;
    r["released"] = g_st.released - before.released;
    Sweep();
    g_test = false;
    g_rename = rename;
    g_dev = wasInstalled ? gameDev : nullptr;
    g_devVt = wasInstalled ? gameVt : g_devVt;
    done();
    if (r["renames"].get<uint64_t>() == 0) bad.push_back("no copy was ever drawn in place of the buffer");
    if (!r["forgotten"].get<bool>()) bad.push_back("the buffer released was still known");
    r["errors"] = bad;
    r["ok"] = bad.empty();
    return r.dump(-1, ' ', false, json::error_handler_t::replace);
}
} // namespace

void VbRenameInstall(void *device9) {
    auto *d = static_cast<IDirect3DDevice9 *>(device9);
    std::string why;
    if (!d) return;
    if (Install(d, &why)) HLOG("vb rename: the game's vertex and index buffers renamed (its locks given memory of their own, the GPU never waited for)");
    else { g_why = why; HWARN("vb rename: off (%s)", why.c_str()); }
}

bool VbRenameMethod(const std::string &method, const std::string &paramsJson, std::string *reply, bool *ok) {
    if (method == "vb.state") { *ok = true; *reply = StateJson().dump(-1, ' ', false, json::error_handler_t::replace); return true; }
    if (method == "vb.set") {
        json p = json::parse(paramsJson.empty() ? "null" : paramsJson, nullptr, false);
        if (p.is_object() && p.contains("rename") && p["rename"].is_boolean()) g_rename = p["rename"].get<bool>();
        *ok = true;
        *reply = StateJson().dump(-1, ' ', false, json::error_handler_t::replace);
        return true;
    }
    if (method == "vb.check") {
        *ok = true;
        *reply = Check().dump(-1, ' ', false, json::error_handler_t::replace);
        return true;
    }
    if (method == "vb.selftest") {
        *reply = SelfTest();
        json r = json::parse(*reply, nullptr, false);
        *ok = r.is_object() && r.value("ok", false);
        return true;
    }
    return false;
}

} // namespace host
