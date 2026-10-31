/* Compile and execute the public helper/host-table call as plain C, not only C++. */
#include "mml/atmosphere.h"
int kit_atmosphere_c(const MmlHost *host, MmlMod *mod, const MmlAtmosphereSettings *settings) {
    MmlAtmosphereSettings neutral = mml_atmosphere_defaults();
    if (neutral.size != sizeof neutral || neutral.extinction != 0 || neutral.sky_distance <= 0) return 0;
    if (!MML_HAS(host, scene_atmosphere)) return 0;
    return host->scene_atmosphere(mod, settings);
}
