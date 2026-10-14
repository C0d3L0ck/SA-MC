#include "game.h"

namespace game {
namespace {
struct PoolHeader {
    void* objects;
    uint8_t* flags;  // bit 0x80 = espacio libre
    int32_t size;
    int32_t first_free;
    bool owns_allocations;
};
}  // namespace

uint32_t pool_used(uintptr_t pool_ptr_addr) {
    const auto* pool = *reinterpret_cast<PoolHeader* const*>(pool_ptr_addr);
    if (!pool || !pool->flags || pool->size <= 0 || pool->size > 20000) return 0;
    uint32_t used = 0;
    for (int32_t i = 0; i < pool->size; ++i)
        if ((pool->flags[i] & 0x80) == 0) ++used;
    return used;
}

}  // namespace game
