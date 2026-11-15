/* Coherent current-plane geometry publication for fluid contact queries. Owned payloads only; exact bytes
 * gate immutable reuse. CPU cycle/device generation gate every stage binding, including after a failed upload. */
#pragma once
#include "mml/scene_waterline.h"
#include "mml/gpu_immutable_buffer.h"
namespace mml { namespace scene {
class WaterlineGpu {
    using Buffer=gfx::ImmutableBuffer;
    struct Publication {
        Buffer elements,nodes,unknown,cells,cellOrder;ID3D11Buffer *constant=nullptr;ID3D11Device *device=nullptr;
        std::shared_ptr<const WaterlineModel> source;unsigned generation=0;
        ~Publication(){if(constant)constant->Release();if(device)device->Release();}
    };
public:
    bool UploadOwned(ID3D11Device *device,std::shared_ptr<const WaterlineModel> source,unsigned generation){
        valid_=false;error_=E_INVALIDARG;stage_="source";
        try{
            if(!device||!source||!source->cycle||!source->coverage.Valid()||source->elements.size()>UINT32_MAX||source->nodes.size()>UINT32_MAX||source->unknown.size()>UINT32_MAX)return false;
            if(source->elements.empty()!=source->nodes.empty())return false;
            if(!source->shape[0]||!source->shape[1]||uint64_t(source->shape[0])*source->shape[1]!=source->cells.size()||source->shape[2]!=source->cells.size()||source->shape[3]!=source->cellOrder.size()||source->grid[3]!=1||!std::isfinite(source->grid[2])||source->grid[2]<=0)return false;
            for(const auto &cell:source->cells)if(uint64_t(cell.first)+cell.count>source->cellOrder.size())return false;
            for(uint32_t index:source->cellOrder)if(index>=source->elements.size())return false;
            for(size_t i=0;i<source->nodes.size();i++){
                const auto &n=source->nodes[i];if(!ray::Bounds{n.lo,n.hi}.Valid()||n.escape<=i||n.escape>source->nodes.size())return false;
                if(n.count){if(uint64_t(n.first)+n.count>source->elements.size())return false;}
                else if(n.first<=i||n.first>=n.escape)return false;
            }
            for(const auto &e:source->elements)if((e.flags&~3u)||!std::isfinite(e.level)||!std::isfinite(e.a[0])||!std::isfinite(e.a[1])||!std::isfinite(e.b[0])||!std::isfinite(e.b[1])||!std::isfinite(e.c[0])||!std::isfinite(e.c[1]))return false;
            for(const auto &box:source->unknown)if(!ray::Bounds{box.lo,box.hi}.Valid())return false;
            auto next=std::make_unique<Publication>();next->source=std::move(source);next->generation=generation;
            const auto *old=current_&&current_->device==device&&current_->generation==generation?current_.get():nullptr;
            stage_="elements";error_=next->elements.MakeOwned(device,gfx::BufferLease(next->source,next->source->elements),old?&old->elements:nullptr);if(FAILED(error_))return false;
            stage_="nodes";error_=next->nodes.MakeOwned(device,gfx::BufferLease(next->source,next->source->nodes),old?&old->nodes:nullptr);if(FAILED(error_))return false;
            stage_="unknown";error_=next->unknown.MakeOwned(device,gfx::BufferLease(next->source,next->source->unknown),old?&old->unknown:nullptr);if(FAILED(error_))return false;
            stage_="cells";error_=next->cells.MakeOwned(device,gfx::BufferLease(next->source,next->source->cells),old?&old->cells:nullptr);if(FAILED(error_))return false;
            stage_="cell-order";error_=next->cellOrder.MakeOwned(device,gfx::BufferLease(next->source,next->source->cellOrder),old?&old->cellOrder:nullptr);if(FAILED(error_))return false;
            struct Constants {uint32_t counts[4];float lo[4],hi[4],grid[4];uint32_t shape[4];} constants{};
            std::copy_n(next->source->grid,4,constants.grid);std::copy_n(next->source->shape,4,constants.shape);
            constants.counts[0]=uint32_t(next->source->elements.size());constants.counts[1]=uint32_t(next->source->nodes.size());constants.counts[2]=uint32_t(next->source->unknown.size());
            for(unsigned a=0;a<3;a++){constants.lo[a]=next->source->coverage.lo[a];constants.hi[a]=next->source->coverage.hi[a];}
            D3D11_BUFFER_DESC desc{};desc.ByteWidth=sizeof constants;desc.Usage=D3D11_USAGE_IMMUTABLE;desc.BindFlags=D3D11_BIND_CONSTANT_BUFFER;
            D3D11_SUBRESOURCE_DATA initial{&constants,0,0};stage_="constants";error_=device->CreateBuffer(&desc,&initial,&next->constant);if(FAILED(error_))return false;
            next->device=device;device->AddRef();current_.swap(next);valid_=true;error_=S_OK;stage_="complete";return true;
        }catch(const std::bad_alloc &){error_=E_OUTOFMEMORY;return false;}catch(...){error_=E_FAIL;return false;}
    }
    bool Ready(uint64_t cycle,unsigned generation)const{return valid_&&current_&&current_->source->cycle==cycle&&current_->generation==generation;}
    HRESULT Error()const{return error_;}const char *ErrorStage()const{return stage_;}
    void Invalidate(){valid_=false;}void Release(){valid_=false;current_.reset();}
    bool BindCS(ID3D11DeviceContext *c,uint64_t cycle,unsigned generation)const{return Bind(c,cycle,generation,0);}
    bool BindPS(ID3D11DeviceContext *c,uint64_t cycle,unsigned generation)const{return Bind(c,cycle,generation,1);}
    bool BindDS(ID3D11DeviceContext *c,uint64_t cycle,unsigned generation)const{return Bind(c,cycle,generation,2);}
    static void UnbindCS(ID3D11DeviceContext *c){Unbind(c,0);}static void UnbindPS(ID3D11DeviceContext *c){Unbind(c,1);}static void UnbindDS(ID3D11DeviceContext *c){Unbind(c,2);}
private:
    bool Bind(ID3D11DeviceContext *c,uint64_t cycle,unsigned generation,unsigned stage)const{
        if(!c||!Ready(cycle,generation))return false;ID3D11Device *actual=nullptr;c->GetDevice(&actual);
        const bool matches=actual==current_->device;if(actual)actual->Release();if(!matches)return false;
        ID3D11ShaderResourceView *v[5]={current_->elements.view,current_->nodes.view,current_->unknown.view,current_->cells.view,current_->cellOrder.view};ID3D11Buffer *b=current_->constant;
        if(stage==0){c->CSSetShaderResources(28,5,v);c->CSSetConstantBuffers(4,1,&b);}
        else if(stage==1){c->PSSetShaderResources(28,5,v);c->PSSetConstantBuffers(4,1,&b);}
        else{c->DSSetShaderResources(28,5,v);c->DSSetConstantBuffers(4,1,&b);}return true;
    }
    static void Unbind(ID3D11DeviceContext *c,unsigned stage){
        if(!c)return;ID3D11ShaderResourceView *v[5]{};ID3D11Buffer *b=nullptr;
        if(stage==0){c->CSSetShaderResources(28,5,v);c->CSSetConstantBuffers(4,1,&b);}
        else if(stage==1){c->PSSetShaderResources(28,5,v);c->PSSetConstantBuffers(4,1,&b);}
        else{c->DSSetShaderResources(28,5,v);c->DSSetConstantBuffers(4,1,&b);}
    }
    std::unique_ptr<Publication> current_;bool valid_=false;HRESULT error_=S_OK;const char *stage_="none";
};
} }
