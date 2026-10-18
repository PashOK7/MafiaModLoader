// physx_gpu: the records its two halves share (physx_gpu.cpp, the 32-bit half; physx_gpu64.cpp, the 64-bit half).
// Fixed-size fields only, no pointers: the same layout in both processes (the static_asserts are checked by both).
//
// "ops" (32 -> 64): what the player asked for (water poured, a cloth dropped, a jelly dropped, everything cleared), the
// last kOps of them by number, with the static world of the region they happen in (the game's own collision,
// world_shapes), so that two asked in one frame are both done.
// "frame" (32 -> 64, at each FRAME): the frame, its time, whether the world runs, the moving objects in the region.
// "out" (64 -> 32, once the frame's step is copied): which shared texture holds the frame's points, how many of each,
// where the scene's origin is (PhysX works about it: the city's coordinates are thousands of metres).
// "topology" (64 -> 32, when a cloth or a jelly is made): their triangles.
//
// The points: two shared textures "pos0" and "pos1" (R32_FLOAT, kTexWidth x kRows), written in turn; a point is four
// texels of a row (x, y, z about the origin, w: PhysX's inverse mass), kPointsPerRow a row; the water from row
// kFluidRow, the cloth's vertices from kClothRow, the jelly's surface from kJellyRow.
#pragma once
#include <stdint.h>

namespace gpux {

const uint32_t kTexWidth = 4096, kPointsPerRow = kTexWidth / 4;
const uint32_t kMaxFluid = 131072, kMaxCloth = 16384, kMaxJelly = 16384;
const uint32_t kFluidRow = 0, kClothRow = kMaxFluid / kPointsPerRow, kJellyRow = kClothRow + kMaxCloth / kPointsPerRow,
               kRows = kJellyRow + kMaxJelly / kPointsPerRow;
const uint32_t kMaxClothTris = 32768, kMaxJellyTris = 32768;
const uint32_t kMaxMovers = 48, kMaxStatics = 8192, kOps = 8;

enum : uint32_t { kTriangle = 1, kBox = 2, kSphere = 3, kPole = 4, kCapsule = 5 };   // (mml.h MML_SHAPE_*, a person)
enum : uint32_t { kOpPour = 1, kOpCloth = 2, kOpJelly = 3, kOpClear = 4 };

// A shape of the static world (world coordinates). triangle: v[0..8] its corners, v[9..11] its normal; box: v[0..2]
// the centre, v[3..5] the half sizes, v[6..14] the axes (rows); sphere: v[0..2], v[3] the radius; pole: v[0] x, v[1] z,
// v[3] the radius.
struct Static {
    uint32_t kind;
    uint32_t pad;
    float v[15];
    float pad2;
};

// One thing asked for: where (world), facing where, how big.
struct Op {
    uint32_t serial;        // 1, 2, 3... (0: none)
    uint32_t kind;          // kOp*
    float at[3];            // its centre (the water's block, the cloth's middle, the jelly's centre)
    float dir[3];           // the player's facing (flat)
    float size;             // metres: the water block's side, the cloth's side, the jelly's side
    uint32_t count;         // the water's particles (at most kMaxFluid); the cloth's vertices along a side
    float param;            // the water: its particles' spacing; the cloth, the jelly: stiffness (Young's modulus, Pa)
    uint32_t pad;
};

struct Ops {
    uint32_t region;        // the region's serial: a new one, new statics
    uint32_t statics;
    float lo[3], hi[3];     // the region (world)
    Op op[kOps];            // the last ones, any order (by serial)
    Static shape[kMaxStatics];
};

struct Mover {
    uint64_t id;
    uint32_t kind;          // kBox, kSphere, kCapsule (center, half[0] radius, half[1] the segment's half length)
    uint32_t pad;
    float center[3];
    float half[3];
    float axes[9];
    float pad2;
};

struct Frame {
    uint64_t frame;
    int64_t qpc;
    float dt;
    uint32_t run;
    uint32_t movers;
    uint32_t lastOp;        // the newest Op::serial in "ops" (read only when it is new: "ops" is large)
    Mover mover[kMaxMovers];
};

struct Out {
    uint64_t frame;         // the frame stepped
    int64_t askQpc;         // its Frame::qpc
    int64_t doneQpc;        // its points whole in the texture (QueryPerformanceCounter)
    uint32_t texture;       // 0: "pos0", 1: "pos1"
    uint32_t fluid, cloth, jelly;   // points of each
    float origin[3];        // world = origin + the points
    float radius;           // the water's particles' radius (what is drawn)
    float lo[3], hi[3];     // the region (world): the 32-bit half sends the movers in it
    uint32_t topology;      // Topology::version
    uint32_t substeps;
    float stepMs;           // simulate and fetchResults (the CPU's wait for the GPU in them)
    float copyMs;           // the points into the texture (CUDA)
    uint32_t region;        // Ops::region applied
    uint32_t lastOp;        // the last Op::serial done
    char error[128];        // the last error ("" none)
};

struct Topology {
    uint32_t version;
    uint32_t clothTris, jellyTris;   // triangles (3 indices each): the cloth's first, then the jelly's; each indexes its
    uint32_t pad;                    // own points (0 .. Out::cloth, 0 .. Out::jelly)
    uint32_t index[3 * (kMaxClothTris + kMaxJellyTris)];
};

static_assert(sizeof(Static) == 72, "Static");
static_assert(sizeof(Op) == 48, "Op");
static_assert(sizeof(Ops) == 32 + 48 * kOps + 72 * kMaxStatics, "Ops");
static_assert(sizeof(Mover) == 80, "Mover");
static_assert(sizeof(Frame) == 32 + 80 * kMaxMovers, "Frame");
static_assert(sizeof(Out) == 232, "Out");

} // namespace gpux
