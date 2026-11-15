/* Slow, double-precision geometry-only reference for GPU query diagnostics. No opacity/material claim.
 * Exhaustively visits each instance's mesh leaves instead of using the top-level/triangle BVH traversal. */
#pragma once
#include "mml/ray_scene.h"
#include "mml/billboard.h"
namespace mml { namespace ray {
struct ReferenceHit {uint32_t status=0,instance=UINT32_MAX,triangle=UINT32_MAX,material=UINT32_MAX;
    float distance=0,bary[2]{};uint32_t primitive=UINT32_MAX;};
inline ReferenceHit ReferenceGeometry(const Scene &scene,const Bounds &coverage,Vec3 origin,Vec3 direction,float tMin,float tMax){
    ReferenceHit hit;hit.distance=tMax;
    if(!coverage.Valid()||!Finite(origin)||!Finite(direction)||!std::isfinite(tMin)||!std::isfinite(tMax)||tMin<0||tMax<tMin||
       double(direction.x)*direction.x+double(direction.y)*direction.y+double(direction.z)*direction.z<1e-20){hit.status=3;return hit;}
    auto box=[&](const Bounds &b,double &entry){double leave=tMax;entry=tMin;
        for(unsigned a=0;a<3;a++){if(direction[a]==0){if(origin[a]<b.lo[a]||origin[a]>b.hi[a])return false;}
            else {double x=(double(b.lo[a])-origin[a])/direction[a],y=(double(b.hi[a])-origin[a])/direction[a];entry=(std::max)(entry,(std::min)(x,y));leave=(std::min)(leave,(std::max)(x,y));}}
        return entry<=leave;};
    double unknown=INFINITY;
    for(unsigned a=0;a<3;a++){
        const double begin=double(origin[a])+double(direction[a])*tMin,end=double(origin[a])+double(direction[a])*tMax;
        if(begin<coverage.lo[a]||begin>coverage.hi[a])unknown=tMin;
        else if(end<coverage.lo[a]||end>coverage.hi[a])unknown=(std::min)(unknown,(double(direction[a]>0?coverage.hi[a]:coverage.lo[a])-origin[a])/direction[a]);
    }
    for(const auto &missing:scene.unknown){double entry;if(box({missing.lo,missing.hi},entry))unknown=(std::min)(unknown,entry);}
    const auto nodes=scene.Nodes();const auto triangles=scene.Triangles();
    for(const auto &instance:scene.instances){
        double o[3]{},d[3]{};for(unsigned a=0;a<3;a++){o[a]=instance.local[a][3];for(unsigned k=0;k<3;k++){o[a]+=double(instance.local[a][k])*origin[k];d[a]+=double(instance.local[a][k])*direction[k];}}
        if(instance.flags&(BillboardFacing|BillboardY)){
            float q[3]={float(o[0]),float(o[1]),float(o[2])},rotation[3][3];const bool facing=(instance.flags&BillboardFacing)!=0;
            if(!billboard::Rotation(q,facing?2:1,facing?0:1,rotation)){double entry;if(box({instance.lo,instance.hi},entry))unknown=(std::min)(unknown,entry);continue;}
            double rotatedOrigin[3]{},rotatedDirection[3]{};
            for(unsigned a=0;a<3;a++)for(unsigned k=0;k<3;k++){rotatedOrigin[a]+=rotation[a][k]*o[k];rotatedDirection[a]+=rotation[a][k]*d[k];}
            std::copy_n(rotatedOrigin,3,o);std::copy_n(rotatedDirection,3,d);
        }
        if(instance.root>=nodes.size())continue;
        for(unsigned n=instance.root;n<nodes[instance.root].escape;n++){
            const auto &node=nodes[n];for(unsigned j=0;j<node.count;j++){
                const unsigned index=node.first+j;const auto &tri=triangles[index];
                double p[3]={d[1]*tri.e2.z-d[2]*tri.e2.y,d[2]*tri.e2.x-d[0]*tri.e2.z,d[0]*tri.e2.y-d[1]*tri.e2.x};
                double det=0;for(unsigned a=0;a<3;a++)det+=double(tri.e1[a])*p[a];const double facing=(instance.flags&1)?-det:det;
                if(det==0||((tri.flags&CullBack)&&facing<=0)||((tri.flags&CullFront)&&facing>=0))continue;
                double s[3]={o[0]-tri.p.x,o[1]-tri.p.y,o[2]-tri.p.z},u=0;for(unsigned a=0;a<3;a++)u+=s[a]*p[a];u/=det;if(u<0||u>1)continue;
                double q[3]={s[1]*tri.e1.z-s[2]*tri.e1.y,s[2]*tri.e1.x-s[0]*tri.e1.z,s[0]*tri.e1.y-s[1]*tri.e1.x};
                double v=0,t=0;for(unsigned a=0;a<3;a++){v+=d[a]*q[a];t+=double(tri.e2[a])*q[a];}v/=det;t/=det;
                if(v<0||u+v>1||t<tMin||t>hit.distance)continue;
                hit.status=1;hit.instance=instance.id;hit.triangle=index;hit.material=instance.Material(tri);hit.primitive=instance.Primitive(tri);
                hit.distance=float(t);hit.bary[0]=float(u);hit.bary[1]=float(v);
            }
        }
    }
    if(unknown<=hit.distance){hit.status=2;hit.distance=float(unknown);hit.instance=hit.triangle=hit.material=hit.primitive=UINT32_MAX;hit.bary[0]=hit.bary[1]=0;}
    return hit;
}
} }
