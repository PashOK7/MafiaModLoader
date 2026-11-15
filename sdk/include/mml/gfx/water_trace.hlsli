// Bounded world-space height-field intersections. The caller supplies WaterTraceHeight(xz), in metres.
// No camera projection, depth buffer, reflected image or upward direction clamp participates in this query.
// Status: 0 escaped the upper height bound, 1 first bracketed surface crossing, 2 unresolved finite search.
#ifndef MML_WATER_TRACE_HLSLI
#define MML_WATER_TRACE_HLSLI
#ifndef MML_WATER_TRACE_REFINE_LOOP
#define MML_WATER_TRACE_REFINE_LOOP [unroll]
#endif
#ifndef MML_WATER_TRACE_STEP_LOOP
#define MML_WATER_TRACE_STEP_LOOP [loop]
#endif
struct WaterWaveHit { float3 p; float distance; uint status; };
WaterWaveHit WaterTraceWaves(float3 origin,float3 direction,float level,float bound,float maxDistance) {
    WaterWaveHit hit;hit.p=origin;hit.distance=0;hit.status=2;
    float begin=.015,end=maxDistance;
    if(direction.y>1e-6) {
        end=min(end,(level+bound-origin.y)/direction.y);
        if(end<=begin){hit.status=0;return hit;}
    } else if(direction.y< -1e-6) {
        begin=max(begin,(level+bound-origin.y)/direction.y);
        end=min(end,(level-bound-origin.y)/direction.y);
    }
    if(begin>=end)return hit;
    float before=begin;
    float gapBefore=origin.y+direction.y*before-WaterTraceHeight(origin.xz+direction.xz*before);
    // A rasterized coarse surface may lie inside the continuous optical surface. Search from the eye for
    // primary repairs; reflected paths start above it. Never accept an inside -> outside crossing as a hit.
    MML_WATER_TRACE_STEP_LOOP for(uint step=0;step<96;step++) {
        float stride=clamp(abs(gapBefore)/max(abs(direction.y)+length(direction.xz),.01),.015,.3);
        float next=min(before+stride,end);
        float3 p=origin+direction*next;
        float gap=p.y-WaterTraceHeight(p.xz);
        if(gapBefore>=0 && gap<=0) {
            float lo=before,hi=next;
            MML_WATER_TRACE_REFINE_LOOP for(uint refine=0;refine<7;refine++) {
                float mid=(lo+hi)*.5;
                float3 q=origin+direction*mid;
                if(q.y>WaterTraceHeight(q.xz))lo=mid;else hi=mid;
            }
            hit.distance=(lo+hi)*.5;hit.p=origin+direction*hit.distance;hit.status=1;return hit;
        }
        if(next>=end){hit.p=p;hit.distance=next;hit.status=direction.y>0 ? 0 : 2;return hit;}
        before=next;gapBefore=gap;
    }
    hit.distance=before;hit.p=origin+direction*before;return hit;
}
#endif
