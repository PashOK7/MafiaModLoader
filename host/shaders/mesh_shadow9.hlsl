// Legacy diagnostic mesh casters; also precompiled, so resizing a map cannot compile HLSL on the game thread.
row_major float4x4 m : register(c0);
struct DepthOut { float4 pos : POSITION; float2 z : TEXCOORD0; };
DepthOut VsMeshShadow(float3 p : POSITION) { DepthOut o; o.pos = mul(float4(p, 1), m); o.z = o.pos.zw; return o; }
float4 PsMeshShadow(DepthOut i) : COLOR0 {
    float z = i.z.x / i.z.y;
    float slope = max(abs(ddx(z)), abs(ddy(z)));
    return float4(z + slope * 1.5, 0, 0, 1);
}
