#include "mml/gfx/ray_scene.hlsli"

struct MmlDxrBounds { float3 lo,hi; };
StructuredBuffer<uint4> mml_dxrRanges:register(t38);
RWStructuredBuffer<float3> mml_dxrVertices:register(u0);
RWStructuredBuffer<uint2> mml_dxrMapping:register(u1);
RWStructuredBuffer<MmlDxrBounds> mml_dxrBillboards:register(u2);
cbuffer MmlDxrBuild:register(b0){uint mml_facetCount,mml_rangeCount,mml_triangleCount,mml_reserved;};

[numthreads(64,1,1)]
void CsSceneFacets(uint3 id:SV_DispatchThreadID){
    if(id.x>=mml_facetCount)return;
    uint lo=0,hi=mml_rangeCount;
    while(lo+1<hi){
        uint mid=(lo+hi)/2;
        if(mml_dxrRanges[mid].x<=id.x)lo=mid;
        else hi=mid;
    }
    uint4 range=mml_dxrRanges[lo];
    MmlRayInstance object=mml_rayInstances[range.w];
    if(id.x>=mml_triangleCount){
        float extent=max(1,max(max(max(abs(object.lo.x),abs(object.lo.y)),abs(object.lo.z)),
                               max(max(abs(object.hi.x),abs(object.hi.y)),abs(object.hi.z))));
        float pad=max(.02,16*1.192092896e-7*extent);
        MmlDxrBounds bounds;bounds.lo=object.lo-pad;bounds.hi=object.hi+pad;
        mml_dxrBillboards[id.x-mml_triangleCount]=bounds;
        mml_dxrMapping[id.x]=uint2(range.w,0xffffffff);
        return;
    }
    uint facetIndex=range.z+id.x-range.x;
    MmlRayTriangle facet=mml_rayTriangles[facetIndex];
    float3 a=object.localX.xyz,b=object.localY.xyz,c=object.localZ.xyz;
    float determinant=dot(a,cross(b,c));
    float3 x=cross(b,c)/determinant,y=cross(c,a)/determinant,z=cross(a,b)/determinant;
    float3 p=facet.p-float3(object.localX.w,object.localY.w,object.localZ.w);
    float3 world=x*p.x+y*p.y+z*p.z;
    float3 e1=x*facet.e1.x+y*facet.e1.y+z*facet.e1.z;
    float3 e2=x*facet.e2.x+y*facet.e2.y+z*facet.e2.z;
    // Broaden the candidate at shared edges. Shading recomputes the local hit.
    const float margin=.00025;
    mml_dxrVertices[id.x*3]=world-(e1+e2)*margin;
    mml_dxrVertices[id.x*3+1]=world+e1*(1+2*margin)-e2*margin;
    mml_dxrVertices[id.x*3+2]=world-e1*margin+e2*(1+2*margin);
    mml_dxrMapping[id.x]=uint2(range.w,facetIndex);
}
