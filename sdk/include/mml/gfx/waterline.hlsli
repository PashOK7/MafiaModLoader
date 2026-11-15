// Nearest current native-geometry intersection with a horizontal fluid plane. No camera/image inputs.
// Local radius is a world distance. Unknown geometry/alpha coverage stays unknown, never a false open bank.
#ifndef MML_WATERLINE_HLSLI
#define MML_WATERLINE_HLSLI
#ifndef MML_WATERLINE_LOOP
#define MML_WATERLINE_LOOP [loop]
#endif
struct MmlWaterlineElement {float2 a,b,c;float level;uint flags;};
struct MmlWaterlineNode {float3 lo;uint first;float3 hi;uint count;uint escape;uint3 pad;};
struct MmlWaterlineUnknown {float3 lo;uint reason;float3 hi;uint pad;};
StructuredBuffer<MmlWaterlineElement> mml_waterlineElements:register(t28);
StructuredBuffer<MmlWaterlineNode> mml_waterlineNodes:register(t29);
StructuredBuffer<MmlWaterlineUnknown> mml_waterlineUnknown:register(t30);
struct MmlWaterlineCell {uint first,count;};
StructuredBuffer<MmlWaterlineCell> mml_waterlineCells:register(t31);
StructuredBuffer<uint> mml_waterlineOrder:register(t32);
cbuffer MmlWaterlineSnapshot:register(b4){uint4 mml_waterlineCounts;float4 mml_waterlineLo,mml_waterlineHi,mml_waterlineGrid;uint4 mml_waterlineShape;};
float MmlWaterlineSegment(float2 p,float2 a,float2 b){
    float2 edge=b-a;float t=saturate(dot(p-a,edge)/max(dot(edge,edge),1e-30));float2 delta=p-a-edge*t;return dot(delta,delta);
}
float MmlWaterlineCross(float2 a,float2 b){return a.x*b.y-a.y*b.x;}
float MmlWaterlineDistance(MmlWaterlineElement e,float2 p){
    float edgeDistance=MmlWaterlineSegment(p,e.a,e.b);
    float triangleDistance=min(edgeDistance,min(MmlWaterlineSegment(p,e.b,e.c),MmlWaterlineSegment(p,e.c,e.a)));
    float area=MmlWaterlineCross(e.b-e.a,e.c-e.a);
    float a=MmlWaterlineCross(e.b-e.a,p-e.a),b=MmlWaterlineCross(e.c-e.b,p-e.b),c=MmlWaterlineCross(e.a-e.c,p-e.c);
    bool inside=area!=0&&((a>=0&&b>=0&&c>=0)||(a<=0&&b<=0&&c<=0));
    return (e.flags&2u)!=0?(inside?0:triangleDistance):edgeDistance;
}
float MmlWaterlineBoxDistance(float2 p,float2 lo,float2 hi){float2 d=max(max(lo-p,p-hi),0);return dot(d,d);}
float2 MmlWaterlineFinish(float3 position,float nearest,float unknown){
    MML_WATERLINE_LOOP for(uint missing=0;missing<mml_waterlineCounts.z;missing++){
        MmlWaterlineUnknown box=mml_waterlineUnknown[missing];if(position.y>=box.lo.y&&position.y<=box.hi.y)unknown=min(unknown,MmlWaterlineBoxDistance(position.xz,box.lo.xz,box.hi.xz));
    }
    return float2(sqrt(nearest),unknown<nearest?2:0);
}
// Exact elements whose bounding boxes expanded by one metre overlap this cell. This is a candidate
// index, not a sampled distance field: segment/triangle distances and opacity retain their precision.
float2 MmlQueryWaterlineCell(float3 position,float radius){
    if(any((asuint(position)&0x7f800000u)==0x7f800000u)||(asuint(radius)&0x7f800000u)==0x7f800000u||radius<=0)return float2(0,3);
    if(any(position.xz-radius<mml_waterlineLo.xz)||any(position.xz+radius>mml_waterlineHi.xz)||position.y<mml_waterlineLo.y||position.y>mml_waterlineHi.y)return float2(radius,2);
    int2 cell=int2(floor((position.xz-mml_waterlineGrid.xy)/mml_waterlineGrid.z));
    if(radius>mml_waterlineGrid.w||any(cell<0)||any(uint2(cell)>=mml_waterlineShape.xy))return float2(radius,2);
    MmlWaterlineCell candidates=mml_waterlineCells[uint(cell.y)*mml_waterlineShape.x+uint(cell.x)];
    float nearest=radius*radius,unknown=asfloat(0x7f800000u);
    MML_WATERLINE_LOOP for(uint i=0;i<candidates.count;i++){
        MmlWaterlineElement e=mml_waterlineElements[mml_waterlineOrder[candidates.first+i]];if(abs(position.y-e.level)>=.001)continue;
        float distance=MmlWaterlineDistance(e,position.xz);if((e.flags&1u)!=0)unknown=min(unknown,distance);else nearest=min(nearest,distance);
    }
    return MmlWaterlineFinish(position,nearest,unknown);
}
// Separate entry prevents the repeated wave solver from inlining the general BVH traversal.
float2 MmlQueryWaterlineLocal(float3 position){return MmlQueryWaterlineCell(position,1);}
// x nearest distance capped at radius, y status0 complete local query,2 unknown,3 invalid.
float2 MmlQueryWaterline(float3 position,float radius){
    if(any((asuint(position)&0x7f800000u)==0x7f800000u)||(asuint(radius)&0x7f800000u)==0x7f800000u||radius<=0)return float2(0,3);
    if(any(position.xz-radius<mml_waterlineLo.xz)||any(position.xz+radius>mml_waterlineHi.xz)||position.y<mml_waterlineLo.y||position.y>mml_waterlineHi.y)return float2(radius,2);
    if(radius<=mml_waterlineGrid.w)return MmlQueryWaterlineCell(position,radius);
    float nearest=radius*radius,unknown=asfloat(0x7f800000u);uint node=0;
    MML_WATERLINE_LOOP while(node<mml_waterlineCounts.y){
        MmlWaterlineNode n=mml_waterlineNodes[node];
        if(position.y<n.lo.y-.001||position.y>n.hi.y+.001||MmlWaterlineBoxDistance(position.xz,n.lo.xz,n.hi.xz)>nearest){node=n.escape;continue;}
        if(n.count==0){node=n.first;continue;}
        MML_WATERLINE_LOOP for(uint i=0;i<n.count;i++){
            MmlWaterlineElement e=mml_waterlineElements[n.first+i];if(abs(position.y-e.level)>=.001)continue;
            float distance=MmlWaterlineDistance(e,position.xz);if((e.flags&1u)!=0)unknown=min(unknown,distance);else nearest=min(nearest,distance);
        }node=n.escape;
    }
    return MmlWaterlineFinish(position,nearest,unknown);
}
#endif
