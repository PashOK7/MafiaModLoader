// Conservative intervals over the current LOD-zero displacement texels.
// The solver still evaluates the original displacement at possible crossings.
#ifndef MML_WATER_BOUNDS_HLSLI
#define MML_WATER_BOUNDS_HLSLI
static const uint MmlWaveLevelStart[6] = {0,1024,1280,1344,1360,1364};
static const uint MmlWaveShoreWidth = 33;
static const uint MmlWaveShoreStart = 9;
static const uint MmlWaveTileStart = MmlWaveShoreStart + MmlWaveShoreWidth * MmlWaveShoreWidth;
static const uint MmlWaveTilesPerCascade = 1365;

uint MmlWaveTileAddress(uint cascade, uint level, uint2 cell) {
    uint width = 32 >> level;
    return MmlWaveTileStart + 2 * (cascade * MmlWaveTilesPerCascade + MmlWaveLevelStart[level] + cell.y * width + cell.x);
}

#ifndef MML_WATER_BOUNDS_BUILD
StructuredBuffer<uint> mml_waveBounds : register(t13);

float3 MmlWaveEnvelope(float strength, float chop) {
    float3 maxima = 0;
    [unroll] for (uint cascade = 0; cascade < 3; cascade++) {
        uint index = cascade * 3;
        maxima += asfloat(uint3(mml_waveBounds[index],mml_waveBounds[index+1],mml_waveBounds[index+2]));
    }
    return maxima * abs(strength) * float3(1,abs(chop),abs(chop)) * 1.0001 + .003;
}

float2 MmlWaveTileHeight(uint cascade, uint level, uint2 cell) {
    uint index = MmlWaveTileAddress(cascade,level,cell);
    return asfloat(uint2(mml_waveBounds[index],mml_waveBounds[index+1]));
}

float2 MmlWaveCascadeHeight(uint cascade, float2 low, float2 high, float scale) {
    // Include both bilinear taps. Bound the unwrapped indices, then wrap.
    float2 a = low * scale - .5, b = high * scale - .5;
    float guard = max(.01,max(max(abs(a.x),abs(a.y)),max(abs(b.x),abs(b.y))) * 1e-5);
    int2 first = int2(floor(a-guard)), last = int2(floor(b+guard))+1;
    uint span = uint(max(last.x-first.x,last.y-first.y));
    uint shift = uint(firstbithigh(clamp(span-1,7u,255u))+1);
    uint2 lo = (uint2(first)&255) >> shift, hi = (uint2(last)&255) >> shift;
    float2 aa = MmlWaveTileHeight(cascade,shift-3,lo), bb = MmlWaveTileHeight(cascade,shift-3,hi);
    float2 ab = MmlWaveTileHeight(cascade,shift-3,uint2(lo.x,hi.y));
    float2 ba = MmlWaveTileHeight(cascade,shift-3,uint2(hi.x,lo.y));
    return float2(min(min(aa.x,bb.x),min(ab.x,ba.x)),max(max(aa.y,bb.y),max(ab.y,ba.y)));
}

float2 MmlWaveHeightInterval(float2 low, float2 high, float3 inverseLengths, float strength) {
    float2 height = 0;
    [unroll] for (uint cascade = 0; cascade < 3; cascade++)
        height += MmlWaveCascadeHeight(cascade,low,high,inverseLengths[cascade]*256);
    height *= strength;
    // Shore attenuation is in [0,1]. Include zero and round the sum out.
    return float2(min(min(height.x,height.y),0),max(max(height.x,height.y),0)) * 1.0001 + float2(-.003,.003);
}
#endif
#endif
