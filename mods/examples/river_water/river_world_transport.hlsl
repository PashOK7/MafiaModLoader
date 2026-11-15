// This pass uses the same raster surface and depth test as composition, and owns the ray traversal.
#define MML_WATER_WORLD_TRANSPORT 1
#define MML_RAY_DIRECTIONAL_ORDER 1
#include "river_world.hlsl"
