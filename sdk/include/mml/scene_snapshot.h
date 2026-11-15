/* Owned native scene collection for world-ray consumers. See docs/SCENE_RADIANCE_SNAPSHOT.md.
 * Geometry queries are available before material evaluation: triangles have NO opaque flag by default.
 * A geometry-only diagnostic may explicitly override MML_RAY_ACCEPT; production must evaluate opacity.
 * The mod must give this builder exclusive ownership of its scene_draw_observe slot during Collect(). */
#pragma once
#include "mml/mml.h"
#include "mml/ray_scene.h"
#include "mml/native_texture.h"
#include "mml/native_texture_revision.h"
#include <array>
#include <atomic>
#include <memory>
#include <unordered_map>
#include <chrono>
#include <tuple>
#include <xmmintrin.h>

namespace mml { namespace scene {
struct SurfaceTriangle {
    float uv[3][4]; // each vertex: UV0.xy, UV1.xy; original primitive order, independent of BVH sorting
    uint32_t color[3],material;
};
static_assert(sizeof(SurfaceTriangle)==64,"surface triangle GPU layout");
struct SourceMaterial {
    std::array<MmlSceneTextureStage,2> stages{}; // texture members NULL: no borrowed COM reference survives
    std::array<uintptr_t,2> textureKeys{}; // provenance only, NEVER dereference outside the callback
    std::array<uint32_t,2> textureIds{{UINT32_MAX,UINT32_MAX}}; // UINT_MAX absent; UINT_MAX-1 unavailable
    std::array<uint32_t,2> maxAnisotropy{{1,1}}; // owned sampler state, used with actual ray-hit gradients
    std::array<uint32_t,2> maxMipLevel{};
    std::array<float,2> lodBias{};
    uint32_t flags=0,alphaTest=0,alphaReference=0,alphaFunction=0,cull=0,blend=0,sourceBlend=0,destinationBlend=0,factor=0;
    uint32_t unsupported=0; // extra fixed-pipeline state outside the verified material subset
};
struct SourceInstance {uintptr_t visual=0,group=0;uint32_t pass=0,kind=0;float world[16]{};};
struct Snapshot {
    ray::Scene geometry;ray::Bounds coverage;
    std::vector<SurfaceTriangle> surfaces; // compact mode addresses by packed triangle index, material is per instance
    std::vector<SourceMaterial> materials;
    std::vector<SourceInstance> sources; // instance id is index+1; not a durable native lifetime handle
    std::vector<TextureRef> textures; // owned immutable texels shared across validated publications
    std::vector<uint64_t> meshRevisions; // owned content identities, NOT native pointer lifetimes
    std::vector<uint64_t> packedMeshRevisions; // one revision per physical BLAS slot
    size_t liveBlasTriangles=0;
    unsigned meshBuilt=0,meshReused=0,textureCopied=0,textureReused=0;
    size_t cachedMeshBytes=0,cachedTextureBytes=0,stagedMeshPeakBytes=0;
    double nativeCollectMs=0,sourceCopyMs=0,meshBuildMs=0,stagedMeshMs=0,textureMs=0,instanceBuildMs=0,finalizeWaitMs=0;
    double stagedDrawMs=0,packedGeometryMs=0,placementMs=0,pruneMs=0;
    size_t recycledArchiveBytes=0,reusedPrefixBytes=0;
    unsigned workerMeshBuilds=0,workerInstances=0,instancesReused=0,workerTopItems=0;
    unsigned workerJobsOnCaller=0,stagedDraws=0,stagedMeshes=0;
    uint32_t instanceMxcsr=0;
    uint16_t instanceFpu=0;
    double workerWaitMs=0;
    size_t textureBytes=0;unsigned unavailableTextures=0;bool materialsCollected=false,instancedMeshes=false;
    bool compactAttributes=false;
    uint32_t primitiveCount=0;
    uint64_t cycle=0,nativeFrame=0;
    unsigned selected=0,draws=0,unavailable=0,degenerate=0;
    std::span<const SurfaceTriangle> Surfaces()const{return archive_?packedSurfaces_:std::span<const SurfaceTriangle>(surfaces);}
    size_t StableTriangles()const{return stablePrefix_.triangles;}
private:
    friend class SnapshotBuilder;
    friend class RadianceGpu;
    struct PackedArchive {ray::Scene geometry;std::vector<SurfaceTriangle> surfaces;};
    struct PackedPrefix {size_t nodes=0,triangles=0,meshes=0;};
    PackedPrefix stablePrefix_,copiedPrefix_;
    std::shared_ptr<PackedArchive> archive_;
    std::span<const SurfaceTriangle> packedSurfaces_;
    std::weak_ptr<const Snapshot> packedFrom_,packedOwner_;
};
class SnapshotBuilder {
    struct BuildBatch;
    struct RecycledArchive {
        std::shared_ptr<Snapshot::PackedArchive> archive;
        std::vector<uint64_t> revisions;
        Snapshot::PackedPrefix prefix;
    } recycled_;
public:
    SnapshotBuilder()=default;SnapshotBuilder(const SnapshotBuilder &)=delete;SnapshotBuilder &operator=(const SnapshotBuilder &)=delete;
    ~SnapshotBuilder(){Drain(false);}
    // Called for every MAIN-world RENDER_BEGIN, and invalidated on frame/reset/scene transitions.
    uint64_t Begin(){Drain(false);valid_=false;previous_=current_;current_.reset();return ++cycle_;} // only an existing GPU/external lease keeps it alive
    void Invalidate(){Drain(false);valid_=false;}
    // Complete any CPU-only finalization before asking for the current lease.
    bool Complete(){Drain(true);return valid_;}
    uint64_t Cycle()const{return cycle_;}
    const Snapshot *Current(uint64_t cycle)const{return valid_&&current_&&current_->cycle==cycle?current_.get():nullptr;}
    std::shared_ptr<const Snapshot> Lease(uint64_t cycle)const{return valid_&&current_&&current_->cycle==cycle?current_:nullptr;}
    void DropSnapshot(){Invalidate();previous_.reset();current_.reset();Discard();recycled_={};textureIds_.clear();materialIds_.clear();placedMeshes_.clear();placements_.clear();unknown_.clear();host_=nullptr;mod_=nullptr;materialDevice_=nullptr;}
    void Release(){DropSnapshot();decltype(meshCache_){}.swap(meshCache_);decltype(textureCache_){}.swap(textureCache_);
        decltype(verifiedMeshes_){}.swap(verifiedMeshes_);
        decltype(textureContent_){}.swap(textureContent_);decltype(textureIds_){}.swap(textureIds_);
        decltype(materialIds_){}.swap(materialIds_);
        decltype(placedMeshes_){}.swap(placedMeshes_);
        decltype(placedMeshContents_){}.swap(placedMeshContents_);
        decltype(stagedDraws_){}.swap(stagedDraws_);
        decltype(stagedGeometry_){}.swap(stagedGeometry_);
        decltype(placements_){}.swap(placements_);decltype(unknown_){}.swap(unknown_);meshBytes_=textureBytes_=0;
        nodeHint_=triangleHint_=surfaceHint_=drawHint_=materialHint_=0;}
    // With retainPacked, read mesh data through Nodes(), Triangles(), Meshes() and Surfaces().
    // With deferFinalize, true means capture accepted. Call Complete() before requesting its lease.
    // A prepared callback can run on the owned CPU worker. Keep its context alive until
    // completion or cancellation and use only owned data. It must not call the host or D3D.
    using Prepared = void (*)(const Snapshot &,void *) noexcept;
    bool Collect(const MmlHost *host,MmlMod *mod,const ray::Bounds &coverage,bool materials=false,bool instanceMeshes=false,bool parallelMeshes=false,bool compactAttributes=false,bool retainPacked=false,
                 uint32_t collectFlags=MML_SCENE_COLLECT_BILLBOARDS,bool deferFinalize=false,Prepared prepared=nullptr,void *context=nullptr,
                 bool stageGeometry=false){
        Invalidate();if(!host||!mod||!cycle_||!coverage.Valid()||!MML_HAS(host,scene_draw_collect)||(compactAttributes&&!instanceMeshes)||(retainPacked&&!compactAttributes))return false;
        if((collectFlags&~MML_SCENE_COLLECT_BILLBOARDS)&&!MML_HAS(host,scene_draw_collect_ex))return false;
        host_=host;mod_=mod;failed_=false;
        materialDevice_=materials&&MML_HAS(host,render_device)&&host->render_device?
            static_cast<IDirect3DDevice9 *>(host->render_device(9)):nullptr;
        try{candidate_.reset(new Snapshot);candidate_->cycle=cycle_;candidate_->coverage=coverage;
            candidate_->materialsCollected=materials;candidate_->instancedMeshes=instanceMeshes;
            candidate_->compactAttributes=compactAttributes;
            ++texturePublication_;
            placements_.clear();unknown_.clear();textureIds_.clear();materialIds_.clear();placedMeshes_.clear();placedMeshContents_.clear();
            if(!retainPacked){
                candidate_->geometry.nodes.reserve(nodeHint_);
                candidate_->geometry.triangles.reserve(triangleHint_);
                candidate_->surfaces.reserve(surfaceHint_);
                candidate_->geometry.meshes.reserve(drawHint_);
            }
            candidate_->sources.reserve(drawHint_);
            candidate_->materials.reserve(compactAttributes?materialHint_:drawHint_);
            candidate_->meshRevisions.reserve(drawHint_);}
        catch(...){candidate_.reset();return false;}
        if(!host_->scene_draw_observe(mod_,Observe,this)){candidate_.reset();return false;}
        MEMORYSTATUSEX memory{sizeof memory};
        const bool parallel=parallelMeshes&&instanceMeshes&&GlobalMemoryStatusEx(&memory)&&memory.ullAvailVirtual>=128ull*1024*1024;
        BuildBatch local;
        std::unique_ptr<DeferredBuild> deferred;
        stageDraws_=false;
        if(deferFinalize&&parallel&&MML_HAS(host_,scene_owned_join)&&host_->scene_owned_join&&host_->scene_owned_submit)
            try{deferred=std::make_unique<DeferredBuild>();}catch(...){}
        if(deferred)try{
            stagedDraws_.reserve((std::min)(drawHint_,MaxStagedDraws()));stageDraws_=true;
        }catch(const std::bad_alloc &){}
        stageGeometry_=stageDraws_&&stageGeometry;
        auto &builds=deferred?deferred->builds:local;
        if(parallel)builds_=&builds;
        collecting_=true;MmlSceneCollection collected{};collected.size=sizeof collected;
        const float lo[3]={coverage.lo.x,coverage.lo.y,coverage.lo.z},hi[3]={coverage.hi.x,coverage.hi.y,coverage.hi.z};
        const auto collectStart=Time::now();const bool ok=MML_HAS(host_,scene_draw_collect_ex)?
            host_->scene_draw_collect_ex(mod_,lo,hi,collectFlags,&collected)!=0:
            host_->scene_draw_collect(mod_,lo,hi,&collected)!=0;
        collecting_=false;candidate_->nativeCollectMs=Elapsed(collectStart);
        // Own missing metadata before another host mutation/unregister can retire its borrowed storage.
        if(ok&&!failed_)try{for(unsigned i=0;i<collected.missing_count;i++){
            const auto &m=collected.missing[i];unknown_.push_back({{m.lo[0],m.lo[1],m.lo[2]},m.reason,{m.hi[0],m.hi[1],m.hi[2]},0});
        }}catch(...){failed_=true;}
        host_->scene_draw_observe(mod_,nullptr,nullptr);
        try{FlushBuilds();}catch(...){failed_=true;}
        builds_=nullptr;
        if(!ok||failed_){Discard();return false;}
        const FinalizeInput input{collected.frame,collected.selected,collected.draws,parallel,retainPacked,prepared,context};
        if(deferred){
            deferred->input=input;deferred->mxcsr=_mm_getcsr();
#if defined(_M_IX86)
            uint16_t nativeControl;
            __asm fnstcw nativeControl
            deferred->fpu=nativeControl;
#endif
            pending_=std::move(deferred);
            pending_->ticket=host_->scene_owned_submit(mod_,RunDeferred,this);
            if(pending_->ticket)return true;
            deferred=std::move(pending_);
        }
        if(!PreparePublication(builds,input))return false;
        Publish();return true;
    }
private:
    struct FinalizeInput {uint64_t frame;unsigned selected,draws;bool parallel,retainPacked;Prepared prepared;void *context;};
    bool PreparePublication(BuildBatch &builds,const FinalizeInput &input){
        try{
            candidate_->nativeFrame=input.frame;candidate_->selected=input.selected;
            const auto buildStart=Time::now();
            bool staged=false;
            {Timer timer{candidate_->stagedDrawMs};staged=BuildStagedDraws(builds,input.parallel);}
            if(!staged){Discard();return false;}
            const auto previous=candidate_->instancedMeshes?previous_.lock():nullptr;
            candidate_->instanceMxcsr=_mm_getcsr()&~0x3fu;
#if defined(_M_IX86)
            uint16_t nativeControl;
            __asm fnstcw nativeControl
            candidate_->instanceFpu=nativeControl;
#endif
            const bool reusePose=previous&&previous->instanceMxcsr==candidate_->instanceMxcsr&&previous->instanceFpu==candidate_->instanceFpu;
            const ray::Scene::InstanceReuse reuse{PreviousShape,previous.get()};
            PlacementBuild placement{input.parallel?&builds.slots[0]:nullptr};
            if(candidate_->draws!=input.draws){Discard();return false;}
            bool packed=false;
            {Timer timer{candidate_->packedGeometryMs};packed=!candidate_->instancedMeshes||PackMeshes(previous,input.retainPacked);}
            if(!packed){Discard();return false;}
            bool placed=false;
            {Timer timer{candidate_->placementMs};
                placed=candidate_->geometry.Place(placements_,unknown_,input.frame,previous?&previous->geometry:nullptr,
                    PlaceParallel,&placement,reusePose?&reuse:nullptr,input.parallel?BuildTopParallel:nullptr,&builds.slots[0]);}
            if(!placed){Discard();return false;}
            candidate_->workerInstances=builds.slots[0].placed;
            candidate_->instancesReused=placement.reused;
            candidate_->workerTopItems=builds.slots[0].topItems;
            candidate_->instanceBuildMs=Elapsed(buildStart);
            nodeHint_=NextCapacity(nodeHint_,candidate_->geometry.Nodes().size());
            triangleHint_=NextCapacity(triangleHint_,candidate_->geometry.Triangles().size());
            surfaceHint_=NextCapacity(surfaceHint_,candidate_->Surfaces().size());
            drawHint_=NextCapacity(drawHint_,candidate_->sources.size());
            if(candidate_->compactAttributes)materialHint_=NextCapacity(materialHint_,candidate_->materials.size());
            {Timer timer{candidate_->pruneMs};PruneCaches();}
            candidate_->cachedMeshBytes=meshBytes_;candidate_->cachedTextureBytes=textureBytes_;
        }catch(...){Discard();return false;}
        if(input.prepared)input.prepared(*candidate_,input.context);
        return true;
    }
    static void RunDeferred(void *context){
        auto &builder=*static_cast<SnapshotBuilder *>(context);
        auto &job=*builder.pending_;
        const unsigned sse=_mm_getcsr();_mm_setcsr(job.mxcsr);
#if defined(_M_IX86)
        uint16_t old,requested=job.fpu;
        __asm fnstcw old
        __asm fldcw requested
#endif
        job.success=builder.PreparePublication(job.builds,job.input);
#if defined(_M_IX86)
        __asm fldcw old
#endif
        _mm_setcsr(sse);
    }
    void Discard(){candidate_.reset();placedMeshContents_.clear();stagedDraws_.clear();stagedGeometry_.clear();stagedGeometryBytes_=0;}
    void Publish(){current_.swap(candidate_);candidate_.reset();valid_=true;}
    void Drain(bool publish){
        if(!pending_)return;
        const auto started=Time::now();
        const bool ready=host_->scene_owned_join(mod_,pending_->ticket,publish?0:1)&&pending_->success;
        pending_.reset();
        if(ready){candidate_->finalizeWaitMs=Elapsed(started);Publish();}
        else Discard();
    }
    using Time=std::chrono::steady_clock;
    static double Elapsed(Time::time_point start){return std::chrono::duration<double,std::milli>(Time::now()-start).count();}
    static size_t NextCapacity(size_t previous,size_t actual){
        // Keep modest headroom for changing actors, and shrink after leaving a larger scene.
        return actual>previous||actual<previous/2?actual+actual/8:previous;
    }
    struct Timer {double &total;Time::time_point start=Time::now();~Timer(){total+=Elapsed(start);}};
    static uint64_t TextureHash(const TextureSnapshot &texture){uint64_t hash=14695981039346656037ull;
        auto add=[&](const void *data,size_t count){const auto *p=static_cast<const uint8_t *>(data);for(size_t i=0;i<count;i++){hash^=p[i];hash*=1099511628211ull;}};
        add(&texture.native,sizeof texture.native);for(const auto &m:texture.mips){add(&m.width,sizeof m.width);add(&m.height,sizeof m.height);add(m.bytes.data(),m.bytes.size());}return hash;}
    static bool EqualTexture(const TextureSnapshot &a,const TextureSnapshot &b){
        if(a.native!=b.native||a.format!=b.format||a.mips.size()!=b.mips.size())return false;
        for(size_t i=0;i<a.mips.size();i++){const auto &x=a.mips[i],&y=b.mips[i];if(x.width!=y.width||x.height!=y.height||x.pitch!=y.pitch||x.rows!=y.rows||x.bytes!=y.bytes)return false;}return true;}
    struct MeshContent {
        std::vector<MmlSceneVertex> vertices;std::vector<uint32_t> indices;
        ray::Scene local;std::vector<SurfaceTriangle> surfaces;
        uint32_t fvf=0,cull=0;unsigned degenerate=0;size_t bytes=0;uint64_t revision=0,seen=0,born=0;
    };
    struct StagedDraw {
        SourceInstance source;
        SourceMaterial material;
        std::shared_ptr<const MeshContent> content;
        uint32_t placementFlags,geometrySlot;
    };
    struct StagedGeometry {
        std::vector<MmlSceneVertex> vertices;
        std::vector<uint32_t> indices;
        uint32_t fvf=0;
        uint64_t revision=0;
    };
    static constexpr size_t MaxStagedDraws(){return (8u*1024*1024)/sizeof(StagedDraw);}
    struct TextureContent {
        std::shared_ptr<const TextureSnapshot> texture;
        size_t bytes=0;
        uint64_t seen=0,revision=0,publication=0;
        uint32_t publicationId=0;
    };
    struct VerifiedMesh {std::weak_ptr<MeshContent> content;uint64_t seen;};
    struct BuildSlot {
        PTP_WORK work=nullptr;
        std::atomic<bool> claimed{true};
        std::shared_ptr<MeshContent> mesh;
        std::vector<ray::Triangle> triangles;
        const ray::Scene::InstanceJob *placement=nullptr;
        const ray::Scene::TopJob *top=nullptr;
        uint32_t hash=0,mxcsr=0;
        uint64_t key=0;
        uint16_t fpu=0;
        bool built=false;
        unsigned placed=0,placementReused=0,topItems=0;
        ~BuildSlot(){if(work){WaitForThreadpoolWorkCallbacks(work,FALSE);CloseThreadpoolWork(work);}}
        bool Prepare(){
            if(!work)work=CreateThreadpoolWork(Run,this,nullptr);
            if(!work)return false;
            built=false;placementReused=0;mxcsr=_mm_getcsr();
#if defined(_M_IX86)
            uint16_t nativeControl;
            __asm fnstcw nativeControl
            fpu=nativeControl;
#endif
            return true;
        }
        void Submit(){
            claimed.store(false,std::memory_order_release);
            SubmitThreadpoolWork(work);
        }
        bool RunIfWaiting(){
            if(claimed.exchange(true,std::memory_order_acq_rel))return false;
            Execute();
            return true;
        }
        void Join(bool onCaller){WaitForThreadpoolWorkCallbacks(work,onCaller?TRUE:FALSE);}
        static void CALLBACK Run(PTP_CALLBACK_INSTANCE,void *context,PTP_WORK){
            static_cast<BuildSlot *>(context)->RunIfWaiting();
        }
        void Execute(){
            const auto sse=_mm_getcsr();_mm_setcsr(mxcsr);
#if defined(_M_IX86)
            uint16_t previous,requested=fpu;
            __asm fnstcw previous
            __asm fldcw requested
#endif
            try{
                if(placement)built=placement->Run(0,placed,&placementReused);
                else if(top){top->Run(0);built=true;}
                else built=mesh->local.AddMesh(triangles)==0;
            }catch(...){built=false;}
#if defined(_M_IX86)
            __asm fldcw previous
#endif
            _mm_setcsr(sse);
        }
    };
    struct BuildBatch {std::array<BuildSlot,2> slots;size_t count=0;};
    struct DeferredBuild {BuildBatch builds;FinalizeInput input{};uint64_t ticket=0;unsigned mxcsr=0;uint16_t fpu=0;bool success=false;};
    struct PlacementBuild { BuildSlot *worker;unsigned reused=0; };
    static const ray::Instance *PreviousShape(size_t i,const ray::Placement &p,const ray::Scene::Mesh &mesh,const void *context) noexcept {
        const auto &old=*static_cast<const Snapshot *>(context);
        const auto meshes=old.geometry.Meshes();
        if(i>=old.sources.size()||i>=old.geometry.instances.size()||p.mesh>=meshes.size())return nullptr;
        const auto &shape=old.geometry.instances[i];const auto &oldMesh=meshes[p.mesh];
        if(shape.root!=oldMesh.root||(shape.flags&(ray::BillboardFacing|ray::BillboardY))!=p.flags||
           memcmp(p.world,old.sources[i].world,sizeof p.world)!=0||
           memcmp(&mesh.bounds.lo,&oldMesh.bounds.lo,sizeof mesh.bounds.lo)!=0||
           memcmp(&mesh.bounds.hi,&oldMesh.bounds.hi,sizeof mesh.bounds.hi)!=0)return nullptr;
        return &shape;
    }
    static bool PlaceParallel(const ray::Scene::InstanceJob &job,void *context){
        auto &placement=*static_cast<PlacementBuild *>(context);
        const size_t count=job.placements.size();
        if(!placement.worker||count<1024||!placement.worker->Prepare())return job.Run(0,count,&placement.reused);
        auto &slot=*placement.worker;
        slot.placement=&job;slot.placed=unsigned(count/2);
        slot.Submit();
        const bool mainOk=job.Run(slot.placed,count,&placement.reused);
        slot.Join(slot.RunIfWaiting());
        slot.placement=nullptr;
        placement.reused+=slot.placementReused;
        return mainOk&&slot.built;
    }
    static bool BuildTopParallel(const ray::Scene::TopJob &job,void *context){
        auto &slot=*static_cast<BuildSlot *>(context);
        if(!slot.Prepare())return false;
        slot.top=&job;slot.topItems=unsigned(job.Items(0));
        slot.Submit();
        job.Run(1);
        slot.Join(slot.RunIfWaiting());
        slot.top=nullptr;
        return true;
    }
    void RememberMesh(const std::shared_ptr<MeshContent> &entry,uint32_t hash,uint64_t key){
        entry->bytes=sizeof(MeshContent)+entry->vertices.capacity()*sizeof(MmlSceneVertex)+entry->indices.capacity()*4+entry->surfaces.capacity()*sizeof(SurfaceTriangle)+
            entry->local.nodes.capacity()*sizeof(ray::Node)+entry->local.triangles.capacity()*sizeof(ray::Triangle)+entry->local.meshes.capacity()*sizeof(ray::Scene::Mesh);
        candidate_->meshBuilt++;
        if(entry->bytes<=128u*1024*1024&&meshBytes_<=128u*1024*1024-entry->bytes){
            meshCache_[hash].push_back(entry);meshBytes_+=entry->bytes;
            if(key)verifiedMeshes_[key]={entry,cycle_};
        }
    }
    void FlushBuilds(){
        if(!builds_||!builds_->count)return;
        const size_t count=builds_->count;
        // If the pool has not scheduled a job yet, finish it on this
        // thread instead of waiting for a worker that has done no work.
        std::array<bool,2> onCaller{};
        for(size_t i=0;i<count;i++){
            onCaller[i]=builds_->slots[i].RunIfWaiting();
            candidate_->workerJobsOnCaller+=onCaller[i]?1u:0u;
        }
        const auto start=Time::now();
        for(size_t i=0;i<count;i++)builds_->slots[i].Join(onCaller[i]);
        builds_->count=0;
        candidate_->workerWaitMs+=Elapsed(start);
        for(size_t i=0;i<count;i++){
            auto &slot=builds_->slots[i];
            if(slot.built)RememberMesh(slot.mesh,slot.hash,slot.key);
            else failed_=true;
            slot.mesh.reset();std::vector<ray::Triangle>().swap(slot.triangles);
        }
    }
    bool BuildStagedDraws(BuildBatch &builds,bool parallel){
        if(!parallel||stagedGeometry_.empty())return PublishStagedDraws();
        // The native collection already joined this same bounded set of workers.
        builds_=&builds;
        bool ok=false;
        try{ok=PublishStagedDraws();}catch(...){}
        try{FlushBuilds();}catch(...){ok=false;}
        builds_=nullptr;
        return ok&&!failed_;
    }
    bool QueueMesh(const std::shared_ptr<MeshContent> &entry,uint32_t hash,uint64_t key,std::vector<ray::Triangle> &triangles){
        if(!builds_||triangles.size()<16||triangles.size()>2048)return false;
        if(builds_->count==builds_->slots.size())FlushBuilds();
        auto &slot=builds_->slots[builds_->count];
        if(!slot.Prepare())return false;
        slot.mesh=entry;slot.hash=hash;slot.key=key;
        slot.triangles.swap(triangles);builds_->count++;candidate_->workerMeshBuilds++;
        slot.Submit();
        return true;
    }
    template<class T>static void ReserveArchive(std::vector<T> &data,size_t size){
        data.reserve(size+(std::min)(size/8,size_t(2u*1024*1024)/sizeof(T)));
    }
    static size_t ArchiveBytes(const Snapshot::PackedArchive &archive){
        const auto &scene=archive.geometry;
        return sizeof(archive)+scene.nodes.capacity()*sizeof(ray::Node)+scene.triangles.capacity()*sizeof(ray::Triangle)+
            scene.meshes.capacity()*sizeof(ray::Scene::Mesh)+archive.surfaces.capacity()*sizeof(SurfaceTriangle);
    }
    void RetireArchive(const std::shared_ptr<const Snapshot> &previous){
        constexpr size_t budget=32u*1024*1024;
        MEMORYSTATUSEX memory{sizeof memory};
        if(!GlobalMemoryStatusEx(&memory)||memory.ullAvailVirtual<128ull*1024*1024+budget){recycled_={};return;}
        if(!previous||!previous->archive_){recycled_={};return;}
        const auto prefix=previous->stablePrefix_;
        if(prefix.meshes>previous->packedMeshRevisions.size()||
           ArchiveBytes(*previous->archive_)+uint64_t(prefix.meshes)*sizeof(uint64_t)>budget){recycled_={};return;}
        try{
            std::vector<uint64_t> revisions(previous->packedMeshRevisions.begin(),previous->packedMeshRevisions.begin()+prefix.meshes);
            recycled_={previous->archive_,std::move(revisions),prefix};
        }catch(const std::bad_alloc &){recycled_={};}
    }
    bool PackMeshes(const std::shared_ptr<const Snapshot> &previous,bool retainPacked){
        if(!retainPacked)recycled_={};
        // Age classifies exact current content for packing only. Recently built
        // meshes stay in the current tail instead of churning the retained prefix.
        auto &revisions=candidate_->packedMeshRevisions;
        std::vector<uint32_t> order(placedMeshContents_.size()),remap(order.size(),UINT32_MAX);
        for(uint32_t i=0;i<order.size();i++)order[i]=i;
        const auto stable=[&](uint32_t i){return retainPacked&&cycle_-placedMeshContents_[i]->born>=2;};
        std::sort(order.begin(),order.end(),[&](uint32_t a,uint32_t b){
            if(stable(a)!=stable(b))return stable(a);
            return placedMeshContents_[a]->revision<placedMeshContents_[b]->revision;
        });
        size_t liveNodes=0,liveTriangles=0;
        for(const auto &content:placedMeshContents_){
            liveNodes+=content->local.nodes.size();liveTriangles+=content->local.triangles.size();
        }
        candidate_->liveBlasTriangles=liveTriangles;
        const bool compact=candidate_->compactAttributes;
        Snapshot::PackedPrefix kept;
        size_t newNodes=liveNodes,newTriangles=liveTriangles,newMeshes=order.size();
        if(previous&&previous->compactAttributes==compact&&previous->packedMeshRevisions.size()==previous->geometry.Meshes().size()&&!order.empty()){
            const auto &old=previous->geometry;
            const auto &oldRevisions=previous->packedMeshRevisions;
            const auto prefix=retainPacked?previous->stablePrefix_:
                Snapshot::PackedPrefix{old.Nodes().size(),old.Triangles().size(),old.Meshes().size()};
            newNodes=newTriangles=newMeshes=0;
            for(uint32_t slot=0;slot<prefix.meshes;slot++){
                const auto match=placedMeshes_.find(oldRevisions[slot]);
                if(match!=placedMeshes_.end())remap[match->second]=slot;
            }
            for(uint32_t i:order)if(remap[i]==UINT32_MAX){
                newNodes+=placedMeshContents_[i]->local.nodes.size();
                newTriangles+=placedMeshContents_[i]->local.triangles.size();
                ++newMeshes;
            }
            const size_t triangleBytes=sizeof(ray::Triangle)+(compact?sizeof(SurfaceTriangle):0);
            const uint64_t liveBytes=uint64_t(liveNodes)*sizeof(ray::Node)+uint64_t(liveTriangles)*triangleBytes;
            const uint64_t packedBytes=uint64_t(prefix.nodes+newNodes)*sizeof(ray::Node)+uint64_t(prefix.triangles+newTriangles)*triangleBytes;
            if(prefix.meshes&&packedBytes<=liveBytes+std::min<uint64_t>(liveBytes/8,8u*1024*1024)){
                kept=prefix;revisions.assign(oldRevisions.begin(),oldRevisions.begin()+prefix.meshes);
                candidate_->copiedPrefix_=prefix;
                candidate_->packedFrom_=previous;
                candidate_->packedOwner_=candidate_;
            }else{
                std::fill(remap.begin(),remap.end(),UINT32_MAX);
                newNodes=liveNodes;newTriangles=liveTriangles;newMeshes=order.size();
            }
        }
        const bool keepPrefix=kept.meshes!=0;
        const auto oldNodes=keepPrefix?previous->geometry.Nodes().first(kept.nodes):std::span<const ray::Node>{};
        const auto oldTriangles=keepPrefix?previous->geometry.Triangles().first(kept.triangles):std::span<const ray::Triangle>{};
        const auto oldMeshes=keepPrefix?previous->geometry.Meshes().first(kept.meshes):std::span<const ray::Scene::Mesh>{};
        const auto oldSurfaces=keepPrefix&&compact?previous->Surfaces().first(kept.triangles):std::span<const SurfaceTriangle>{};
        const size_t nodeCount=oldNodes.size()+newNodes,triangleCount=oldTriangles.size()+newTriangles;
        const size_t meshCount=oldMeshes.size()+newMeshes;
        std::shared_ptr<Snapshot::PackedArchive> archive;
        Snapshot::PackedPrefix recycledPrefix;
        if(retainPacked&&keepPrefix&&previous->archive_){
            const auto &old=*previous->archive_;
            // Only append into untouched reserved capacity. Published snapshots hold fixed-length
            // spans and never consult the mutable vector size or a later generation's tail.
            if(old.geometry.nodes.size()==oldNodes.size()&&old.geometry.nodes.capacity()>=nodeCount&&
               old.geometry.triangles.size()==oldTriangles.size()&&old.geometry.triangles.capacity()>=triangleCount&&
               old.geometry.meshes.size()==oldMeshes.size()&&old.geometry.meshes.capacity()>=meshCount&&
               old.surfaces.size()==oldSurfaces.size()&&old.surfaces.capacity()>=triangleCount)archive=previous->archive_;
        }
        const bool appendToArchive=archive!=nullptr;
        if(retainPacked&&!archive&&recycled_.archive&&recycled_.archive.use_count()==1){
            // The archive is private and has no remaining Snapshot or Scene lease.
            // A matching owned content-revision sequence proves this prefix's bytes,
            // including compact attributes and rebased BLAS offsets. Retire only
            // this unobserved allocation's tail, never a published reader's data.
            const auto &old=*recycled_.archive;
            if(old.geometry.nodes.capacity()>=nodeCount&&old.geometry.triangles.capacity()>=triangleCount&&
               old.geometry.meshes.capacity()>=meshCount&&old.surfaces.capacity()>=triangleCount){
                const size_t common=(std::min)(recycled_.prefix.meshes,kept.meshes);
                if(common&&recycled_.revisions.size()>=common&&
                   std::equal(recycled_.revisions.begin(),recycled_.revisions.begin()+common,revisions.begin()))
                    recycledPrefix=common==kept.meshes?kept:recycled_.prefix;
                archive=std::move(recycled_.archive);
                candidate_->recycledArchiveBytes=ArchiveBytes(*archive);
            }
        }
        if(retainPacked&&!archive){
            archive=std::make_shared<Snapshot::PackedArchive>();
            ReserveArchive(archive->geometry.nodes,nodeCount);ReserveArchive(archive->geometry.triangles,triangleCount);
            ReserveArchive(archive->geometry.meshes,meshCount);ReserveArchive(archive->surfaces,triangleCount);
        }
        auto &scene=archive?archive->geometry:candidate_->geometry;
        auto &surfaces=archive?archive->surfaces:candidate_->surfaces;
        if(keepPrefix&&!appendToArchive){
            scene.nodes.resize(recycledPrefix.nodes);scene.triangles.resize(recycledPrefix.triangles);
            scene.meshes.resize(recycledPrefix.meshes);
            if(compact)surfaces.resize(recycledPrefix.triangles);
            scene.nodes.insert(scene.nodes.end(),oldNodes.begin()+recycledPrefix.nodes,oldNodes.end());
            scene.triangles.insert(scene.triangles.end(),oldTriangles.begin()+recycledPrefix.triangles,oldTriangles.end());
            scene.meshes.insert(scene.meshes.end(),oldMeshes.begin()+recycledPrefix.meshes,oldMeshes.end());
            scene.geometryVersion=previous->geometry.geometryVersion;
            if(compact)surfaces.insert(surfaces.end(),oldSurfaces.begin()+recycledPrefix.triangles,oldSurfaces.end());
            candidate_->reusedPrefixBytes=recycledPrefix.nodes*sizeof(ray::Node)+recycledPrefix.triangles*
                (sizeof(ray::Triangle)+(compact?sizeof(SurfaceTriangle):0))+recycledPrefix.meshes*sizeof(ray::Scene::Mesh);
        }
        if(!keepPrefix&&!appendToArchive){
            scene.nodes.clear();scene.triangles.clear();scene.meshes.clear();scene.geometryVersion=0;
            if(compact)surfaces.clear();
        }
        candidate_->stablePrefix_=retainPacked?kept:Snapshot::PackedPrefix{};
        for(uint32_t i:order){
            if(remap[i]!=UINT32_MAX)continue;
            const auto &content=*placedMeshContents_[i];
            const int mesh=scene.AppendMesh(content.local,0,0);
            if(mesh<0)return false;
            if(compact)for(const auto &triangle:content.local.triangles)
                surfaces.push_back(content.surfaces[triangle.primitive]);
            remap[i]=uint32_t(mesh);
            revisions.push_back(content.revision);
            if(stable(i))candidate_->stablePrefix_={scene.nodes.size(),scene.triangles.size(),scene.meshes.size()};
        }
        if(archive){
            candidate_->geometry.UsePacked(scene,archive);
            candidate_->packedSurfaces_=surfaces;
            candidate_->archive_=std::move(archive);
        }
        if(retainPacked&&previous&&candidate_->archive_!=previous->archive_)RetireArchive(previous);
        for(auto &p:placements_)p.mesh=remap[p.mesh];
        placedMeshContents_.clear();return true;
    }
    static uint64_t ReadRevision(IDirect3DBaseTexture9 *texture){
        uint64_t revision=0;
        DWORD bytes=sizeof revision;
        if(FAILED(texture->GetPrivateData(NativeTextureRevision,&revision,&bytes))||bytes!=sizeof revision)return 0;
        return revision;
    }
    static void HashBytes(uint32_t &hash,const void *data,size_t bytes){
        // The hash only selects a bucket. Exact vertex/index equality still
        // decides reuse, so a word hash avoids x86's per-byte 64-bit multiply.
        auto *p=static_cast<const uint8_t *>(data);
        while(bytes>=sizeof(uint32_t)){
            uint32_t word;
            memcpy(&word,p,sizeof word);
            hash=(hash^word)*16777619u;
            p+=sizeof word;
            bytes-=sizeof word;
        }
        while(bytes--!=0)hash=(hash^*p++)*16777619u;
    }
    static auto MaterialState(const SourceMaterial &m){
        return std::tie(m.textureKeys,m.textureIds,m.maxAnisotropy,m.maxMipLevel,m.flags,m.alphaTest,m.alphaReference,
                        m.alphaFunction,m.cull,m.blend,m.sourceBlend,m.destinationBlend,m.factor,m.unsupported);
    }
    static bool EqualMaterial(const SourceMaterial &a,const SourceMaterial &b){
        constexpr size_t stageBytes=offsetof(MmlSceneTextureStage,mip_filter)+sizeof(uint32_t)-offsetof(MmlSceneTextureStage,color_op);
        static_assert(stageBytes==15*sizeof(uint32_t),"native stage numeric fields");
        return MaterialState(a)==MaterialState(b)&&memcmp(a.lodBias.data(),b.lodBias.data(),sizeof a.lodBias)==0&&
            memcmp(&a.stages[0].color_op,&b.stages[0].color_op,stageBytes)==0&&
            memcmp(&a.stages[1].color_op,&b.stages[1].color_op,stageBytes)==0;
    }
    uint32_t OwnMaterial(const SourceMaterial &material){
        auto &snapshot=*candidate_;
        const uint32_t next=uint32_t(snapshot.materials.size());
        if(snapshot.compactAttributes){
            const uint32_t key[]={material.textureIds[0],material.textureIds[1],material.flags,material.alphaTest,
                material.alphaReference,material.cull,material.blend,material.factor,material.stages[0].color_op};
            uint32_t hash=2166136261u;HashBytes(hash,key,sizeof key);
            const auto range=materialIds_.equal_range(hash);
            for(auto i=range.first;i!=range.second;++i)
                if(EqualMaterial(snapshot.materials[i->second],material))return i->second;
            materialIds_.emplace(hash,next);
        }
        snapshot.materials.push_back(material);
        return next;
    }
    static const MmlSceneVertex &Vertex(const MmlSceneGeometry &g,unsigned index){
        return *reinterpret_cast<const MmlSceneVertex *>(reinterpret_cast<const uint8_t *>(g.vertex)+size_t(index)*g.vertex_stride);
    }
    static bool Degenerate(ray::Vec3 a,ray::Vec3 b){
        const ray::Vec3 cross{a.y*b.z-a.z*b.y,a.z*b.x-a.x*b.z,a.x*b.y-a.y*b.x};
        return double(cross.x)*cross.x+double(cross.y)*cross.y+double(cross.z)*cross.z<=1e-24;
    }
    std::shared_ptr<MeshContent> VerifiedContent(const MmlSceneGeometry &g,uint32_t cull){
        const uint64_t key=cull<4?(g.content_revision<<2)|cull:0;
        if(g.content_revision&&key){
            const auto found=verifiedMeshes_.find(key);
            if(found!=verifiedMeshes_.end())if(auto entry=found->second.content.lock()){
                found->second.seen=cycle_;
                entry->seen=cycle_;candidate_->meshReused++;return entry;
            }
        }
        return {};
    }
    std::shared_ptr<MeshContent> OwnMesh(const MmlSceneGeometry &g,uint32_t cull,StagedGeometry *staged=nullptr){
        if(g.triangles>UINT32_MAX/3||g.vertices>UINT32_MAX/sizeof(MmlSceneVertex))return {};
        if(auto verified=VerifiedContent(g,cull))return verified;
        const uint64_t key=cull<4?(g.content_revision<<2)|cull:0;
        uint32_t hash=2166136261u;const uint32_t info[]={g.vertices,g.triangles,g.source_fvf,cull};HashBytes(hash,info,sizeof info);
        const bool contiguous=g.vertex_stride==sizeof(MmlSceneVertex);
        if(contiguous)HashBytes(hash,g.vertex,size_t(g.vertices)*sizeof(MmlSceneVertex));
        else for(unsigned i=0;i<g.vertices;i++)HashBytes(hash,&Vertex(g,i),sizeof(MmlSceneVertex));
        HashBytes(hash,g.index,size_t(g.triangles)*3*sizeof(uint32_t));
        auto equalMesh=[&](const MeshContent &entry){
            if(entry.fvf!=g.source_fvf||entry.cull!=cull||entry.vertices.size()!=g.vertices||entry.indices.size()!=size_t(g.triangles)*3)return false;
            bool equal=memcmp(entry.indices.data(),g.index,entry.indices.size()*sizeof(uint32_t))==0;
            if(equal&&contiguous)equal=memcmp(entry.vertices.data(),g.vertex,size_t(g.vertices)*sizeof(MmlSceneVertex))==0;
            else for(unsigned i=0;equal&&i<g.vertices;i++)equal=memcmp(&entry.vertices[i],&Vertex(g,i),sizeof(MmlSceneVertex))==0;
            return equal;
        };
        const auto bucket=meshCache_.find(hash);
        if(bucket!=meshCache_.end())for(const auto &entry:bucket->second)if(equalMesh(*entry)){
                entry->seen=cycle_;candidate_->meshReused++;
                if(g.content_revision&&key)verifiedMeshes_[key]={entry,cycle_};
                return entry;
        }
        if(builds_)for(size_t i=0;i<builds_->count;i++){
            const auto &slot=builds_->slots[i];
            if(slot.hash==hash&&equalMesh(*slot.mesh)){candidate_->meshReused++;return slot.mesh;}
        }
        Timer buildTimer{staged?candidate_->stagedMeshMs:candidate_->meshBuildMs};
        auto entry=std::make_shared<MeshContent>();entry->fvf=g.source_fvf;entry->cull=cull;entry->revision=++revision_;entry->seen=entry->born=cycle_;
        if(staged){entry->vertices=std::move(staged->vertices);entry->indices=std::move(staged->indices);}
        else{
            entry->vertices.reserve(g.vertices);for(unsigned i=0;i<g.vertices;i++)entry->vertices.push_back(Vertex(g,i));
            entry->indices.assign(g.index,g.index+size_t(g.triangles)*3);
        }
        std::vector<ray::Triangle> triangles;triangles.reserve(g.triangles);
        const uint32_t sidedness=cull==D3DCULL_CCW?ray::CullBack:cull==D3DCULL_CW?ray::CullFront:0;
        for(unsigned t=0;t<g.triangles;t++){
            ray::Vec3 p[3];SurfaceTriangle surface{};
            for(unsigned c=0;c<3;c++){
                const auto index=entry->indices[t*3+c];if(index>=entry->vertices.size())return {};
                const auto &v=entry->vertices[index];p[c]={v.pos[0],v.pos[1],v.pos[2]};if(!ray::Finite(p[c]))return {};
                surface.color[c]=v.color;for(unsigned u=0;u<2;u++){
                    if(!ray::Finite(v.uv0[u])||!ray::Finite(v.uv1[u]))return {};surface.uv[c][u]=v.uv0[u];surface.uv[c][u+2]=v.uv1[u];
                }
            }
            const auto a=p[1]-p[0],b=p[2]-p[0];
            if(Degenerate(a,b)){entry->degenerate++;continue;}
            triangles.push_back({p[0],0,a,uint32_t(entry->surfaces.size()),b,sidedness});entry->surfaces.push_back(surface);
        }
        if(QueueMesh(entry,hash,g.content_revision?key:0,triangles))return entry;
        if(!triangles.empty()&&entry->local.AddMesh(triangles)!=0)return {};
        RememberMesh(entry,hash,g.content_revision?key:0);
        return entry;
    }
    uint32_t OwnTexture(void *borrowed){
        if(!borrowed)return UINT32_MAX;Timer textureTimer{candidate_->textureMs};const auto key=reinterpret_cast<uintptr_t>(borrowed);auto *native=static_cast<IDirect3DBaseTexture9 *>(borrowed);
        const uint64_t revision=ReadRevision(native);
        std::shared_ptr<const TextureSnapshot> owned;auto found=textureCache_.find(key);
        TextureContent *cached=found==textureCache_.end()?nullptr:&found->second;
        if(cached){
            bool equal=revision!=0&&cached->revision==revision;
            if(equal&&cached->publication==texturePublication_){
                candidate_->textureReused++;return cached->publicationId;
            }
            if(!equal&&(cached->texture->Verify(native,equal)!=TextureResult::Valid||
                       (revision!=0&&ReadRevision(native)!=revision))){
                candidate_->unavailableTextures++;return UINT32_MAX-1;
            }
            if(equal){
                owned=cached->texture;cached->seen=cycle_;cached->revision=revision;
                cached->publication=0;candidate_->textureReused++;
            }
        }
        if(!owned){TextureSnapshot texture;
            if(texture.Read(native)!=TextureResult::Valid||(revision!=0&&ReadRevision(native)!=revision)){
                candidate_->unavailableTextures++;return UINT32_MAX-1;
            }
            candidate_->textureCopied++;
            auto &bucket=textureContent_[TextureHash(texture)];
            for(auto &weak:bucket)if(auto value=weak.lock())if(EqualTexture(texture,*value)){owned=std::move(value);break;}
            if(!owned){owned=std::make_shared<const TextureSnapshot>(std::move(texture));bucket.push_back(owned);}
            size_t bytes=0;for(const auto &m:owned->mips)bytes+=m.bytes.size();
            if(cached){textureBytes_-=cached->bytes;textureCache_.erase(found);cached=nullptr;}
            if(bytes<=256u*1024*1024&&textureBytes_<=256u*1024*1024-bytes){
                cached=&textureCache_[key];*cached={owned,bytes,cycle_,revision};textureBytes_+=bytes;
            }
        }
        auto previous=textureIds_.find(owned.get());if(previous!=textureIds_.end()){
            if(cached){cached->publication=texturePublication_;cached->publicationId=previous->second;}
            return previous->second;
        }
        size_t bytes=0;for(const auto &m:owned->mips)bytes+=m.bytes.size();
        if(bytes>512u*1024*1024||candidate_->textureBytes>512u*1024*1024-bytes||candidate_->textures.size()>=UINT32_MAX-1){candidate_->unavailableTextures++;return UINT32_MAX-1;}
        const auto id=uint32_t(candidate_->textures.size());candidate_->textures.push_back(owned);candidate_->textureBytes+=bytes;textureIds_[owned.get()]=id;
        if(cached){cached->publication=texturePublication_;cached->publicationId=id;}
        return id;
    }
    void PruneCaches(){
        for(auto i=meshCache_.begin();i!=meshCache_.end();){auto &entries=i->second;
            entries.erase(std::remove_if(entries.begin(),entries.end(),[&](const auto &entry){if(entry->seen==cycle_)return false;meshBytes_-=entry->bytes;return true;}),entries.end());
            if(entries.empty())i=meshCache_.erase(i);else ++i;
        }
        for(auto i=verifiedMeshes_.begin();i!=verifiedMeshes_.end();)
            if(i->second.seen!=cycle_||i->second.content.expired())i=verifiedMeshes_.erase(i);else ++i;
        for(auto i=textureCache_.begin();i!=textureCache_.end();)if(i->second.seen!=cycle_){textureBytes_-=i->second.bytes;i=textureCache_.erase(i);}else ++i;
        for(auto i=textureContent_.begin();i!=textureContent_.end();){auto &entries=i->second;
            entries.erase(std::remove_if(entries.begin(),entries.end(),[](const auto &weak){return weak.expired();}),entries.end());
            if(entries.empty())i=textureContent_.erase(i);else ++i;
        }
    }
    static void Observe(const MmlSceneDraw *draw,void *user){auto &self=*static_cast<SnapshotBuilder *>(user);if(!self.collecting_||self.failed_)return;
        try{self.Add(*draw);}catch(...){self.failed_=true;}}
    bool ReserveStagedDraw(){
        if(!stageDraws_||stagedDraws_.size()>=MaxStagedDraws())return false;
        if(stagedDraws_.size()==stagedDraws_.capacity())try{
            const size_t growth=(std::max)(size_t(1024),stagedDraws_.size()/2);
            stagedDraws_.reserve((std::min)(MaxStagedDraws(),stagedDraws_.size()+growth));
        }catch(const std::bad_alloc &){return false;}
        return true;
    }
    uint32_t StageMesh(const MmlSceneGeometry &g){
        constexpr size_t budget=8u*1024*1024;
        const uint64_t bytes=sizeof(StagedGeometry)+uint64_t(g.vertices)*sizeof(MmlSceneVertex)+uint64_t(g.triangles)*3*sizeof(uint32_t);
        if(!g.triangles||bytes>budget-stagedGeometryBytes_)return UINT32_MAX;
        // Only a known nonempty mesh can adopt texture IDs before building. Empty and
        // uncertain geometry takes the original ordered path.
        ray::Vec3 p[3];
        for(unsigned i=0;i<3;i++){
            if(g.index[i]>=g.vertices)return UINT32_MAX;
            const auto &v=Vertex(g,g.index[i]);p[i]={v.pos[0],v.pos[1],v.pos[2]};
            if(!ray::Finite(p[i]))return UINT32_MAX;
        }
        if(Degenerate(p[1]-p[0],p[2]-p[0]))return UINT32_MAX;
        try{
            StagedGeometry owned;
            owned.fvf=g.source_fvf;owned.revision=g.content_revision;
            owned.vertices.reserve(g.vertices);
            if(g.vertex_stride==sizeof(MmlSceneVertex))owned.vertices.assign(g.vertex,g.vertex+g.vertices);
            else for(unsigned i=0;i<g.vertices;i++)owned.vertices.push_back(Vertex(g,i));
            owned.indices.assign(g.index,g.index+size_t(g.triangles)*3);
            const size_t actual=sizeof(StagedGeometry)+owned.vertices.capacity()*sizeof(MmlSceneVertex)+owned.indices.capacity()*sizeof(uint32_t);
            if(actual>budget-stagedGeometryBytes_)return UINT32_MAX;
            stagedGeometry_.push_back(std::move(owned));
            stagedGeometryBytes_+=actual;
            candidate_->stagedMeshPeakBytes=(std::max)(candidate_->stagedMeshPeakBytes,stagedGeometryBytes_);
            return uint32_t(stagedGeometry_.size()-1);
        }catch(const std::bad_alloc &){return UINT32_MAX;}
    }
    void Add(const MmlSceneDraw &draw){
        if(!(draw.flags&MML_SCENE_DRAW_COLLECTION)||draw.stage_stride<sizeof(MmlSceneTextureStage)||!draw.stages){failed_=true;return;}
        auto &s=*candidate_;s.draws++;
        MmlSceneBillboard billboard{};billboard.size=sizeof billboard;
        if(draw.kind==3&&(!MML_HAS(host_,scene_draw_billboard)||!host_->scene_draw_billboard(mod_,&draw,&billboard))){
            s.unavailable++;unknown_.push_back({s.coverage.lo,MML_SCENE_MISSING_DEFORMATION,s.coverage.hi,0});return;
        }
        MmlSceneGeometry geometry{};geometry.size=sizeof geometry;
        const auto copyStart=Time::now();const bool copied=host_->scene_draw_geometry(mod_,&draw,&geometry)!=0;s.sourceCopyMs+=Elapsed(copyStart);
        if(!copied){
            s.unavailable++;unknown_.push_back({s.coverage.lo,0x100u|draw.flags,s.coverage.hi,0});return;
        }
        if(geometry.vertex_stride<sizeof(MmlSceneVertex)||!geometry.vertex||!geometry.index||
           s.primitiveCount>UINT32_MAX-geometry.triangles||s.materials.size()>=UINT32_MAX||s.sources.size()>=UINT32_MAX){failed_=true;return;}
        SourceMaterial material;material.flags=draw.flags;material.alphaTest=draw.alpha_test;material.alphaReference=draw.alpha_reference;
        if(s.materialsCollected){
            auto *device=materialDevice_;
            const auto *detail=geometry.size>=offsetof(MmlSceneGeometry,material_detail)+sizeof(geometry.material_detail)?geometry.material_detail:nullptr;
            if(detail){
                if(detail->stage2_color_op!=D3DTOP_DISABLE)material.unsupported|=2;
                for(unsigned stage=0;stage<2;stage++){
                    const auto &state=detail->stages[stage];
                    if(state.srgb!=0)material.unsupported|=4;
                    if(state.result_arg!=D3DTA_CURRENT)material.unsupported|=8;
                    if(state.max_anisotropy==0||state.max_anisotropy>16)material.unsupported|=128;
                    else material.maxAnisotropy[stage]=state.max_anisotropy;
                    material.maxMipLevel[stage]=state.max_mip_level;
                    memcpy(&material.lodBias[stage],&state.lod_bias_bits,sizeof state.lod_bias_bits);
                    if(!ray::Finite(material.lodBias[stage]))material.unsupported|=128;
                }
                if(draw.blend&&detail->blend_op!=D3DBLENDOP_ADD)material.unsupported|=32;
                if((detail->color_write&7)!=7)material.unsupported|=64;
            }
            else if(!device)material.unsupported|=1;
            else {DWORD value=0;if(FAILED(device->GetTextureStageState(2,D3DTSS_COLOROP,&value))||value!=D3DTOP_DISABLE)material.unsupported|=2;
                for(unsigned stage=0;stage<2;stage++){
                    if(FAILED(device->GetSamplerState(stage,D3DSAMP_SRGBTEXTURE,&value))||value!=0)material.unsupported|=4;
                    if(FAILED(device->GetSamplerState(stage,D3DSAMP_MAXANISOTROPY,&value))||value==0||value>16)material.unsupported|=128;
                    else material.maxAnisotropy[stage]=value;
                    if(FAILED(device->GetSamplerState(stage,D3DSAMP_MAXMIPLEVEL,&value)))material.unsupported|=128;
                    else material.maxMipLevel[stage]=value;
                    if(FAILED(device->GetSamplerState(stage,D3DSAMP_MIPMAPLODBIAS,&value)))material.unsupported|=128;
                    else{memcpy(&material.lodBias[stage],&value,sizeof value);if(!ray::Finite(material.lodBias[stage]))material.unsupported|=128;}
                    if(FAILED(device->GetTextureStageState(stage,D3DTSS_RESULTARG,&value))||value!=D3DTA_CURRENT)material.unsupported|=8;
                }
                if(draw.blend && (FAILED(device->GetRenderState(D3DRS_BLENDOP,&value))||value!=D3DBLENDOP_ADD))material.unsupported|=32;
                if(FAILED(device->GetRenderState(D3DRS_COLORWRITEENABLE,&value))||(value&7)!=7)material.unsupported|=64;
            }
            if(geometry.source_fvf&D3DFVF_SPECULAR)material.unsupported|=16;
        }
        material.alphaFunction=draw.alpha_function;material.cull=draw.cull;material.blend=draw.blend;material.sourceBlend=draw.source_blend;
        material.destinationBlend=draw.destination_blend;material.factor=draw.texture_factor;
        for(unsigned stage=0;stage<2;stage++){
            material.stages[stage]=*reinterpret_cast<const MmlSceneTextureStage *>(reinterpret_cast<const uint8_t *>(draw.stages)+size_t(stage)*draw.stage_stride);
        }
        const bool stage=ReserveStagedDraw();
        if(!stage&&stageDraws_){
            stageDraws_=false;
            if(!PublishStagedDraws()){failed_=true;return;}
        }
        std::shared_ptr<MeshContent> content;
        uint32_t geometrySlot=UINT32_MAX;
        if(stage&&stageGeometry_){
            content=VerifiedContent(geometry,material.cull);
            if(!content)geometrySlot=StageMesh(geometry);
        }
        if(geometrySlot==UINT32_MAX){
            if(!content){
                // Earlier unresolved meshes must claim content revisions first.
                if(!stagedGeometry_.empty()&&!PublishStagedDraws()){failed_=true;return;}
                content=OwnMesh(geometry,material.cull);
            }
            if(!content){failed_=true;return;}
            s.degenerate+=content->degenerate;
            if(content->surfaces.empty())return;
        }
        for(unsigned stage=0;stage<2;stage++){
            void *borrowed=material.stages[stage].texture;
            if(s.materialsCollected&&material.stages[stage].color_op!=1)
                material.textureIds[stage]=OwnTexture(borrowed);
            material.textureKeys[stage]=reinterpret_cast<uintptr_t>(borrowed);
            material.stages[stage].texture=nullptr;
        }
        SourceInstance source{reinterpret_cast<uintptr_t>(draw.visual),reinterpret_cast<uintptr_t>(draw.group),draw.pass,draw.kind};
        const float *world=draw.kind==3?billboard.world:draw.world;
        std::copy_n(world,16,source.world);
        const uint32_t flags=draw.kind==3?(billboard.mode==0?ray::BillboardFacing:ray::BillboardY):0;
        if(stage){
            stagedDraws_.push_back({source,material,std::move(content),flags,geometrySlot});
            ++s.stagedDraws;s.stagedMeshes+=geometrySlot!=UINT32_MAX;return;
        }
        if(!PublishDraw(source,material,content,flags))failed_=true;
    }
    bool PublishStagedDraws(){
        for(const auto &draw:stagedDraws_){
            auto content=draw.content;
            if(draw.geometrySlot!=UINT32_MAX){
                auto &owned=stagedGeometry_[draw.geometrySlot];
                MmlSceneGeometry g{};g.vertex=owned.vertices.data();g.index=owned.indices.data();
                g.vertices=uint32_t(owned.vertices.size());g.triangles=uint32_t(owned.indices.size()/3);
                g.vertex_stride=sizeof(MmlSceneVertex);g.source_fvf=owned.fvf;g.content_revision=owned.revision;
                content=OwnMesh(g,draw.material.cull,&owned);
                if(!content||content->surfaces.empty())return false;
                candidate_->degenerate+=content->degenerate;
            }
            if(!PublishDraw(draw.source,draw.material,content,draw.placementFlags))return false;
        }
        stagedDraws_.clear();stagedGeometry_.clear();stagedGeometryBytes_=0;return true;
    }
    bool PublishDraw(const SourceInstance &source,const SourceMaterial &material,
                     const std::shared_ptr<const MeshContent> &content,uint32_t placementFlags){
        auto &s=*candidate_;
        if(content->surfaces.size()>UINT32_MAX-s.primitiveCount||s.materials.size()>=UINT32_MAX||s.sources.size()>=UINT32_MAX)return false;
        const uint32_t materialId=OwnMaterial(material);
        const uint32_t primitiveBase=s.primitiveCount;
        s.primitiveCount+=uint32_t(content->surfaces.size());
        int mesh=-1;
        if(s.instancedMeshes){
            auto [entry,inserted]=placedMeshes_.try_emplace(content->revision,int(placedMeshContents_.size()));
            if(inserted)placedMeshContents_.push_back(content);
            mesh=entry->second;
        }else mesh=s.geometry.AppendMesh(content->local,materialId,primitiveBase);
        if(mesh<0)return false;
        if(!s.compactAttributes)for(auto surface:content->surfaces){surface.material=materialId;s.surfaces.push_back(surface);}
        s.meshRevisions.push_back(content->revision);
        s.sources.push_back(source);
        ray::Placement placement;placement.mesh=uint32_t(mesh);placement.id=uint32_t(s.sources.size());std::copy_n(source.world,16,placement.world);
        if(s.instancedMeshes){
            placement.material=materialId;placement.primitiveBase=primitiveBase;
            placement.primitiveCount=uint32_t(content->surfaces.size());
        }
        placement.flags=placementFlags;
        placements_.push_back(placement);return true;
    }
    const MmlHost *host_=nullptr;MmlMod *mod_=nullptr;uint64_t cycle_=0;bool valid_=false,collecting_=false,failed_=false;
    IDirect3DDevice9 *materialDevice_=nullptr;
    std::shared_ptr<Snapshot> current_,candidate_;std::vector<ray::Placement> placements_;std::vector<ray::Unknown> unknown_;
    std::weak_ptr<const Snapshot> previous_;
    std::unordered_map<uint32_t,std::vector<std::shared_ptr<MeshContent>>> meshCache_;
    std::unordered_map<uint64_t,VerifiedMesh> verifiedMeshes_;
    std::unordered_map<uint64_t,int> placedMeshes_; // current publication's local mesh IDs by owned content revision
    std::vector<std::shared_ptr<const MeshContent>> placedMeshContents_;
    std::unordered_map<uintptr_t,TextureContent> textureCache_; // producer revision or current-borrow byte verification
    std::unordered_map<uint64_t,std::vector<std::weak_ptr<const TextureSnapshot>>> textureContent_;
    std::unordered_map<const TextureSnapshot *,uint32_t> textureIds_; // owned identity within this publication
    uint64_t texturePublication_=0; // Collect may retry in the same frame after clearing textureIds_.
    std::unordered_multimap<uint32_t,uint32_t> materialIds_; // hash selects candidates; exact numeric state decides
    BuildBatch *builds_=nullptr; // a Collect-local batch, joined before packing and publication
    std::unique_ptr<DeferredBuild> pending_;
    bool stageDraws_=false,stageGeometry_=false;
    std::vector<StagedDraw> stagedDraws_;
    std::vector<StagedGeometry> stagedGeometry_;
    size_t stagedGeometryBytes_=0;
    size_t meshBytes_=0,textureBytes_=0,nodeHint_=0,triangleHint_=0,surfaceHint_=0,drawHint_=0,materialHint_=0;uint64_t revision_=0;
};
} }
