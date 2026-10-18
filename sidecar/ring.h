// The rings of proto.h, written and read (both processes). Windows only; include after <windows.h>.
#pragma once
#include <string.h>
#include <string>
#include "proto.h"

namespace side {

// Writes one message; waits for room up to waitMs (the reader is slow or hung). false: no room in time, or too big.
inline bool RingWrite(RingEnd &w, uint32_t type, uint32_t aux, const void *payload, uint32_t bytes, uint32_t waitMs) {
    if (!w.ring || bytes > kMaxMessage) return false;
    const uint32_t size = w.ring->size;
    const uint32_t need = static_cast<uint32_t>(sizeof(MsgHeader)) + Pad8(bytes);
    ULONGLONG until = GetTickCount64() + waitMs;
    for (;;) {
        uint64_t head = w.ring->head, tail = w.ring->tail;
        MemoryBarrier();
        uint32_t at = static_cast<uint32_t>(head % size);
        uint32_t toEnd = size - at;
        // a message never crosses the end: what is left there is a filler (or nothing, if no header fits)
        uint64_t used = head - tail;
        uint32_t extra = toEnd < need ? toEnd : 0;
        if (used + extra + need <= size) {
            if (extra) {
                if (extra >= sizeof(MsgHeader)) {
                    MsgHeader f{extra - static_cast<uint32_t>(sizeof(MsgHeader)), kFiller, 0, 0};
                    memcpy(w.data + at, &f, sizeof f);
                }
                head += extra;
                at = 0;
            }
            MsgHeader h{bytes, type, aux, 0};
            memcpy(w.data + at, &h, sizeof h);
            if (bytes) memcpy(w.data + at + sizeof h, payload, bytes);
            MemoryBarrier();
            w.ring->head = head + need;
            SetEvent(w.dataEvent);
            return true;
        }
        DWORD left = static_cast<DWORD>(until > GetTickCount64() ? until - GetTickCount64() : 0);
        if (!left) return false;
        WaitForSingleObject(w.spaceEvent, left < 50 ? left : 50);
    }
}

// Takes the next message if there is one (fillers skipped): its type, aux and payload.
inline bool RingRead(RingEnd &r, uint32_t *type, uint32_t *aux, std::string *payload) {
    if (!r.ring) return false;
    const uint32_t size = r.ring->size;
    for (;;) {
        uint64_t head = r.ring->head, tail = r.ring->tail;
        MemoryBarrier();
        if (tail == head) return false;
        uint32_t at = static_cast<uint32_t>(tail % size);
        uint32_t toEnd = size - at;
        if (toEnd < sizeof(MsgHeader)) {   // a gap too small for a header: the writer went to the start
            r.ring->tail = tail + toEnd;
            continue;
        }
        MsgHeader h;
        memcpy(&h, r.data + at, sizeof h);
        uint32_t len = static_cast<uint32_t>(sizeof h) + Pad8(h.bytes);
        if (h.type == kFiller) {
            r.ring->tail = tail + sizeof h + h.bytes;
            SetEvent(r.spaceEvent);
            continue;
        }
        if (h.bytes > kMaxMessage || len > toEnd) return false;   // (a broken ring: nothing more is read)
        *type = h.type;
        *aux = h.aux;
        payload->assign(reinterpret_cast<const char *>(r.data + at + sizeof h), h.bytes);
        MemoryBarrier();
        r.ring->tail = tail + len;
        SetEvent(r.spaceEvent);
        return true;
    }
}

} // namespace side
