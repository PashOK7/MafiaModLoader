#ifndef MML_ATMOSPHERE_H
#define MML_ATMOSPHERE_H
#include "mml.h"

/* Fully initialized neutral API defaults, not a weather preset. Explicitly set extinction/color for a scene.
 * Kept in a C-compatible helper so C, C++ and other native authors need no loader-private headers. */
static inline MmlAtmosphereSettings mml_atmosphere_defaults(void) {
    MmlAtmosphereSettings s = {0};
    s.size = sizeof s;
    s.scale_height = 60.0f;
    s.color[0] = s.color[1] = s.color[2] = 0.5f;
    s.sky_distance = 8000.0f;
    s.max_opacity = 0.995f;
    s.sky_blend = 1.0f;
    s.anisotropy = 0.55f;
    s.sky_falloff = 8.0f;
    return s;
}
#endif
