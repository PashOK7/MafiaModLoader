// debug_views, walls: the walls of the static world that have no drawing (collision only), painted in the frame.
//
// What they are: the game's static collision has triangles of a material (the surface number in the game's own
// table: MmlHit's flags >> 24, MmlShape's flags the same): material 33 is a wall nothing draws, the race's barriers
// and the hill's sides (1.0: a wall 50 m wide and 55 m high beside the car at gates 10-11; 1.2: the start straight's
// walls). The walls are triangles, so each one is tested as a ray against the triangle, pixel by pixel: where the
// frame's depth says the nearest thing is farther than the wall (or the sky), the wall is shown; where a drawn object
// is in front, it hides the wall. The look is a glowing edge, a grid over the face, a fresnel rim and a band that rises
// through it every few seconds.
//
// Commands: debug_views.walls.state (the walls near the player, their corners and normals, counts by material).
// Settings (config.json): show and the hotkey (F9), the range the walls are read within, the grid's
// cell, the band's period, the colour and the strength.
#include <windows.h>
#include <d3d11.h>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <map>
#include <string>
#include <vector>
#include "mml/mml.h"
#include "mml/game/game_build.h"
#include "mml/game/game.h"
#include "shaders/PsWalls.h"
#include "views.h"

#define SECTION "walls."   // its settings' keys, under its section of the config

namespace {
const MmlHost *H;
MmlMod *M;

struct Settings {
    float range = 60, grid = 1.0f, period = 8.0f, strength = 1.0f;
    float color[3] = {0.30f, 0.85f, 1.00f};
    bool show = true;
} S;

constexpr int kMaxWalls = 128;     // triangles drawn: the nearest ones
struct Wall { float c[3][3]; float normal[3]; };
std::vector<Wall> g_walls;         // the walls around the player, nearest first
std::vector<MmlShape> g_scan;      // the buffer world_shapes fills (the static world's shapes in the box)
DWORD g_scanAt;                    // when the walls were last read (every 250 ms)
size_t g_seen;                     // the shapes the last read saw
std::map<uint32_t, int> g_materials;   // the static triangles near the player, by material (the last read)
// The surface number of the walls nothing draws (found in 1.0 on the hill beside gates 10-11 and in 1.2 on the start
// straight): the one material the mod shows. Other materials are not walls to it.
constexpr uint32_t kWallMaterial = 33;

ID3D11PixelShader *g_ps;
ID3D11Buffer *g_cb;
unsigned g_madeFor;                // the device generation they were made on

// cbuffer Walls (b1), its layout in walls.hlsl
struct WallsCb {
    float meta[4];                 // x: the walls, y: the grid's cell, z: the edge's width (m), w: the band's period (s)
    float colour[4];               // rgb, a: the strength
    float tri[kMaxWalls][3][4];    // each wall's three corners
};
static_assert(sizeof(WallsCb) == 32 + kMaxWalls * 48, "Walls' layout");

template <class T> void SafeRelease(T *&p) { if (p) { p->Release(); p = nullptr; } }

// the game's memory, through the host (a stale pointer is a zero, not a fault)
template <class T> T R(uintptr_t a) {
    T v{};
    if (a) H->mem_read(a, &v, sizeof v);
    return v;
}

void ReadSettings() {
    S.show = H->config_number(M, SECTION "show", 1) != 0;
    S.range = static_cast<float>(H->config_number(M, SECTION "range", 60));
    S.grid = static_cast<float>(H->config_number(M, SECTION "grid", 1));
    S.period = static_cast<float>(H->config_number(M, SECTION "period", 8));
    S.strength = static_cast<float>(H->config_number(M, SECTION "strength", 1));
    const char *c = H->config_string(M, SECTION "color", "#4DD9FF");
    if (*c == '#') c++;
    unsigned v = static_cast<unsigned>(strtoul(c, nullptr, 16));
    S.color[0] = ((v >> 16) & 255) / 255.0f;
    S.color[1] = ((v >> 8) & 255) / 255.0f;
    S.color[2] = (v & 255) / 255.0f;
    H->hotkey(M, H->config_string(M, SECTION "hotkey", "F9"), [](void *) { S.show = !S.show; H->config_set(M, SECTION "show", S.show ? "true" : "false"); }, nullptr);
}

// The walls around the player: the static world's triangles of the material, in a box of `range` metres (game thread).
bool g_game;                       // the build is known (game::Detect): the game's addresses are read

void Scan() {
    g_scanAt = GetTickCount();
    g_walls.clear();
    if (!g_game) return;
    const int state = H->mission_state();
    if (state != MML_MISSION_RUNNING && state != MML_MISSION_PAUSED) return;
    // the player's place: his frame's world matrix, read the host's way (a stale pointer is a zero, not a fault)
    void *p = H->human_player(M);   // the host's answer: no game global is read here (the frame of a mission being opened)
    if (!p) return;
    const uintptr_t frame = R<uintptr_t>(reinterpret_cast<uintptr_t>(p) + game::kActorFrame);
    float w[16];
    if (!frame || !H->frame_world(M, reinterpret_cast<void *>(frame), w)) return;
    const V3 at{w[12], w[13], w[14]};
    const float lo[3] = {at.x - S.range, at.y - S.range, at.z - S.range};
    const float hi[3] = {at.x + S.range, at.y + S.range, at.z + S.range};
    if (g_scan.size() < 4096) g_scan.resize(4096);
    // world_shapes returns every shape in the box, and writes only the first `size` of them: a box with more (a range
    // of 150 m has 5000 triangles near the track) would be read past the buffer. Only the written ones are read.
    const int total = H->world_shapes(M, lo, hi, MML_WORLD_STATIC, g_scan.data(), static_cast<int>(g_scan.size()));
    const int n = std::min(total, static_cast<int>(g_scan.size()));
    if (total > n) {
        static bool warned;
        if (!warned) {
            warned = true;
            H->log(M, MML_LOG_WARN, "walls: the box holds %d shapes, the first %d are read", total, n);
        }
    }
    if (n <= 0) return;
    g_seen = static_cast<size_t>(total);
    g_materials.clear();
    std::vector<std::pair<float, Wall>> found;
    for (int k = 0; k < n; k++) {
        const MmlShape &s = g_scan[k];
        if (s.kind != MML_SHAPE_TRIANGLE) continue;
        g_materials[s.flags >> 24]++;   // every material near the player (the state command shows them)
        if ((s.flags >> 24) != kWallMaterial) continue;
        Wall w;
        for (int j = 0; j < 3; j++) for (int i = 0; i < 3; i++) w.c[j][i] = s.corner[j][i];
        for (int i = 0; i < 3; i++) w.normal[i] = s.normal[i];
        const float cx = (w.c[0][0] + w.c[1][0] + w.c[2][0]) / 3 - at.x;
        const float cy = (w.c[0][1] + w.c[1][1] + w.c[2][1]) / 3 - at.y;
        const float cz = (w.c[0][2] + w.c[1][2] + w.c[2][2]) / 3 - at.z;
        found.push_back({cx * cx + cy * cy + cz * cz, w});
    }
    std::sort(found.begin(), found.end(), [](const auto &a, const auto &b) { return a.first < b.first; });
    for (size_t k = 0; k < found.size() && k < static_cast<size_t>(kMaxWalls); k++) g_walls.push_back(found[k].second);
}

bool MakePass(ID3D11Device *d) {
    SafeRelease(g_ps);
    SafeRelease(g_cb);
    if (FAILED(d->CreatePixelShader(kPsWalls, sizeof kPsWalls, nullptr, &g_ps))) return false;
    D3D11_BUFFER_DESC bd{};
    bd.ByteWidth = sizeof(WallsCb);
    bd.Usage = D3D11_USAGE_DEFAULT;
    bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    return SUCCEEDED(d->CreateBuffer(&bd, nullptr, &g_cb));
}

void Pass(const MmlGfxFrame *f, void *) {
    if (!S.show || g_walls.empty()) return;
    auto *d = static_cast<ID3D11Device *>(f->device);
    auto *ctx = static_cast<ID3D11DeviceContext *>(f->context);
    if (g_madeFor != f->generation || !g_ps) {
        if (!MakePass(d)) return;
        g_madeFor = f->generation;
    }
    static WallsCb cb;   // the constants are big: kept, the parts that matter set each frame
    cb.meta[0] = static_cast<float>((std::min)(g_walls.size(), static_cast<size_t>(kMaxWalls)));
    cb.meta[1] = S.grid;
    cb.meta[2] = 0.06f;
    cb.meta[3] = S.period;
    cb.colour[0] = S.color[0];
    cb.colour[1] = S.color[1];
    cb.colour[2] = S.color[2];
    cb.colour[3] = S.strength;
    for (size_t k = 0; k < g_walls.size() && k < static_cast<size_t>(kMaxWalls); k++)
        for (int j = 0; j < 3; j++) {
            cb.tri[k][j][0] = g_walls[k].c[j][0];
            cb.tri[k][j][1] = g_walls[k].c[j][1];
            cb.tri[k][j][2] = g_walls[k].c[j][2];
            cb.tri[k][j][3] = 0;
        }
    ctx->UpdateSubresource(g_cb, 0, nullptr, &cb, 0, 0);
    ctx->PSSetConstantBuffers(1, 1, &g_cb);
    ctx->PSSetShader(g_ps, nullptr, 0);
    ctx->Draw(3, 0);   // the bridge's full-screen triangle (its vertex shader is bound)
}

void OnFrame(const MmlEvent *, void *) {
    if (GetTickCount() - g_scanAt > 250) Scan();
}

// debug_views.walls.state: the walls near the player as data
int State(const char *, MmlReply *reply, void *) {
    std::string out = "{\"material\":" + std::to_string(kWallMaterial) + ",\"walls\":" + std::to_string(g_walls.size()) +
                      ",\"shapes_seen\":" + std::to_string(g_seen) + ",\"show\":" + (S.show ? "true" : "false") + ",\"first\":[";
    char b[320];
    for (size_t k = 0; k < g_walls.size() && k < 6; k++) {
        const Wall &w = g_walls[k];
        snprintf(b, sizeof b, "%s{\"corners\":[[%.2f,%.2f,%.2f],[%.2f,%.2f,%.2f],[%.2f,%.2f,%.2f]],\"normal\":[%.3f,%.3f,%.3f]}",
                 k ? "," : "", w.c[0][0], w.c[0][1], w.c[0][2], w.c[1][0], w.c[1][1], w.c[1][2], w.c[2][0], w.c[2][1], w.c[2][2],
                 w.normal[0], w.normal[1], w.normal[2]);
        out += b;
    }
    out += "],\"materials\":{";   // every material of the triangles near the player: "number": triangles
    for (const auto &m : g_materials) {
        snprintf(b, sizeof b, "%s\"%u\":%d", out.back() == '{' ? "" : ",", static_cast<unsigned>(m.first), m.second);
        out += b;
    }
    out += "}}";
    H->reply_append(reply, out.c_str(), out.size());
    return 0;
}
} // namespace

int WallsLoad(const MmlHost *host, MmlMod *mod) {
    H = host;
    M = mod;
    if (!MML_HAS(host, world_shapes)) return 1;   // a loader older than this mod
    g_game = game::Detect(host->module_base("LS3DF.dll"));   // the build's addresses (before the game is touched)
    if (!g_game) host->log(mod, MML_LOG_WARN, "walls: not a known build of the game: nothing is read");
    ReadSettings();
    host->subscribe(mod, MML_EVENT_FRAME, OnFrame, nullptr);
    host->subscribe(mod, MML_EVENT_CONFIG_CHANGED, [](const MmlEvent *, void *) { ReadSettings(); }, nullptr);
    if (MML_HAS(host, gfx_pass) && host->gfx_pass(mod, 110, Pass, nullptr) <= 0)
        host->log(mod, MML_LOG_WARN, "walls: the graphics bridge refused the pass (gfx_state says why)");
    host->command(mod, "walls.state", "the walls near the player: their material, count, and the nearest corners and normals", State, nullptr);
    host->log(mod, MML_LOG_INFO, "walls loaded (game %s)", host->game_build());
    return 0;
}

void WallsUnload() {
    SafeRelease(g_ps);
    SafeRelease(g_cb);
}
