// debug_views: what the game does, drawn where it does it, for whoever writes missions and mods (both builds). Four
// views, each in its own file and its own section of the settings (Options > Mods > Debug views: a tab each), each
// shown and hidden by its own key:
// - routes (routes.cpp, F6): the route each person and car the game moves follows, where it is sent now, its way;
// - triggers (triggers.cpp, F7): the mission scripts' detectors, their radii and use points, the scene's areas;
// - race (race.cpp, F8): the race of mission 6: its gates, the player's zone, the racers, their places and laps;
// - walls (walls.cpp, F9): the static world's collision walls that nothing draws, painted in the frame.
// Their commands: debug_views.routes.state, debug_views.triggers.list, debug_views.race.state, debug_views.walls.state.
// A view that cannot run here (a build it does not know, a refused hook) says so in the log; the others run.
#include "views.h"

MML_EXPORT uint32_t mml_mod_api(void) { return MML_API_VERSION; }

MML_EXPORT int mml_mod_load(const MmlHost *host, MmlMod *mod) {
    const int routes = RoutesLoad(host, mod), triggers = TriggersLoad(host, mod), race = RaceLoad(host, mod),
              walls = WallsLoad(host, mod);
    return routes && triggers && race && walls ? 1 : 0;   // (1 from each: none of them runs)
}

MML_EXPORT void mml_mod_unload(MmlMod *) {
    RoutesUnload();
    TriggersUnload();
    RaceUnload();
    WallsUnload();
}
