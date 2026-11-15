#include "index_owner.h"
#include <cassert>
#include <cstdio>
#include <cstring>

int main() {
    uint32_t owner[3] = {0x1234, 0x5678, 36};
    const uint32_t original[3] = {0x1234, 0x5678, 36};
    assert(!host::ClearIndexOwner(owner, 0x1235, 0x5678, 36));
    assert(!host::ClearIndexOwner(owner, 0x1234, 0x5679, 36));
    assert(!host::ClearIndexOwner(owner, 0x1234, 0x5678, 37));
    assert(!memcmp(owner, original, sizeof owner));
    assert(host::ClearIndexOwner(owner, 0x1234, 0x5678, 36));
    assert(owner[0] == 0 && owner[1] == 0 && owner[2] == 0);
    assert(!host::ClearIndexOwner(nullptr, 0, 0, 0));
    assert(!host::ClearIndexOwner(reinterpret_cast<uint32_t *>(UINTPTR_MAX - 3), 0, 0, 0));

    SYSTEM_INFO system{};
    GetSystemInfo(&system);
    const size_t page = system.dwPageSize;
    auto *memory = static_cast<uint8_t *>(VirtualAlloc(nullptr, 2 * page, MEM_RESERVE, PAGE_NOACCESS));
    assert(memory);
    assert(!host::ClearIndexOwner(reinterpret_cast<uint32_t *>(memory), 0x1234, 0x5678, 36));
    assert(VirtualAlloc(memory, 2 * page, MEM_COMMIT, PAGE_READWRITE) == memory);
    auto *split = reinterpret_cast<uint32_t *>(memory + page - sizeof(uint32_t));
    memcpy(split, original, sizeof original);
    DWORD previous = 0;
    assert(VirtualProtect(memory + page, page, PAGE_READONLY, &previous));
    assert(!host::ClearIndexOwner(split, 0x1234, 0x5678, 36));
    assert(!memcmp(split, original, sizeof original));
    MEMORY_BASIC_INFORMATION region{};
    assert(VirtualQuery(memory + page, &region, sizeof region));
    assert(region.Protect == PAGE_READONLY);
    assert(VirtualProtect(memory + page, page, PAGE_READWRITE | PAGE_GUARD, &previous));
    assert(!host::ClearIndexOwner(split, 0x1234, 0x5678, 36));
    assert(VirtualQuery(memory + page, &region, sizeof region));
    assert(region.Protect & PAGE_GUARD);
    assert(VirtualProtect(memory + page, page, PAGE_READWRITE, &previous));
    assert(host::ClearIndexOwner(split, 0x1234, 0x5678, 36));
    assert(split[0] == 0 && split[1] == 0 && split[2] == 0);
    assert(VirtualFree(memory, 2 * page, MEM_DECOMMIT));
    assert(!host::ClearIndexOwner(split, 0x1234, 0x5678, 36));
    assert(VirtualFree(memory, 0, MEM_RELEASE));
    puts("index owner shutdown tests PASSED");
}
