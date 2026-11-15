// Differential transport follows the retained path; it never launches additional object/water rays.
// The water normal's local spatial derivative supplies the next facet's differential at a bounce.
#ifndef MML_RIVER_PATH_FOOTPRINT_HLSLI
#define MML_RIVER_PATH_FOOTPRINT_HLSLI
WaterOpticalFrame WaterAdjacentOptical(WaterOpticalFrame frame,float3 normalDelta,float3 tangentDelta,float2 alphaDelta){
    frame.n=normalize(frame.n+normalDelta);float3 tangent=frame.t+tangentDelta;
    frame.t=normalize(tangent-frame.n*dot(tangent,frame.n));frame.b=cross(frame.n,frame.t);
    frame.alpha=max(frame.alpha+alphaDelta,1e-5);return frame;
}
float3 WaterMirrorObserver(float3 observer,float3 position,float3 normal){
    return observer-2*dot(observer-position,normal)*normal;
}
void WaterBounceDifferential(float3 originDelta,float3 directionDelta,float3 origin,float3 direction,
                              float3 position,float3 normal,float distance,float3 normalX,float3 normalZ,
                              float3 observer,float3 observerDelta,out float3 nextOriginDelta,
                              out float3 nextDirectionDelta,out float3 nextObserverDelta){
    float denominator=dot(normal,direction);
    nextOriginDelta=originDelta+distance*directionDelta;
    nextOriginDelta-=direction*(dot(normal,nextOriginDelta)/denominator);
    float3 adjacentNormal=normalize(normal+normalX*nextOriginDelta.x+normalZ*nextOriginDelta.z);
    nextDirectionDelta=reflect(normalize(direction+directionDelta),adjacentNormal)-reflect(direction,normal);
    nextObserverDelta=WaterMirrorObserver(observer+observerDelta,position+nextOriginDelta,adjacentNormal)-WaterMirrorObserver(observer,position,normal);
}
void WaterBounceNormals(WaterWaveHit hit,out float3 normal,out float3 normalX,out float3 normalZ){
    float3 values[3]={(float3)0,(float3)0,(float3)0};
    const float epsilon=.02;
    MML_WATER_PATH_LOOP for(uint offsetIndex=0;offsetIndex<3;offsetIndex++){
        float3 offset=offsetIndex==1?float3(epsilon,0,0):offsetIndex==2?float3(0,0,epsilon):0;
        values[offsetIndex]=WaterTraceNormal(hit.p+offset);
    }
    normal=values[0];normalX=(values[1]-normal)/epsilon;normalZ=(values[2]-normal)/epsilon;
}
void WaterBounceFootprint(inout MmlRayFootprint footprint,float3 origin,float3 direction,WaterWaveHit hit,
                           float3 normal,float3 normalX,float3 normalZ){
    MmlRayFootprint next=footprint;
    WaterBounceDifferential(footprint.originDx,footprint.directionDx,origin,direction,hit.p,normal,hit.distance,normalX,normalZ,
        footprint.observer,footprint.observerDx,next.originDx,next.directionDx,next.observerDx);
    WaterBounceDifferential(footprint.originDy,footprint.directionDy,origin,direction,hit.p,normal,hit.distance,normalX,normalZ,
        footprint.observer,footprint.observerDy,next.originDy,next.directionDy,next.observerDy);
    next.observer=WaterMirrorObserver(footprint.observer,hit.p,normal);footprint=next;
}
#endif
