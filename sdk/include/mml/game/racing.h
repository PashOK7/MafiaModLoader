// The race of mission 6 (the autodrom, `mise06-autodrom`: C_racing, its circuit, its racers and their gates), as each
// build keeps it. Mission 06-03 "Before The Race" starts it (racing_mission6_init); outside a race the global is null.
//
// Read in the running race of both labs (save 105 of profile 0: the race's own start, the player at 7 of 13 in 1.2,
// 12 of 13 in 1.0 by the HUD) and checked against the code that uses each field (1.2: RacingMisse06 0x549D00, the
// racers' tick 0x555E40, the gate check 0x558190; 1.0: racing.md's PC table, the DoGates code at 0x616510). Where the
// builds differ, the offsets differ: a field is named for the build that has it, in kRacing12 / kRacing10 below.
//
// What is shared: a web node (C_web_node, a 30-byte record) starts with its position as three floats; a gate
// (C_racing_gate, 0x2C bytes) holds its track node at +0, its outer marker at +4 (the gate's line runs between the
// two: its midpoint is their average), the next gate at +0x14 and its number at +0x1C; a racer's name is its first
// 0x20 bytes, its car (C_car, an actor) at +0x28, the player's own entrant has C_player at +0x2C (0 for the others).
#pragma once
#include <cstdint>
#include "game_build.h"

namespace game {

struct RacingLayout {
    uintptr_t global;         // C_racing* (the singleton: null outside the race)
    int circuit;              // C_racing: the current C_circuit*
    int player;               // C_racing: the player's entrant, embedded (a C_opponent)
    int racers;               // C_racing: 20 C_opponent* (the live racers, the player among them; 0 = empty slot)
    int laps;                 // C_racing: the race's lap count (the HUD's "Lap n/laps")
    int gates;                // C_circuit: vector<C_racing_gate*>, begin at this offset, end at +4
    int entrantZone;          // C_opponent: the zone parameter t: 0 at the previous gate's line, 1 at the current one's
    int entrantGate;          // C_opponent: the gate the racer heads for (a C_racing_gate*)
    int entrantPlace;         // C_opponent: its place, 1 first (the HUD's "Pos n/racers")
    int entrantLap;           // C_opponent: laps completed (the HUD shows it plus one)
};

inline constexpr int kRacerSlots = 20;        // the live array's length in both builds
inline constexpr int kEntrantName = 0x00;     // char[0x20], the driver's name ("Tommy Angelo" for the player)
inline constexpr int kEntrantCar = 0x28;      // C_car* (an actor: its frame at game::kActorFrame)
inline constexpr int kEntrantPlayer = 0x2C;   // C_player*, set for the player's entrant only
inline constexpr int kGateNode = 0x00;        // C_web_node* of the track node (the gate's one end)
inline constexpr int kGateOuter = 0x04;       // C_web_node* of the outer marker (the other end)
inline constexpr int kGateNext = 0x14;        // C_racing_gate* of the next gate on the circuit
inline constexpr int kGateNumber = 0x1C;      // the gate's number: its index in the circuit's vector (0 = the finish)
inline constexpr int kNodeX = 0x00;           // C_web_node: its position, three floats

// 1.2 (Game.exe 1.2): RacingMisse06 writes g_pRacing at 0x646DBC; the player's name at C_racing+0x3C.
inline constexpr RacingLayout kRacing12 = {
    0x646DBC,  // global
    0x2C,      // circuit
    0x3C,      // player
    0x238,     // racers
    0x28C,     // laps
    0x44,      // gates
    0x4C,      // entrantZone
    0x54,      // entrantGate
    0x5C,      // entrantPlace
    0x64,      // entrantLap
};

// 1.0 (Game.exe 1.0): g_racing at 0x6F958C (racing.md); the player's name at C_racing+0x40.
inline constexpr RacingLayout kRacing10 = {
    0x6F958C,  // global
    0x30,      // circuit
    0x40,      // player
    0x228,     // racers
    0x27C,     // laps
    0x44,      // gates
    0x4C,      // entrantZone
    0x5C,      // entrantGate
    0x64,      // entrantPlace
    0x6C,      // entrantLap
};

// The layout of the build running, or null when it is not a build the mod knows (Detect first).
inline const RacingLayout *Racing() {
    if (g_exe == &kExe12) return &kRacing12;
    if (g_exe == &kExe10) return &kRacing10;
    return nullptr;
}

} // namespace game
