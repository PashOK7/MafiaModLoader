#pragma once
#include <windows.h>
#include <cstdint>

namespace host {

// C_game::Done has already detached and released the shadow. Only its receiver's Update remains.
inline bool CanUpdateShadowFrame(const void *receiver) {
    if (!receiver) return false;
    uintptr_t update = 0;
    __try {
        const auto *table = *static_cast<const uintptr_t *const *>(receiver);
        if (!table) return false;
        update = table[0x18 / sizeof(uintptr_t)];
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
    MEMORY_BASIC_INFORMATION region{};
    constexpr DWORD executable = PAGE_EXECUTE | PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY;
    return update && VirtualQuery(reinterpret_cast<const void *>(update), &region, sizeof region) &&
           region.State == MEM_COMMIT && (region.Protect & executable) && !(region.Protect & PAGE_GUARD);
}

} // namespace host
