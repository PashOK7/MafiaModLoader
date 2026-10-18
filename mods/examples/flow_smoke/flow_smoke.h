// flow_smoke: the records its two halves share (flow_smoke.cpp, the 32-bit half; flow_smoke64.cpp, the 64-bit half).
// Fixed-size fields only, no pointers: the same layout in both processes (the static_asserts are checked by both).
//
// "frame" (32 -> 64, at each FRAME): the frame, its time, whether the world runs, the sources of smoke and fire, the
// ground under them, the scene's light. Positions are about `origin` (the city's coordinates are thousands of metres;
// Flow works about the origin).
// "scenery" (32 -> 64, when it changes): the static world around the sources as the game draws it: the triangles of
// its meshes (scene_pieces), in one soup about the origin. Flow keeps the gas out of them.
// "movers" (32 -> 64, at each FRAME): what moves there, as meshes too: each car's own (actor_meshes: its body, doors,
// wheels where they are now) and each person's body made of capsules along his bones (the host has no skinned
// meshes), each vertex with its velocity: they push the gas aside and drag it along.
// "view" (32 -> 64, in the graphics pass, after the pass has put the frame into "color_in" and "depth_in"): the
// camera this frame was drawn with; the 64-bit half ray-marches the gas into "color_out" over it.
// "done" (64 -> 32): that frame's picture is whole in "color_out" (or why not).
//
// The textures (the 64-bit half's, made at the frame's size, opened by the bridge's Direct3D 11 device):
// "color_in"  R16G16B16A16_FLOAT  the frame so far, linear (the game's colours decoded from their gamma)
// "depth_in"  R32_FLOAT           its depth as the projection in "view" makes it (0 near .. 1 far; the sky 1)
// "color_out" R16G16B16A16_FLOAT  the frame with the gas over it, linear
#pragma once
#include <stdint.h>

namespace fsm {

const uint32_t kMaxSources = 16;
const uint32_t kMaxSceneVertices = 1u << 18, kMaxSceneTriangles = 1u << 18;
const uint32_t kMaxMovers = 32, kMaxMoverVertices = 1u << 16, kMaxMoverTriangles = 1u << 16;

enum : uint32_t { kSmoke = 1, kFire = 2, kSteam = 3 };   // Source::kind
enum : uint32_t { kCar = 1, kPerson = 2 };                // Mover::kind

// A source: a sphere giving off gas.
struct Source {
    uint32_t id;            // the 32-bit half's (flow_smoke.emit's reply)
    uint32_t kind;          // kSmoke, kFire, kSteam
    float at[3];            // its centre, about the origin
    float radius;           // metres
    float velocity[3];      // what it gives the gas, m/s (its own motion added: a car's exhaust)
    float strength;         // 0..: how much it gives (1: the default)
};

struct Frame {
    uint64_t frame;
    double time;            // seconds since the 32-bit half's load (the simulation's clock)
    float dt;               // this frame's, seconds
    uint32_t run;           // 0: the world stands (a menu, no mission): no step
    double origin[3];       // world = origin + a position here
    uint32_t clear;         // a new number: everything blown away (flow_smoke.clear, a mission closed)
    uint32_t sources;
    float ground;           // the ground's height under the sources, about the origin (the gas does not go below)
    float wind[3];          // m/s, the air's own motion
    float toSun[3];         // towards the sun (unit): Flow's self-shadowing lights the gas from there
    float light[3];         // the scene's light on the gas (its sun and ambient, linear): smoke is as bright as it
    float pad[2];
    Source source[kMaxSources];
};

// "scenery": this header, then float positions[3 * vertices], then uint32_t indices[3 * triangles].
struct Scenery {
    uint32_t serial;        // a new one each time it is written
    uint32_t vertices, triangles;
    uint32_t pieces;        // the scene's pieces it was made of
};

// "movers": this header, then float positions[3 * vertices], float velocities[3 * vertices], uint32_t indices[3 *
// triangles]; a mover's indices count from its own first vertex.
struct Mover {
    uint32_t kind;          // kCar, kPerson
    uint32_t firstVertex, vertices;
    uint32_t firstTriangle, triangles;
    uint32_t topology;      // changes when its triangles change (another car, a door's piece gone): else only the
                            // positions and velocities are new
    uint64_t id;            // the actor (the 32-bit half's address)
};
struct Movers {
    uint64_t frame;
    uint32_t count, vertices, triangles, pad;
    Mover mover[kMaxMovers];
};

struct View {
    uint64_t frame;
    int64_t askQpc;         // QueryPerformanceCounter when the 32-bit half asked
    uint32_t width, height; // the frame's (the textures are made at this size)
    float view[16], proj[16];   // row vectors, world about the origin: p * view * proj
    uint32_t inGeneration[2];   // the generations of "color_in" and "depth_in" the 32-bit half wrote
};

struct Done {
    uint64_t frame;         // View::frame answered
    int64_t askQpc;         // its View::askQpc
    int64_t doneQpc;        // "color_out" whole (the CUDA copy over)
    uint32_t ok;            // 1: "color_out" holds this frame's picture
    uint32_t blocks;        // Flow's active blocks (0: no gas anywhere)
    uint32_t outGeneration; // "color_out"'s generation
    uint32_t steps;         // simulation steps so far
    float simulateMs;       // the last step's CPU time (recording and submitting)
    float renderMs;         // the frame's: copies in, ray march, copies out, waited for on the CPU
    float gpuMs;            // the last frame's GPU time in Flow (its profiler; 0 unknown)
    uint32_t sceneTriangles;   // the scenery's triangles in Flow now
    char error[160];        // the last error ("" none)
};

const uint32_t kSceneryBytes = sizeof(Scenery) + 12 * kMaxSceneVertices + 12 * kMaxSceneTriangles;
const uint32_t kMoversBytes = sizeof(Movers) + 24 * kMaxMoverVertices + 12 * kMaxMoverTriangles;

static_assert(sizeof(Source) == 40, "Source");
static_assert(sizeof(Frame) == 104 + 40 * kMaxSources, "Frame");
static_assert(sizeof(Scenery) == 16, "Scenery");
static_assert(sizeof(Mover) == 32, "Mover");
static_assert(sizeof(Movers) == 24 + 32 * kMaxMovers, "Movers");
static_assert(sizeof(View) == 160, "View");
static_assert(sizeof(Done) == 216, "Done");

} // namespace fsm
