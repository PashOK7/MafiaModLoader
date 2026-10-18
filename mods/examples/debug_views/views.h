// debug_views' parts: each a view of its own (its file, its hooks and events, its settings under its section of the
// config, its command), loaded and unloaded by debug_views.cpp. A Load returns 0 when the view runs, else 1 (the log
// says why); the mod runs while one of them does.
#pragma once
#include "mml/mml.h"

int RoutesLoad(const MmlHost *host, MmlMod *mod);     // routes.cpp: the people's and the cars' routes
void RoutesUnload();
int TriggersLoad(const MmlHost *host, MmlMod *mod);   // triggers.cpp: the mission scripts' detectors and areas
void TriggersUnload();
int RaceLoad(const MmlHost *host, MmlMod *mod);       // race.cpp: the race of mission 6
void RaceUnload();
bool RaceShown();                                     // the race view is on and a race runs (its walls are painted)
int WallsLoad(const MmlHost *host, MmlMod *mod);      // walls.cpp: the race's walls nothing draws (part of race)
void WallsUnload();
int CollisionLoad(const MmlHost *host, MmlMod *mod);  // collision.cpp: the static world's collision, every shape, by surface
void CollisionUnload();
int WebLoad(const MmlHost *host, MmlMod *mod);        // web.cpp: the pedestrians' web (check.bin)
void WebUnload();
int NamesLoad(const MmlHost *host, MmlMod *mod);      // names.cpp: the actors' names and kinds
void NamesUnload();
