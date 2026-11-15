#include "mml/gfx/frame.hlsli"
#include "mml/gfx/water.hlsli"
#include "mml/gfx/water_optics.hlsli"
static bool g_testWaves;
static float g_testLevel;
float WaterTraceHeight(float2 p){return g_testLevel+(g_testWaves ? .2*cos(2*p.x) : 0);}
#include "mml/gfx/water_trace.hlsli"
RWStructuredBuffer<float4> result : register(u0);
[numthreads(64,1,1)]
void CsWaterLightingTest(uint3 id:SV_DispatchThreadID) {
    uint count,stride;result.GetDimensions(count,stride);g_testLevel=float(count)-64;
    float3 n=float3(0,1,0),v=normalize(float3(0,.25,-1)),sun=normalize(float3(0,.25,1));
    WaterOpticalFrame f=WaterOpticalBasis(n,float2(.0004,.0009),0,0);
    float4 value=0;
    if(id.x==0) {
        value=float4(WaterDirectSun(f,v,sun),WaterDirectSun(f,v,normalize(float3(1,.25,1))),
                     WaterDirectSun(f,v,normalize(float3(1,.25,0))),WaterDirectSun(f,v,normalize(float3(0,.25,-1))));
    } else if(id.x==1) {
        value=float4(WaterMediumBeam(n,n,n,10,.1,0,0),WaterDirectSun(f,v,float3(0,-1,0)));
    } else if(id.x==2) {
        float3 a=normalize(float3(.01,.3,-1)),b=normalize(float3(-.01,.2,1));
        value=float4(WaterDirectSun(f,a,b)/dot(n,b),WaterDirectSun(f,b,a)/dot(n,a),0,1);
    } else if(id.x==3) {
        value=float4(WaterMediumBeam(n,n,n,100,.1,.2,0).x,
                     WaterMediumBeam(n,n,n,100,1,.2,0).x,
                     WaterMediumBeam(n,n,n,0,.1,.2,0).x,
                     WaterMediumBeam(n,n,n,100,.1,.2,0).x*(1-WaterFresnel(1)));
    } else if(id.x==4) {
        value=float4(WaterDisplayRadiance(float3(.1,.2,.3)),1);
    } else if(id.x==5) {
        value=float4(WaterDisplayRadiance(float3(100,50,25)),1);
    } else if(id.x==6) {
        float3 view=normalize(float3(.8,.001,.6));
        WaterOpticalFrame rough=WaterOpticalBasis(n,float2(.1,.2),0,0);
        float minVisible=1,maxWeight=0,energy=0;
        [unroll] for(uint sampleIndex=0;sampleIndex<8;sampleIndex++) {
            float3 m=WaterVisibleFacet(rough,view,WaterOpticalSample(sampleIndex));
            float weight=WaterReflectionWeight(rough,view,reflect(-view,m),m);
            minVisible=min(minVisible,dot(m,view));maxWeight=max(maxWeight,weight);energy+=weight/8;
        }
        value=float4(minVisible,maxWeight,energy,1);
    } else if(id.x==7) {
        float3 medium=WaterMediumBeam(n,n,n,.00001,.1,.2,0);
        value=float4(medium,WaterMediumBeam(n,n,n,10,0,0,0).x);
    } else if(id.x==8){
        value=float4(WaterProjectedShoreWeight(10,5,100),WaterProjectedShoreWeight(10,20,100),
                     WaterProjectedShoreWeight(10,10,100),WaterProjectedShoreWeight(0,5,100));
    } else if(id.x>=9 && id.x<=14){
        g_testWaves=id.x>=11;
        WaterWaveHit hit;
        if(id.x==9)hit=WaterTraceWaves(float3(0,.1,0),float3(0,1,0),0,.25,10);
        else if(id.x==10)hit=WaterTraceWaves(float3(0,1,0),float3(0,-1,0),0,.25,10);
        else if(id.x==11)hit=WaterTraceWaves(float3(0,1,0),float3(0,-1,0),0,.25,10);
        else if(id.x==12)hit=WaterTraceWaves(float3(-1.5707963,.1,0),float3(1,0,0),0,.25,10);
        else if(id.x==13)hit=WaterTraceWaves(float3(-1.5707963,.1,0),normalize(float3(1,-.1,0)),0,.25,10);
        else hit=WaterTraceWaves(float3(-1.5707963,.1,0),float3(1,0,0),0,.25,.05);
        value=float4(hit.status,hit.distance,hit.p.y,hit.p.y-WaterTraceHeight(hit.p.xz));
    } else {
        float elevation=.001+.998*(id.x-14)/49;
        float angle=id.x*2.39996323;
        float3 view=normalize(float3(cos(angle)*sqrt(1-elevation*elevation),elevation,sin(angle)*sqrt(1-elevation*elevation)));
        float3 mean=normalize(float3(.2,1,-.1));
        WaterOpticalFrame basis=WaterOpticalBasis(mean,float2(.003,.007),0,0);
        float minVisible=1,minWeight=1,maxWeight=0,energy=0;
        // Remain above the actual macrosurface, including nearly grazing views.
        view=normalize(basis.t*sqrt(1-elevation*elevation)+mean*elevation);
        [unroll] for(uint directionIndex=0;directionIndex<8;directionIndex++) {
            float3 m=WaterVisibleFacet(basis,view,WaterOpticalSample(directionIndex));
            float weight=WaterReflectionWeight(basis,view,reflect(-view,m),m);
            minVisible=min(minVisible,dot(m,view));minWeight=min(minWeight,weight);maxWeight=max(maxWeight,weight);energy+=weight/8;
        }
        value=float4(minVisible,minWeight,maxWeight,energy);
    }
    result[id.x]=value;
}
