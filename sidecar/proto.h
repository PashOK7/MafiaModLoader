// The loader and its 64-bit sidecar (mml-sidecar64.exe) talk through one section of shared memory: two rings of
// messages, one each way, each with a single writer and a single reader. Compiled into both: host/sidecar.cpp (32
// bits) and sidecar/ (64 bits), so every field has the same size and place in both (fixed-width types only).
//
// The section is "Local\mml-side-<game pid>" (the game makes it, the sidecar opens it by the name on its command
// line). A ring: `head` (bytes ever written) and `tail` (bytes ever read), the data after them; a message is
// `MsgHeader` then its payload, padded to 8, and never split at the ring's end (a writer that would cross it writes a
// filler message and starts again at the beginning). The writer signals the ring's "data" event after each message;
// the reader its "space" event after taking one, so a writer waiting for room wakes.
#pragma once
#include <stdint.h>

namespace side {

constexpr uint32_t kMagic = 0x53444D4D;   // "MMDS"
constexpr uint32_t kVersion = 1;
constexpr uint32_t kRingBytes = 4u << 20;  // each way
constexpr uint32_t kMaxMessage = 1u << 20;

// What a message is (MsgHeader::type). "h>s": the loader to the sidecar; "s>h": back.
enum MsgType : uint32_t {
    kFiller = 0,          // (ring wrap)
    // h>s
    kHello = 1,           // json {game_pid, game_dir, mml_dir, build, luid: [low, high] or null}
    kModLoad = 2,         // json {id, dir, entry, data_dir, config, generation}
    kModUnload = 3,       // json {id, why}
    kEvent = 4,           // json {type, ...the event's fields}: to the mods subscribed to its type
    kCallReply = 5,       // aux: the call's seq; json {ok, result | error}
    kCommand = 6,         // aux: seq; json {mod, name, args}: a bridge call to a 64-bit mod's command
    kAdapter = 7,         // json {luid: [low, high]}: the adapter the game draws on (once the graphics bridge knows it)
    kQuit = 8,
    // s>h
    kReady = 100,         // json {pid, version}
    kLog = 101,           // json {mod, level, text}
    kCall = 102,          // aux: seq; json {method, params, timeout_ms}: a 64-bit mod calls the bridge
    kCommandReply = 103,  // aux: seq; json {ok, text}
    kCommandAdd = 104,    // json {mod, name, help}
    kCommandRemove = 105, // json {mod, name}
    kSubscribe = 106,     // json {mod, types: [...]}: the event types a mod wants (all it asked so far)
    kModState = 107,      // json {id, generation, state: "loaded" | "failed" | "unloaded" | "faulted", error, where}
    kTexture = 108,       // json {mod, name, handle (in the game's process), width, height, format, generation}
    kTextureGone = 109,   // json {mod, name}
    kFence = 110,         // json {mod, name, handle (in the game's process), generation}
    kFenceGone = 111,     // json {mod, name}
};

struct MsgHeader {
    uint32_t bytes;    // payload bytes
    uint32_t type;     // MsgType
    uint32_t aux;      // a call's sequence number, or 0
    uint32_t reserved;
};

struct Ring {
    volatile uint64_t head;   // written by the writer only
    volatile uint64_t tail;   // written by the reader only
    uint32_t size;            // bytes of data (kRingBytes)
    uint32_t pad[13];         // (the data starts on a cache line of its own)
};

struct Section {
    uint32_t magic, version;
    uint32_t gamePid, sidecarPid;
    volatile uint32_t sidecarAlive;   // set by the sidecar once it reads; cleared when it quits cleanly
    uint32_t pad[11];
    Ring toSide;      // h>s, then its data
    // (data of toSide: kRingBytes) then Ring toHost, then its data: see the offsets below
};

constexpr uint32_t kOffToSide = 64;
constexpr uint32_t kOffToSideData = kOffToSide + sizeof(Ring);
constexpr uint32_t kOffToHost = kOffToSideData + kRingBytes;
constexpr uint32_t kOffToHostData = kOffToHost + sizeof(Ring);
constexpr uint32_t kSectionBytes = kOffToHostData + kRingBytes;

inline uint32_t Pad8(uint32_t n) { return (n + 7u) & ~7u; }

// The events' and the section's names, from the game's pid. Each ring has "data" and "space" events.
inline void Names(uint32_t gamePid, wchar_t section[64], wchar_t toSideData[64], wchar_t toSideSpace[64],
                  wchar_t toHostData[64], wchar_t toHostSpace[64]) {
    const wchar_t *fmt[5] = {L"Local\\mml-side-%u", L"Local\\mml-side-%u-sd", L"Local\\mml-side-%u-ss",
                             L"Local\\mml-side-%u-hd", L"Local\\mml-side-%u-hs"};
    wchar_t *out[5] = {section, toSideData, toSideSpace, toHostData, toHostSpace};
    for (int i = 0; i < 5; i++) {
        // (no swprintf in a header shared by two CRTs' worth of settings: digits by hand)
        const wchar_t *f = fmt[i];
        wchar_t *o = out[i];
        for (; *f; f++) {
            if (f[0] == L'%' && f[1] == L'u') {
                wchar_t digits[12];
                int n = 0;
                uint32_t v = gamePid;
                do { digits[n++] = static_cast<wchar_t>(L'0' + v % 10); v /= 10; } while (v);
                while (n) *o++ = digits[--n];
                f++;
            } else {
                *o++ = *f;
            }
        }
        *o = 0;
    }
}

// One ring end. Not thread-safe by itself: one writer thread (or a lock around Write) and one reader thread.
struct RingEnd {
    Ring *ring = nullptr;
    uint8_t *data = nullptr;
    void *dataEvent = nullptr;    // HANDLE: set after a write
    void *spaceEvent = nullptr;   // HANDLE: set after a read
};

} // namespace side
