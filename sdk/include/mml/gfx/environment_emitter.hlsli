// A bounded, image-shaped distant emitter. The source profile is measured in a world-directional LDR capture;
// its missing radiometric scale is supplied EXPLICITLY as perpendicular irradiance by the caller. This does
// not recover HDR from an LDR image. The profile is zero outside the selected cap and for a uniform source.
#ifndef MML_ENVIRONMENT_EMITTER_HLSLI
#define MML_ENVIRONMENT_EMITTER_HLSLI
static const float kEnvPi=3.14159265359;
float EnvLuminance(float3 rgb){return dot(rgb,float3(.2126,.7152,.0722));}
float3 EnvCapDirection(uint index,uint count,float3 centre,float cosineRadius){
    float u=(index+.5)/count,phi=2*kEnvPi*(reversebits(index)*2.3283064365386963e-10);
    float cosine=lerp(1,cosineRadius,u),sine=sqrt(max(1-cosine*cosine,0));
    float3 t=normalize(cross(abs(centre.y)<.95?float3(0,1,0):float3(1,0,0),centre)),b=cross(centre,t);
    return centre*cosine+(t*cos(phi)+b*sin(phi))*sine;
}
float3 EnvOpeningProfile(float3 radiance,float cosine,float minimum,float maximum,float cosineRadius){
    float contrast=maximum-minimum;
    if(contrast<=1e-5 || cosine<=cosineRadius)return 0;
    float bright=saturate((EnvLuminance(radiance)-minimum)/contrast);
    float rim=smoothstep(cosineRadius,lerp(cosineRadius,1,.1),cosine);
    return max(radiance,0)*(bright*bright*rim);
}
float3 EnvEmitterRadiance(float3 radiance,float3 direction,float4 range,float4 integral,float4 centre,float3 irradiance){
    if(range.w<.5)return 0;
    float3 profile=EnvOpeningProfile(radiance,dot(direction,centre.xyz),range.x,range.y,range.z);
    return profile*max(irradiance,0)/max(integral.rgb,1e-8);
}
#endif
