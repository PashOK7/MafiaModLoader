/* GPU-only D3D11 producer / D3D12 consumer descriptions for owner-scoped channels.
 * These records contain metadata and NT handles, never CPU geometry or texels.
 * Duplicate producer handles into the consumer before opening them. A handle is
 * meaningful only with its exact producer pid, owner nonce and resource id.
 * The producer retains every published allocation until the consumer has either
 * rejected the request or acknowledged its queued completion signal. GPU reuse
 * additionally waits for that signal, or for the native consumed signal after
 * a same-frame output read. Unsupported preparation uses the native renderer.
 */
#pragma once
#include <stdint.h>
#include <stddef.h>

namespace mml::gfx {
inline constexpr uint32_t TransferVersion=1;
inline constexpr uint32_t TransferMaxResources=48;
inline constexpr uint32_t TransferMaxSamplers=8;
enum class TransferKind:uint32_t {Words=1,Image=2,Output=3};
enum class TransferOperation:uint32_t {Submit=0,Release=1};
inline constexpr uint32_t TransferImmutable=1;
inline constexpr uint32_t TransferIntegerLoad=2;

struct TransferResource {
    uint64_t id;
    uint64_t handle;
    uint64_t revision;
    uint32_t kind;
    uint32_t binding;
    uint32_t width,height,layers,mips;
    uint32_t resourceFormat,viewFormat;
    uint32_t stride,elements,flags,viewDimension;
};
static_assert(sizeof(TransferResource)==72);

struct TransferSampler {
    uint32_t binding,filter,addressU,addressV,addressW,maxAnisotropy,comparison;
    float mipBias,minLod,maxLod,border[4];
    uint32_t reserved[2];
};
static_assert(sizeof(TransferSampler)==64);

struct TransferRequest {
    uint32_t version=TransferVersion;
    uint32_t size=sizeof(TransferRequest);
    uint32_t producerPid=0;
    uint32_t count=0;
    uint64_t owner=0;
    uint64_t cycle=0;
    uint64_t frame=0;
    uint64_t fenceHandle=0;
    uint64_t inputReady=0;
    uint64_t outputReady=0;
    uint64_t consumed=0;
    uint32_t generation=0;
    uint32_t width=0,height=0,instances=0;
    uint32_t samplerCount=0,operation=uint32_t(TransferOperation::Submit);
    TransferResource resources[TransferMaxResources]{};
    TransferSampler samplers[TransferMaxSamplers]{};
};
static_assert(offsetof(TransferRequest,resources)==96);
static_assert(sizeof(TransferRequest)==4064);

enum class TransferStatus:uint32_t {Rejected=1,Submitted=2,Faulted=3};
struct TransferReply {
    uint32_t version=TransferVersion;
    uint32_t size=sizeof(TransferReply);
    uint64_t owner=0;
    uint64_t cycle=0;
    uint64_t frame=0;
    uint64_t outputReady=0;
    uint32_t generation=0;
    uint32_t status=uint32_t(TransferStatus::Rejected);
    int32_t error=0;
    uint32_t reserved=0;
};
static_assert(sizeof(TransferReply)==56);

inline bool TransferMatches(const TransferRequest &request,const TransferReply &reply) noexcept {
    return reply.version==TransferVersion&&reply.size==sizeof reply&&
        reply.owner==request.owner&&reply.cycle==request.cycle&&
        reply.frame==request.frame&&reply.generation==request.generation&&
        reply.outputReady==request.outputReady;
}
}
