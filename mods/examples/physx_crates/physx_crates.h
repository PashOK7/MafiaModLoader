// physx_crates: the records its two halves share through channels (physx_crates.cpp, the 32-bit half;
// physx_crates64.cpp, the 64-bit half). Fixed-size fields only, no pointers: the same layout in both processes (the
// static_asserts below are checked by both builds).
//
// "setup" (32 -> 64, written when a pile is made, moved or brought back): the bodies, where they are and how they move,
// and the static world around them, as the game's own collision keeps it (world_shapes). Each new version is a new
// PhysX scene.
// "frame" (32 -> 64, written at each FRAME): the frame, its time, whether the world runs, and the moving objects
// around the pile (people, cars: the game's collision boxes and spheres), which push the bodies.
// "poses" (64 -> 32, written once the frame's step is done): the bodies after that frame's step.
#pragma once
#include <stdint.h>

namespace crates {

const uint32_t kMaxBodies = 64;
const uint32_t kMaxMovers = 48;
const uint32_t kMaxStatics = 8192;

enum : uint32_t { kTriangle = 1, kBox = 2, kSphere = 3, kPole = 4,   // (mml.h MML_SHAPE_*)
                  kCapsule = 5 };                                     // a person: upright, from his feet to his head

// A body: its place, its rotation (x y z w, PhysX's order and handedness: the game's rotation is its conjugate), its
// velocities.
struct Body {
    float p[3];
    float q[4];
    float v[3];
    float w[3];
};

// A shape of the static world. triangle: v[0..8] its corners, v[9..11] its normal (out of the solid); box: v[0..2] the
// centre, v[3..5] the half sizes, v[6..14] the axes (rows); sphere: v[0..2] the centre, v[3] the radius; pole: v[0] x,
// v[1] z, v[3] the radius (it stands from far below to far above).
struct Static {
    uint32_t kind;
    uint32_t pad;
    float v[15];
    float pad2;
};

struct Setup {
    uint32_t serial;        // the pile's (the poses name it)
    uint32_t bodies;
    float half;             // a crate's half size (a cube), metres
    float mass;             // kg
    float lo[3], hi[3];     // the region the statics cover; a body below lo[1] - 20 is lost
    uint32_t statics;
    uint32_t pad;
    Body body[kMaxBodies];
    Static shape[kMaxStatics];
};

// A moving object (world_shapes MML_WORLD_MOVING): a box (center, half, axes), a sphere (center, half[0]) or a capsule
// (center, half[0] its radius, half[1] the half length of its segment, upright).
struct Mover {
    uint64_t id;            // the game's object (its primitive's address): the same one from frame to frame
    uint32_t kind;          // kBox, kSphere
    uint32_t pad;
    float center[3];
    float half[3];
    float axes[9];
    float pad2;
};

struct Frame {
    uint64_t frame;         // the 32-bit half's frame number
    int64_t qpc;            // QueryPerformanceCounter at FRAME
    float dt;               // the frame's seconds
    uint32_t run;           // 1: the mission's world runs (0: paused, loading: the bodies stay)
    uint32_t serial;        // the pile it goes with
    uint32_t movers;
    Mover mover[kMaxMovers];
};

struct Poses {
    uint64_t frame;         // the frame stepped (Frame::frame)
    int64_t askQpc;         // its Frame::qpc
    int64_t doneQpc;        // when the step was over (the same clock: QueryPerformanceCounter is the machine's)
    uint32_t serial;        // the pile
    uint32_t bodies;
    float stepMs;           // the step's time (PhysX's simulate and fetchResults, its substeps)
    uint32_t substeps;
    uint32_t awake;         // bodies not asleep
    uint32_t lost;          // bodies fallen out of the region
    uint32_t movers;        // kinematic bodies in the scene
    uint32_t triangles;     // the static world's triangles in the scene
    Body body[kMaxBodies];
};

static_assert(sizeof(Body) == 52, "Body");
static_assert(sizeof(Static) == 72, "Static");
static_assert(sizeof(Mover) == 80, "Mover");
static_assert(sizeof(Setup) == 48 + 52 * kMaxBodies + 72 * kMaxStatics, "Setup");
static_assert(sizeof(Frame) == 32 + 80 * kMaxMovers, "Frame");
static_assert(sizeof(Poses) == 56 + 52 * kMaxBodies, "Poses");

} // namespace crates
