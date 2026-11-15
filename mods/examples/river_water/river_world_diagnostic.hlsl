// Keep native geometry diagnostics in their own compilation unit. Roll
// the same wave refinement and texture taps used by these views.
#define MML_WATER_TRACE_REFINE_LOOP [loop]
#define MML_SCENE_STAGE_LOOP [loop]
#define MML_NATIVE_TEXTURE_COMPACT 1
#include "river_water.hlsl"
