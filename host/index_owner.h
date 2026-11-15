// Shutdown-only validation for LS3D 1.2's borrowed COMPLETE_IB_STRUCT owner slot.
#pragma once
#include <windows.h>
#include <cstdint>

namespace host {
inline bool ClearIndexOwner(uint32_t *owner, uint32_t buffer, uint32_t direct3dBuffer, uint32_t index) {
    const uintptr_t begin = reinterpret_cast<uintptr_t>(owner);
    if (!begin || begin > UINTPTR_MAX - 3 * sizeof(uint32_t)) return false;
    const uintptr_t end = begin + 3 * sizeof(uint32_t);
    for (uintptr_t at = begin; at < end;) {
        MEMORY_BASIC_INFORMATION region{};
        constexpr DWORD writable = PAGE_READWRITE | PAGE_WRITECOPY | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY;
        if (!VirtualQuery(reinterpret_cast<const void *>(at), &region, sizeof region) ||
            region.State != MEM_COMMIT || !(region.Protect & writable) || (region.Protect & PAGE_GUARD)) return false;
        const uintptr_t next = reinterpret_cast<uintptr_t>(region.BaseAddress) + region.RegionSize;
        if (next <= at) return false;
        at = next;
    }
    __try {
        if (owner[0] != buffer || owner[1] != direct3dBuffer || owner[2] != index) return false;
        owner[0] = owner[1] = owner[2] = 0;
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}
} // namespace host
