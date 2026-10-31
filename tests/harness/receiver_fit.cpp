#include "../../host/receiver_fit.h"
#include <cassert>
#include <cstdio>
#include <random>
using namespace receiver_fit;
int main() {
    MmlRenderCamera c{};c.fwd.z=1;c.right.x=1;c.up.y=1;c.near_z=.05f;c.far_z=5000;c.proj[0]=1.3f;c.proj[5]=1.7f;
    c.proj[8]=.1f;c.proj[9]=-.2f;
    const V axes[3]={{.8f,0,.6f},{0,1,0},{-.6f,0,.8f}};
    const float t[9]={-100,-4,15, 200,-4,30, 10,-4,200};
    float lo[3]={1e30f,1e30f,1e30f},hi[3]={-1e30f,-1e30f,-1e30f};
    Include(c,t,1,3,axes,lo,hi);
    std::mt19937 rng(72);std::uniform_real_distribution<float> u(0,1),offset(-3,3);
    unsigned tested=0;
    for(unsigned i=0;i<200000;i++) {
        float a=u(rng),b=u(rng);if(a+b>1){a=1-a;b=1-b;}
        V p={t[0]+a*(t[3]-t[0])+b*(t[6]-t[0])+offset(rng),
             t[1]+a*(t[4]-t[1])+b*(t[7]-t[1])+offset(rng),
             t[2]+a*(t[5]-t[2])+b*(t[8]-t[2])+offset(rng)};
        if(p.z<c.near_z || p.z>c.far_z || std::fabs(p.x*c.proj[0]/p.z+c.proj[8])>1 || std::fabs(p.y*c.proj[5]/p.z+c.proj[9])>1) continue;
        tested++;
        for(int k=0;k<3;k++){float q=Dot(p,axes[k]);assert(q>=lo[k]-.001f && q<=hi[k]+.001f);}
    }
    assert(tested>10000 && hi[2]<400); // finite water, not an infinite receiver slab out to 5 km
    const float hidden[9]={1000,0,2,1002,1,2,1001,0,3};
    float a[3]={1e30f,1e30f,1e30f},b[3]={-1e30f,-1e30f,-1e30f};
    Include(c,hidden,1,.1f,axes,a,b);assert(a[0]>b[0]);
    const float depth[6]={-10,-6,15,10,-2,80};
    float l[3]={1e30f,1e30f,1e30f},h[3]={-1e30f,-1e30f,-1e30f};
    IntersectLightBounds(t,1,3,axes,depth,l,h);
    assert(l[0]<=h[0] && l[1]>=depth[1] && h[2]<=depth[5]);
    for(unsigned i=0;i<50000;i++) {
        float x=u(rng),y=u(rng);if(x+y>1){x=1-x;y=1-y;}
        V p={t[0]+x*(t[3]-t[0])+y*(t[6]-t[0])+offset(rng),-4+offset(rng),
             t[2]+x*(t[5]-t[2])+y*(t[8]-t[2])+offset(rng)};
        float q[3];bool inside=true;
        for(int k=0;k<3;k++){q[k]=Dot(p,axes[k]);inside=inside && q[k]>=depth[k] && q[k]<=depth[k+3];}
        if(inside)for(int k=0;k<3;k++)assert(q[k]>=l[k]-.001f && q[k]<=h[k]+.001f);
    }
    float emptyLo[3]={1e30f,1e30f,1e30f},emptyHi[3]={-1e30f,-1e30f,-1e30f};
    IntersectLightBounds(hidden,1,.1f,axes,depth,emptyLo,emptyHi);assert(emptyLo[0]>emptyHi[0]);
    std::puts("finite receiver bounds: displaced points, asymmetric frustum, outside rejection: ALL PASSED");
}
