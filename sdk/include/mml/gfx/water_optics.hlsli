// Water interface and homogeneous-medium transport. Include after frame.hlsli / water.hlsli.
// Beckmann slopes, Smith visibility and visible-normal sampling use one local distribution. Reflections carry
// Fresnel once; medium output excludes eye-side Fresnel so the material applies interface transmission once.
// Model background: PBRT Microfacet Models / Rough Dielectric BSDF. No screen-space illumination inputs here.
#ifndef MML_WATER_OPTICS_HLSLI
#define MML_WATER_OPTICS_HLSLI
#ifndef MML_WATER_OPTICS_LOOP
#define MML_WATER_OPTICS_LOOP [unroll]
#endif
static const float kWaterPi=3.14159265359;
float WaterErf(float x) {
    float a=abs(x),t=1/(1+.3275911*a);
    float polynomial=(((((1.061405429*t-1.453152027)*t)+1.421413741)*t-.284496736)*t+.254829592)*t;
    // Beyond four deviations the correction is below float precision. Bound even the unselected expression:
    // FXC constant-folds synthetic tests in double precision before eliminating branches.
    return a>=4 ? sign(x) : sign(x)*(1-polynomial*exp(-min(a*a,16)));
}
float WaterInvErf(float x) {
    x=clamp(x,-.999999,.999999);
    float l=log(max(1-x*x,1e-12)),q=2/(kWaterPi*.147)+l*.5;
    float r=sign(x)*sqrt(max(sqrt(max(q*q-l/.147,0))-q,0));
    // Refine the closed-form initial approximation against the same CDF used by visible-normal sampling.
    MML_WATER_OPTICS_LOOP for(uint j=0;j<2;j++) r-=(WaterErf(r)-x)/(1.128379167*exp(-r*r));
    return r;
}
struct WaterOpticalFrame { float3 n,t,b;float2 alpha; };
WaterOpticalFrame WaterOpticalBasis(float3 n,float2 worldVariance,float3 normalDx,float3 normalDy) {
    WaterOpticalFrame f;f.n=n;
    float3 t=normalize(float3(n.y,-n.x,0)),b=cross(n,t);
    float3 covariance=n.y*n.y*float3(dot(t.xz*t.xz,worldVariance),dot(b.xz*b.xz,worldVariance),dot(t.xz*b.xz,worldVariance));
    float2 dx=float2(dot(normalDx,t),dot(normalDx,b)),dy=float2(dot(normalDy,t),dot(normalDy,b));
    covariance+=float3(dx.x*dx.x+dy.x*dy.x,dx.y*dx.y+dy.y*dy.y,dx.x*dx.y+dy.x*dy.y)/12;
    float angle=0;
    if(abs(covariance.z)+abs(covariance.x-covariance.y)>1e-15)
        angle=.5*atan2(2*covariance.z,covariance.x-covariance.y);
    float c=cos(angle),s=sin(angle);
    f.t=t*c+b*s;f.b=b*c-t*s;
    float mean=.5*(covariance.x+covariance.y),spread=length(float2(.5*(covariance.x-covariance.y),covariance.z));
    f.alpha=sqrt(2*max(float2(mean+spread,mean-spread),1e-10));
    return f;
}
float3 WaterToLocal(WaterOpticalFrame f,float3 d) { return float3(dot(d,f.t),dot(d,f.b),dot(d,f.n)); }
float WaterSmithLambda(float3 d,float2 alpha) {
    float variance=dot(d.xy*d.xy,alpha*alpha);
    if(variance<1e-18) return 0;
    float a=max(d.z,1e-7)/sqrt(variance);
    if(a>=4) return 0;
    return max(.5*(WaterErf(a)-1+exp(-min(a*a,16))/max(a*sqrt(kWaterPi),1e-20)),0);
}
float WaterBeckmannD(float3 h,float2 alpha) {
    if(h.z<=0) return 0;
    float2 slope=h.xy/(h.z*alpha);
    return exp(-dot(slope,slope))/(kWaterPi*alpha.x*alpha.y*h.z*h.z*h.z*h.z);
}
// Invert the visible x-slope CDF directly on its monotone support. The independent y slope has Gaussian CDF.
// This avoids accepting invisible facets and then renormalizing a few quadrature nodes depending on the view.
float3 WaterVisibleFacet(WaterOpticalFrame f,float3 v,float2 u) {
    float3 vl=WaterToLocal(f,v);
    float3 stretched=normalize(float3(f.alpha*vl.xy,max(vl.z,1e-6)));
    float2 slopes;
    float sinTheta=length(stretched.xy);
    if(sinTheta<.001) {
        float radius=sqrt(-log(max(1-u.x,1e-7))),angle=2*kWaterPi*u.y;
        slopes=radius*float2(cos(angle),sin(angle));
    } else {
        float tangent=sinTheta/max(stretched.z,1e-7),cotangent=1/tangent;
        float normalization=1+WaterErf(cotangent)+tangent*exp(-min(cotangent*cotangent,16))/sqrt(kWaterPi);
        float lo=-8,hi=min(cotangent,8);
        MML_WATER_OPTICS_LOOP for(uint step=0;step<12;step++) {
            float x=(lo+hi)*.5;
            float cdf=(1+WaterErf(x)+tangent*exp(-min(x*x,16))/sqrt(kWaterPi))/normalization;
            if(cdf<u.x) lo=x;else hi=x;
        }
        slopes=float2((lo+hi)*.5,WaterInvErf(2*u.y-1));
        float2 azimuth=stretched.xy/sinTheta;
        slopes=float2(azimuth.x*slopes.x-azimuth.y*slopes.y,azimuth.y*slopes.x+azimuth.x*slopes.y);
    }
    slopes*=f.alpha;
    return normalize(f.n-f.t*slopes.x-f.b*slopes.y);
}
float2 WaterOpticalSample(uint index,uint count=8) {
    const float reverse[8]={.0625,.5625,.3125,.8125,.1875,.6875,.4375,.9375};
    return float2((index+.5)/count,reverse[index&7]+(count==4 ? .0625 : 0));
}
// Contribution after sampling a visible facet. Every white-environment sample is bounded by one: Fresnel times
// G2/G1, with correlated Smith visibility. No view-dependent energy renormalization or border-derived light.
float WaterReflectionWeight(WaterOpticalFrame f,float3 v,float3 reflected,float3 facet) {
    float3 vl=WaterToLocal(f,v),rl=WaterToLocal(f,reflected);
    if(vl.z<=0 || rl.z<=0 || dot(v,facet)<=0) return 0;
    float lv=WaterSmithLambda(vl,f.alpha),lr=WaterSmithLambda(rl,f.alpha);
    return WaterFresnel(dot(v,facet))*(1+lv)/(1+lv+lr);
}
// BRDF times incident cosine, per unit perpendicular irradiance. Finite sun-disc variance is convolved in the
// half-vector domain (uniform disc radius .266 degrees); unresolved waves and pixel filtering remain distinct.
float WaterDirectSun(WaterOpticalFrame f,float3 v,float3 light) {
    if(light.y<=0) return 0;
    float3 vl=WaterToLocal(f,v),ll=WaterToLocal(f,light);
    if(vl.z<=0 || ll.z<=0) return 0;
    float3 halfVector=normalize(v+light),hl=WaterToLocal(f,halfVector);
    float2 alpha=sqrt(f.alpha*f.alpha+2*(.00465*.00465/16));
    float visibility=1/(1+WaterSmithLambda(vl,alpha)+WaterSmithLambda(ll,alpha));
    return WaterFresnel(dot(v,halfVector))*WaterBeckmannD(hl,alpha)*visibility/(4*vl.z);
}
// A directional beam scattered in a homogeneous slab. The material multiplies by incident perpendicular
// irradiance (or sky radiance times its quadrature solid angle). sigma_a / sigma_s are per metre, depth vertical.
// Entry transmission is included; eye-side transmission is NOT. No bed, colored emitter or invented crest face.
float3 WaterMediumBeam(float3 v,float3 n,float3 light,float depth,float3 sigmaA,float3 sigmaS,float phase) {
    if(light.y<=0 || dot(n,v)<=0 || depth<=0) return 0;
    const float eta=1.333;
    float3 outgoing=-refract(-v,n,1/eta),incident=refract(-light,float3(0,1,0),1/eta);
    float muV=max(outgoing.y,.001),muS=max(-incident.y,.001);
    float3 extinction=max(sigmaA,0)+max(sigmaS,0),k=extinction*(1/muV+1/muS),x=k*depth;
    float3 removed=x<.001 ? x*(1-.5*x) : 1-exp(-min(x,16));
    float3 integral=removed/max(extinction*(1+muV/muS),1e-20);
    return max(sigmaS,0)*WaterPhase(clamp(dot(incident,outgoing),-1,1),clamp(phase,0,.95))*integral*
           (light.y/muS)*(1-WaterFresnel(light.y))/(eta*eta);
}
// Display-referred legacy scene integration: preserve ordinary dark/mid radiance, roll highlights into the
// display range continuously. This is an explicit display mapping, not recovery of HDR from painted clouds.
float3 WaterDisplayRadiance(float3 radiance) {
    radiance=max(radiance,0);
    float peak=max(max(radiance.r,radiance.g),radiance.b);
    float display=peak<=.6 ? peak : .6+.4*(1-exp(-min((peak-.6)/.4,16)));
    return radiance*(display/max(peak,1e-20));
}
#endif
