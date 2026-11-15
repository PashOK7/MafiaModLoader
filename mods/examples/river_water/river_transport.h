#pragma once
#include <stdint.h>
#include "mml/gpu_transfer_protocol.h"
namespace river {
inline constexpr uint32_t TransportSamples=4;
struct HardwareRequest {
    mml::gfx::TransferRequest transfer;
    uint32_t triangles=0,billboards=0;
};
static_assert(sizeof(HardwareRequest)==4072);
struct HardwareStatus {
    uint32_t size=sizeof(HardwareStatus);
    uint32_t ready=0;
    uint64_t worker=0;
    uint64_t submitted=0,rejected=0;
    int32_t error=0;
    uint32_t facetReuses=0;
    double enqueueMs=0;
    double gpuInputsMs=0,gpuBoundsMs=0,gpuTraceMs=0,gpuPackMs=0;
};
static_assert(sizeof(HardwareStatus)==80);
}
