/* Camera-independent triangle queries for D3D11 consumers. Local mesh BVHs are immutable; instance transforms
 * and the top-level BVH are updated together for the caller's current frame. No image, view matrix or depth
 * reconstruction participates. This is the query core, not a native scene exporter or a material renderer. */
#pragma once
#include <algorithm>
#include <bit>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <limits>
#include <memory>
#include <span>
#include <vector>

namespace mml { namespace scene { class SnapshotBuilder; } }
namespace mml { namespace ray {
struct Vec3 { float x=0,y=0,z=0;float &operator[](unsigned i){return (&x)[i];}float operator[](unsigned i)const{return (&x)[i];} };
inline Vec3 operator+(Vec3 a,Vec3 b){return {a.x+b.x,a.y+b.y,a.z+b.z};}
inline Vec3 operator-(Vec3 a,Vec3 b){return {a.x-b.x,a.y-b.y,a.z-b.z};}
inline Vec3 operator*(Vec3 a,float s){return {a.x*s,a.y*s,a.z*s};}
inline bool Finite(float value){return (std::bit_cast<uint32_t>(value)&0x7f800000u)!=0x7f800000u;}
inline bool Finite(Vec3 v){return Finite(v.x)&&Finite(v.y)&&Finite(v.z);}
struct Bounds {
    Vec3 lo{INFINITY,INFINITY,INFINITY},hi{-INFINITY,-INFINITY,-INFINITY};
    void Add(Vec3 p){for(unsigned a=0;a<3;a++){lo[a]=std::min(lo[a],p[a]);hi[a]=std::max(hi[a],p[a]);}}
    void Add(const Bounds &b){Add(b.lo);Add(b.hi);}
    bool Valid()const{return Finite(lo)&&Finite(hi)&&lo.x<=hi.x&&lo.y<=hi.y&&lo.z<=hi.z;}
};
enum : uint32_t { Opaque=1, CullBack=2, CullFront=4 };
enum : uint32_t { InstanceMirrored=1, BillboardFacing=2, BillboardY=4, InstanceAttributes=8 };
struct Triangle { Vec3 p;uint32_t material;Vec3 e1;uint32_t primitive;Vec3 e2;uint32_t flags; };
struct Node { Vec3 lo;uint32_t first;Vec3 hi;uint32_t count;uint32_t escape,pad[3]; }; // reverse escape, right child, split axis
struct Instance {
    float local[3][4]; // dot(float4(world,1), local[axis]); ray direction uses w=0, never normalized after transform
    uint32_t root,id,flags,pad;
    Vec3 lo;uint32_t pad1;Vec3 hi;uint32_t pad2;
    uint32_t Material(const Triangle &triangle)const{return flags&InstanceAttributes?pad:triangle.material;}
    uint32_t Primitive(const Triangle &triangle)const{return flags&InstanceAttributes?pad1+triangle.primitive:triangle.primitive;}
};
struct Unknown { Vec3 lo;uint32_t reason;Vec3 hi;uint32_t pad; };
static_assert(sizeof(Triangle)==48 && sizeof(Node)==48 && sizeof(Instance)==96 && sizeof(Unknown)==32,"ray GPU layout");
struct Placement {
    uint32_t mesh=0,id=0;float world[16]={1,0,0,0,0,1,0,0,0,0,1,0,0,0,0,1};uint32_t flags=0;
    uint32_t material=UINT32_MAX,primitiveBase=0,primitiveCount=0;
};

class Scene {
    struct Item{Bounds bounds;uint32_t id;};
    struct Branch {uint32_t root,reverse;size_t first,last;};
public:
    struct Mesh { uint32_t root;Bounds bounds;uint32_t maxPrimitive; };
    // The caller may supply a previously built shape after proving the current
    // world matrix, local bounds, billboard mode and arithmetic controls match.
    // Instance identity, BLAS root and primitive/material bindings remain current.
    struct InstanceReuse {
        const Instance *(*find)(size_t, const Placement &, const Mesh &, const void *) noexcept;
        const void *context;
    };
    struct InstanceJob {
        const Scene &scene;
        const std::vector<Placement> &placements;
        std::vector<Instance> &result;
        const InstanceReuse *reuse=nullptr;
        bool Run(size_t first,size_t last,unsigned *reused=nullptr)const noexcept{
            if(first>last||last>placements.size()||last>result.size())return false;
            unsigned count=0;
            const auto meshes=scene.Meshes();
            for(size_t i=first;i<last;i++){
                const auto &p=placements[i];
                if(p.mesh>=meshes.size())return false;
                const auto &mesh=meshes[p.mesh];
                const Instance *shape=reuse&&reuse->find?reuse->find(i,p,mesh,reuse->context):nullptr;
                if(!MakeInstance(p,mesh,result[i],shape))return false;
                count+=shape!=nullptr;
            }
            if(reused)*reused=count;
            return true;
        }
    };
    using InstanceExecutor=bool (*)(const InstanceJob &,void *);
    class TopJob {
        friend class Scene;
        std::vector<Node> &nodes_;std::vector<Item> &items_;std::vector<uint32_t> &order_;
        Branch branches_[2];
        TopJob(std::vector<Node> &nodes,std::vector<Item> &items,std::vector<uint32_t> &order,Branch left,Branch right)
            :nodes_(nodes),items_(items),order_(order),branches_{left,right}{}
    public:
        size_t Items(unsigned side)const noexcept{return branches_[side].last-branches_[side].first;}
        void Run(unsigned side)const noexcept{Fill(nodes_,items_,order_,branches_[side]);}
    };
    // Run both disjoint branches or return false without starting either.
    using TopExecutor=bool (*)(const TopJob &,void *);
    // Reject invalid/degenerate geometry before changing the scene. Primitive ids/materials remain the caller's.
    int AddMesh(const std::vector<Triangle> &source) {
        if(packedOwner_||&source==&triangles||source.empty()||source.size()>UINT32_MAX-triangles.size())return -1;
        uint32_t maxPrimitive=0;
        for(const auto &t:source){
            const Vec3 c{t.e1.y*t.e2.z-t.e1.z*t.e2.y,t.e1.z*t.e2.x-t.e1.x*t.e2.z,t.e1.x*t.e2.y-t.e1.y*t.e2.x};
            if(!Finite(t.p)||!Finite(t.e1)||!Finite(t.e2)||!Finite(t.p+t.e1)||!Finite(t.p+t.e2)||
               !Finite(c)||double(c.x)*c.x+double(c.y)*c.y+double(c.z)*c.z<=1e-24)return -1;
            maxPrimitive=(std::max)(maxPrimitive,t.primitive);
        }
        std::vector<Item> items;items.reserve(source.size());
        const uint32_t first=static_cast<uint32_t>(triangles.size());
        for(uint32_t i=0;i<source.size();i++){Bounds b;b.Add(source[i].p);b.Add(source[i].p+source[i].e1);b.Add(source[i].p+source[i].e2);items.push_back({b,first+i});}
        std::vector<uint32_t> order;order.reserve(source.size());
        triangles.resize(first+source.size());
        const uint32_t root=Build(nodes,items,order);
        for(size_t i=0;i<order.size();i++)triangles[first+i]=source[order[i]-first];
        for(size_t i=root;i<nodes.size();i++)if(nodes[i].count)nodes[i].first+=first;
        meshes.push_back({root,{nodes[root].lo,nodes[root].hi},maxPrimitive});++geometryVersion;return static_cast<int>(meshes.size()-1);
    }
    // Assemble an already-built owned local mesh without sorting/building its BVH again. Geometry remains
    // immutable; per-publication material/primitive bindings are rebased, never written back into the cache.
    int AppendMesh(const Scene &source,uint32_t material,uint32_t primitiveBase){
        const auto sourceMeshes=source.Meshes();const auto sourceNodes=source.Nodes();const auto sourceTriangles=source.Triangles();
        if(packedOwner_||&source==this||sourceMeshes.size()!=1||sourceMeshes[0].root!=0||sourceNodes.empty()||sourceTriangles.empty()||
           sourceNodes.size()>UINT32_MAX-nodes.size()||sourceTriangles.size()>UINT32_MAX-triangles.size())return -1;
        for(const auto &t:sourceTriangles)if(t.primitive>UINT32_MAX-primitiveBase)return -1;
        const uint32_t nodeBase=uint32_t(nodes.size()),triangleBase=uint32_t(triangles.size());
        for(auto node:sourceNodes){
            node.escape+=nodeBase;node.first+=node.count?triangleBase:nodeBase;
            if(node.pad[0]!=UINT32_MAX)node.pad[0]+=nodeBase;
            if(node.pad[1])node.pad[1]+=nodeBase;
            nodes.push_back(node);
        }
        for(auto triangle:sourceTriangles){triangle.material=material;triangle.primitive+=primitiveBase;triangles.push_back(triangle);}
        meshes.push_back({nodeBase,sourceMeshes[0].bounds,sourceMeshes[0].maxPrimitive+primitiveBase});++geometryVersion;return int(meshes.size()-1);
    }
    // Whole replacement, not an accumulating cache: moved or removed occluders cannot leave last-frame instances.
    // Failed placement leaves the preceding snapshot intact and returns false. Callers must not label it current.
    bool Place(const std::vector<Placement> &placements,const std::vector<Unknown> &incomplete,uint64_t frame,const Scene *previous=nullptr,
               InstanceExecutor execute=nullptr,void *context=nullptr,const InstanceReuse *reuse=nullptr,
               TopExecutor buildTop=nullptr,void *topContext=nullptr) {
        if(placements.size()>UINT32_MAX)return false;
        std::vector<Instance> next(placements.size());
        const InstanceJob job{*this,placements,next,reuse};
        if(!(execute?execute(job,context):job.Run(0,placements.size())))return false;
        for(const auto &u:incomplete)if(!Bounds{u.lo,u.hi}.Valid())return false;
        std::vector<Node> top;std::vector<uint32_t> order;
        unsigned refits=0;
        if(!previous||!Refit(*previous,next,top,order,refits)){
            top.clear();order.clear();
            std::vector<Item> items;items.reserve(next.size());
            for(uint32_t i=0;i<next.size();i++)items.push_back({{next[i].lo,next[i].hi},i});
            if(!items.empty())Build(top,items,order,buildTop,topContext);
        }
        topRefits=refits;
        instances.swap(next);topNodes.swap(top);instanceOrder.swap(order);unknown=incomplete;frameId=frame;++placementVersion;
        return true;
    }
    void Clear(){*this=Scene{};}
    std::span<const Node> Nodes()const{return packedOwner_?packedNodes_:std::span<const Node>(nodes);}
    std::span<const Triangle> Triangles()const{return packedOwner_?packedTriangles_:std::span<const Triangle>(triangles);}
    std::span<const Mesh> Meshes()const{return packedOwner_?packedMeshes_:std::span<const Mesh>(meshes);}
    std::vector<Node> nodes,topNodes;
    std::vector<Triangle> triangles;
    std::vector<Instance> instances;
    std::vector<uint32_t> instanceOrder;
    std::vector<Unknown> unknown;
    std::vector<Mesh> meshes;
    uint64_t frameId=0,geometryVersion=0,placementVersion=0;
    unsigned topRefits=0;
private:
    friend class mml::scene::SnapshotBuilder;
    void UsePacked(const Scene &storage,std::shared_ptr<const void> owner){
        packedNodes_=storage.nodes;packedTriangles_=storage.triangles;packedMeshes_=storage.meshes;
        geometryVersion=storage.geometryVersion;packedOwner_=std::move(owner);
    }
    std::shared_ptr<const void> packedOwner_;
    std::span<const Node> packedNodes_;
    std::span<const Triangle> packedTriangles_;
    std::span<const Mesh> packedMeshes_;
    static bool MakeInstance(const Placement &p,const Mesh &mesh,Instance &instance,const Instance *shape)noexcept{
        if((p.flags&~uint32_t(BillboardFacing|BillboardY))||p.flags==(BillboardFacing|BillboardY))return false;
        instance.flags=p.flags;
        if(p.material!=UINT32_MAX){
            if(mesh.maxPrimitive>=p.primitiveCount||p.primitiveCount>UINT32_MAX-p.primitiveBase)return false;
            instance.flags|=InstanceAttributes;
            instance.pad=p.material;instance.pad1=p.primitiveBase;instance.pad2=p.primitiveCount;
        }
        instance.root=mesh.root;instance.id=p.id;
        if(shape){
            memcpy(instance.local,shape->local,sizeof instance.local);
            instance.flags|=shape->flags&InstanceMirrored;
            instance.lo=shape->lo;instance.hi=shape->hi;
            return true;
        }
        if(!Invert(p.world,instance.local))return false;
        const auto *w=p.world;const double det=double(w[0])*(double(w[5])*w[10]-double(w[9])*w[6])-
            double(w[4])*(double(w[1])*w[10]-double(w[9])*w[2])+double(w[8])*(double(w[1])*w[6]-double(w[5])*w[2]);
        if(det<0)instance.flags|=InstanceMirrored;
        Bounds b;Bounds local=mesh.bounds;
        if(p.flags){
            // Rotation is around original local origin, including off-center native billboard meshes.
            const double x=(std::max)(std::abs(local.lo.x),std::abs(local.hi.x)),y=(std::max)(std::abs(local.lo.y),std::abs(local.hi.y)),z=(std::max)(std::abs(local.lo.z),std::abs(local.hi.z));
            const float radius=float(std::sqrt(x*x+y*y+z*z));local={{-radius,-radius,-radius},{radius,radius,radius}};
        }
        for(unsigned corner=0;corner<8;corner++){
            Vec3 x{corner&1?local.hi.x:local.lo.x,corner&2?local.hi.y:local.lo.y,corner&4?local.hi.z:local.lo.z};
            b.Add(Vec3{x.x*p.world[0]+x.y*p.world[4]+x.z*p.world[8]+p.world[12],
                   x.x*p.world[1]+x.y*p.world[5]+x.z*p.world[9]+p.world[13],
                   x.x*p.world[2]+x.y*p.world[6]+x.z*p.world[10]+p.world[14]});
        }
        if(!b.Valid())return false;
        instance.lo=b.lo;instance.hi=b.hi;
        return true;
    }
    static bool Refit(const Scene &previous,const std::vector<Instance> &instances,
                      std::vector<Node> &top,std::vector<uint32_t> &order,unsigned &refits){
        // Refit a coherent scene cheaply. Rebuild after broader changes or accumulated moves,
        // so actors passing through a static city do not gradually degrade traversal quality.
        if(instances.size()<32||instances.size()!=previous.instances.size()||
           previous.instanceOrder.size()!=instances.size()||previous.topNodes.empty())return false;
        size_t changed=0;
        for(size_t i=0;i<instances.size();i++){
            const auto &a=instances[i],&b=previous.instances[i];
            if(a.id!=b.id)return false;
            if(memcmp(&a.lo,&b.lo,sizeof a.lo)||memcmp(&a.hi,&b.hi,sizeof a.hi))changed++;
            if(changed>instances.size()/16)return false;
        }
        if(changed&&previous.topRefits>=16)return false;
        top=previous.topNodes;order=previous.instanceOrder;
        if(changed)for(size_t i=top.size();i--!=0;){
            auto &node=top[i];Bounds bounds;
            if(node.count){
                if(uint64_t(node.first)+node.count>order.size())return false;
                for(uint32_t j=0;j<node.count;j++){
                    const uint32_t index=order[node.first+j];if(index>=instances.size())return false;
                    const auto &instance=instances[index];
                    bounds.Add(Bounds{instance.lo,instance.hi});
                }
            }else{
                if(node.first<=i||node.pad[1]<=i||node.first>=top.size()||node.pad[1]>=top.size())return false;
                const auto &left=top[node.first],&right=top[node.pad[1]];
                bounds.Add(Bounds{left.lo,left.hi});bounds.Add(Bounds{right.lo,right.hi});
            }
            node.lo=bounds.lo;node.hi=bounds.hi;
        }
        refits=previous.topRefits+(changed!=0);return true;
    }
    static uint32_t TreeNodes(size_t count)noexcept{
        if(count<=4)return 1;
        const size_t groups=std::bit_floor(count/4);
        // At this level each group has floor(count/groups) items or one
        // more. Only the groups larger than four split once more.
        return uint32_t(2*(groups+(std::min)(groups,count-4*groups))-1);
    }
    static uint32_t Build(std::vector<Node> &out,std::vector<Item> &items,std::vector<uint32_t> &order,TopExecutor execute=nullptr,void *context=nullptr){
        const auto root=uint32_t(out.size());
        out.resize(out.size()+TreeNodes(items.size()));order.resize(items.size());
        Fill(out,items,order,{root,UINT32_MAX,0,items.size()},execute,context);
        return root;
    }
    static void Fill(std::vector<Node> &out,std::vector<Item> &items,std::vector<uint32_t> &order,Branch branch,
                     TopExecutor execute=nullptr,void *context=nullptr)noexcept{
        const size_t first=branch.first,last=branch.last;
        Bounds bounds,centres;
        for(size_t j=first;j<last;j++){bounds.Add(items[j].bounds);centres.Add(items[j].bounds.lo*.5f+items[j].bounds.hi*.5f);}
        Node &node=out[branch.root];node={};node.lo=bounds.lo;node.hi=bounds.hi;node.pad[0]=branch.reverse;
        node.escape=branch.root+TreeNodes(last-first);
        if(last-first<=4){
            node.first=uint32_t(first);node.count=uint32_t(last-first);
            for(size_t j=first;j<last;j++)order[j]=items[j].id;
            return;
        }
        Vec3 span=centres.hi-centres.lo;unsigned axis=span.y>span.x?1:0;if(span.z>span[axis])axis=2;
        const size_t mid=first+(last-first)/2;
        std::nth_element(items.begin()+first,items.begin()+mid,items.begin()+last,[axis](const Item &a,const Item &b){
            float x=a.bounds.lo[axis]*.5f+a.bounds.hi[axis]*.5f,y=b.bounds.lo[axis]*.5f+b.bounds.hi[axis]*.5f;return x==y?a.id<b.id:x<y;
        });
        const Branch left{branch.root+1,branch.reverse,first,mid};
        const Branch right{left.root+TreeNodes(mid-first),left.root,mid,last};
        node.first=left.root;node.pad[1]=right.root;node.pad[2]=axis;
        if(execute&&last-first>=1024&&execute(TopJob{out,items,order,left,right},context))return;
        Fill(out,items,order,left);Fill(out,items,order,right);
    }
    static bool Invert(const float *m,float out[3][4]){
        for(unsigned i=0;i<16;i++)if(!Finite(m[i]))return false;
        if(m[3]!=0||m[7]!=0||m[11]!=0||m[15]!=1)return false;
        const double a=m[0],b=m[4],c=m[8],d=m[1],e=m[5],f=m[9],g=m[2],h=m[6],i=m[10];
        const double det=a*(e*i-f*h)-b*(d*i-f*g)+c*(d*h-e*g);if(!std::isfinite(det)||std::fabs(det)<1e-18)return false;
        const double inverse[3][3]={{(e*i-f*h)/det,(c*h-b*i)/det,(b*f-c*e)/det},
            {(f*g-d*i)/det,(a*i-c*g)/det,(c*d-a*f)/det},{(d*h-e*g)/det,(b*g-a*h)/det,(a*e-b*d)/det}};
        for(unsigned row=0;row<3;row++){
            double shift=0;for(unsigned col=0;col<3;col++){out[row][col]=static_cast<float>(inverse[row][col]);shift+=inverse[row][col]*m[12+col];}
            out[row][3]=static_cast<float>(-shift);for(float v:out[row])if(!Finite(v))return false;
        }
        return true;
    }
};
} } // mml::ray
