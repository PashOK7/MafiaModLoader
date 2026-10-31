// A frame and the camera's last second, captured from the same Present. Ctrl+F11; camera_capture.capture/replay/stop.
// No addresses, thread or Windows input hook: the host's render events, camera and hotkey services (both builds).
#include <windows.h>
#include <d3d9.h>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <deque>
#include <string>
#include <vector>
#include "json.hpp"
#include "mml/mml.h"
#include "mml/path.h"

using json = nlohmann::json;
namespace {
const MmlHost *H;
MmlMod *M;
struct Pose { ULONGLONG tick; MmlViewCamera camera; float projection[16]; int width, height; };
std::deque<Pose> g_history;
std::vector<Pose> g_snapshot, g_replay;
bool g_requested, g_pending, g_playing, g_done, g_realtime;
size_t g_index;
ULONGLONG g_started;
unsigned g_serial;
std::string g_dir, g_last, g_error;
constexpr size_t kMaxFrames = 4096;

std::string Dump(const json &v) { return v.dump(2, ' ', false, json::error_handler_t::replace); }
int Reply(MmlReply *r, const json &v) { std::string s = Dump(v); H->reply_append(r, s.c_str(), s.size()); return 0; }
int Fail(MmlReply *r, const std::string &s) { H->reply_error(r, s.c_str()); return 1; }
void Note(const std::string &s) { H->log(M, MML_LOG_INFO, "%s", s.c_str()); H->hud_console(M, s.c_str(), 0xFFFFD060); }
bool Exists(const std::string &p) { wchar_t w[1024]; return mml_widen(p.c_str(), w, 1024) && GetFileAttributesW(w) != INVALID_FILE_ATTRIBUTES; }
json Read(const std::string &p) {
    FILE *f = mml_fopen(p.c_str(), "rb");
    if (!f) return nullptr;
    if (fseek(f, 0, SEEK_END)) { fclose(f); return nullptr; }
    long n = ftell(f);
    if (n < 0 || n > 4 * 1024 * 1024 || fseek(f, 0, SEEK_SET)) { fclose(f); return nullptr; }
    std::string s(static_cast<size_t>(n), '\0');
    bool ok = fread(s.data(), 1, s.size(), f) == s.size(); fclose(f);
    return ok ? json::parse(s, nullptr, false, true) : json();
}
bool Write(const std::string &p, const std::string &s) {
    if (Exists(p)) return false;   // captures are immutable; never replace an earlier report
    FILE *f = mml_fopen(p.c_str(), "wb");
    if (!f) return false;
    bool ok = fwrite(s.data(), 1, s.size(), f) == s.size();
    return fclose(f) == 0 && ok;
}
std::vector<std::string> Names() {
    wchar_t pattern[1024]; std::vector<std::string> out;
    if (!mml_widen((g_dir + "camera-*.json").c_str(), pattern, 1024)) return out;
    WIN32_FIND_DATAW data{}; HANDLE h = FindFirstFileW(pattern, &data);
    if (h == INVALID_HANDLE_VALUE) return out;
    do {
        if (data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
        // Our generated basenames are ASCII, regardless of the game folder's spelling.
        std::string s; bool ascii = true;
        for (const wchar_t *p = data.cFileName; *p; ++p) { if (*p > 127) { ascii = false; break; } s += static_cast<char>(*p); }
        if (ascii) out.push_back(s.substr(0, s.size() - 5));
    } while (FindNextFileW(h, &data));
    FindClose(h); std::sort(out.begin(), out.end()); return out;
}
bool SafeName(std::string *s) {
    if (s->size() > 5 && s->substr(s->size() - 5) == ".json") s->resize(s->size() - 5);
    if (s->empty() || s->size() > 128) return false;
    for (char c : *s) if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-' || c == '_')) return false;
    return true;
}
json Settings() {
    json out = json::object();
    for (const char *id : {"river_water", "dlss_nr", "devtools", "loader"}) {
        std::string root = H->game_dir();
        json defaults = Read(root + "mml/mods/" + id + "/config.json");
        json changes = Read(root + "mml/data/" + id + "/config.json");
        json effective = defaults.is_object() ? defaults : json::object();
        if (changes.is_object()) effective.merge_patch(changes);
        out[id] = {{"effective_from_files", effective}, {"changes", changes}};
    }
    return out;
}

// A borrowed Direct3D 9 device (render_device); same resolve/readback path as the host's screenshot, only on demand.
std::string Bitmap(const std::string &path, int *width, int *height) {
    auto *d = static_cast<IDirect3DDevice9 *>(H->render_device(9));
    if (!d) return "screenshot needs the Direct3D 9 wrapper";
    IDirect3DSurface9 *bb = nullptr, *rt = nullptr, *sys = nullptr;
    D3DSURFACE_DESC desc{};
    HRESULT hr = d->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, &bb);
    if (SUCCEEDED(hr)) hr = bb->GetDesc(&desc);
    if (SUCCEEDED(hr) && (!desc.Width || !desc.Height || static_cast<uint64_t>(desc.Width) * desc.Height > 32 * 1024 * 1024)) hr = E_INVALIDARG;
    if (SUCCEEDED(hr)) hr = d->CreateRenderTarget(desc.Width, desc.Height, D3DFMT_X8R8G8B8, D3DMULTISAMPLE_NONE, 0, FALSE, &rt, nullptr);
    if (SUCCEEDED(hr)) hr = d->StretchRect(bb, nullptr, rt, nullptr, D3DTEXF_NONE);
    if (SUCCEEDED(hr)) hr = d->CreateOffscreenPlainSurface(desc.Width, desc.Height, D3DFMT_X8R8G8B8, D3DPOOL_SYSTEMMEM, &sys, nullptr);
    if (SUCCEEDED(hr)) hr = d->GetRenderTargetData(rt, sys);
    D3DLOCKED_RECT lr{};
    if (SUCCEEDED(hr)) hr = sys->LockRect(&lr, nullptr, D3DLOCK_READONLY);
    std::string error;
    if (SUCCEEDED(hr)) {
        FILE *f = Exists(path) ? nullptr : mml_fopen(path.c_str(), "wb");
        if (!f) error = "cannot create the screenshot";
        else {
            BITMAPFILEHEADER head{}; BITMAPINFOHEADER info{};
            info.biSize = sizeof info; info.biWidth = desc.Width; info.biHeight = -static_cast<LONG>(desc.Height);
            info.biPlanes = 1; info.biBitCount = 32; info.biSizeImage = desc.Width * desc.Height * 4;
            head.bfType = 0x4D42; head.bfOffBits = sizeof head + sizeof info; head.bfSize = head.bfOffBits + info.biSizeImage;
            bool ok = fwrite(&head, 1, sizeof head, f) == sizeof head && fwrite(&info, 1, sizeof info, f) == sizeof info;
            for (UINT y = 0; ok && y < desc.Height; ++y)
                ok = fwrite(static_cast<const uint8_t *>(lr.pBits) + static_cast<size_t>(y) * lr.Pitch, 4, desc.Width, f) == desc.Width;
            if (fclose(f) || !ok) error = "screenshot write failed";
            *width = desc.Width; *height = desc.Height;
        }
        sys->UnlockRect();
    } else { char b[80]; snprintf(b, sizeof b, "screenshot readback failed: 0x%08X", static_cast<unsigned>(hr)); error = b; }
    if (sys) sys->Release(); if (rt) rt->Release(); if (bb) bb->Release();
    return error;
}
void Save() {
    g_pending = false;
    SYSTEMTIME st{}; GetLocalTime(&st);
    char name[128];
    snprintf(name, sizeof name, "camera-%04u%02u%02u-%02u%02u%02u-%03u-%u-%04u", st.wYear, st.wMonth, st.wDay,
             st.wHour, st.wMinute, st.wSecond, st.wMilliseconds, GetCurrentProcessId(), ++g_serial);
    std::string base = name;
    if (Exists(g_dir + base + ".json") || Exists(g_dir + base + ".bmp")) { g_error = "capture name already exists"; Note(g_error); return; }
    int w = 0, h = 0;
    g_error = Bitmap(g_dir + base + ".bmp", &w, &h);
    json samples = json::array();
    ULONGLONG start = g_snapshot.front().tick;
    for (const Pose &p : g_snapshot) {
        samples.push_back({{"ms", p.tick - start}, {"world", p.camera.world},
                           {"frustum", {p.camera.tan_left, p.camera.tan_right, p.camera.tan_down, p.camera.tan_up}},
                           {"near", p.camera.near_z}, {"far", p.camera.far_z}, {"projection_drawn", p.projection}});
    }
    json out = {{"format", "mml-camera-capture"}, {"version", 1}, {"game", H->game_build()}, {"mission", H->mission_name()},
                {"duration_ms", g_snapshot.back().tick - start}, {"viewport", {g_snapshot.back().width, g_snapshot.back().height}},
                {"screenshot", base + ".bmp"}, {"screenshot_size", {w, h}}, {"screenshot_error", g_error},
                {"settings", Settings()}, {"samples", samples},
                {"note", "Camera inputs of each rendered frame; the image is the final Present, possibly containing pipelined DLSS colour. World actors and wave time are not saved."}};
    if (!Write(g_dir + base + ".json", Dump(out))) g_error = "cannot create the camera trace";
    if (g_error.empty()) { g_last = base; Note("Camera capture saved: " + base); }
    else Note("Camera capture: " + g_error);
    g_snapshot.clear();
}
bool Request() {
    if (g_requested || g_pending) return false;
    if (g_history.size() < 2) { g_error = "no camera history yet"; return false; }
    g_requested = true; g_error.clear(); return true;
}
void OnKey(void *) { if (!Request()) Note(g_error.empty() ? "Camera capture already queued" : g_error); }
void SettingsChanged() { H->hotkey(M, H->config_string(M, "hotkey", "Ctrl+F11"), OnKey, nullptr); }
void Observe() {
    MmlRenderCamera c{}; c.size = sizeof c;
    if (!H->render_camera(&c) || !(c.proj[0] > 0 && c.proj[5] > 0)) return;
    Pose p{}; p.tick = GetTickCount64(); p.width = c.width; p.height = c.height;
    memcpy(p.projection, c.proj, sizeof p.projection);
    MmlViewCamera &v = p.camera; v.size = sizeof v; v.flags = MML_VIEW_OWN_SECTOR;
    memcpy(v.world, &c.right, 3 * sizeof(float)); memcpy(v.world + 4, &c.up, 3 * sizeof(float));
    memcpy(v.world + 8, &c.fwd, 3 * sizeof(float)); memcpy(v.world + 12, &c.eye, 3 * sizeof(float)); v.world[15] = 1;
    // The main projection's offsets include DLSS jitter; record them for analysis, do not inject jitter twice on replay.
    v.tan_left = v.tan_right = 1 / c.proj[0]; v.tan_down = v.tan_up = 1 / c.proj[5];
    v.near_z = c.near_z; v.far_z = c.far_z;
    if (!g_history.empty() && p.tick - g_history.back().tick > 2000) g_history.clear();
    g_history.push_back(p);
    while (g_history.size() > 2 && (g_history[1].tick + 1000 < p.tick || g_history.size() > kMaxFrames)) g_history.pop_front();
    if (g_requested) { g_snapshot.assign(g_history.begin(), g_history.end()); g_requested = false; g_pending = true; }
}
int ReplayPose(MmlViewCamera *out, void *) {
    if (!g_playing || g_replay.empty()) return 0;
    if (g_realtime) {
        ULONGLONG t = GetTickCount64() - g_started;
        while (g_index + 1 < g_replay.size() && g_replay[g_index + 1].tick <= t) ++g_index;
    }
    *out = g_replay[g_index].camera;
    g_done = g_index + 1 == g_replay.size();
    if (!g_realtime && !g_done) ++g_index;
    return 1;   // hold the last frame until stop; frame mode draws every recorded pose, even on a slower machine
}
void Stop() { H->camera_override(M, nullptr, nullptr); g_playing = g_done = false; g_replay.clear(); g_index = 0; }
void Event(const MmlEvent *e, void *) {
    if (e->type == MML_EVENT_CONFIG_CHANGED) SettingsChanged();
    else if (e->type == MML_EVENT_MISSION_CLOSE) { g_history.clear(); g_snapshot.clear(); g_requested = g_pending = false; Stop(); }
    else if (e->type == MML_EVENT_RENDER_WORLD && e->world) Observe();
    else if (e->type == MML_EVENT_PRESENT && g_pending) Save();
}
json State() {
    return {{"hotkey", H->config_string(M, "hotkey", "Ctrl+F11")}, {"directory", g_dir}, {"history_frames", g_history.size()},
            {"history_ms", g_history.empty() ? 0 : g_history.back().tick - g_history.front().tick},
            {"queued", g_requested || g_pending}, {"last", g_last}, {"error", g_error},
            {"replay", {{"active", g_playing}, {"done", g_done}, {"frame", g_index}, {"frames", g_replay.size()},
                        {"mode", g_realtime ? "time" : "frames"}}}};
}
int Status(const char *, MmlReply *r, void *) { return Reply(r, State()); }
int Capture(const char *, MmlReply *r, void *) { if (!Request()) return Fail(r, g_error.empty() ? "capture already queued" : g_error); return Reply(r, State()); }
int List(const char *, MmlReply *r, void *) { auto names = Names(); return Reply(r, {{"directory", g_dir}, {"names", names}, {"last", names.empty() ? "" : names.back()}}); }
int End(const char *, MmlReply *r, void *) { Stop(); return Reply(r, State()); }
int Replay(const char *args, MmlReply *r, void *) {
    try {
        json a = json::parse(args && *args ? args : "{}"); if (!a.is_object()) return Fail(r, "replay expects an object");
        std::string name = a.value("name", "last"), mode = a.value("mode", "frames");
        if (name == "last") { auto names = Names(); if (names.empty()) return Fail(r, "no saved camera capture"); name = names.back(); }
        if (!SafeName(&name)) return Fail(r, "name must be a capture basename inside the data directory");
        if (mode != "frames" && mode != "time") return Fail(r, "mode must be frames or time");
        json data = Read(g_dir + name + ".json");
        if (!data.is_object() || data.value("format", "") != "mml-camera-capture" || data.value("version", 0) != 1)
            return Fail(r, "not a version-1 camera capture");
        if (data.value("mission", "") != H->mission_name()) return Fail(r, "load the capture's mission before replay");
        const json &samples = data.at("samples");
        if (!samples.is_array() || samples.empty() || samples.size() > kMaxFrames) return Fail(r, "capture must contain 1..4096 poses");
        std::vector<Pose> poses;
        for (const auto &s : samples) {
            Pose p{}; p.tick = s.at("ms").get<ULONGLONG>();
            if (p.tick > 60000 || (!poses.empty() && p.tick < poses.back().tick)) return Fail(r, "capture times must increase within 60 seconds");
            const auto &w = s.at("world"), &f = s.at("frustum");
            if (!w.is_array() || w.size() != 16 || !f.is_array() || f.size() != 4) return Fail(r, "invalid camera layout");
            for (int i = 0; i < 16; ++i) { p.camera.world[i] = w[i].get<float>(); if (!std::isfinite(p.camera.world[i])) return Fail(r, "nonfinite pose"); }
            float fr[4]; for (int i = 0; i < 4; ++i) { fr[i] = f[i].get<float>(); if (!(fr[i] > 0 && fr[i] < 100)) return Fail(r, "invalid frustum"); }
            float up = 0, forward = 0, cross = 0;
            for (int i = 0; i < 3; ++i) { up += p.camera.world[4+i] * p.camera.world[4+i]; forward += p.camera.world[8+i] * p.camera.world[8+i];
                float c = p.camera.world[4+(i+1)%3] * p.camera.world[8+(i+2)%3] - p.camera.world[4+(i+2)%3] * p.camera.world[8+(i+1)%3]; cross += c*c; }
            if (!(up > 0.1f && forward > 0.1f && cross > 0.01f)) return Fail(r, "degenerate camera basis");
            p.camera.size = sizeof p.camera; p.camera.flags = MML_VIEW_OWN_SECTOR;
            p.camera.tan_left = fr[0]; p.camera.tan_right = fr[1]; p.camera.tan_down = fr[2]; p.camera.tan_up = fr[3];
            p.camera.near_z = s.at("near").get<float>(); p.camera.far_z = s.at("far").get<float>();
            if (!(p.camera.near_z > 0 && p.camera.far_z > p.camera.near_z && std::isfinite(p.camera.far_z))) return Fail(r, "invalid camera range");
            poses.push_back(p);
        }
        if (!H->mission_state()) return Fail(r, "no active mission");
        if (!H->camera_override(M, ReplayPose, nullptr)) return Fail(r, "camera override refused");
        g_replay = std::move(poses); g_index = 0; g_started = GetTickCount64(); g_realtime = mode == "time"; g_playing = true; g_done = false;
        return Reply(r, {{"name", name}, {"state", State()}, {"settings", data.value("settings", json::object())}});
    } catch (const std::exception &e) { return Fail(r, std::string("invalid capture: ") + e.what()); }
}
}
MML_EXPORT uint32_t mml_mod_api(void) { return MML_API_VERSION; }
MML_EXPORT int mml_mod_load(const MmlHost *host, MmlMod *mod) {
    H = host; M = mod;
    if (!MML_HAS(H, hud_console) || !MML_HAS(H, camera_override)) return 1;
    g_dir = H->mod_data_dir(M); SettingsChanged();
    for (auto ev : {MML_EVENT_RENDER_WORLD, MML_EVENT_PRESENT, MML_EVENT_CONFIG_CHANGED, MML_EVENT_MISSION_CLOSE}) H->subscribe(M, ev, Event, nullptr);
    H->command(M, "status", "history, last capture and replay; Ctrl+F11 saves the last second and that frame", Status, nullptr);
    H->command(M, "capture", "queue a screenshot and the last second of camera poses at the next Present", Capture, nullptr);
    H->command(M, "list", "saved capture basenames in the mod's data directory", List, nullptr);
    H->command(M, "replay", "replay {name:last|basename, mode:frames|time}; holds the last pose until stop; settings returned, not applied", Replay, nullptr);
    H->command(M, "stop", "release this mod's replay camera (then reenable devtools freecam if needed)", End, nullptr);
    return 0;
}
MML_EXPORT void mml_mod_unload(MmlMod *) { Stop(); }
