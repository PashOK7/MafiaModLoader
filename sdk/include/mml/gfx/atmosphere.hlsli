// Homogeneous below the base, exponentially thinner above it. Exact line integral, stable for horizontal rays
// and either crossing direction. No temporal noise, ray-march steps or scene-dependent density discontinuities.
float AtmosphereIntegral(float y0, float y1, float distance, float base, float falloff) {
    if (falloff <= 0) return distance;
    float lo = min(y0, y1) - base, hi = max(y0, y1) - base;
    if (hi <= 0) return distance;
    float below = lo < 0 ? saturate(-lo / max(hi - lo, 1e-6)) : 0;
    lo = max(lo, 0);
    float q = (hi - lo) * falloff;
    float average = q < 0.001 ? 1 - q * 0.5 + q * q / 6 : (1 - exp(-q)) / max(q, 1e-6);
    return distance * (below + (1 - below) * exp(-lo * falloff) * average);
}

float AtmosphereOpacity(float3 origin, float3 direction, float distance, bool clearNear, bool sky) {
    if (g_atmosphereInfo.x < 0.5 || g_atmosphere.y <= 0) return 0;
    float begin = clearNear ? min(g_atmosphere.x, distance) : 0;
    float length = max(distance - begin, 0);
    float y0 = origin.y + direction.y * begin;
    float integral = AtmosphereIntegral(y0, y0 + direction.y * length, length, g_atmosphere.z, g_atmosphere.w);
    float opacity = 1 - exp(-min(integral * g_atmosphere.y, 80));
    // The painted sky already contains atmospheric clouds. Merge its horizon with the fog while preserving the
    // upper sky and the cloudy sun; no extra solar disc is drawn. Reflected sky uses the actual outgoing ray.
    if (sky) opacity *= g_atmosphereInfo.w * exp(-max(direction.y, 0) * g_atmospherePhase.y);
    return min(opacity, g_atmosphereInfo.z);
}

float3 AtmosphereLight(float3 direction) {
    float3 sun = g_sunSeen.w > 0.5 ? g_sunSeen.xyz : g_sun.xyz;
    float mu = clamp(dot(direction, sun), -1, 1);
    float asymmetry = g_atmospherePhase.x;
    float phase = (1 - asymmetry * asymmetry) /
                  pow(max(1 + asymmetry * asymmetry - 2 * asymmetry * mu, 0.01), 1.5);
    float daylight = smoothstep(-0.05, 0.15, sun.y);
    return Lin(g_atmosphereColor.rgb) + max(g_sunColor.rgb, 0) *
           (0.075 * g_atmosphereColor.w * phase * daylight);
}

// Input/output light is linear. clearNear is true for the camera leg only, false for a reflected leg.
float3 ApplyAtmosphere(float3 color, float3 origin, float3 direction, float distance, bool clearNear, bool sky) {
    return lerp(color, AtmosphereLight(direction), AtmosphereOpacity(origin, direction, distance, clearNear, sky));
}
