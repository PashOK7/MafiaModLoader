#include "environment_emitter.hlsli"
TextureCube<float4> envSource:register(t11);
SamplerState envLinear:register(s3);
RWStructuredBuffer<float4> envResult:register(u1);
cbuffer EnvEmitterSettings:register(b4){float4 envCentreRadius;};
groupshared float4 envPixels[1024];
groupshared float4 envWork[256];
[numthreads(256,1,1)]void CsEnvironmentEmitter(uint3 id:SV_GroupThreadID){
    uint lane=id.x;float3 centre=normalize(envCentreRadius.xyz);float cosineRadius=envCentreRadius.w;
    float minimum=1e30,maximum=0;
    [unroll]for(uint block=0;block<4;block++){
        uint sampleIndex=lane+256*block;float3 direction=EnvCapDirection(sampleIndex,1024,centre,cosineRadius);
        float3 rgb=max(envSource.SampleLevel(envLinear,direction,0).rgb,0);float luminance=EnvLuminance(rgb);
        envPixels[sampleIndex]=float4(rgb,0);minimum=min(minimum,luminance);maximum=max(maximum,luminance);
    }
    envWork[lane]=float4(minimum,maximum,0,0);GroupMemoryBarrierWithGroupSync();
    [unroll]for(uint stride=128;stride>0;stride>>=1){
        if(lane<stride){envWork[lane].x=min(envWork[lane].x,envWork[lane+stride].x);envWork[lane].y=max(envWork[lane].y,envWork[lane+stride].y);}
        GroupMemoryBarrierWithGroupSync();
    }
    minimum=envWork[0].x;maximum=envWork[0].y;GroupMemoryBarrierWithGroupSync();
    float4 sum=0;float area=2*kEnvPi*(1-cosineRadius);
    [unroll]for(uint groupIndex=0;groupIndex<4;groupIndex++){
        uint sampleIndex=lane+256*groupIndex;float3 direction=EnvCapDirection(sampleIndex,1024,centre,cosineRadius);
        float3 profile=EnvOpeningProfile(envPixels[sampleIndex].rgb,dot(direction,centre),minimum,maximum,cosineRadius);
        float luminance=EnvLuminance(profile);envPixels[sampleIndex]=float4(profile,luminance);
        sum+=float4(profile*dot(direction,centre)*(area/1024),luminance);
    }
    envWork[lane]=sum;GroupMemoryBarrierWithGroupSync();
    [unroll]for(uint reduce=128;reduce>0;reduce>>=1){if(lane<reduce)envWork[lane]+=envWork[lane+reduce];GroupMemoryBarrierWithGroupSync();}
    if(lane==0){
        float4 total=envWork[0];envResult[0]=float4(minimum,maximum,cosineRadius,total.w>1e-5?1:0);
        envResult[1]=float4(total.rgb,1);envResult[2]=float4(centre,area);
        uint at=0;float cumulative=envPixels[0].w;
        [loop]for(uint packet=0;packet<16;packet++){
            float target=(packet+.5)/16*total.w;
            [loop]while(at<1023&&cumulative<target){at++;cumulative+=envPixels[at].w;}
            float3 direction=EnvCapDirection(at,1024,centre,cosineRadius);
            float scale=total.w>1e-5?area/1024*total.w/max(envPixels[at].w,1e-20)/16:0;
            envResult[3+packet*2]=float4(direction,1);
            envResult[4+packet*2]=float4(envPixels[at].rgb*scale,1);
        }
    }
}
