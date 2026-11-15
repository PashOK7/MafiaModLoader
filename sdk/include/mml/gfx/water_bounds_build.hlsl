#define MML_WATER_BOUNDS_BUILD 1
#include "mml/gfx/water_bounds.hlsli"
#include "mml/gfx/waterline.hlsli"

Texture2DArray<float4> mml_waveDisplacement : register(t8);
RWStructuredBuffer<uint> mml_waveBoundsOutput : register(u0);
groupshared uint3 absolutes[64];
groupshared float2 tileHeights[MmlWaveTilesPerCascade];

float2 JoinHeight(float2 a,float2 b) { return float2(min(a.x,b.x),max(a.y,b.y)); }
void WriteHeight(uint address,float2 limits) {
    mml_waveBoundsOutput[address]=asuint(limits.x);
    mml_waveBoundsOutput[address+1]=asuint(limits.y);
}

[numthreads(8,8,1)]
void CsWaterBoundsTiles(uint3 id:SV_DispatchThreadID,uint3 group:SV_GroupID,uint lane:SV_GroupIndex) {
    float3 displacement=mml_waveDisplacement.Load(int4(id,0)).xyz;
    absolutes[lane]=min(asuint(abs(displacement)),0x7f800000u);
    bool finiteHeight=(asuint(displacement.x)&0x7f800000u)!=0x7f800000u;
    tileHeights[lane]=finiteHeight?displacement.xx:float2(-1e30,1e30);
    GroupMemoryBarrierWithGroupSync();
    [unroll] for(uint step=32;step;step>>=1) {
        if(lane<step) {
            absolutes[lane]=max(absolutes[lane],absolutes[lane+step]);
            tileHeights[lane]=JoinHeight(tileHeights[lane],tileHeights[lane+step]);
        }
        GroupMemoryBarrierWithGroupSync();
    }
    if(lane==0) {
        uint index=id.z*3;
        InterlockedMax(mml_waveBoundsOutput[index],absolutes[0].x);
        InterlockedMax(mml_waveBoundsOutput[index+1],absolutes[0].y);
        InterlockedMax(mml_waveBoundsOutput[index+2],absolutes[0].z);
        WriteHeight(MmlWaveTileAddress(id.z,0,group.xy),tileHeights[0]);
    }
}

[numthreads(256,1,1)]
void CsWaterBoundsHierarchy(uint3 group:SV_GroupID,uint lane:SV_GroupIndex) {
    uint base=MmlWaveTileAddress(group.x,0,uint2(0,0));
    [unroll] for(uint batch=0;batch<4;batch++) {
        uint index=batch*256+lane;
        tileHeights[index]=asfloat(uint2(mml_waveBoundsOutput[base+index*2],mml_waveBoundsOutput[base+index*2+1]));
    }
    GroupMemoryBarrierWithGroupSync();
    [unroll] for(uint level=1;level<6;level++) {
        uint width=32>>level;
        if(lane<width*width) {
            uint2 cell=uint2(lane%width,lane/width);
            uint src=MmlWaveLevelStart[level-1]+cell.y*4*width+cell.x*2;
            float2 limits=JoinHeight(JoinHeight(tileHeights[src],tileHeights[src+1]),
                JoinHeight(tileHeights[src+2*width],tileHeights[src+2*width+1]));
            tileHeights[MmlWaveLevelStart[level]+lane]=limits;
            WriteHeight(MmlWaveTileAddress(group.x,level,cell),limits);
        }
        GroupMemoryBarrierWithGroupSync();
    }
}

// Each bit marks whether a tile contains uncertain contact coverage.
[numthreads(8,8,1)]
void CsWaterBoundsShore(uint3 group:SV_GroupID,uint3 thread:SV_GroupThreadID,uint lane:SV_GroupIndex) {
    uint uncertain=0;
    [unroll] for(uint i=0;i<4;i++) {
        uint2 cell=group.xy*16+thread.xy*2+uint2(i&1,i>>1);
        if(any(cell>=mml_waterlineShape.xy))continue;
        MmlWaterlineCell candidates=mml_waterlineCells[cell.y*mml_waterlineShape.x+cell.x];
        [loop] for(uint item=0;item<candidates.count;item++) {
            if(mml_waterlineElements[mml_waterlineOrder[candidates.first+item]].flags&1) {
                uncertain=1;
                break;
            }
        }
    }
    absolutes[lane].x=uncertain;
    GroupMemoryBarrierWithGroupSync();
    [unroll] for(uint step=32;step;step>>=1) {
        if(lane<step)absolutes[lane].x|=absolutes[lane+step].x;
        GroupMemoryBarrierWithGroupSync();
    }
    if(lane==0)mml_waveBoundsOutput[MmlWaveShoreStart+group.y*MmlWaveShoreWidth+group.x]=absolutes[0].x;
}
