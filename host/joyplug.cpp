// Joysticks plugged in while the game runs: a fix of the game (knowledge/notes/ijoy.md).
//
// Both builds open their DirectInput game controllers once, in InitSystem: IJoy.dll's CreateJoyBase enumerates the
// attached ones (none attached: no base at all), the game makes an IJoy of each into its vector (joyInit), and the
// controls keep their own list of them (controlsInitDevices). A binding names a joystick by its index in that list,
// and giving the profile's bindings to the controls drops a joystick's whose index is not below the list's count. So
// a pad plugged in after the start is never opened, and its bindings, kept in the profile, are dropped until the game
// starts again. A pad that was there at the start and comes back is read again by IJoy's own Update (its Poll fails,
// it acquires the device again) when DirectInput's device object recovers; when it does not, it stays dead.
//
// The loader watches the system's HID device interfaces (CM_Register_Notification: no window, no thread of its own;
// the callback, on a pool thread, only notes the time). Where that is missing (Windows 7, a Wine without it), a thread
// of its own enumerates DirectInput's game controllers every kPollMs instead. On the game thread, before the
// controls' update, once the devices have been quiet for kQuietMs (a plug-in is a burst of arrivals), and again
// kRetryMs later (a driver still starting), it compares DirectInput's attached game controllers (a DirectInput 7 of
// its own, enumerated as IJoy does) with the base's. A controller the base does not have, or one it has whose device
// can neither be polled nor acquired again, has the game's joysticks opened again by the game's own functions, in the
// order InitSystem and the profile use them: the old IJoys' devices let go (force feedback effects may keep an IJoy
// alive, and its exclusive acquisition would keep the device from the new one), the controls' list and bindings
// emptied (controlsRelease), the game's vector and base released (joyDone), all opened again (joyInit), the controls'
// list made of them (controlsInitDevices), the profile's bindings given again (keyBindingsApply), a joystick's
// included. A controller unplugged is no reason: its IJoy stays (its Update reads nothing), every index stays.
//
// Compromises: the force feedback effects the running mission made stay on the old IJoys (C_game::Init makes them
// again at the next mission's load); joyInit calls a null IJoy when a CreateJoy fails (a device unplugged between
// the enumeration and its opening): the loader catches that fault, the devices opened before it are kept.
//
// Bridge methods (game thread): joy.state, joy.rescan {force}, joy.hotplug {on}, joy.drop.
#include "joyplug.h"
#include "host.h"
#define DIRECTINPUT_VERSION 0x0700
#include <dinput.h>
#include <cfgmgr32.h>
#include <atomic>
#include <cstdio>
#include <vector>
#include "json.hpp"
#include "mml/game/game_build.h"

using json = nlohmann::json;

namespace host {
namespace {

constexpr DWORD kQuietMs = 750, kRetryMs = 3000, kPollMs = 2000;
// IID_IDirectInput7A (dinput.h declares it; dxguid.lib would define it)
const GUID kIidDi7 = {0x9A4CB684, 0x236D, 0x11D3, {0x8E, 0x9D, 0x00, 0xC0, 0x4F, 0x68, 0x44, 0xAE}};
const GUID kHidInterfaces = {0x4D1E55B2, 0xF16F, 0x11CF, {0x88, 0xCB, 0x00, 0x11, 0x11, 0x00, 0x00, 0x30}};

// IJoy.dll's objects (the same file in both builds; the layouts read in its code, mafia-re's decompilation):
// C_joy_base +0x10 the device records (a list), +0x24 their count; a record +0x00 its DIDEVICEINSTANCEA, +0x38 the
// next; C_joy +0x0C its record, +0x18 its IDirectInputDevice7A, +0x1B8 a bool: Update acquires again when it fails.
constexpr uintptr_t kBaseDevices = 0x10, kBaseCount = 0x24, kRecNext = 0x38, kJoyRecord = 0x0C, kJoyDevice = 0x18,
                    kJoyAcquired = 0x1B8;

std::atomic<unsigned> g_events{0};   // devices changed (the notification's callback, or the poll thread)
std::atomic<DWORD> g_eventAt{0};
unsigned g_seen;                     // the last change the game thread acted on
bool g_retry;
DWORD g_retryAt;
bool g_on = true;
std::string g_watch = "none", g_watchWhy;
unsigned g_checks, g_reopens;
json g_last;                         // the last check that opened the joysticks again

uint32_t Rd(uintptr_t a) {
    uint32_t v = 0;
    MemRead(a, &v, 4);
    return v;
}

std::string GuidText(const GUID &g) {
    char s[40];
    snprintf(s, sizeof s, "%08lX-%04X-%04X-%02X%02X-%02X%02X%02X%02X%02X%02X", g.Data1, g.Data2, g.Data3, g.Data4[0],
             g.Data4[1], g.Data4[2], g.Data4[3], g.Data4[4], g.Data4[5], g.Data4[6], g.Data4[7]);
    return s;
}

struct Dev {
    GUID guid{};
    std::string name, product;
    uintptr_t record = 0, joy = 0;   // the base's (opened ones only)
};

// ---- DirectInput's attached game controllers, enumerated as IJoy does (DIDEVTYPE_JOYSTICK, attached only)
BOOL CALLBACK EnumAttached(LPCDIDEVICEINSTANCEA d, LPVOID ctx) {
    Dev v;
    v.guid = d->guidInstance;
    v.name = AcpToUtf8(d->tszInstanceName);
    v.product = AcpToUtf8(d->tszProductName);
    static_cast<std::vector<Dev> *>(ctx)->push_back(v);
    return DIENUM_CONTINUE;
}

bool Attached(std::vector<Dev> *out, std::string *err) {
    using CreateFn = HRESULT(WINAPI *)(HINSTANCE, DWORD, REFIID, LPVOID *, LPUNKNOWN);
    HMODULE m = GetModuleHandleW(L"dinput.dll");   // (IJoy.dll's: loaded with it)
    if (!m) m = LoadLibraryW(L"dinput.dll");
    auto create = m ? reinterpret_cast<CreateFn>(GetProcAddress(m, "DirectInputCreateEx")) : nullptr;
    if (!create) { *err = "dinput.dll's DirectInputCreateEx not found"; return false; }
    IDirectInput7A *di = nullptr;
    HRESULT hr = create(GetModuleHandleW(nullptr), DIRECTINPUT_VERSION, kIidDi7, reinterpret_cast<void **>(&di), nullptr);
    if (FAILED(hr) || !di) { *err = "DirectInputCreateEx failed: " + std::to_string(static_cast<long>(hr)); return false; }
    hr = di->EnumDevices(DIDEVTYPE_JOYSTICK, EnumAttached, out, DIEDFL_ATTACHEDONLY);
    di->Release();
    if (FAILED(hr)) { *err = "EnumDevices failed: " + std::to_string(static_cast<long>(hr)); return false; }
    return true;
}

// The base's devices, each with the game's IJoy of it (0 when it has none).
std::vector<Dev> Opened() {
    const game::ExeBuild &e = game::Exe();
    std::vector<Dev> out;
    uintptr_t base = Rd(e.joyBase);
    if (!base) return out;
    uint32_t begin = Rd(e.joyList), end = Rd(e.joyList + 4);
    uintptr_t rec = Rd(base + kBaseDevices);
    for (uint32_t i = 0, n = Rd(base + kBaseCount); rec && i < n && i < 64; i++, rec = Rd(rec + kRecNext)) {
        Dev d;
        d.record = rec;
        uintptr_t inst = Rd(rec);
        char name[MAX_PATH] = {}, product[MAX_PATH] = {};
        MemRead(inst + offsetof(DIDEVICEINSTANCEA, guidInstance), &d.guid, sizeof d.guid);
        MemRead(inst + offsetof(DIDEVICEINSTANCEA, tszInstanceName), name, sizeof name - 1);
        MemRead(inst + offsetof(DIDEVICEINSTANCEA, tszProductName), product, sizeof product - 1);
        d.name = AcpToUtf8(name);
        d.product = AcpToUtf8(product);
        for (uint32_t j = begin; begin && j < end; j += 4)
            if (Rd(Rd(j) + kJoyRecord) == rec) d.joy = Rd(j);
        out.push_back(d);
    }
    return out;
}

// An opened device that can be read: polled, or acquired again (what IJoy's Update does when Poll fails).
bool Readable(uintptr_t joy) {
    auto *dev = joy ? reinterpret_cast<IDirectInputDevice7A *>(Rd(joy + kJoyDevice)) : nullptr;
    if (!dev) return false;
    __try { return SUCCEEDED(dev->Poll()) || SUCCEEDED(dev->Acquire()); }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

// ---- the game's own functions, each guarded (a fault inside is logged; the next step still runs)
using VoidFn = void(__cdecl *)();
using InitFn = void(__fastcall *)(HINSTANCE);
using ThisFn = char(__fastcall *)(uintptr_t self, void *edx);
using DevicesFn = char(__fastcall *)(uintptr_t self, void *edx, uintptr_t igraph, uintptr_t joys, int count);

DWORD SafeVoid(uintptr_t fn) {
    __try { reinterpret_cast<VoidFn>(fn)(); return 0; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return GetExceptionCode(); }
}
DWORD SafeInit(uintptr_t fn, HINSTANCE h) {
    __try { reinterpret_cast<InitFn>(fn)(h); return 0; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return GetExceptionCode(); }
}
DWORD SafeThis(uintptr_t fn, uintptr_t self) {
    __try { reinterpret_cast<ThisFn>(fn)(self, nullptr); return 0; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return GetExceptionCode(); }
}
DWORD SafeDevices(uintptr_t fn, uintptr_t self, uintptr_t igraph, uintptr_t joys, int count) {
    __try { reinterpret_cast<DevicesFn>(fn)(self, nullptr, igraph, joys, count); return 0; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return GetExceptionCode(); }
}
void LetGo(uintptr_t joy) {
    const uint8_t no = 0;
    MemWrite(joy + kJoyAcquired, &no, 1);
    auto *dev = reinterpret_cast<IDirectInputDevice7A *>(Rd(joy + kJoyDevice));
    if (!dev) return;
    __try { dev->Unacquire(); }
    __except (EXCEPTION_EXECUTE_HANDLER) {}
}

// The game's joysticks opened again (game thread, before the controls' update); with `open` false only let go, as a
// start with none attached leaves them (joy.drop: a plug-in tested without a controller).
json Reopen(const std::string &why, bool open = true) {
    const game::ExeBuild &e = game::Exe();
    json faults = json::array();
    auto note = [&](const char *step, DWORD code) {
        if (!code) return;
        char c[16];
        snprintf(c, sizeof c, "0x%08lX", code);
        faults.push_back({{"step", step}, {"exception", c}});
        HWARN("joysticks: %s faulted (%s)%s", step, c,
              strcmp(step, "joyInit") ? "" : ": a device could not be opened (the game's own start would have crashed); the ones before it are kept");
    };
    size_t before = Opened().size();
    for (uint32_t j = Rd(e.joyList), end = Rd(e.joyList + 4); j && j < end; j += 4) LetGo(Rd(j));
    note("controlsRelease", SafeThis(e.controlsRelease, e.controls));
    note("joyDone", SafeVoid(e.joyDone));
    if (open) note("joyInit", SafeInit(e.joyInit, GetModuleHandleW(nullptr)));
    uint32_t begin = Rd(e.joyList), end = Rd(e.joyList + 4);
    int count = begin && end > begin ? static_cast<int>((end - begin) / 4) : 0;
    note("controlsInitDevices", SafeDevices(e.controlsInitDevices, e.controls, Rd(e.igraph), begin, count));
    note("keyBindingsApply", SafeThis(e.keyBindingsApply, e.loadSave));
    if (open) g_reopens++;
    std::vector<Dev> now = Opened();
    json names = json::array();
    for (const Dev &d : now) names.push_back(d.name);
    HLOG("joysticks %s (%s): %u before, %u now%s%s", open ? "opened again" : "dropped", why.c_str(), static_cast<unsigned>(before),
         static_cast<unsigned>(now.size()), now.empty() ? "" : ": ", names.empty() ? "" : names.dump(-1, ' ', false, json::error_handler_t::replace).c_str());
    return {{"reopened", open}, {"dropped", !open}, {"why", why}, {"before", before}, {"opened", names}, {"joysticks", count},
            {"faults", faults}, {"tick", GetTickCount()}};
}

// Compares DirectInput's attached game controllers with the game's; opens them again when one is new, or back and
// not readable (or when forced).
json Check(bool force, const char *when) {
    g_checks++;
    std::vector<Dev> att, have = Opened();
    std::string err;
    if (!Attached(&att, &err)) {
        HWARN("joysticks: %s", err.c_str());
        return {{"reopened", false}, {"error", err}};
    }
    std::string why;
    for (const Dev &a : att) {
        const Dev *h = nullptr;
        for (const Dev &d : have)
            if (IsEqualGUID(d.guid, a.guid)) h = &d;
        if (!h) { why = "new: " + a.name; break; }
        if (!Readable(h->joy)) { why = "back but not readable: " + a.name; break; }
    }
    if (why.empty() && force) why = "asked";
    if (why.empty()) return {{"reopened", false}, {"attached", att.size()}, {"opened", have.size()}, {"when", when}};
    g_last = Reopen(why);
    g_last["when"] = when;
    return g_last;
}

// ---- the watch: device interfaces' arrivals (a pool thread), or a poll of DirectInput's list (a thread of ours)
void Changed() {
    g_eventAt.store(GetTickCount());
    g_events.fetch_add(1);
}

DWORD CALLBACK OnDevice(HCMNOTIFICATION, PVOID, CM_NOTIFY_ACTION action, PCM_NOTIFY_EVENT_DATA, DWORD) {
    if (action == CM_NOTIFY_ACTION_DEVICEINTERFACEARRIVAL || action == CM_NOTIFY_ACTION_DEVICEINTERFACEREMOVAL) Changed();
    return ERROR_SUCCESS;
}

DWORD WINAPI PollThread(LPVOID) {
    std::string last;
    for (bool first = true;; first = false) {
        std::vector<Dev> att;
        std::string err, now;
        if (Attached(&att, &err))
            for (const Dev &d : att) now += GuidText(d.guid);
        if (!first && now != last) Changed();
        last = now;
        Sleep(kPollMs);
    }
}

json State() {
    std::vector<Dev> att;
    std::string err;
    bool ok = Attached(&att, &err);
    json opened = json::array(), attached = json::array();
    int i = 0;
    for (const Dev &d : Opened()) {
        uint8_t acq = 0;
        if (d.joy) MemRead(d.joy + kJoyAcquired, &acq, 1);
        opened.push_back({{"index", i++}, {"name", d.name}, {"product", d.product}, {"guid", GuidText(d.guid)},
                          {"ijoy", d.joy != 0}, {"acquires", acq != 0}});
    }
    for (const Dev &d : att) attached.push_back({{"name", d.name}, {"product", d.product}, {"guid", GuidText(d.guid)}});
    json out = {{"hotplug", g_on}, {"watch", g_watch}, {"events", g_events.load()}, {"checks", g_checks},
                {"reopens", g_reopens}, {"opened", opened}, {"attached", ok ? attached : json()}, {"last", g_last}};
    if (!g_watchWhy.empty()) out["watch_why"] = g_watchWhy;
    if (!ok) out["attached_error"] = err;
    return out;
}

bool Known() { return game::g_exe && game::Exe().joyInit && game::Exe().controls && game::Exe().loadSave; }

void Watch() {
    using RegisterFn = decltype(&CM_Register_Notification);
    HMODULE cm = LoadLibraryW(L"cfgmgr32.dll");
    auto reg = cm ? reinterpret_cast<RegisterFn>(GetProcAddress(cm, "CM_Register_Notification")) : nullptr;
    if (reg) {
        CM_NOTIFY_FILTER f = {};
        f.cbSize = sizeof f;
        f.FilterType = CM_NOTIFY_FILTER_TYPE_DEVICEINTERFACE;
        f.u.DeviceInterface.ClassGuid = kHidInterfaces;
        HCMNOTIFICATION h = nullptr;
        CONFIGRET r = reg(&f, nullptr, OnDevice, &h);
        if (r == CR_SUCCESS) g_watch = "device interfaces";
        else g_watchWhy = "CM_Register_Notification: " + std::to_string(r);
    } else {
        g_watchWhy = "CM_Register_Notification not found";
    }
    if (g_watch == "none") {
        HANDLE t = CreateThread(nullptr, 0, PollThread, nullptr, 0, nullptr);
        if (t) { g_watch = "poll"; CloseHandle(t); }
    }
    HLOG("joysticks: plugged in while the game runs are opened (watching: %s%s%s)", g_watch.c_str(),
         g_watchWhy.empty() ? "" : "; ", g_watchWhy.c_str());
}

} // namespace

void JoyBeforeControls() {
    static bool started;   // (the game thread's; a plain bool, no initializer to run)
    if (!started) {
        started = true;
        if (Known()) Watch();
    }
    if (!g_on || g_watch == "none") return;
    unsigned ev = g_events.load();
    DWORD now = GetTickCount();
    if (ev != g_seen) {
        if (now - g_eventAt.load() < kQuietMs) return;
        g_seen = ev;
        g_retry = true;
        g_retryAt = now + kRetryMs;
        Check(false, "the devices changed");
    } else if (g_retry && static_cast<int>(now - g_retryAt) >= 0) {
        g_retry = false;
        Check(false, "a moment after the devices changed");
    }
}

bool JoyMethod(const std::string &method, const std::string &paramsJson, std::string *reply, bool *ok) {
    if (method.compare(0, 4, "joy.")) return false;
    *ok = false;
    json p;
    try { p = json::parse(paramsJson.empty() || paramsJson == "null" ? "{}" : paramsJson); } catch (...) {}
    if (!p.is_object()) p = json::object();
    if (!Known()) { *reply = "joy: the game's joysticks are not known for this build"; return true; }
    json out;
    if (method == "joy.state") {
        out = State();
    } else if (method == "joy.rescan") {
        out = Check(p.value("force", false), "asked");
    } else if (method == "joy.drop") {
        out = Reopen("asked", false);
    } else if (method == "joy.hotplug") {
        if (p.contains("on")) g_on = p["on"].get<bool>();
        out = State();
    } else {
        return false;
    }
    *reply = out.dump(-1, ' ', false, json::error_handler_t::replace);
    *ok = true;
    return true;
}

} // namespace host
