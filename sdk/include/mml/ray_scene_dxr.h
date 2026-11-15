/* GPU-built conservative instance broad phase for ray_scene_dxr.hlsli.
 * Only a source count and the current GPU instance buffer are supplied by CPU.
 * World bounds, original local narrow phase and ray-dependent billboards remain
 * complete. Record after input publication, then call Submitted only after the
 * command list is queued. On discarded commands, call Discard. The caller owns
 * queue completion and keeps this object alive until its GPU work completes.
 */
#pragma once
#include <d3d12.h>
#include <wrl/client.h>
#include <algorithm>
#include <cstring>
#include <span>
#include <utility>

namespace mml::ray {
class DxrScene {
    template<class T>using Ptr=Microsoft::WRL::ComPtr<T>;
    Ptr<ID3D12Device5> device_;
    Ptr<ID3D12RootSignature> signature_;
    Ptr<ID3D12PipelineState> pipeline_;
    Ptr<ID3D12DescriptorHeap> heap_;
    Ptr<ID3D12Resource> bounds_,bottom_,top_,scratch_,instance_;
    UINT capacity_=0;
    bool readable_=false,pending_=false;
    HRESULT error_=S_OK;
    bool Buffer(Ptr<ID3D12Resource> &out,uint64_t bytes,D3D12_HEAP_TYPE heap,D3D12_RESOURCE_STATES state,D3D12_RESOURCE_FLAGS flags){
        D3D12_HEAP_PROPERTIES properties{};properties.Type=heap;
        D3D12_RESOURCE_DESC description{};description.Dimension=D3D12_RESOURCE_DIMENSION_BUFFER;description.Width=bytes;
        description.Height=description.DepthOrArraySize=description.MipLevels=1;description.SampleDesc.Count=1;
        description.Layout=D3D12_TEXTURE_LAYOUT_ROW_MAJOR;description.Flags=flags;
        error_=device_->CreateCommittedResource(&properties,D3D12_HEAP_FLAG_NONE,&description,state,nullptr,IID_PPV_ARGS(&out));
        return SUCCEEDED(error_);
    }
    bool Allocate(UINT count){
        const UINT capacity=count+std::min(count/8,4096u);
        D3D12_RAYTRACING_GEOMETRY_DESC geometry{};geometry.Type=D3D12_RAYTRACING_GEOMETRY_TYPE_PROCEDURAL_PRIMITIVE_AABBS;
        geometry.AABBs.AABBCount=capacity;geometry.AABBs.AABBs.StrideInBytes=sizeof(D3D12_RAYTRACING_AABB);
        D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_INPUTS inputs{};inputs.Type=D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL;
        inputs.Flags=D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_PREFER_FAST_TRACE;
        inputs.DescsLayout=D3D12_ELEMENTS_LAYOUT_ARRAY;inputs.NumDescs=1;inputs.pGeometryDescs=&geometry;
        D3D12_RAYTRACING_ACCELERATION_STRUCTURE_PREBUILD_INFO bottom{},top{};
        device_->GetRaytracingAccelerationStructurePrebuildInfo(&inputs,&bottom);
        inputs.Type=D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL;inputs.InstanceDescs=0;
        device_->GetRaytracingAccelerationStructurePrebuildInfo(&inputs,&top);
        if(!bottom.ResultDataMaxSizeInBytes||!top.ResultDataMaxSizeInBytes){error_=E_FAIL;return false;}
        Ptr<ID3D12Resource> bounds,bottomBuffer,topBuffer,scratch,instance;
        constexpr auto uav=D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
        if(!Buffer(bounds,uint64_t(capacity)*sizeof(D3D12_RAYTRACING_AABB),D3D12_HEAP_TYPE_DEFAULT,D3D12_RESOURCE_STATE_UNORDERED_ACCESS,uav)||
           !Buffer(bottomBuffer,bottom.ResultDataMaxSizeInBytes,D3D12_HEAP_TYPE_DEFAULT,D3D12_RESOURCE_STATE_RAYTRACING_ACCELERATION_STRUCTURE,uav)||
           !Buffer(topBuffer,top.ResultDataMaxSizeInBytes,D3D12_HEAP_TYPE_DEFAULT,D3D12_RESOURCE_STATE_RAYTRACING_ACCELERATION_STRUCTURE,uav)||
           !Buffer(scratch,std::max(bottom.ScratchDataSizeInBytes,top.ScratchDataSizeInBytes),D3D12_HEAP_TYPE_DEFAULT,D3D12_RESOURCE_STATE_UNORDERED_ACCESS,uav)||
           !Buffer(instance,sizeof(D3D12_RAYTRACING_INSTANCE_DESC),D3D12_HEAP_TYPE_UPLOAD,D3D12_RESOURCE_STATE_GENERIC_READ,D3D12_RESOURCE_FLAG_NONE))return false;
        D3D12_RAYTRACING_INSTANCE_DESC identity{};for(unsigned axis=0;axis<3;axis++)identity.Transform[axis][axis]=1;
        identity.InstanceMask=0xff;identity.AccelerationStructure=bottomBuffer->GetGPUVirtualAddress();
        void *mapped=nullptr;error_=instance->Map(0,nullptr,&mapped);if(FAILED(error_))return false;
        std::memcpy(mapped,&identity,sizeof identity);instance->Unmap(0,nullptr);
        bounds_=std::move(bounds);bottom_=std::move(bottomBuffer);top_=std::move(topBuffer);scratch_=std::move(scratch);instance_=std::move(instance);
        capacity_=capacity;readable_=false;return true;
    }
    static void Uav(ID3D12GraphicsCommandList *command){
        D3D12_RESOURCE_BARRIER barrier{};barrier.Type=D3D12_RESOURCE_BARRIER_TYPE_UAV;command->ResourceBarrier(1,&barrier);
    }
public:
    DxrScene()=default;
    DxrScene(const DxrScene &)=delete;
    DxrScene &operator=(const DxrScene &)=delete;
    ~DxrScene(){Release();}
    void Release(){
        Discard();heap_.Reset();pipeline_.Reset();signature_.Reset();device_.Reset();
    }
    void Discard(){bounds_.Reset();bottom_.Reset();top_.Reset();scratch_.Reset();instance_.Reset();capacity_=0;readable_=pending_=false;}
    bool Initialize(ID3D12Device *device,std::span<const uint8_t> boundsShader){
        Release();if(!device||boundsShader.empty())return false;
        error_=device->QueryInterface(IID_PPV_ARGS(&device_));if(FAILED(error_))return false;
        D3D12_FEATURE_DATA_D3D12_OPTIONS5 features{};error_=device_->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS5,&features,sizeof features);
        if(FAILED(error_)||features.RaytracingTier<D3D12_RAYTRACING_TIER_1_1){error_=DXGI_ERROR_UNSUPPORTED;return false;}
        D3D12_DESCRIPTOR_RANGE ranges[]={{D3D12_DESCRIPTOR_RANGE_TYPE_SRV,1,19,0,0},{D3D12_DESCRIPTOR_RANGE_TYPE_UAV,1,0,0,1}};
        D3D12_ROOT_PARAMETER roots[2]{};roots[0].ParameterType=D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;roots[0].DescriptorTable={2,ranges};
        roots[1].ParameterType=D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;roots[1].Constants={0,0,4};
        D3D12_ROOT_SIGNATURE_DESC rs{};rs.NumParameters=2;rs.pParameters=roots;Ptr<ID3DBlob> bytes,error;
        error_=D3D12SerializeRootSignature(&rs,D3D_ROOT_SIGNATURE_VERSION_1,&bytes,&error);if(FAILED(error_))return false;
        error_=device_->CreateRootSignature(0,bytes->GetBufferPointer(),bytes->GetBufferSize(),IID_PPV_ARGS(&signature_));if(FAILED(error_))return false;
        D3D12_COMPUTE_PIPELINE_STATE_DESC ps{};ps.pRootSignature=signature_.Get();ps.CS={boundsShader.data(),boundsShader.size()};
        error_=device_->CreateComputePipelineState(&ps,IID_PPV_ARGS(&pipeline_));if(FAILED(error_))return false;
        D3D12_DESCRIPTOR_HEAP_DESC hd{};hd.Type=D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;hd.NumDescriptors=2;hd.Flags=D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
        error_=device_->CreateDescriptorHeap(&hd,IID_PPV_ARGS(&heap_));return SUCCEEDED(error_);
    }
    HRESULT Error()const{return error_;}
    ID3D12Resource *Resource()const{return top_.Get();}
    void Submitted(){if(pending_){readable_=true;pending_=false;}}
    bool Record(ID3D12GraphicsCommandList4 *command,ID3D12Resource *instances,UINT count){
        if(!device_||!command||!instances||!count||count>64u*65535u||pending_){error_=E_INVALIDARG;return false;}
        if(instances->GetDesc().Dimension!=D3D12_RESOURCE_DIMENSION_BUFFER||instances->GetDesc().Width<uint64_t(count)*96){error_=E_INVALIDARG;return false;}
        if(count>capacity_&&!Allocate(count))return false;
        if(readable_){
            D3D12_RESOURCE_BARRIER barrier{};barrier.Type=D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
            barrier.Transition={bounds_.Get(),D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,D3D12_RESOURCE_STATE_UNORDERED_ACCESS};command->ResourceBarrier(1,&barrier);
        }
        auto cpu=heap_->GetCPUDescriptorHandleForHeapStart();
        D3D12_SHADER_RESOURCE_VIEW_DESC sv{};sv.ViewDimension=D3D12_SRV_DIMENSION_BUFFER;sv.Shader4ComponentMapping=D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        sv.Buffer.NumElements=count;sv.Buffer.StructureByteStride=96;device_->CreateShaderResourceView(instances,&sv,cpu);
        cpu.ptr+=device_->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
        D3D12_UNORDERED_ACCESS_VIEW_DESC uv{};uv.ViewDimension=D3D12_UAV_DIMENSION_BUFFER;uv.Buffer.NumElements=count;uv.Buffer.StructureByteStride=sizeof(D3D12_RAYTRACING_AABB);
        device_->CreateUnorderedAccessView(bounds_.Get(),nullptr,&uv,cpu);
        ID3D12DescriptorHeap *heaps[]{heap_.Get()};command->SetDescriptorHeaps(1,heaps);command->SetComputeRootSignature(signature_.Get());command->SetPipelineState(pipeline_.Get());
        command->SetComputeRootDescriptorTable(0,heap_->GetGPUDescriptorHandleForHeapStart());
        const uint32_t constants[]{count,0,0,0};command->SetComputeRoot32BitConstants(1,4,constants,0);command->Dispatch((count+63)/64,1,1);
        D3D12_RESOURCE_BARRIER barrier{};barrier.Type=D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        barrier.Transition={bounds_.Get(),D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,D3D12_RESOURCE_STATE_UNORDERED_ACCESS,D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE};command->ResourceBarrier(1,&barrier);
        D3D12_RAYTRACING_GEOMETRY_DESC geometry{};geometry.Type=D3D12_RAYTRACING_GEOMETRY_TYPE_PROCEDURAL_PRIMITIVE_AABBS;
        geometry.AABBs.AABBCount=count;geometry.AABBs.AABBs={bounds_->GetGPUVirtualAddress(),sizeof(D3D12_RAYTRACING_AABB)};
        D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_DESC build{};build.Inputs.Type=D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL;
        build.Inputs.Flags=D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_PREFER_FAST_TRACE;
        build.Inputs.DescsLayout=D3D12_ELEMENTS_LAYOUT_ARRAY;build.Inputs.NumDescs=1;build.Inputs.pGeometryDescs=&geometry;
        build.DestAccelerationStructureData=bottom_->GetGPUVirtualAddress();build.ScratchAccelerationStructureData=scratch_->GetGPUVirtualAddress();
        command->BuildRaytracingAccelerationStructure(&build,0,nullptr);Uav(command);
        build.Inputs.Type=D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL;build.Inputs.InstanceDescs=instance_->GetGPUVirtualAddress();
        build.DestAccelerationStructureData=top_->GetGPUVirtualAddress();command->BuildRaytracingAccelerationStructure(&build,0,nullptr);Uav(command);
        pending_=true;return true;
    }
};
}
