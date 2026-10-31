// Conservative current-camera bounds for a finite displaced receiver surface. No depth history or nested Render.
#pragma once
#include "mml/mml.h"
#include <algorithm>
#include <cmath>

namespace receiver_fit {
struct V { float x, y, z; };
inline V operator+(V a,V b){return {a.x+b.x,a.y+b.y,a.z+b.z};}
inline V operator-(V a,V b){return {a.x-b.x,a.y-b.y,a.z-b.z};}
inline V operator*(V a,float s){return {a.x*s,a.y*s,a.z*s};}
inline float Dot(V a,V b){return a.x*b.x+a.y*b.y+a.z*b.z;}
inline float L1(V a){return std::fabs(a.x)+std::fabs(a.y)+std::fabs(a.z);}
struct Plane { V n; float d; };
inline void Include(const MmlRenderCamera &c,const float *tris,unsigned count,float pad,const V axes[3],float lo[3],float hi[3]) {
    const V eye{c.eye.x,c.eye.y,c.eye.z},r{c.right.x,c.right.y,c.right.z},u{c.up.x,c.up.y,c.up.z},f{c.fwd.x,c.fwd.y,c.fwd.z};
    Plane planes[6]={{r*c.proj[0]+f*(1+c.proj[8]),0},{r*(-c.proj[0])+f*(1-c.proj[8]),0},
                     {u*c.proj[5]+f*(1+c.proj[9]),0},{u*(-c.proj[5])+f*(1-c.proj[9]),0},
                     {f,-c.near_z},{f*(-1),c.far_z}};
    // A displaced sample inside the frustum has its undisplaced point inside these expanded planes.
    for(auto &p:planes) p.d+=-Dot(p.n,eye)+L1(p.n)*pad+0.002f;
    for(unsigned t=0;t<count;t++) {
        V a[16],b[16];int n=3;
        for(int i=0;i<3;i++) a[i]={tris[t*9+i*3],tris[t*9+i*3+1],tris[t*9+i*3+2]};
        for(const auto &p:planes) {
            int m=0;
            for(int i=0;i<n;i++) {
                V v=a[i],w=a[(i+1)%n];float d=Dot(p.n,v)+p.d,e=Dot(p.n,w)+p.d;
                if(d>=0) b[m++]=v;
                if((d>=0)!=(e>=0)) b[m++]=v+(w-v)*(d/(d-e));
            }
            n=m;for(int i=0;i<n;i++) a[i]=b[i];
            if(!n) break;
        }
        for(int i=0;i<n;i++) for(int k=0;k<3;k++) {
            const float q=Dot(a[i],axes[k]),margin=L1(axes[k])*pad;
            lo[k]=std::min(lo[k],q-margin);hi[k]=std::max(hi[k],q+margin);
        }
    }
}
inline void IntersectLightBounds(const float *tris,unsigned count,float pad,const V axes[3],
                                 const float bounds[6],float lo[3],float hi[3]) {
    Plane planes[6];
    for(int k=0;k<3;k++) {
        const float margin=L1(axes[k])*pad+.002f;
        planes[k*2]={axes[k],-bounds[k]+margin};
        planes[k*2+1]={axes[k]*(-1),bounds[k+3]+margin};
    }
    for(unsigned t=0;t<count;t++) {
        V a[16],b[16];int n=3;
        for(int i=0;i<3;i++) a[i]={tris[t*9+i*3],tris[t*9+i*3+1],tris[t*9+i*3+2]};
        for(const auto &p:planes) {
            int m=0;
            for(int i=0;i<n;i++) {
                V v=a[i],w=a[(i+1)%n];float d=Dot(p.n,v)+p.d,e=Dot(p.n,w)+p.d;
                if(d>=0) b[m++]=v;
                if((d>=0)!=(e>=0)) b[m++]=v+(w-v)*(d/(d-e));
            }
            n=m;for(int i=0;i<n;i++) a[i]=b[i];
            if(!n) break;
        }
        for(int i=0;i<n;i++)for(int k=0;k<3;k++) {
            const float q=Dot(a[i],axes[k]),margin=L1(axes[k])*pad;
            lo[k]=std::min(lo[k],std::max(bounds[k],q-margin));
            hi[k]=std::max(hi[k],std::min(bounds[k+3],q+margin));
        }
    }
}
}
