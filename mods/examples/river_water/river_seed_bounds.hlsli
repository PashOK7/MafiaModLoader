// Used only by the ordered raster seed. Unknown or unproven intervals still
// use the original wave trace, including its shore classification.
#include "mml/gfx/water_bounds.hlsli"

static bool g_seedBoundsEnabled;
static float4 g_emptyShoreCell;

bool RiverInsideEmptyCell(float2 position) {
    return all(position>=g_emptyShoreCell.xy)&&all(position<g_emptyShoreCell.zw);
}

float RiverTraceShore(float3 position) {
    if(RiverInsideEmptyCell(position.xz))return 1;
    if(mml_waterlineCounts.z==0&&mml_waterlineGrid.w>=1&&
       position.y>=mml_waterlineLo.y&&position.y<=mml_waterlineHi.y) {
        int2 cell=int2(floor((position.xz-mml_waterlineGrid.xy)/mml_waterlineGrid.z));
        if(all(cell>=0)&&all(uint2(cell)<mml_waterlineShape.xy)) {
            uint index=uint(cell.y)*mml_waterlineShape.x+uint(cell.x);
            if(mml_waterlineCells[index].count==0) {
                float margin=max(.002,max(abs(position.x),abs(position.z))*4e-6);
                float2 low=max(mml_waterlineGrid.xy+float2(cell)*mml_waterlineGrid.z,mml_waterlineLo.xz+1)+margin;
                float2 high=min(mml_waterlineGrid.xy+float2(cell+1)*mml_waterlineGrid.z,mml_waterlineHi.xz-1)-margin;
                g_emptyShoreCell=float4(low,high);
                if(RiverInsideEmptyCell(position.xz))return 1;
            }
        }
    }
    return WorldShoreWeight(position);
}

bool RiverShoreKnown(float2 low,float2 high) {
    if(mml_waterlineCounts.z||mml_waterlineGrid.w<1||g_traceLevel<mml_waterlineLo.y||g_traceLevel>mml_waterlineHi.y||
       any(low<mml_waterlineLo.xz+1.002)||any(high>mml_waterlineHi.xz-1.002))return false;
    float2 start=(low-mml_waterlineGrid.xy)/mml_waterlineGrid.z;
    float2 finish=(high-mml_waterlineGrid.xy)/mml_waterlineGrid.z;
    if(any(start<.001)||any(finish>=float2(mml_waterlineShape.xy)-.001))return false;
    uint2 a=uint2((start-.001)/16),b=uint2((finish+.001)/16);
    if(any(b-a>1))return false;
    uint rowA=MmlWaveShoreStart+a.y*MmlWaveShoreWidth,rowB=MmlWaveShoreStart+b.y*MmlWaveShoreWidth;
    return (mml_waveBounds[rowA+a.x]|mml_waveBounds[rowA+b.x]|mml_waveBounds[rowB+a.x]|mml_waveBounds[rowB+b.x])==0;
}

bool RiverNoWaveCrossing(float3 direction,float end,bool requireKnownShore) {
    float3 envelope=MmlWaveEnvelope(g_traceStrength,g_light.w);
    if(any((asuint(envelope)&0x7f800000u)==0x7f800000u))return false;
    // Match the interval of WaterTraceWaves. Ninety-six steps of at most .3 m
    // cannot advance farther than 29.2 m, including float roundoff.
    float begin=.015,bound=max(g_geometry.z,.05);
    if(direction.y>1e-6)end=min(end,(g_traceLevel+bound-g_eye.y)/direction.y);
    else if(direction.y< -1e-6) {
        begin=max(begin,(g_traceLevel+bound-g_eye.y)/direction.y);
        end=min(end,(g_traceLevel-bound-g_eye.y)/direction.y);
    }
    if(begin>=end)return true;
    end=min(end,begin+29.2);
    float3 a=g_eye.xyz+direction*begin,b=g_eye.xyz+direction*end;
    float2 low=min(a.xz,b.xz)-envelope.yz,high=max(a.xz,b.xz)+envelope.yz;
    float lower=min(a.y,b.y)-g_traceLevel,upper=max(a.y,b.y)-g_traceLevel;
    if(lower<=envelope.x&&upper>=-envelope.x) {
        float2 limits=MmlWaveHeightInterval(low,high,g_cascade.xyz,g_traceStrength);
        if(lower<=limits.y&&upper>=limits.x)return false;
    }
    return !requireKnownShore||RiverShoreKnown(low,high);
}

bool RiverSeedQuadOccluded(float3 direction,float nominalDepth,float distance,float2 pixel,float level) {
    bool occluded=g_eye.y<=level;
    float opaqueDepth=SceneDepth(pixel);
    [branch] if(!occluded&&nominalDepth>opaqueDepth+.03) {
        float forward=dot(direction,g_fwd.xyz);
        g_traceLevel=level;
        g_traceStrength=g_water.y*g_geometry.x;
        // Include the full possible crossing bracket beyond the opaque plane.
        occluded=g_geometry.y<=.5||(forward>1e-5&&
            RiverNoWaveCrossing(direction,min(distance+2,(opaqueDepth+.025)/forward+.305),false));
    }
    // The original fragment derivatives also depend on helper lanes. Skip the
    // solve only when all four lanes of this primitive are already occluded.
    float reject=occluded?1:0;
    float row=reject*(1-abs(ddx_fine(reject)));
    return row*(1-abs(ddy_fine(row)))>.5;
}
