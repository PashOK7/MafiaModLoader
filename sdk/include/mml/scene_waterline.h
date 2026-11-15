/* Current native geometry intersected with horizontal fluid planes. No camera projection, collision proxy,
 * texture-name bank list or retained native resource. Continuous opaque boundaries are definite; uncertain
 * alpha/blended coverage stays explicit. View-facing billboard cards are not physical fluid boundaries. */
#pragma once
#include "mml/scene_snapshot.h"
#include "mml/native_alpha_bounds.h"
#include <numeric>
namespace mml { namespace scene {
struct WaterlineElement {float a[2],b[2],c[2],level;uint32_t flags;};
enum : uint32_t { WaterlineUnknown=1,WaterlineArea=2 };
static_assert(sizeof(WaterlineElement)==32,"waterline GPU layout");
struct WaterlineModel {
    struct Cell {uint32_t first=0,count=0;};
    struct UncertainSource {uintptr_t visual;uint32_t material,alphaTest,alphaFunction,alphaReference,blend,count;float at[3];};
    std::vector<WaterlineElement> elements;std::vector<ray::Node> nodes;
    std::vector<UncertainSource> uncertainSources; // provenance for diagnostics ONLY, never dereferenced
    std::vector<ray::Unknown> unknown;ray::Bounds coverage;
    std::vector<Cell> cells;std::vector<uint32_t> cellOrder;
    float grid[4]{};uint32_t shape[4]{}; // originXZ,cell metres,supported local radius; width,height,cells,indices
    uint64_t cycle=0,nativeFrame=0;unsigned definite=0,uncertain=0,billboards=0;
};
inline ray::Vec3 WaterlineWorld(ray::Vec3 p,const float *w){
    return {float(double(p.x)*w[0]+double(p.y)*w[4]+double(p.z)*w[8]+w[12]),
            float(double(p.x)*w[1]+double(p.y)*w[5]+double(p.z)*w[9]+w[13]),
            float(double(p.x)*w[2]+double(p.y)*w[6]+double(p.z)*w[10]+w[14])};
}
inline ray::Bounds WaterlineBounds(const WaterlineElement &e){
    ray::Bounds b;b.Add(ray::Vec3{e.a[0],e.level,e.a[1]});b.Add(ray::Vec3{e.b[0],e.level,e.b[1]});
    if(e.flags&WaterlineArea)b.Add(ray::Vec3{e.c[0],e.level,e.c[1]});return b;
}
inline uint32_t WaterlineTree(WaterlineModel &model,std::vector<uint32_t> &order,size_t first,size_t last){
    const uint32_t root=uint32_t(model.nodes.size());model.nodes.push_back({});ray::Bounds box,centres;
    for(size_t j=first;j<last;j++){const auto b=WaterlineBounds(model.elements[order[j]]);box.Add(b);centres.Add((b.lo+b.hi)*.5f);}
    ray::Node node{};node.lo=box.lo;node.hi=box.hi;
    if(last-first<=8){node.first=uint32_t(first);node.count=uint32_t(last-first);}
    else{
        const unsigned axis=centres.hi.x-centres.lo.x>=centres.hi.z-centres.lo.z?0:2;const size_t middle=first+(last-first)/2;
        std::nth_element(order.begin()+first,order.begin()+middle,order.begin()+last,[&](uint32_t a,uint32_t b){
            const auto x=WaterlineBounds(model.elements[a]),y=WaterlineBounds(model.elements[b]);
            const float ca=x.lo[axis]+x.hi[axis],cb=y.lo[axis]+y.hi[axis];return ca==cb?a<b:ca<cb;
        });
        node.first=WaterlineTree(model,order,first,middle);WaterlineTree(model,order,middle,last);
    }
    node.escape=uint32_t(model.nodes.size());model.nodes[root]=node;return root;
}
inline bool WaterlineGrid(WaterlineModel &model){
    // Binning changes only candidate selection, never the geometric distance or boundary precision.
    const double width=double(model.coverage.hi.x)-model.coverage.lo.x,height=double(model.coverage.hi.z)-model.coverage.lo.z;
    const float cell=float((std::max)(4.0,(std::max)(width,height)/512));
    model.grid[0]=float(std::floor(double(model.coverage.lo.x)/cell)*cell);
    model.grid[1]=float(std::floor(double(model.coverage.lo.z)/cell)*cell);model.grid[2]=cell;model.grid[3]=1;
    if(!std::isfinite(model.grid[0])||!std::isfinite(model.grid[1])||!std::isfinite(cell)||cell<=0)return false;
    const unsigned nx=unsigned(std::ceil((double(model.coverage.hi.x)-model.grid[0])/cell))+1;
    const unsigned nz=unsigned(std::ceil((double(model.coverage.hi.z)-model.grid[1])/cell))+1;
    if(!nx||!nz||uint64_t(nx)*nz>1024u*1024)return false;
    model.cells.assign(size_t(nx)*nz,{});model.shape[0]=nx;model.shape[1]=nz;model.shape[2]=uint32_t(model.cells.size());
    const auto range=[&](const WaterlineElement &element,int box[4]){
        const auto b=WaterlineBounds(element);
        const double loX=std::floor((double(b.lo.x)-1-model.grid[0])/cell),hiX=std::floor((double(b.hi.x)+1-model.grid[0])/cell);
        const double loZ=std::floor((double(b.lo.z)-1-model.grid[1])/cell),hiZ=std::floor((double(b.hi.z)+1-model.grid[1])/cell);
        if(hiX<0||hiZ<0||loX>=nx||loZ>=nz)return false;
        box[0]=int((std::clamp)(loX,0.0,double(nx-1)));box[1]=int((std::clamp)(hiX,0.0,double(nx-1)));
        box[2]=int((std::clamp)(loZ,0.0,double(nz-1)));box[3]=int((std::clamp)(hiZ,0.0,double(nz-1)));return true;
    };
    uint64_t references=0;
    for(const auto &e:model.elements){int b[4];if(!range(e,b))continue;
        references+=uint64_t(b[1]-b[0]+1)*uint64_t(b[3]-b[2]+1);if(references>4u*1024*1024)return false;
        for(int z=b[2];z<=b[3];z++)for(int x=b[0];x<=b[1];x++)model.cells[size_t(z)*nx+x].count++;
    }
    uint32_t first=0;for(auto &c:model.cells){const unsigned count=c.count;c.first=first;c.count=0;first+=count;}
    model.cellOrder.resize(first);model.shape[3]=first;
    for(uint32_t i=0;i<model.elements.size();i++){int b[4];if(!range(model.elements[i],b))continue;
        for(int z=b[2];z<=b[3];z++)for(int x=b[0];x<=b[1];x++){auto &c=model.cells[size_t(z)*nx+x];model.cellOrder[c.first+c.count++]=i;}
    }return true;
}
inline bool BuildWaterline(const Snapshot &source,const std::vector<float> &levels,WaterlineModel &out){
    WaterlineModel model;if(!source.coverage.Valid()||source.geometry.frameId!=source.nativeFrame||!source.materialsCollected)return false;
    for(float y:levels)if(!std::isfinite(y))return false;
    model.coverage=source.coverage;model.cycle=source.cycle;model.nativeFrame=source.nativeFrame;model.unknown=source.geometry.unknown;
    const auto nodes=source.geometry.Nodes();const auto triangles=source.geometry.Triangles();
    for(const auto &instance:source.geometry.instances){
        if(instance.id==0||instance.id>source.sources.size()||instance.root>=nodes.size())return false;
        const auto &origin=source.sources[instance.id-1];
        if(origin.kind==3){model.billboards++;continue;}
        bool selected=false;for(float y:levels)selected=selected||(y>=instance.lo.y&&y<=instance.hi.y);if(!selected)continue;
        const auto stop=nodes[instance.root].escape;if(stop<=instance.root||stop>nodes.size())return false;
        for(uint32_t n=instance.root;n<stop;n++){
            const auto &node=nodes[n];if(!node.count)continue;
            if(uint64_t(node.first)+node.count>triangles.size())return false;
            for(uint32_t t=node.first;t<node.first+node.count;t++){
                const auto &triangle=triangles[t];
                const uint32_t materialIndex=instance.Material(triangle),primitive=instance.Primitive(triangle);
                const ray::Vec3 p[3]={WaterlineWorld(triangle.p,origin.world),WaterlineWorld(triangle.p+triangle.e1,origin.world),WaterlineWorld(triangle.p+triangle.e2,origin.world)};
                if(!ray::Finite(p[0])||!ray::Finite(p[1])||!ray::Finite(p[2]))return false;
                bool uncertain=true;
                if(materialIndex<source.materials.size()){
                    const auto &material=source.materials[materialIndex];
                    if(material.alphaTest&&material.alphaFunction==1)continue; // native NEVER contributes no boundary
                    uncertain=(material.flags&14u)!=0||material.blend!=0||(material.alphaTest&&material.alphaFunction!=8);
                }
                for(float level:levels){
                    const double d[3]={double(p[0].y)-level,double(p[1].y)-level,double(p[2].y)-level};
                    if((d[0]>0&&d[1]>0&&d[2]>0)||(d[0]<0&&d[1]<0&&d[2]<0))continue;
                    WaterlineElement e{};e.level=level;e.flags=uncertain?WaterlineUnknown:0;
                    std::array<float,2> barySection[3]{};unsigned sectionCount=0;
                    if(d[0]==0&&d[1]==0&&d[2]==0){
                        e.a[0]=p[0].x;e.a[1]=p[0].z;e.b[0]=p[1].x;e.b[1]=p[1].z;e.c[0]=p[2].x;e.c[1]=p[2].z;e.flags|=WaterlineArea;
                        barySection[0]={0,0};barySection[1]={1,0};barySection[2]={0,1};sectionCount=3;
                    }else{
                        double x[3]{},z[3]{},u[3]{},v[3]{};unsigned count=0;
                        for(unsigned j=0;j<3;j++)if(d[j]==0){x[count]=p[j].x;z[count]=p[j].z;u[count]=j==1?1:0;v[count]=j==2?1:0;count++;}
                        for(unsigned j=0;j<3;j++){
                            const unsigned k=(j+1)%3;if((d[j]<0&&d[k]>0)||(d[j]>0&&d[k]<0)){
                                if(count>=3)return false;const double fraction=d[j]/(d[j]-d[k]);
                                x[count]=p[j].x+(double(p[k].x)-p[j].x)*fraction;z[count]=p[j].z+(double(p[k].z)-p[j].z)*fraction;
                                u[count]=(j==1?1-fraction:0)+(k==1?fraction:0);v[count]=(j==2?1-fraction:0)+(k==2?fraction:0);count++;
                            }
                        }
                        if(count==0||count>2)return false;
                        e.a[0]=float(x[0]);e.a[1]=float(z[0]);e.b[0]=float(x[count-1]);e.b[1]=float(z[count-1]);
                        sectionCount=count;for(unsigned j=0;j<count;j++)barySection[j]={float(u[j]),float(v[j])};
                    }
                    bool sectionUncertain=uncertain;
                    if(sectionUncertain&&materialIndex<source.materials.size()){
                        const auto &material=source.materials[materialIndex];AlphaInterval alpha;
                        const std::vector<std::array<float,2>> section(barySection,barySection+sectionCount);
                        const bool bounded=source.compactAttributes?NativeSectionAlphaBounds(source,t,materialIndex,section,alpha):
                            NativeSectionAlphaBounds(source,primitive,section,alpha);
                        if(bounded){
                            const unsigned coverage=material.alphaTest?NativeAlphaCoverage(alpha,material.alphaFunction,material.alphaReference):1;
                            if(coverage==0)continue;
                            if(material.blend==0&&coverage==1)sectionUncertain=false;
                            else if(material.blend&&material.sourceBlend==5&&material.destinationBlend==6){
                                if(alpha.hi==0)continue;if(coverage==1&&alpha.lo==1)sectionUncertain=false;
                            }
                        }
                    }
                    if(!sectionUncertain)e.flags&=~WaterlineUnknown;
                    if(model.elements.size()>=UINT32_MAX)return false;
                    model.elements.push_back(e);if(sectionUncertain){
                        model.uncertain++;auto found=std::find_if(model.uncertainSources.begin(),model.uncertainSources.end(),[&](const auto &v){return v.visual==origin.visual&&v.material==materialIndex;});
                        if(found==model.uncertainSources.end()){
                            WaterlineModel::UncertainSource info{};info.visual=origin.visual;info.material=materialIndex;
                            info.at[0]=(e.a[0]+e.b[0])*.5f;info.at[1]=level;info.at[2]=(e.a[1]+e.b[1])*.5f;
                            if(materialIndex<source.materials.size()){const auto &m=source.materials[materialIndex];info.alphaTest=m.alphaTest;info.alphaFunction=m.alphaFunction;info.alphaReference=m.alphaReference;info.blend=m.blend;}
                            model.uncertainSources.push_back(info);found=model.uncertainSources.end()-1;
                        }found->count++;
                    }else model.definite++;
                }
            }
        }
    }
    if(!model.elements.empty()){
        std::vector<uint32_t> order(model.elements.size());std::iota(order.begin(),order.end(),0u);WaterlineTree(model,order,0,order.size());
        std::vector<WaterlineElement> sorted;sorted.reserve(order.size());for(uint32_t i:order)sorted.push_back(model.elements[i]);model.elements.swap(sorted);
    }
    if(!WaterlineGrid(model))return false;
    out=std::move(model);return true;
}
struct WaterlineResult {float distance=0;uint32_t status=0;}; // 0 definite local distance, 2 unknown, 3 invalid
inline double WaterlineSegmentDistance(double x,double z,const float *a,const float *b){
    const double dx=double(b[0])-a[0],dz=double(b[1])-a[1],length=dx*dx+dz*dz;
    const double t=length>0?(std::clamp)(((x-a[0])*dx+(z-a[1])*dz)/length,0.0,1.0):0;
    const double ex=x-a[0]-t*dx,ez=z-a[1]-t*dz;return ex*ex+ez*ez;
}
inline double WaterlineDistance(const WaterlineElement &e,ray::Vec3 p){
    double distance=WaterlineSegmentDistance(p.x,p.z,e.a,e.b);
    if(e.flags&WaterlineArea){
        const double a=(double(e.b[0])-e.a[0])*(p.z-e.a[1])-(double(e.b[1])-e.a[1])*(p.x-e.a[0]);
        const double b=(double(e.c[0])-e.b[0])*(p.z-e.b[1])-(double(e.c[1])-e.b[1])*(p.x-e.b[0]);
        const double c=(double(e.a[0])-e.c[0])*(p.z-e.c[1])-(double(e.a[1])-e.c[1])*(p.x-e.c[0]);
        const double area=(double(e.b[0])-e.a[0])*(double(e.c[1])-e.a[1])-(double(e.b[1])-e.a[1])*(double(e.c[0])-e.a[0]);
        if(area!=0&&((a>=0&&b>=0&&c>=0)||(a<=0&&b<=0&&c<=0)))return 0;
        distance=(std::min)(distance,(std::min)(WaterlineSegmentDistance(p.x,p.z,e.b,e.c),WaterlineSegmentDistance(p.x,p.z,e.c,e.a)));
    }return distance;
}
inline WaterlineResult WaterlineReference(const WaterlineModel &model,ray::Vec3 p,float radius){
    WaterlineResult result{radius,0};if(!ray::Finite(p)||!std::isfinite(radius)||radius<=0)return {0,3};
    if(p.x-radius<model.coverage.lo.x||p.x+radius>model.coverage.hi.x||p.z-radius<model.coverage.lo.z||p.z+radius>model.coverage.hi.z||p.y<model.coverage.lo.y||p.y>model.coverage.hi.y)return {radius,2};
    double nearest=double(radius)*radius,unknown=INFINITY;
    for(const auto &e:model.elements)if(std::abs(p.y-e.level)<.001f){const double d=WaterlineDistance(e,p);if(e.flags&WaterlineUnknown)unknown=(std::min)(unknown,d);else nearest=(std::min)(nearest,d);}
    for(const auto &box:model.unknown)if(p.y>=box.lo.y&&p.y<=box.hi.y){
        const double dx=(std::max)(0.0,(std::max)(double(box.lo.x)-p.x,double(p.x)-box.hi.x));
        const double dz=(std::max)(0.0,(std::max)(double(box.lo.z)-p.z,double(p.z)-box.hi.z));unknown=(std::min)(unknown,dx*dx+dz*dz);
    }
    result.distance=float(std::sqrt(nearest));if(unknown<nearest)result.status=2;return result;
}
} }
