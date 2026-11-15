TextureCube<float4> sky : register(t0);
RWStructuredBuffer<float4> result : register(u0);
SamplerState linearClamp : register(s0);
[numthreads(6,1,1)]
void CsEnvironmentTest(uint3 id:SV_DispatchThreadID) {
    const float3 directions[6]={float3(1,0,0),float3(-1,0,0),float3(0,1,0),float3(0,-1,0),float3(0,0,1),float3(0,0,-1)};
    result[id.x]=sky.SampleLevel(linearClamp,directions[id.x],0);
}
