// debug_views, web: the mission's pedestrians' web (missions\<name>\check.bin) drawn where it is: the places people
// walk between and the ways between them, what the game builds their routes on (a debug view for mission and mod
// authors; after pet1kk's developer tools, PR 2, which first drew it).
//
// What is drawn, within `range` of the camera (web_nodes): each node a short upright line and a small cross, in the
// colour of its kind (the game's bits: what its searches ask for); each way between two nodes a line, a one-way way
// in orange from where it may be walked. Labels (`labels`, off by default): a node's id, index and kind. The lines
// are in the scene, hidden by what stands in front of them (`through` draws them over everything).
//
// The web is read again when the camera has moved 5 m or every 2 s. Its key (Shift+F6) shows and hides it; Options >
// Mods > Debug views > Web has the rest. Command: debug_views.web.state.
#include <windows.h>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <string>
#include <unordered_map>
#include <vector>
#include "mml/mml.h"
#include "lines.h"
#include "views.h"

#define SECTION "web."

namespace {
const MmlHost *H;
MmlMod *M;
// The mission's world is there to draw over (in play, or under the pause menu: the menus are drawn over the views).
bool Shown() { const int st = H->mission_state(); return st == MML_MISSION_RUNNING || st == MML_MISSION_PAUSED; }
bool g_on;

struct P3 { float x, y, z; };
P3 operator-(P3 a, P3 b) { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
float Len(P3 a) { return std::sqrt(a.x * a.x + a.y * a.y + a.z * a.z); }

std::vector<MmlWebNode> g_nodes;
std::vector<lines::Vertex> g_lines;
int g_count, g_oneWay;
P3 g_readAt{1e9f, 0, 0};
DWORD g_readWhen;
size_t g_drawn;

void Read(const P3 &eye) {
    g_readAt = eye;
    g_readWhen = GetTickCount();
    const float range = static_cast<float>(H->config_number(M, SECTION "range", 80));
    const float at[3] = {eye.x, eye.y, eye.z};
    if (g_nodes.size() < 2048) g_nodes.resize(2048);
    int n = H->web_nodes(M, at, range, g_nodes.data(), static_cast<int>(g_nodes.size()));
    if (n > static_cast<int>(g_nodes.size()) && n < 100000) {
        g_nodes.resize(static_cast<size_t>(n));
        n = H->web_nodes(M, at, range, g_nodes.data(), static_cast<int>(g_nodes.size()));
    }
    g_count = (std::max)(0, (std::min)(n, static_cast<int>(g_nodes.size())));
    g_lines.clear();
    g_oneWay = 0;
    std::unordered_map<int, const MmlWebNode *> byIndex;
    for (int i = 0; i < g_count; i++) byIndex[g_nodes[i].index] = &g_nodes[i];
    for (int i = 0; i < g_count; i++) {
        const MmlWebNode &w = g_nodes[i];
        const uint32_t col = lines::Palette(w.kind);
        const float p[3] = {w.pos[0], w.pos[1] + 0.05f, w.pos[2]};
        const float top[3] = {p[0], p[1] + 0.6f, p[2]};
        lines::Add(g_lines, p, top, col);
        const float a[3] = {p[0] - 0.25f, p[1], p[2]}, b[3] = {p[0] + 0.25f, p[1], p[2]}, c[3] = {p[0], p[1], p[2] - 0.25f}, d[3] = {p[0], p[1], p[2] + 0.25f};
        lines::Add(g_lines, a, b, col);
        lines::Add(g_lines, c, d, col);
        for (int k = 0; k < w.link_count && k < MML_WEB_LINKS; k++) {
            auto it = byIndex.find(w.link[k]);
            if (it == byIndex.end()) continue;   // (beyond the range)
            const bool oneWay = (w.link_flags[k] & MML_WEB_ONE_WAY) != 0;
            if (!oneWay && w.link[k] < w.index) continue;   // (a two-way way drawn once)
            g_oneWay += oneWay;
            const float q[3] = {it->second->pos[0], it->second->pos[1] + 0.05f, it->second->pos[2]};
            lines::Add(g_lines, p, q, oneWay ? 0xFFFF9030u : (col & 0x00FFFFFFu) | 0xC0000000u);
        }
    }
}

bool Eye(P3 *eye) {
    float o[3], d[3];
    if (!H->camera_ray(M, 400, 300, o, d)) return false;
    *eye = {o[0], o[1], o[2]};
    return true;
}

void OnFrame(const MmlEvent *, void *) {
    if (!g_on || !Shown()) return;
    P3 eye;
    if (!Eye(&eye)) return;
    if (Len(eye - g_readAt) > 5 || GetTickCount() - g_readWhen > 2000) Read(eye);
    if (H->config_number(M, SECTION "labels", 0) == 0) return;
    const float labels = static_cast<float>(H->config_number(M, SECTION "label_range", 15));
    for (int i = 0, shown = 0; i < g_count && shown < 40; i++) {   // (nearest first)
        const MmlWebNode &w = g_nodes[i];
        P3 p{w.pos[0], w.pos[1], w.pos[2]};
        if (Len(p - eye) > labels) break;
        char b[64];
        snprintf(b, sizeof b, "node %u (#%d), kind 0x%X, %d way(s)", static_cast<unsigned>(w.id), w.index, static_cast<unsigned>(w.kind), w.link_count);
        const float at[3] = {p.x, p.y + 0.7f, p.z};
        shown += lines::Label(H, M, at, {b}, lines::Palette(w.kind));
    }
}

void OnRenderWorld(const MmlEvent *ev, void *) {
    if (!g_on || !Shown()) return;
    g_drawn = lines::Draw(H, ev, g_lines, H->config_number(M, SECTION "through", 0) != 0);
}

void ReadSettings() {
    g_on = H->config_number(M, SECTION "show", 0) != 0;
    g_readWhen = 0;
    H->hotkey(M, H->config_string(M, SECTION "hotkey", "Shift+F6"), [](void *) { g_on = !g_on; H->config_set(M, SECTION "show", g_on ? "true" : "false"); }, nullptr);
}

// debug_views.web.state: the web read around the camera
int State(const char *, MmlReply *reply, void *) {
    char b[256];
    snprintf(b, sizeof b, "{\"show\":%s,\"nodes\":%d,\"one_way_ways\":%d,\"lines\":%zu,\"drawn\":%zu", g_on ? "true" : "false", g_count, g_oneWay,
             g_lines.size() / 2, g_drawn);
    std::string out = b;
    if (g_count) {
        const MmlWebNode &w = g_nodes[0];
        snprintf(b, sizeof b, ",\"nearest\":{\"index\":%d,\"id\":%u,\"kind\":%u,\"pos\":[%.2f,%.2f,%.2f],\"ways\":%d}", w.index, static_cast<unsigned>(w.id),
                 static_cast<unsigned>(w.kind), w.pos[0], w.pos[1], w.pos[2], w.link_count);
        out += b;
    }
    out += "}";
    H->reply_append(reply, out.c_str(), out.size());
    return 0;
}
} // namespace

int WebLoad(const MmlHost *host, MmlMod *mod) {
    H = host;
    M = mod;
    if (!MML_HAS(host, web_nodes)) return 1;   // a loader older than this view
    ReadSettings();
    host->subscribe(mod, MML_EVENT_FRAME, OnFrame, nullptr);
    host->subscribe(mod, MML_EVENT_RENDER_WORLD, OnRenderWorld, nullptr);
    host->subscribe(mod, MML_EVENT_CONFIG_CHANGED, [](const MmlEvent *, void *) { ReadSettings(); }, nullptr);
    host->subscribe(mod, MML_EVENT_MISSION_CLOSE, [](const MmlEvent *, void *) { g_lines.clear(); g_count = 0; g_readAt = {1e9f, 0, 0}; }, nullptr);
    host->command(mod, "web.state", "the pedestrians' web read around the camera: its nodes, its one-way ways, the nearest node", State, nullptr);
    host->log(mod, MML_LOG_INFO, "web loaded (game %s)", host->game_build());
    return 0;
}

void WebUnload() {}
