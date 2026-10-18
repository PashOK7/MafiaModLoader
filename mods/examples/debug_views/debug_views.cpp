// debug_views: what the game does, drawn where it does it, for whoever writes missions and mods (both builds). Seven
// views, each in its own file and its own section of the settings (Options > Mods > Debug views: a tab each), each
// shown and hidden by its own key:
// - routes (routes.cpp, F6): the route each person and car the game moves follows, where it is sent now, its way;
// - triggers (triggers.cpp, F7): the mission scripts' detectors, their radii and use points, the scene's areas;
// - race (race.cpp, F8): the race of mission 6: its gates, the player's zone, the racers, their places and laps;
// - walls (walls.cpp, F9): the static world's collision walls that nothing draws, painted in the frame;
// - collision (collision.cpp, Shift+F9): the static world's whole collision, every shape, by surface and its name;
// - web (web.cpp, Shift+F6): the pedestrians' web, the places people walk between;
// - names (names.cpp, Shift+F8): the actors' names and kinds.
// The last three after pet1kk's developer tools (PR 2); they start hidden.
// Their commands: debug_views.routes.state, debug_views.triggers.list, debug_views.race.state, debug_views.walls.state,
// debug_views.collision.state, debug_views.web.state, debug_views.names.list.
// A view that cannot run here (a build it does not know, a refused hook) says so in the log; the others run.
#include "lines.h"
#include "views.h"

MML_EXPORT uint32_t mml_mod_api(void) { return MML_API_VERSION; }

MML_EXPORT int mml_mod_load(const MmlHost *host, MmlMod *mod) {
    // every view's labels share one frame's places (lines.h): cleared before the views draw (subscribed first)
    host->subscribe(mod, MML_EVENT_FRAME, [](const MmlEvent *, void *) { lines::NewFrame(); }, nullptr);
    const int routes = RoutesLoad(host, mod), triggers = TriggersLoad(host, mod), race = RaceLoad(host, mod),
              walls = WallsLoad(host, mod), collision = CollisionLoad(host, mod), web = WebLoad(host, mod), names = NamesLoad(host, mod);
    return routes && triggers && race && walls && collision && web && names ? 1 : 0;   // (1 from each: none of them runs)
}

MML_EXPORT void mml_mod_unload(MmlMod *) {
    RoutesUnload();
    TriggersUnload();
    RaceUnload();
    WallsUnload();
    CollisionUnload();
    WebUnload();
    NamesUnload();
}
