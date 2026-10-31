// Fit shadow receivers to this frame's opaque depth, before rendering the water's shadow map.
// A ray's interval through each conservative horizontal receiver slab is clipped by the opaque depth.
// Every water sample surviving the surface shader is inside this superset; occluded land does not expand it.
#include "mml/gfx/frame.hlsli"
cbuffer ReceiverFit : register(b1) {
    float4 g_draw;       // render width/height, current jitter x/y in render pixels
    float4 g_range;      // near, far, slab count, unused
    float4 g_lightX, g_lightY, g_lightZ;
    float4 g_boxes[32];  // min xyz / max xyz, per finite receiver level, displacement included
};
RWStructuredBuffer<uint> g_bounds : register(u0); // ordered float min xyz, max xyz, sample count
groupshared uint s_min[3], s_max[3], s_count;
uint Ordered(float v) { uint b = asuint(v); return (b & 0x80000000) ? ~b : (b ^ 0x80000000); }
void Include(float3 p) {
    uint3 q = uint3(Ordered(dot(p,g_lightX.xyz)),Ordered(dot(p,g_lightY.xyz)),Ordered(dot(p,g_lightZ.xyz)));
    [unroll] for(uint a=0;a<3;a++) { InterlockedMin(s_min[a],q[a]); InterlockedMax(s_max[a],q[a]); }
}
[numthreads(8,8,1)]
void CsShadowReceivers(uint3 id : SV_DispatchThreadID, uint lane : SV_GroupIndex) {
    if(lane<3) { s_min[lane]=0xffffffff; s_max[lane]=0; }
    if(lane==0) s_count=0;
    GroupMemoryBarrierWithGroupSync();
    if(all(id.xy < (uint2)g_draw.xy)) {
        float2 ndc = (float2(id.xy)+0.5-g_draw.zw)/g_draw.xy*float2(2,-2)+float2(-1,1);
        float3 ray = g_fwd.xyz + g_right.xyz*((ndc.x-g_proj.z)/g_proj.x) + g_up.xyz*((ndc.y-g_proj.w)/g_proj.y);
        float scene = min(g_sceneDepth.Load(int3(id.xy,0)) + 0.021, g_range.y);
        [loop] for(uint n=0;n<(uint)g_range.z;n++) {
            float lo=g_range.x, hi=scene;
            [unroll] for(uint axis=0;axis<3;axis++) {
                if(abs(ray[axis])<1e-8) {
                    if(g_eye[axis]<g_boxes[n*2][axis] || g_eye[axis]>g_boxes[n*2+1][axis]) hi=-1;
                } else {
                    float a=(g_boxes[n*2][axis]-g_eye[axis])/ray[axis],b=(g_boxes[n*2+1][axis]-g_eye[axis])/ray[axis];
                    lo=max(lo,min(a,b));hi=min(hi,max(a,b));
                }
            }
            if(lo<=hi) { Include(g_eye.xyz+ray*lo); Include(g_eye.xyz+ray*hi); InterlockedAdd(s_count,1); }
        }
    }
    GroupMemoryBarrierWithGroupSync();
    if(lane<3 && s_count) { InterlockedMin(g_bounds[lane],s_min[lane]); InterlockedMax(g_bounds[lane+3],s_max[lane]); }
    if(lane==0 && s_count) InterlockedAdd(g_bounds[6],s_count);
}
