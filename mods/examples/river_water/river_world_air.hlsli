// Outgoing segments only. The shared compositor retains the accepted incoming camera leg. Native fog
// fallback retains its camera-view-depth convention; reflected finite legs always use actual hit length.
#ifndef MML_RIVER_WORLD_AIR_HLSLI
#define MML_RIVER_WORLD_AIR_HLSLI
static float g_worldAirTravel=0;
float WaterWorldAirOpacity(float3 origin,float3 direction,float distance,bool sky){
    if(g_atmosphereInfo.x>.5){
        float begin=min(max(g_atmosphere.x-g_worldAirTravel,0),max(distance,0));
        return AtmosphereOpacity(origin+direction*begin,direction,max(distance-begin,0),false,sky);
    }
    // The native painted sky is already aerial perspective; do not replace its whole hemisphere by fog.
    if(sky)return 0;
    float before=FogAmount(g_worldAirTravel),after=FogAmount(g_worldAirTravel+max(distance,0));
    return saturate((after-before)/max(1-before,1e-6));
}
float3 WaterWorldAirLight(float3 direction){
    return g_atmosphereInfo.x>.5?AtmosphereLight(direction):Lin(g_fogColor.rgb);
}
float3 WaterWorldLayerLight(float3 color,float3 origin,float3 direction,float distance,bool additive){
    // Native RGB is already prelit and decoded once. Preserve its exposure; only integrate outgoing air.
    float opacity=WaterWorldAirOpacity(origin,direction,distance,false);
    // Additive draws do not block the deeper layer and must not insert another copy of air inscattering.
    return color*(1-opacity)+(additive?0:WaterWorldAirLight(direction)*opacity);
}
#define MML_SCENE_LAYER_LIGHT_EX(color,origin,direction,distance,additive) WaterWorldLayerLight(color,origin,direction,distance,additive)
#endif
