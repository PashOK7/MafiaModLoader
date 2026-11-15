// Same-frame composition reads the reflected radiance written by the water surface pass.
#define MML_WATER_WORLD_MATERIAL 1
#define MML_WATER_TRACE_REFINE_LOOP [loop] [fastopt]
#define MML_WATER_TRACE_STEP_LOOP [loop] [fastopt]
#define MML_RAY_TRAVERSAL_LOOP [loop] [fastopt]
#define MML_SCENE_LAYER_LOOP [loop] [fastopt]
#define MML_WATER_PATH_LOOP [loop] [fastopt]
#define MML_WATER_OPTICS_LOOP [loop]
#define MML_WATER_SKY_LOOP [loop]
#define MML_WATERLINE_LOOP [loop] [fastopt]
#include "river_water.hlsl"
