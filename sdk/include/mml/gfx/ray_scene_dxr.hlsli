// DXR enumerates conservative world-space instance boxes. The accepted local
// triangle, billboard, material and coplanar-order rules remain the narrow phase.
// One AABB primitive maps directly to the same source-instance array index.
RaytracingAccelerationStructure mml_dxrWorld:register(t40);
#ifdef MML_RAY_DXR_FACETS
StructuredBuffer<uint2> mml_dxrFacets:register(t41);
#endif

void MmlDxrFacet(MmlRayInstance object,uint instanceIndex,uint triangleIndex,float3 origin,float3 direction,
                 float3 localOrigin,float3 localDirection,float tMin,inout MmlRayHit hit,inout float unknownAt){
    if(triangleIndex>=mml_rayCounts.y){unknownAt=min(unknownAt,tMin);return;}
    MmlRayTriangle facet=mml_rayTriangles[triangleIndex];float distance;float2 bary;
    if(!MmlRayTriangleHit(facet,localOrigin,localDirection,tMin,hit.distance,(object.flags&1)!=0,distance,bary))return;
    bool instanced=(object.flags&8)!=0;
    uint primitive=facet.primitive+(instanced?object.pad1:0);
    MML_RAY_HIT_CONTEXT(object,origin,direction,triangleIndex,distance);
    uint accepted=MML_RAY_ACCEPT_PRIMITIVE(triangleIndex,primitive,bary,object.id,distance);
    if(accepted==2){unknownAt=min(unknownAt,distance);return;}
    if(accepted==0||!MML_RAY_WINS(distance,object.id,primitive,hit.distance,hit.instance,hit.primitive))return;
    hit.status=1;hit.instance=object.id;hit.triangleIndex=triangleIndex;hit.material=instanced?object.pad:facet.material;
    hit.distance=distance;hit.bary=bary;hit.primitive=primitive;
    MML_RAY_SELECTED_CONTEXT(hit);
    MML_RAY_SELECTED_INSTANCE_INDEX(instanceIndex);
}

void MmlDxrInstance(MmlRayInstance object,uint instanceIndex,float3 origin,float3 direction,float tMin,float tMax,
                    float3 localOrigin,float3 localDirection,inout MmlRayHit hit,inout float unknownAt){
    if((object.flags&6)!=0){
        float3x3 rotation;bool facing=(object.flags&2)!=0;
        float3 observer=MML_RAY_OBSERVER(origin);
        float3 localObserver=float3(dot(object.localX,float4(observer,1)),dot(object.localY,float4(observer,1)),dot(object.localZ,float4(observer,1)));
        if(!MmlBillboardRotation(localObserver,facing?2u:1u,facing?0u:1u,rotation)){
            float entry;if(MmlRayBox(origin,direction,object.lo,object.hi,tMin,hit.distance,entry))unknownAt=min(unknownAt,entry);
            return;
        }
        localOrigin=mul(rotation,localOrigin);localDirection=mul(rotation,localDirection);
    }
#if defined(MML_RAY_CACHE_SLABS)
    MmlRaySlabs slabs=MmlPrepareSlabs(localDirection);
    float roundoff=8*1.192092896e-7*max(1,tMax);
#endif
    MmlRayNode root=mml_rayNodes[object.root];
    bool reverse=MmlRayReverse(root,localDirection);
    uint nodeIndex=object.root;
    MML_RAY_TRAVERSAL_LOOP while(nodeIndex<root.escape){
        MmlRayNode node=mml_rayNodes[nodeIndex];
#if defined(MML_RAY_CACHE_SLABS)
        bool visit=MmlRayNodeBox(localOrigin,slabs,node.lo,node.hi,tMin,hit.distance,roundoff);
#else
        float entry;bool visit=MmlRayBox(localOrigin,localDirection,node.lo,node.hi,tMin,hit.distance,entry);
#endif
        if(!visit){nodeIndex=reverse?node.pad.x:node.escape;continue;}
        if(!node.count){nodeIndex=reverse?node.pad.y:node.first;continue;}
        MML_RAY_TRAVERSAL_LOOP for(uint i=0;i<node.count;i++)
            MmlDxrFacet(object,instanceIndex,node.first+i,origin,direction,localOrigin,localDirection,tMin,hit,unknownAt);
        nodeIndex=reverse?node.pad.x:node.escape;
    }
}

void MmlDxrWorld(float3 origin,float3 direction,float tMin,float tMax,inout MmlRayHit hit,inout float unknownAt){
    if(mml_rayCounts.w==0||unknownAt<=tMin)return;
    RayDesc ray;ray.Origin=origin;ray.Direction=direction;ray.TMin=tMin;ray.TMax=min(tMax,unknownAt);
#ifdef MML_RAY_DXR_FACETS
    RayQuery<RAY_FLAG_FORCE_NON_OPAQUE> query;
#else
    RayQuery<RAY_FLAG_FORCE_NON_OPAQUE|RAY_FLAG_SKIP_TRIANGLES> query;
#endif
    query.TraceRayInline(mml_dxrWorld,RAY_FLAG_NONE,0xff,ray);
    MML_RAY_TRAVERSAL_LOOP while(query.Proceed()){
#ifdef MML_RAY_DXR_FACETS
        uint2 candidate=mml_dxrFacets[query.CandidateInstanceID()+query.CandidatePrimitiveIndex()];
        uint index=candidate.x;
#else
        if(query.CandidateType()!=CANDIDATE_PROCEDURAL_PRIMITIVE)continue;
        uint index=query.CandidatePrimitiveIndex();
#endif
        if(index>=mml_rayCounts.w){unknownAt=min(unknownAt,tMin);return;}
        MmlRayInstance object=mml_rayInstances[index];
        float3 localOrigin=float3(dot(object.localX,float4(origin,1)),dot(object.localY,float4(origin,1)),dot(object.localZ,float4(origin,1)));
        float3 localDirection=float3(dot(object.localX.xyz,direction),dot(object.localY.xyz,direction),dot(object.localZ.xyz,direction));
#ifdef MML_RAY_DXR_FACETS
        if(candidate.y!=0xffffffff){
            MmlDxrFacet(object,index,candidate.y,origin,direction,localOrigin,localDirection,tMin,hit,unknownAt);
            continue;
        }
#endif
        MmlDxrInstance(object,index,origin,direction,tMin,tMax,localOrigin,localDirection,hit,unknownAt);
    }
}
