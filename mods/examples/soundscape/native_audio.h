// Read-only views of native LS3D audio. Both builds checked in Ghidra, 2026-11-14.
// Render 1.0 +5ED80 / 1.2 +62030 reads the driver sound vector at +38, count +40.
// Sound vtables are already verified in game_build.h. SetRange +54EF0/+57DD0 writes
// +168/+170; SetVolume +550F0/+57FD0 writes +18C. IsPlaying +54D60/+57C40:
// stdcall(frame,bool), false logical playback, true the backend's current state.
// Source filename getter +564D0/+59370 reads its refcounted string at +C, chars +8.
// Environment update +48600/+4AAA0: camera +3F8 selects sector, sector +168 properties.
#pragma once
#include <windows.h>
#include <algorithm>
#include <cmath>
#include <string>
#include <vector>
#include "mml/mml.h"
#include "mml/game/game.h"

namespace audio_view {
struct Eax {
    uint32_t environment;
    float size, diffusion;
    int32_t room, roomHF, roomLF;
    float decay, decayHF, decayLF;
    int32_t reflections;
    float reflectionsDelay, reflectionsPan[3];
    int32_t reverb;
    float reverbDelay, reverbPan[3], echoTime, echoDepth, modulationTime, modulationDepth;
    float airAbsorptionHF, hfReference, lfReference, roomRolloff;
    uint32_t flags;
};
static_assert(sizeof(Eax) == 112, "EAX3 property packet");
struct Source {
    uintptr_t frame = 0;
    std::string name;
    float pos[3]{}, nearRange = 0, farRange = 0, volume = 0;
    bool logical = false, voiced = false, loop = false;
    uint32_t started = 0; // native SetOn's timeGetTime stamp, +150 in both builds
    uintptr_t buffer = 0; // ISND sound +8, native DirectSound buffer
    int type = 0;
};
struct Snapshot {
    std::vector<Source> sources;
    Eax eax{};
    bool eaxActive = false;
    uintptr_t sector = 0;
    std::string sectorName;
    unsigned registered = 0;
    float master = 0;
};
template<class T> T Read(const MmlHost *h, uintptr_t at) {
    T out{};
    if (at) h->mem_read(at, &out, sizeof out);
    return out;
}
inline std::string Text(const MmlHost *h, uintptr_t at) {
    char out[192]{};
    if (at) h->mem_read(at, out, sizeof out - 1);
    return game::TextUtf8(out);
}
inline bool Playing(uintptr_t frame, bool backend) {
    using Fn = bool(__stdcall *)(void *, bool);
    __try {
        return reinterpret_cast<Fn>((*reinterpret_cast<uintptr_t **>(frame))[0x54/4])(
            reinterpret_cast<void *>(frame), backend);
    } __except(EXCEPTION_EXECUTE_HANDLER) { return false; }
}
inline bool Environment(uintptr_t property, Eax *out) {
    const GUID id{0xA8FA6882,0xB476,0x11D3,{0xBD,0xB9,0x00,0xC0,0xF0,0x2D,0xDF,0x87}};
    using Fn = HRESULT(__stdcall *)(void *,const GUID &,ULONG,void *,ULONG,void *,ULONG,ULONG *);
    __try {
        ULONG bytes = 0;
        const HRESULT hr = reinterpret_cast<Fn>((*reinterpret_cast<uintptr_t **>(property))[3])(
            reinterpret_cast<void *>(property), id, 1, nullptr, 0, out, sizeof *out, &bytes);
        return SUCCEEDED(hr) && bytes == sizeof *out && std::isfinite(out->decay) &&
            out->decay >= .1f && out->decay <= 20 && std::isfinite(out->size) &&
            out->size >= 1 && out->size <= 100 && std::isfinite(out->diffusion);
    } __except(EXCEPTION_EXECUTE_HANDLER) { return false; }
}
inline Snapshot Capture(const MmlHost *h, MmlMod *mod) {
    Snapshot out;
    if (!game::g_exe || !game::g_ls3d) return out;
    const uintptr_t base = h->module_base("LS3DF.dll");
    const uintptr_t driver = Read<uintptr_t>(h, game::Exe().i3dDriver);
    const uintptr_t list = driver ? Read<uintptr_t>(h, driver + 0x38) : 0;
    const unsigned count = driver ? Read<unsigned>(h, driver + 0x40) : 0;
    if (list && count <= 4096) {
        out.registered = count;
        std::vector<uintptr_t> frames(count);
        if (count && h->mem_read(list, frames.data(), count * sizeof(uintptr_t))) {
            for (uintptr_t f : frames) {
                if (!f || Read<uintptr_t>(h, f) != base + game::Ls3d().soundVtable) continue;
                Source s;
                s.frame = f;
                s.logical = Playing(f, false);
                if (!s.logical) continue;
                s.voiced = Playing(f, true);
                const uintptr_t voice = Read<uintptr_t>(h, f + 0x15C);
                s.buffer = voice ? Read<uintptr_t>(h, voice + 8) : 0;
                s.type = Read<int>(h, f + 0x19C);
                s.loop = Read<uint8_t>(h, f + 0x164) != 0;
                s.started = Read<uint32_t>(h, f + 0x150);
                s.volume = Read<float>(h, f + 0x18C);
                s.nearRange = Read<float>(h, f + 0x168);
                s.farRange = Read<float>(h, f + 0x170);
                float matrix[16]{};
                if (!h->frame_world(mod, reinterpret_cast<void *>(f), matrix)) continue;
                std::copy(matrix + 12, matrix + 15, s.pos);
                const uintptr_t source = Read<uintptr_t>(h, f + 0x160);
                const uintptr_t str = source ? Read<uintptr_t>(h, source + 0xC) : 0;
                s.name = str ? Text(h, str + 8) : Text(h, Read<uintptr_t>(h, f + game::kFrameName));
                if (std::isfinite(s.volume) && std::isfinite(s.nearRange) && std::isfinite(s.farRange) &&
                    std::isfinite(s.pos[0]) && std::isfinite(s.pos[1]) && std::isfinite(s.pos[2]))
                    out.sources.push_back(std::move(s));
            }
        }
    }
    const uintptr_t camera = Read<uintptr_t>(h, base + game::Ls3d().currentCamera);
    out.sector = camera ? Read<uintptr_t>(h, camera + 0x3F8) : 0;
    if (out.sector) out.sectorName = Text(h, Read<uintptr_t>(h, out.sector + game::kFrameName));
    const uintptr_t sound = Read<uintptr_t>(h, base + game::Ls3d().sndDriver);
    const unsigned flags = sound ? Read<unsigned>(h, sound + 0x30) : 0;
    out.master = sound ? Read<float>(h, sound + 0x34) : 0;
    if(!std::isfinite(out.master))out.master=0;
    const uintptr_t prop = sound ? Read<uintptr_t>(h, sound + 0x10) : 0;
    if (prop && (flags & 0x200000)) out.eaxActive = Environment(prop, &out.eax);
    return out;
}
struct Backend { bool valid=false; float gain=0; DWORD cursor=0,frequency=0; long millibels=-10000; };
inline Backend Meter(uintptr_t buffer) {
    Backend out;
    if(!buffer)return out;
    __try {
        void *b=reinterpret_cast<void *>(buffer);
        const auto vt=*reinterpret_cast<uintptr_t **>(b);
        using Volume=HRESULT(__stdcall *)(void *,long *);
        using Cursor=HRESULT(__stdcall *)(void *,DWORD *,DWORD *);
        using Frequency=HRESULT(__stdcall *)(void *,DWORD *);
        DWORD write=0;
        out.valid=SUCCEEDED(reinterpret_cast<Volume>(vt[6])(b,&out.millibels))&&
            SUCCEEDED(reinterpret_cast<Cursor>(vt[4])(b,&out.cursor,&write));
        reinterpret_cast<Frequency>(vt[8])(b,&out.frequency);
        if(out.valid)out.gain=out.millibels<=-10000?0:std::pow(10.0f,out.millibels/2000.0f);
    } __except(EXCEPTION_EXECUTE_HANDLER) { out.valid=false; }
    return out;
}
} // namespace audio_view
