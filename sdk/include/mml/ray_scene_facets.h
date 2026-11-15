/* Frame-owned GPU triangle candidates. The source range table indexes existing
 * instance and triangle buffers. The caller serializes Record and GPU use. */
#pragma once
#include <d3d12.h>
#include <wrl/client.h>
#include <algorithm>
#include <cstring>
#include <span>

namespace mml::ray {
struct DxrFacetRange { uint32_t start,count,first,instance; };
inline constexpr uint32_t DxrFacetLimit=64u*65535u;

class DxrFacetScene {
    template<class T> using Ptr=Microsoft::WRL::ComPtr<T>;
    Ptr<ID3D12Device5> device_;
    Ptr<ID3D12RootSignature> signature_;
    Ptr<ID3D12PipelineState> pipeline_;
    Ptr<ID3D12DescriptorHeap> heap_;
    Ptr<ID3D12Resource> vertices_,mapping_,boxes_,bottom_,billboards_,top_,scratch_,instances_;
    UINT triangleCapacity_=0,boxCapacity_=0;
    bool readable_=false;
    HRESULT error_=S_OK;

    bool Buffer(Ptr<ID3D12Resource> &out,uint64_t bytes,D3D12_RESOURCE_STATES state){
        D3D12_HEAP_PROPERTIES heap{};
        const bool upload=state==D3D12_RESOURCE_STATE_GENERIC_READ;
        heap.Type=upload?D3D12_HEAP_TYPE_UPLOAD:D3D12_HEAP_TYPE_DEFAULT;
        D3D12_RESOURCE_DESC desc{};
        desc.Dimension=D3D12_RESOURCE_DIMENSION_BUFFER;desc.Width=bytes;
        desc.Height=desc.DepthOrArraySize=desc.MipLevels=desc.SampleDesc.Count=1;
        desc.Layout=D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        desc.Flags=upload?D3D12_RESOURCE_FLAG_NONE:D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
        error_=device_->CreateCommittedResource(&heap,D3D12_HEAP_FLAG_NONE,&desc,state,nullptr,IID_PPV_ARGS(&out));
        return SUCCEEDED(error_);
    }
    D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_INPUTS Inputs(D3D12_RAYTRACING_GEOMETRY_DESC *geometry,UINT count){
        D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_INPUTS input{};
        input.Type=geometry?D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL:D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL;
        input.DescsLayout=D3D12_ELEMENTS_LAYOUT_ARRAY;
        input.Flags=D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_PREFER_FAST_TRACE;
        input.NumDescs=count;input.pGeometryDescs=geometry;
        return input;
    }
    static D3D12_RAYTRACING_GEOMETRY_DESC Triangles(UINT count,ID3D12Resource *vertices=nullptr){
        D3D12_RAYTRACING_GEOMETRY_DESC desc{};
        desc.Type=D3D12_RAYTRACING_GEOMETRY_TYPE_TRIANGLES;
        desc.Triangles.VertexCount=count*3;desc.Triangles.VertexFormat=DXGI_FORMAT_R32G32B32_FLOAT;
        desc.Triangles.VertexBuffer={vertices?vertices->GetGPUVirtualAddress():0,12};
        return desc;
    }
    static D3D12_RAYTRACING_GEOMETRY_DESC Boxes(UINT count,ID3D12Resource *boxes=nullptr){
        D3D12_RAYTRACING_GEOMETRY_DESC desc{};
        desc.Type=D3D12_RAYTRACING_GEOMETRY_TYPE_PROCEDURAL_PRIMITIVE_AABBS;
        desc.AABBs.AABBCount=count;
        desc.AABBs.AABBs={boxes?boxes->GetGPUVirtualAddress():0,sizeof(D3D12_RAYTRACING_AABB)};
        return desc;
    }
    bool Allocate(UINT triangles,UINT boxes){
        triangles=std::max(triangleCapacity_,triangles+std::min(triangles/8,32768u));
        boxes=std::max(boxCapacity_,std::max(1u,boxes+std::min(boxes/8,512u)));
        auto trianglesDesc=Triangles(triangles),boxesDesc=Boxes(boxes);
        D3D12_RAYTRACING_ACCELERATION_STRUCTURE_PREBUILD_INFO mesh{},billboard{},top{};
        const auto meshInputs=Inputs(&trianglesDesc,1),billboardInputs=Inputs(&boxesDesc,1),topInputs=Inputs(nullptr,2);
        device_->GetRaytracingAccelerationStructurePrebuildInfo(&meshInputs,&mesh);
        device_->GetRaytracingAccelerationStructurePrebuildInfo(&billboardInputs,&billboard);
        device_->GetRaytracingAccelerationStructurePrebuildInfo(&topInputs,&top);
        if(!mesh.ResultDataMaxSizeInBytes||!billboard.ResultDataMaxSizeInBytes||!top.ResultDataMaxSizeInBytes){error_=E_FAIL;return false;}
        Ptr<ID3D12Resource> vertices,mapping,bounds,bottom,billboards,tree,scratch,instances;
        constexpr auto as=D3D12_RESOURCE_STATE_RAYTRACING_ACCELERATION_STRUCTURE;
        constexpr auto uav=D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
        if(!Buffer(vertices,uint64_t(triangles)*36,uav)||!Buffer(mapping,uint64_t(triangles+boxes)*8,uav)||
           !Buffer(bounds,uint64_t(boxes)*sizeof(D3D12_RAYTRACING_AABB),uav)||
           !Buffer(bottom,mesh.ResultDataMaxSizeInBytes,as)||!Buffer(billboards,billboard.ResultDataMaxSizeInBytes,as)||
           !Buffer(tree,top.ResultDataMaxSizeInBytes,as)||
           !Buffer(scratch,std::max({mesh.ScratchDataSizeInBytes,billboard.ScratchDataSizeInBytes,top.ScratchDataSizeInBytes}),uav)||
           !Buffer(instances,2*sizeof(D3D12_RAYTRACING_INSTANCE_DESC),D3D12_RESOURCE_STATE_GENERIC_READ))return false;
        vertices_=std::move(vertices);mapping_=std::move(mapping);boxes_=std::move(bounds);
        bottom_=std::move(bottom);billboards_=std::move(billboards);top_=std::move(tree);
        scratch_=std::move(scratch);instances_=std::move(instances);
        triangleCapacity_=triangles;boxCapacity_=boxes;readable_=false;
        return true;
    }
    void Build(ID3D12GraphicsCommandList4 *command,D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_INPUTS input,ID3D12Resource *result){
        D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_DESC build{};
        build.Inputs=input;build.DestAccelerationStructureData=result->GetGPUVirtualAddress();
        build.ScratchAccelerationStructureData=scratch_->GetGPUVirtualAddress();
        command->BuildRaytracingAccelerationStructure(&build,0,nullptr);
        D3D12_RESOURCE_BARRIER barrier{};
        barrier.Type=D3D12_RESOURCE_BARRIER_TYPE_UAV;command->ResourceBarrier(1,&barrier);
    }
    void Transition(ID3D12GraphicsCommandList4 *command,bool toRead){
        constexpr auto read=D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
        constexpr auto write=D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
        D3D12_RESOURCE_BARRIER barriers[3]{};
        ID3D12Resource *buffers[]{vertices_.Get(),mapping_.Get(),boxes_.Get()};
        for(unsigned i=0;i<3;i++){
            barriers[i].Type=D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
            barriers[i].Transition={buffers[i],D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,toRead?write:read,toRead?read:write};
        }
        command->ResourceBarrier(3,barriers);
    }
public:
    bool Initialize(ID3D12Device5 *device,std::span<const uint8_t> shader){
        if(!device||shader.empty())return false;
        device_=device;
        D3D12_DESCRIPTOR_RANGE ranges[]={{D3D12_DESCRIPTOR_RANGE_TYPE_SRV,1,17,0,0},
            {D3D12_DESCRIPTOR_RANGE_TYPE_SRV,1,19,0,1},{D3D12_DESCRIPTOR_RANGE_TYPE_SRV,1,38,0,2},
            {D3D12_DESCRIPTOR_RANGE_TYPE_UAV,3,0,0,3}};
        D3D12_ROOT_PARAMETER roots[2]{};
        roots[0].ParameterType=D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;roots[0].DescriptorTable={4,ranges};
        roots[1].ParameterType=D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;roots[1].Constants={0,0,4};
        D3D12_ROOT_SIGNATURE_DESC rs{};rs.NumParameters=2;rs.pParameters=roots;
        Ptr<ID3DBlob> bytes;
        error_=D3D12SerializeRootSignature(&rs,D3D_ROOT_SIGNATURE_VERSION_1,&bytes,nullptr);if(FAILED(error_))return false;
        error_=device_->CreateRootSignature(0,bytes->GetBufferPointer(),bytes->GetBufferSize(),IID_PPV_ARGS(&signature_));if(FAILED(error_))return false;
        D3D12_COMPUTE_PIPELINE_STATE_DESC ps{};ps.pRootSignature=signature_.Get();ps.CS={shader.data(),shader.size()};
        error_=device_->CreateComputePipelineState(&ps,IID_PPV_ARGS(&pipeline_));if(FAILED(error_))return false;
        D3D12_DESCRIPTOR_HEAP_DESC heap{};heap.Type=D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
        heap.NumDescriptors=6;heap.Flags=D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
        error_=device_->CreateDescriptorHeap(&heap,IID_PPV_ARGS(&heap_));return SUCCEEDED(error_);
    }
    HRESULT Error()const{return error_;}
    ID3D12Resource *Resource()const{return top_.Get();}
    ID3D12Resource *Mapping()const{return mapping_.Get();}
    void Submitted(){readable_=true;}
    bool Record(ID3D12GraphicsCommandList4 *command,ID3D12Resource *facets,UINT facetCount,ID3D12Resource *instances,
                ID3D12Resource *ranges,UINT rangeCount,UINT triangleCount,UINT boxCount){
        if(!facets||!instances||!ranges||!rangeCount||!triangleCount||uint64_t(triangleCount)+boxCount>DxrFacetLimit){error_=E_INVALIDARG;return false;}
        if((triangleCount>triangleCapacity_||boxCount>boxCapacity_)&&!Allocate(triangleCount,boxCount))return false;
        D3D12_RAYTRACING_INSTANCE_DESC entries[2]{};
        for(auto &entry:entries){for(unsigned axis=0;axis<3;axis++)entry.Transform[axis][axis]=1;entry.InstanceMask=0xff;}
        entries[0].AccelerationStructure=bottom_->GetGPUVirtualAddress();
        entries[1].InstanceID=triangleCount;entries[1].AccelerationStructure=billboards_->GetGPUVirtualAddress();
        void *mapped=nullptr;error_=instances_->Map(0,nullptr,&mapped);if(FAILED(error_))return false;
        std::memcpy(mapped,entries,sizeof entries);instances_->Unmap(0,nullptr);
        const UINT count=triangleCount+boxCount;
        const auto step=device_->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
        auto cpu=heap_->GetCPUDescriptorHandleForHeapStart();
        ID3D12Resource *inputs[]{facets,instances,ranges};
        const UINT lengths[]{facetCount,rangeCount,rangeCount},strides[]{48,96,sizeof(DxrFacetRange)};
        for(unsigned i=0;i<3;i++){
            D3D12_SHADER_RESOURCE_VIEW_DESC view{};view.ViewDimension=D3D12_SRV_DIMENSION_BUFFER;
            view.Shader4ComponentMapping=D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
            view.Buffer.NumElements=lengths[i];view.Buffer.StructureByteStride=strides[i];
            device_->CreateShaderResourceView(inputs[i],&view,cpu);cpu.ptr+=step;
        }
        ID3D12Resource *outputs[]{vertices_.Get(),mapping_.Get(),boxes_.Get()};
        const UINT outputLengths[]{triangleCount*3,count,std::max(1u,boxCount)},outputStrides[]{12,8,sizeof(D3D12_RAYTRACING_AABB)};
        for(unsigned i=0;i<3;i++){
            D3D12_UNORDERED_ACCESS_VIEW_DESC view{};view.ViewDimension=D3D12_UAV_DIMENSION_BUFFER;
            view.Buffer.NumElements=outputLengths[i];view.Buffer.StructureByteStride=outputStrides[i];
            device_->CreateUnorderedAccessView(outputs[i],nullptr,&view,cpu);cpu.ptr+=step;
        }
        if(readable_)Transition(command,false);
        ID3D12DescriptorHeap *heaps[]{heap_.Get()};command->SetDescriptorHeaps(1,heaps);
        command->SetComputeRootSignature(signature_.Get());command->SetPipelineState(pipeline_.Get());
        command->SetComputeRootDescriptorTable(0,heap_->GetGPUDescriptorHandleForHeapStart());
        const UINT values[]{count,rangeCount,triangleCount,0};command->SetComputeRoot32BitConstants(1,4,values,0);
        command->Dispatch((count+63)/64,1,1);Transition(command,true);
        auto mesh=Triangles(triangleCount,vertices_.Get());Build(command,Inputs(&mesh,1),bottom_.Get());
        if(boxCount){auto boxes=Boxes(boxCount,boxes_.Get());Build(command,Inputs(&boxes,1),billboards_.Get());}
        auto top=Inputs(nullptr,boxCount?2u:1u);top.InstanceDescs=instances_->GetGPUVirtualAddress();Build(command,top,top_.Get());
        return true;
    }
};
}
