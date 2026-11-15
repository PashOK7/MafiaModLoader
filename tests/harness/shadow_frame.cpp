#include "shadow_frame.h"
#include <cassert>

namespace {
void __stdcall Update(void *) {}
}

int main() {
    uintptr_t table[7]{};
    const uintptr_t *receiver = table;
    table[6] = reinterpret_cast<uintptr_t>(&Update);
    assert(host::CanUpdateShadowFrame(&receiver));
    assert(!host::CanUpdateShadowFrame(nullptr));

    table[6] = reinterpret_cast<uintptr_t>(&table);
    assert(!host::CanUpdateShadowFrame(&receiver));
    table[6] = 0;
    assert(!host::CanUpdateShadowFrame(&receiver));

    void *reserved = VirtualAlloc(nullptr, 4096, MEM_RESERVE, PAGE_NOACCESS);
    assert(reserved);
    receiver = static_cast<const uintptr_t *>(reserved);
    assert(!host::CanUpdateShadowFrame(&receiver));
    assert(!host::CanUpdateShadowFrame(reserved));
    assert(VirtualFree(reserved, 0, MEM_RELEASE));
}
