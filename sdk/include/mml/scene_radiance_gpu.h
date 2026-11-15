/* Coherent geometry/primitive/texture publication. Every resource is built in a private candidate, then
 * swapped as one snapshot. A failed update retains allocations but exposes NO current publication. */
#pragma once
#include "mml/scene_snapshot.h"
#include "mml/ray_scene_gpu.h"
#include "mml/native_texture_pages.h"
namespace mml { namespace scene {
struct GpuNativeStage {uint32_t color[4],alpha[4],map[4],sampling[4],extra[4];};
struct GpuNativeMaterial {GpuNativeStage stages[2];uint32_t info[4],blend[4],factor[4];};
static_assert(sizeof(GpuNativeStage)==80&&sizeof(GpuNativeMaterial)==208,"native material GPU layout");
class RadianceGpu {
    using Buffer=gfx::ImmutableBuffer;
    struct Publication {ray::GpuScene geometry;GpuTexturePages textures;Buffer surfaces,materials;size_t pageBytes=0;
        std::shared_ptr<const std::vector<TextureRef>> pageSources;size_t textureUploadBytes=0;
        std::weak_ptr<const Snapshot> owner;
        uint64_t cycle=0;unsigned generation=0;bool hasMaterials=false,texturesReused=false;};
public:
    bool Upload(ID3D11Device *device,const Snapshot &snapshot,unsigned generation){
        try{return UploadOwned(device,std::make_shared<const Snapshot>(snapshot),generation);}
        catch(const std::bad_alloc &){valid_=false;error_=E_OUTOFMEMORY;return false;}catch(...){valid_=false;error_=E_FAIL;return false;}
    }
    bool UploadOwned(ID3D11Device *device,const std::shared_ptr<const Snapshot> &owned,unsigned generation,bool appendPacked=true){
        valid_=false;error_=E_INVALIDARG;stage_="source-frame";
        try{
            if(!owned)return false;const auto &snapshot=*owned;
            auto next=std::make_unique<Publication>();
            const auto *reuse=current_&&current_->generation==generation?current_.get():nullptr;
            if(snapshot.geometry.frameId!=snapshot.nativeFrame)return false;
            stage_="geometry";const auto geometry=std::shared_ptr<const ray::Scene>(owned,&snapshot.geometry);
            // Copies of the public snapshot never inherit the producer proof.
            const auto packedFrom=snapshot.packedOwner_.lock().get()==&snapshot?snapshot.packedFrom_.lock():nullptr;
            const bool append=appendPacked&&snapshot.archive_!=nullptr;
            if(!next->geometry.UploadPacked(device,geometry,snapshot.coverage,snapshot.cycle,generation,reuse?&reuse->geometry:nullptr,
                                            packedFrom?&packedFrom->geometry:nullptr,snapshot.copiedPrefix_.nodes,snapshot.copiedPrefix_.triangles,
                                            snapshot.compactAttributes,append)){error_=next->geometry.Error();return false;}
            stage_="primitive-bindings";
            const auto surfaces=snapshot.Surfaces();
            if(snapshot.compactAttributes&&(!snapshot.instancedMeshes||surfaces.size()!=snapshot.geometry.Triangles().size()))return false;
            if(snapshot.instancedMeshes){
                const size_t count=snapshot.compactAttributes?snapshot.primitiveCount:surfaces.size();
                for(const auto &i:snapshot.geometry.instances){
                    if(!(i.flags&ray::InstanceAttributes)||i.pad>=snapshot.materials.size()||
                       uint64_t(i.pad1)+i.pad2>count)return false;
                    if(!snapshot.compactAttributes)for(uint32_t p=0;p<i.pad2;p++)
                        if(surfaces[i.pad1+p].material!=i.pad)return false;
                }
            }else{
                for(const auto &t:snapshot.geometry.Triangles())
                    if(t.primitive>=surfaces.size()||t.material>=snapshot.materials.size()||surfaces[t.primitive].material!=t.material)return false;
            }
            std::vector<uint32_t> textureRemap;
            if(!UploadTextures(device,snapshot,reuse,*next,generation,textureRemap))return false;
            std::vector<GpuNativeMaterial> materials;materials.reserve(snapshot.materials.size());
            for(const auto &source:snapshot.materials){GpuNativeMaterial material{};
                for(unsigned i=0;i<2;i++){const auto &s=source.stages[i];auto &d=material.stages[i];
                    const uint32_t color[]={s.color_op,s.color_arg0,s.color_arg1,s.color_arg2},alpha[]={s.alpha_op,s.alpha_arg0,s.alpha_arg1,s.alpha_arg2};
                    std::copy_n(color,4,d.color);std::copy_n(alpha,4,d.alpha);
                    const auto textureId=source.textureIds[i];
                    d.map[0]=textureId;
                    if(!textureRemap.empty()&&textureId<UINT32_MAX-1)
                        d.map[0]=textureId<textureRemap.size()?textureRemap[textureId]:UINT32_MAX-1;
                    d.map[1]=s.coordinate;d.map[2]=s.transform_flags;d.map[3]=source.textureKeys[i]!=0;
                    d.sampling[0]=s.address_u;d.sampling[1]=s.address_v;d.sampling[2]=s.min_filter;d.sampling[3]=s.mag_filter;d.extra[0]=s.mip_filter;d.extra[1]=source.maxAnisotropy[i];
                    d.extra[2]=source.maxMipLevel[i];memcpy(&d.extra[3],&source.lodBias[i],sizeof d.extra[3]);
                }
                material.info[0]=source.alphaTest;material.info[1]=source.alphaReference;material.info[2]=source.alphaFunction;material.info[3]=source.cull;
                material.blend[0]=source.blend;material.blend[1]=source.sourceBlend;material.blend[2]=source.destinationBlend;material.blend[3]=source.flags;
                material.factor[0]=source.factor;material.factor[1]=snapshot.materialsCollected?1:0;material.factor[2]=source.unsupported;materials.push_back(material);
            }
            const size_t surfacePrefix=snapshot.compactAttributes&&packedFrom&&reuse&&
                reuse->owner.lock()==packedFrom?snapshot.copiedPrefix_.triangles:0;
            stage_="surface-buffer";error_=next->surfaces.MakeView(device,surfaces,owned,
                reuse?&reuse->surfaces:nullptr,surfacePrefix,append);if(FAILED(error_))return false;
            stage_="material-buffer";auto materialData=std::make_shared<const std::vector<GpuNativeMaterial>>(std::move(materials));
            error_=next->materials.MakeOwned(device,std::move(materialData),reuse?&reuse->materials:nullptr);if(FAILED(error_))return false;
            next->owner=owned;next->cycle=snapshot.cycle;next->generation=generation;next->hasMaterials=snapshot.materialsCollected;
            current_.swap(next);valid_=true;error_=S_OK;stage_="complete";return true;
        }catch(const std::bad_alloc &){error_=E_OUTOFMEMORY;return false;}catch(...){error_=E_FAIL;return false;}
    }
    HRESULT Error()const{return error_;}
    const char *ErrorStage()const{return stage_;}
    unsigned ReusedBuffers()const{return valid_&&current_?current_->geometry.ReusedBuffers()+(current_->surfaces.reused?1u:0u)+(current_->materials.reused?1u:0u)+(current_->texturesReused?3u:0u):0;}
    unsigned AppendedBuffers()const{return valid_&&current_?current_->geometry.AppendedBuffers()+(current_->surfaces.appended?1u:0u):0;}
    size_t UploadedBytes()const{return valid_&&current_?current_->geometry.UploadedBytes()+current_->surfaces.uploadedBytes+current_->materials.uploadedBytes+current_->textureUploadBytes:0;}
    size_t TextureUploadedBytes()const{return valid_&&current_?current_->textureUploadBytes:0;}
    size_t GeometryUploadedBytes()const{return valid_&&current_?current_->geometry.UploadedBytes():0;}
    size_t MaterialUploadedBytes()const{return valid_&&current_?current_->materials.uploadedBytes:0;}
    bool TexturesReused()const{return valid_&&current_&&current_->texturesReused;}
    bool Ready(uint64_t cycle,unsigned generation)const{return valid_&&current_&&current_->cycle==cycle&&current_->generation==generation;}
    size_t PageBytes()const{return valid_&&current_?current_->pageBytes:0;}
    bool HasMaterials()const{return valid_&&current_&&current_->hasMaterials;}
    void Invalidate(){valid_=false;}
    void Release(){valid_=false;current_.reset();}
    bool BindCS(ID3D11DeviceContext *context,uint64_t cycle,unsigned generation)const{return Bind(context,cycle,generation,true);}
    bool BindPS(ID3D11DeviceContext *context,uint64_t cycle,unsigned generation)const{return Bind(context,cycle,generation,false);}
    static void UnbindCS(ID3D11DeviceContext *context){ray::GpuScene::UnbindCS(context);GpuTexturePages::Unbind(context,true);ID3D11ShaderResourceView *none[2]{};context->CSSetShaderResources(26,2,none);}
    static void UnbindPS(ID3D11DeviceContext *context){ray::GpuScene::UnbindPS(context);GpuTexturePages::Unbind(context,false);ID3D11ShaderResourceView *none[2]{};context->PSSetShaderResources(26,2,none);}
private:
    static size_t RecordBytes(const std::vector<TextureRef> &textures){
        size_t bytes=textures.size()*sizeof(TextureRecord);
        for(const auto &texture:textures)bytes+=texture.value->mips.size()*sizeof(MipRecord);
        return bytes;
    }
    bool UploadTextures(ID3D11Device *device,const Snapshot &snapshot,const Publication *reuse,Publication &next,
                        unsigned generation,std::vector<uint32_t> &remap){
        if(reuse&&reuse->pageSources&&reuse->pageSources->size()==snapshot.textures.size()&&
           std::equal(snapshot.textures.begin(),snapshot.textures.end(),reuse->pageSources->begin(),
                      [](const TextureRef &a,const TextureRef &b){return a.value&&a.value==b.value;})&&
           next.textures.Share(device,reuse->textures,snapshot.cycle,generation)){
            next.pageSources=reuse->pageSources;next.pageBytes=reuse->pageBytes;next.texturesReused=true;
            return true;
        }
        if(reuse&&reuse->pageSources&&!reuse->pageSources->empty()&&!snapshot.textures.empty()){
            const auto &old=*reuse->pageSources;
            std::unordered_map<const TextureSnapshot *,uint32_t> ids;ids.reserve(old.size()+snapshot.textures.size());
            for(uint32_t i=0;i<old.size();i++)ids.emplace(old[i].value.get(),i);
            std::vector<uint8_t> live(old.size());std::vector<TextureRef> additions;
            remap.reserve(snapshot.textures.size());
            for(const auto &texture:snapshot.textures){
                if(!texture.value){error_=E_INVALIDARG;return false;}
                const auto [match,added]=ids.try_emplace(texture.value.get(),uint32_t(old.size()+additions.size()));
                if(added)additions.push_back(texture);
                if(match->second<old.size())live[match->second]=1;
                remap.push_back(match->second);
            }
            size_t unused=0;
            for(size_t i=0;i<old.size();i++)if(!live[i])
                for(const auto &mip:old[i].value->mips)unused+=(mip.bytes.size()+3)&~size_t(3);
            // Keep stable IDs only while the obsolete tail stays small. The
            // old bank owns its immutable bytes and the current draw remaps IDs.
            const size_t retention=(std::clamp)(snapshot.textureBytes/8,TexturePages::PageBytes,size_t(8u*1024*1024));
            if(unused<=retention){
                if(additions.empty()&&next.textures.Share(device,reuse->textures,snapshot.cycle,generation)){
                    next.pageSources=reuse->pageSources;next.pageBytes=reuse->pageBytes;next.texturesReused=true;
                    return true;
                }
                if(!additions.empty()){
                    auto combined=old;combined.insert(combined.end(),additions.begin(),additions.end());
                    stage_="texture-page-append";
                    size_t written=0;
                    if(next.textures.UploadStreamedFrom(device,combined,snapshot.cycle,generation,&reuse->textures,old.size(),&next.pageBytes,&written)){
                        next.textureUploadBytes=written+RecordBytes(combined);
                        next.pageSources=std::make_shared<const std::vector<TextureRef>>(std::move(combined));
                        return true;
                    }
                }
            }
        }
        remap.clear();stage_="texture-page-build";TexturePages pages;
        // Large banks already own their source mips. Avoid another contiguous
        // CPU allocation when the 32-bit process has little address space left.
        const bool stream=sizeof(void*)==4&&snapshot.textureBytes>=32u*1024*1024;
        const bool packed=!stream&&pages.Build(snapshot.textures);
        if(!stream&&!packed&&pages.error!=E_OUTOFMEMORY){error_=pages.error;return false;}
        stage_=packed?"texture-page-upload":"texture-page-stream";
        if(packed){
            if(!next.textures.Upload(device,pages,snapshot.cycle,generation)){error_=next.textures.Error();return false;}
            next.pageBytes=pages.packedBytes;
        }else if(!next.textures.UploadStreamed(device,snapshot.textures,snapshot.cycle,generation,&next.pageBytes)){
            error_=next.textures.Error();return false;
        }
        next.pageSources=std::make_shared<const std::vector<TextureRef>>(snapshot.textures);
        next.textureUploadBytes=next.pageBytes+RecordBytes(snapshot.textures);
        return true;
    }
    bool Bind(ID3D11DeviceContext *context,uint64_t cycle,unsigned generation,bool compute)const{
        if(!Ready(cycle,generation))return false;
        if(!(compute?current_->geometry.BindCS(context,cycle,generation):current_->geometry.BindPS(context,cycle,generation)))return false;
        if(!current_->textures.Bind(context,cycle,generation,compute)){if(compute)UnbindCS(context);else UnbindPS(context);return false;}
        ID3D11ShaderResourceView *views[2]={current_->surfaces.view,current_->materials.view};
        if(compute)context->CSSetShaderResources(26,2,views);else context->PSSetShaderResources(26,2,views);return true;
    }
    std::unique_ptr<Publication> current_;bool valid_=false;HRESULT error_=S_OK;const char *stage_="none";
};
} }
