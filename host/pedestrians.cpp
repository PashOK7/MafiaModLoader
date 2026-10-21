// The traffic's pedestrians (mml.h pedestrians_list; docs/MODDING.md "Traffic pedestrians").
//
// The traffic's walkers are not actors. The pedestrian manager (an actor: game_build.h pedManagerVtable) keeps its
// elements in an array of 0x200-byte records (manager +0x7C, their count +0x84), and the game's own list of every
// element ([begin, end) at pedElements: what the pedestrians' hits and the cars' sweeps go through) holds those and the
// ones outside the array (a driver who parked and got out walks on as an element of his own). An element: +0x0C
// spawned, +0x10 its place (the world's), +0x40 its model frame, +0x15C its health (below 0: dead; some seconds later
// the manager gives the element to a new pedestrian, its health above zero again). The same layouts in both builds
// (mafia-ragdoll's core.cpp; the fields read in both builds' pedHit and pedBulletHit).
//
// Bridge methods (game thread): pedestrians.list {all}.
#include "host.h"
#include <algorithm>
#include <string>
#include <vector>
#include "json.hpp"
#include "mml/game/game_build.h"
#include "mml/game/game.h"

using json = nlohmann::json;

namespace host {
namespace {

constexpr int kMgrArray = 0x7C, kMgrCount = 0x84, kElementSize = 0x200;
constexpr int kPedActive = 0x0C, kPedPos = 0x10, kPedModel = 0x40, kPedHealth = 0x15C;

template <class F> bool Guarded(F &f) {
    __try { f(); return true; } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

void Collect(std::vector<void *> *out) {
    out->clear();
    if (!game::g_exe || !game::Exe().pedElements || !game::Exe().pedManagerVtable || !game::Mission()) return;
    auto f = [&] {
        std::vector<void *> actors;
        game::ListActors(&actors);
        for (void *a : actors) {
            if (game::at<uintptr_t>(a, 0) != game::Exe().pedManagerVtable) continue;
            uint8_t *arr = game::at<uint8_t *>(a, kMgrArray);
            int n = game::at<int>(a, kMgrCount);
            for (int i = 0; arr && i < n && i < 256; i++) out->push_back(arr + i * kElementSize);
        }
        void **b = *reinterpret_cast<void ***>(game::Exe().pedElements);
        void **e = *reinterpret_cast<void ***>(game::Exe().pedElements + 4);
        if (b && e >= b && e - b <= 1024)
            for (void **p = b; p < e; p++)
                if (*p && std::find(out->begin(), out->end(), *p) == out->end()) out->push_back(*p);
    };
    if (!Guarded(f)) out->clear();
}

} // namespace

std::vector<void *> PedestrianElements() {
    std::vector<void *> v;
    Collect(&v);
    return v;
}

bool PedestrianKnown(void *element) {
    if (!element) return false;
    std::vector<void *> v;
    Collect(&v);
    return std::find(v.begin(), v.end(), element) != v.end();
}

void *PedestrianOfFrame(void *model) {
    if (!model) return nullptr;
    std::vector<void *> v;
    Collect(&v);
    for (void *e : v) {
        void *m = nullptr;
        auto f = [&] { m = game::at<void *>(e, kPedModel); };
        if (Guarded(f) && m == model) return e;
    }
    return nullptr;
}

bool PedestrianRead(void *element, MmlPedestrian *out) {
    auto f = [&] {
        out->size = sizeof *out;
        out->element = element;
        out->model = game::at<void *>(element, kPedModel);
        const V3 &p = game::at<V3>(element, kPedPos);
        out->pos = {p.x, p.y, p.z};
        out->health = game::at<float>(element, kPedHealth);
        out->alive = out->health > 0;
        out->spawned = game::at<uint8_t>(element, kPedActive) != 0;
    };
    return Guarded(f);
}

float PedestrianHealth(void *element) {
    float h = 0;
    auto f = [&] { h = game::at<float>(element, kPedHealth); };
    Guarded(f);
    return h;
}

bool PedestriansMethod(const std::string &method, const std::string &paramsJson, std::string *reply, bool *ok) {
    if (method != "pedestrians.list") return false;
    *ok = false;
    json p = json::parse(paramsJson.empty() ? "null" : paramsJson, nullptr, false);
    if (!p.is_object()) p = json::object();
    bool all = p.value("all", false);
    json list = json::array();
    for (void *e : PedestrianElements()) {
        MmlPedestrian d{};
        if (!PedestrianRead(e, &d) || (!all && (!d.spawned || !d.model))) continue;
        char b[2][16];
        snprintf(b[0], sizeof b[0], "0x%08X", static_cast<unsigned>(reinterpret_cast<uintptr_t>(d.element)));
        snprintf(b[1], sizeof b[1], "0x%08X", static_cast<unsigned>(reinterpret_cast<uintptr_t>(d.model)));
        list.push_back({{"element", b[0]}, {"model", b[1]}, {"pos", {d.pos.x, d.pos.y, d.pos.z}}, {"health", d.health},
                        {"alive", d.alive != 0}, {"spawned", d.spawned != 0}});
    }
    *reply = json({{"count", list.size()}, {"pedestrians", list}}).dump(-1, ' ', false, json::error_handler_t::replace);
    *ok = true;
    return true;
}

} // namespace host
