// The Direct3D 9 device's state followed, not asked (d3dmirror.cpp; ROADMAP item 31b): what the game and everyone
// else set on the device the game draws with, kept from its Set* calls, so the host's work at each draw (the shader
// rules' constants, their shaders set and put back) needs no Get* call, and a shader's constants are written only
// where they differ from what the device holds.
#pragma once
#include <d3d9.h>
#include <string>

namespace host {

struct D3dMirror {
    D3DMATRIX world, view, proj;
    DWORD rs[256];                       // render states (every D3DRS_* is below 256)
    DWORD tss[8][33];                    // texture stage states, stages 0..7
    DWORD samp[4][14];                   // sampler states, samplers 0..3
    IDirect3DBaseTexture9 *tex[8];       // the textures set (not referenced: compared and set back only)
    D3DMATERIAL9 material;
    D3DLIGHT9 light[8];
    BOOL lightOn[8];
    D3DVIEWPORT9 viewport;
    IDirect3DVertexShader9 *vs;          // (not referenced)
    IDirect3DPixelShader9 *ps;
    float vsc[256][4], psc[224][4];      // the shaders' constant registers
};

// Users: while one wants it, the device's Set* calls are followed (the hooks made at the first use, on the device's
// vtable). Game thread.
void MirrorUse(bool on);
// The state of `d` now, read from the device once if it is not held (the mirror's first use, a new frame, a state
// block applied, a Reset, a call from another thread); null when the mirror is not in use or `d` is not the device
// it follows (then ask the device). Game thread.
const D3dMirror *MirrorOf(IDirect3DDevice9 *d);
// Shader constants written where they differ from what the device holds (runs of changed registers, each one call);
// without the mirror, written whole. `pixel`: the pixel shader's.
void MirrorConstants(IDirect3DDevice9 *d, bool pixel, UINT start, const float *v, UINT count);
// The next frame's start (Present): the mirror is read again from the device at its next use.
void MirrorFrame();
// The mirror against the device's own answers at the next `draws` draws (shader.mirror's check): what differs, by name.
void MirrorCheckNext(int draws);
void MirrorCheckDraw(IDirect3DDevice9 *d);
// shader.mirror's answer: whether it is on, its reads, what invalidated it, the last check's mismatches.
std::string MirrorStateJson();

} // namespace host
