// Channels (side.h channel_open, mml.h channel_open): a named block of shared memory a mod's two halves see, one
// writer. Two slots: a write goes into the slot the latest version is not in, then becomes the latest; a reader copies
// the latest and checks that the writer did not start on that slot meanwhile (it would be two versions on), else
// copies again. So a reader always gets one whole version, never half of two. Compiled into both processes.
//
// The memory: "Local\mml-ch-<game pid>-<mod>-<name>", a Header, then two slots of `size` bytes.
#pragma once
#include <string.h>
#include <string>
#include <windows.h>

namespace side {

struct ChannelHeader {
    uint32_t magic;            // 'MMCH'
    uint32_t size;             // each slot's bytes
    volatile uint64_t version; // the latest whole version (0: none yet); it is in slot version & 1
    volatile uint64_t writing; // the version being written now (version + 1 while a write runs, else version)
    volatile uint32_t bytes[2];// each slot's length
    uint32_t pad[8];
};
constexpr uint32_t kChannelMagic = 0x48434D4D;

struct Channel {
    HANDLE map = nullptr;
    ChannelHeader *h = nullptr;
    uint8_t *slot[2] = {};
    uint32_t size = 0;
};

inline std::wstring ChannelName(uint32_t gamePid, const std::string &mod, const std::string &name) {
    std::wstring w = L"Local\\mml-ch-" + std::to_wstring(gamePid) + L"-";
    for (char c : mod + "-" + name) w += (c == '\\' || c == '/' || static_cast<unsigned char>(c) < 32) ? L'_' : static_cast<wchar_t>(static_cast<unsigned char>(c));
    return w;
}

// Opens (or makes) the channel; size must match the other half's. false: no memory, or another size.
inline bool ChannelOpen(Channel *c, uint32_t gamePid, const std::string &mod, const std::string &name, uint32_t size, std::string *err) {
    if (!size || size > (256u << 20)) { *err = "size: 1 byte .. 256 MB"; return false; }
    uint64_t total = sizeof(ChannelHeader) + 2ull * size;
    std::wstring n = ChannelName(gamePid, mod, name);
    HANDLE m = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, static_cast<DWORD>(total >> 32),
                                  static_cast<DWORD>(total), n.c_str());
    if (!m) { *err = "CreateFileMapping failed (" + std::to_string(GetLastError()) + ")"; return false; }
    bool existed = GetLastError() == ERROR_ALREADY_EXISTS;
    void *p = MapViewOfFile(m, FILE_MAP_ALL_ACCESS, 0, 0, 0);
    if (!p) { CloseHandle(m); *err = "MapViewOfFile failed (" + std::to_string(GetLastError()) + ")"; return false; }
    ChannelHeader *h = static_cast<ChannelHeader *>(p);
    if (!existed) {
        h->size = size;
        MemoryBarrier();
        h->magic = kChannelMagic;
    } else {
        for (int i = 0; i < 200 && h->magic != kChannelMagic; i++) Sleep(1);   // (the other half is setting it up)
        if (h->magic != kChannelMagic || h->size != size) {
            UnmapViewOfFile(p);
            CloseHandle(m);
            *err = "the channel exists with another size (" + std::to_string(h->size) + " bytes a version)";
            return false;
        }
    }
    c->map = m;
    c->h = h;
    c->size = size;
    c->slot[0] = static_cast<uint8_t *>(p) + sizeof(ChannelHeader);
    c->slot[1] = c->slot[0] + size;
    return true;
}

inline void ChannelClose(Channel *c) {
    if (c->h) UnmapViewOfFile(c->h);
    if (c->map) CloseHandle(c->map);
    *c = Channel{};
}

inline bool ChannelWrite(Channel *c, const void *data, uint32_t bytes) {
    if (!c->h || bytes > c->size) return false;
    uint64_t next = c->h->version + 1;
    c->h->writing = next;
    MemoryBarrier();
    uint32_t s = static_cast<uint32_t>(next & 1);
    memcpy(c->slot[s], data, bytes);
    c->h->bytes[s] = bytes;
    MemoryBarrier();
    c->h->version = next;
    return true;
}

inline uint32_t ChannelRead(Channel *c, void *data, uint32_t cap, uint64_t *version) {
    if (version) *version = 0;
    if (!c->h) return 0;
    for (int tries = 0; tries < 64; tries++) {
        uint64_t v = c->h->version;
        MemoryBarrier();
        if (!v) return 0;
        uint32_t s = static_cast<uint32_t>(v & 1);
        uint32_t n = c->h->bytes[s];
        if (n > cap) n = cap;
        memcpy(data, c->slot[s], n);
        MemoryBarrier();
        if (c->h->writing < v + 2) {   // the writer has not come back to this slot
            if (version) *version = v;
            return n;
        }
    }
    return 0;
}

} // namespace side
