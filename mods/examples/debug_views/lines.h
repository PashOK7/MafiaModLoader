// debug_views' lines and labels in the world, shared by the views (web's lines; every view's labels).
// Lines: drawn with the game's own device at RENDER_WORLD, inside the scene (depth-tested: what stands in front hides
// them, as race.cpp draws its trail; `through` draws them over the world instead); the loader keeps the game's state
// around that event. Labels: the frame's 2D drawing (800x600 units) where camera_project puts a point.
#pragma once
#include <windows.h>
#include <d3d9.h>
#include <algorithm>
#include <cmath>
#include <string>
#include <vector>
#include "mml/mml.h"

namespace lines {

struct Vertex { float x, y, z; uint32_t color; };   // D3DFVF_XYZ | D3DFVF_DIFFUSE

inline void Add(std::vector<Vertex> &v, const float a[3], const float b[3], uint32_t argb) {
    v.push_back({a[0], a[1], a[2], argb});
    v.push_back({b[0], b[1], b[2], argb});
}
// A circle of radius r around c, in the plane `plane` (0 the ground's, 1 x-y, 2 z-y), in `segs` pieces.
inline void Circle(std::vector<Vertex> &v, const float c[3], float r, int plane, int segs, uint32_t argb) {
    for (int k = 0; k < segs; k++) {
        float p[2][3];
        for (int e = 0; e < 2; e++) {
            const float t = 6.2831853f * (k + e) / segs, u = std::cos(t) * r, w = std::sin(t) * r;
            p[e][0] = c[0] + (plane == 2 ? 0 : u);
            p[e][1] = c[1] + (plane == 0 ? 0 : w);
            p[e][2] = c[2] + (plane == 0 ? w : plane == 2 ? u : 0);
        }
        Add(v, p[0], p[1], argb);
    }
}

// The device for drawing into the scene (at RENDER_WORLD of the mission's world), its states set: the points in the
// world, the vertices' colours, blended, depth-tested (`through`: not) and never written; null: not there.
inline IDirect3DDevice9 *Begin(const MmlHost *h, const MmlEvent *ev, bool through) {
    if (!MML_EVENT_HAS(ev, world) || !ev->world) return nullptr;
    auto *d = static_cast<IDirect3DDevice9 *>(h->render_device(9));
    if (!d) return nullptr;
    D3DMATRIX world = {};
    world._11 = world._22 = world._33 = world._44 = 1;   // (the points are in the world already)
    d->SetTransform(D3DTS_WORLD, &world);
    d->SetVertexShader(nullptr);
    d->SetPixelShader(nullptr);
    d->SetFVF(D3DFVF_XYZ | D3DFVF_DIFFUSE);
    d->SetTexture(0, nullptr);
    d->SetTextureStageState(0, D3DTSS_COLOROP, D3DTOP_SELECTARG1);
    d->SetTextureStageState(0, D3DTSS_COLORARG1, D3DTA_DIFFUSE);
    d->SetTextureStageState(0, D3DTSS_ALPHAOP, D3DTOP_SELECTARG1);
    d->SetTextureStageState(0, D3DTSS_ALPHAARG1, D3DTA_DIFFUSE);
    d->SetTextureStageState(1, D3DTSS_COLOROP, D3DTOP_DISABLE);
    d->SetRenderState(D3DRS_LIGHTING, FALSE);
    d->SetRenderState(D3DRS_FOGENABLE, FALSE);
    d->SetRenderState(D3DRS_CULLMODE, D3DCULL_NONE);
    d->SetRenderState(D3DRS_FILLMODE, D3DFILL_SOLID);
    d->SetRenderState(D3DRS_ZENABLE, through ? D3DZB_FALSE : D3DZB_TRUE);
    d->SetRenderState(D3DRS_ZFUNC, D3DCMP_LESSEQUAL);
    d->SetRenderState(D3DRS_ZWRITEENABLE, FALSE);
    d->SetRenderState(D3DRS_ALPHATESTENABLE, FALSE);
    d->SetRenderState(D3DRS_ALPHABLENDENABLE, TRUE);
    d->SetRenderState(D3DRS_SRCBLEND, D3DBLEND_SRCALPHA);
    d->SetRenderState(D3DRS_DESTBLEND, D3DBLEND_INVSRCALPHA);
    return d;
}

// Primitives of `per` vertices each (2 a line list, 3 a triangle list), in draws of at most 60000 (within every
// device's limit): how many were drawn.
inline size_t DrawList(IDirect3DDevice9 *d, D3DPRIMITIVETYPE type, size_t per, const std::vector<Vertex> &v) {
    const size_t count = v.size() / per;
    for (size_t at = 0; d && at < count; at += 60000) {
        const UINT n = static_cast<UINT>((std::min)(count - at, static_cast<size_t>(60000)));
        d->DrawPrimitiveUP(type, n, &v[at * per], sizeof(Vertex));
    }
    return d ? count : 0;
}

// The lines drawn into the scene (at RENDER_WORLD of the mission's world): how many were drawn.
inline size_t Draw(const MmlHost *h, const MmlEvent *ev, const std::vector<Vertex> &v, bool through) {
    if (v.size() < 2) return 0;
    return DrawList(Begin(h, ev, through), D3DPT_LINELIST, 2, v);
}

// Filled triangles (three vertices each) drawn into the scene, translucent; how many were drawn.
inline size_t DrawTriangles(const MmlHost *h, const MmlEvent *ev, const std::vector<Vertex> &v, bool through) {
    if (v.size() < 3) return 0;
    return DrawList(Begin(h, ev, through), D3DPT_TRIANGLELIST, 3, v);
}

// A colour scaled (its alpha kept): a face's shade.
inline uint32_t Shade(uint32_t argb, float k, uint32_t alpha) {
    k = (std::max)(0.0f, (std::min)(1.0f, k));
    const uint32_t r = static_cast<uint32_t>(((argb >> 16) & 255) * k), g = static_cast<uint32_t>(((argb >> 8) & 255) * k),
                   b = static_cast<uint32_t>((argb & 255) * k);
    return (alpha << 24) | (r << 16) | (g << 8) | b;
}

// The labels drawn this frame by every view of the mod (debug_views.cpp clears them first each frame): a label
// that would cover one drawn before it goes up past it, so no two lie over each other.
struct Placed { float x0, y0, x1, y1; };
inline std::vector<Placed> &PlacedThisFrame() {
    static std::vector<Placed> placed;
    return placed;
}
// Places a view keeps for itself on the screen (the collision view's line under the crosshair): every label of every
// view goes round them, whatever order the views draw in (kept from frame to frame: Reserve again each frame it is
// drawn, Unreserve when it is not).
struct Reserved { int id; Placed box; };
inline std::vector<Reserved> &ReservedPlaces() {
    static std::vector<Reserved> kept;
    return kept;
}
inline void Reserve(int id, float x0, float y0, float x1, float y1) {
    for (Reserved &r : ReservedPlaces()) if (r.id == id) { r.box = {x0, y0, x1, y1}; return; }
    ReservedPlaces().push_back({id, {x0, y0, x1, y1}});
}
inline void Unreserve(int id) {
    std::vector<Reserved> &k = ReservedPlaces();
    k.erase(std::remove_if(k.begin(), k.end(), [id](const Reserved &r) { return r.id == id; }), k.end());
}
inline void NewFrame() {
    PlacedThisFrame().clear();
    for (const Reserved &r : ReservedPlaces()) PlacedThisFrame().push_back(r.box);
}

// A label of a few lines over a point of the world (its box's foot on the point; the first line in `argb`, the rest
// light grey), kept on the screen and off the labels drawn before it this frame. False when the point is behind the
// camera, or no free place is near it (a crowd of labels: the later ones left out).
inline bool Label(const MmlHost *h, MmlMod *m, const float at[3], const std::vector<std::string> &text, uint32_t argb) {
    float s[3];
    if (text.empty() || !h->camera_project(m, at, s)) return false;
    const float size = 13;
    float w = 0;
    for (const std::string &l : text) w = (std::max)(w, h->draw_text_width(MML_FONT_SMALL, size, l.c_str()));
    const float half = w / 2 + 4, tall = size * text.size() + 4;
    if (s[0] + half < 0 || s[0] - half > 800 || s[1] < 0 || s[1] - tall > 600) return false;   // (its place is off the screen)
    float x = (std::max)(half, (std::min)(800 - half, s[0])), y = (std::max)(tall, (std::min)(600.0f, s[1]));
    std::vector<Placed> &placed = PlacedThisFrame();
    bool free = false;
    for (int moved = 0; moved < 24 && !free; moved++) {
        free = true;
        for (const Placed &b : placed)
            if (x - half < b.x1 && x + half > b.x0 && y - tall < b.y1 && y > b.y0) { y = b.y0 - 1; free = false; }
        if (y - tall < 0) return false;   // (pushed off the top: no room near its point)
    }
    if (!free) return false;
    placed.push_back({x - half, y - tall, x + half, y});
    h->draw_rect(m, x - half, y - tall, half * 2, tall, 0x90000000u, 1);
    for (size_t k = 0; k < text.size(); k++)
        h->draw_text(m, x, y - tall + 2 + size * k, size, k ? 0xFFE0E0E0u : argb, MML_FONT_SMALL, MML_DRAW_CENTER, text[k].c_str());
    return true;
}

// A colour of a small palette for a number (a material, a kind): the same number, the same colour.
inline uint32_t Palette(uint32_t n) {
    static const uint32_t k[12] = {0xFFFFC040u, 0xFF40E0FFu, 0xFFFF6090u, 0xFF80FF50u, 0xFFB080FFu, 0xFFFF9030u,
                                   0xFF5090FFu, 0xFFFFFF60u, 0xFF40FFC0u, 0xFFFF60FFu, 0xFFC0FF40u, 0xFFFF4040u};
    return k[(n * 2654435761u >> 16) % 12];
}

} // namespace lines
