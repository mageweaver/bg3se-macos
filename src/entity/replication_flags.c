/**
 * BG3SE-macOS - Read-only SyncBuffers replication flag traversal.
 *
 * ReplicatedTypeContext globals are generated per exact symbol. Traversal
 * offsets retain their independent version gate. Every game-memory read is
 * guarded, and this module performs no writes, allocations in game memory,
 * insertions, or dirty notifications.
 */

#include "replication_flags.h"
#include "generated_typeids.h"

#include "../core/safe_memory.h"
#include "../core/version_detect.h"

#include <limits.h>
#include <stddef.h>
#include <string.h>

#define GHIDRA_BASE 0x100000000ULL

#define SYNC_POOLS_OFFSET 0x00U
#define SYNC_POOL_CAPACITY_OFFSET 0x08U
#define SYNC_POOL_COUNT_OFFSET 0x0cU
#define REPLICATION_POOL_STRIDE 0x40U

#define MAP_BUCKET_HEADS_OFFSET 0x00U
#define MAP_BUCKET_COUNT_OFFSET 0x08U
#define MAP_NEXT_INDICES_OFFSET 0x10U
#define MAP_NEXT_CAPACITY_OFFSET 0x18U
#define MAP_NEXT_COUNT_OFFSET 0x1cU
#define MAP_KEYS_OFFSET 0x20U
#define MAP_KEY_CAPACITY_OFFSET 0x28U
#define MAP_KEY_COUNT_OFFSET 0x2cU
#define MAP_VALUES_OFFSET 0x30U
#define MAP_VALUE_COUNT_OFFSET 0x38U

#define BITSET_STORAGE_OFFSET 0x00U
#define BITSET_SIZE_OFFSET 0x08U
#define BITSET_CAPACITY_OFFSET 0x0cU
#define BITSET_STRIDE 0x10U

/* Fail closed on corrupted counts before doing expensive traversal. */
#define MAX_REPLICATION_POOLS (1U << 20)
#define MAX_MAP_ENTRIES (1U << 24)

typedef struct {
    const char *name;
    const char *component_name;
    const char *mangled_symbol;
    const char *context;
    const char *build_id;
    uintptr_t replicated_type_va;
} ReplicatedTypeGlobal;

#define REPLICATED_TYPE_ENTRY(api_name, component_name, mangled_symbol, context, \
                              build_id, preferred_va)                          \
    { api_name, component_name, mangled_symbol, context, build_id, preferred_va },

static const ReplicatedTypeGlobal k_replicated_type_globals[] = {
    GENERATED_REPLICATED_TYPE_CONTEXT_ENTRIES(REPLICATED_TYPE_ENTRY)
};

#undef REPLICATED_TYPE_ENTRY

static bool checked_add(uintptr_t base, uintptr_t offset, uintptr_t *out) {
    if (!out || UINTPTR_MAX - base < offset) {
        return false;
    }

    *out = base + offset;
    return true;
}

static bool read_pointer_at(uintptr_t base, uintptr_t offset, void **out) {
    uintptr_t address = 0;
    return checked_add(base, offset, &address) &&
           safe_memory_read_pointer((mach_vm_address_t)address, out);
}

static bool read_i32_at(uintptr_t base, uintptr_t offset, int32_t *out) {
    uintptr_t address = 0;
    return checked_add(base, offset, &address) &&
           safe_memory_read_i32((mach_vm_address_t)address, out);
}

static bool read_u32_at(uintptr_t base, uintptr_t offset, uint32_t *out) {
    uintptr_t address = 0;
    return checked_add(base, offset, &address) &&
           safe_memory_read_u32((mach_vm_address_t)address, out);
}

static bool read_u64_at(uintptr_t base, uintptr_t offset, uint64_t *out) {
    uintptr_t address = 0;
    return checked_add(base, offset, &address) &&
           safe_memory_read_u64((mach_vm_address_t)address, out);
}

static const ReplicatedTypeGlobal *find_replicated_type(
    const char *component_name) {
    if (!component_name) {
        return NULL;
    }

    for (size_t i = 0;
         i < sizeof(k_replicated_type_globals) /
                 sizeof(k_replicated_type_globals[0]);
         i++) {
        if (strcmp(component_name, k_replicated_type_globals[i].name) == 0) {
            return &k_replicated_type_globals[i];
        }
    }

    return NULL;
}

bool replication_flags_get(void *entity_world, uint64_t entity_handle,
                           const char *component_name, uint32_t qword,
                           uint64_t *out_flags) {
    if (!entity_world || !out_flags || !version_detect_matches()) {
        return false;
    }

    const ReplicatedTypeGlobal *replicated_type =
        find_replicated_type(component_name);
    if (!replicated_type) {
        return false;
    }

    const char *detected_build = version_detect_get_version();
    if (!detected_build ||
        strcmp(detected_build, GENERATED_TYPEIDS_BUILD_ID) != 0 ||
        strcmp(replicated_type->build_id, GENERATED_TYPEIDS_BUILD_ID) != 0) {
        return false;
    }

    void *binary_base_ptr = version_detect_get_binary_base();
    if (!binary_base_ptr || replicated_type->replicated_type_va < GHIDRA_BASE) {
        return false;
    }

    uintptr_t replicated_type_address = 0;
    if (!checked_add((uintptr_t)binary_base_ptr,
                     replicated_type->replicated_type_va - GHIDRA_BASE,
                     &replicated_type_address)) {
        return false;
    }

    int32_t replication_index = -1;
    if (!safe_memory_read_i32((mach_vm_address_t)replicated_type_address,
                              &replication_index) ||
        replication_index < 0) {
        return false;
    }

    void *sync_ptr = NULL;
    if (!read_pointer_at((uintptr_t)entity_world, 0, &sync_ptr) || !sync_ptr) {
        return false;
    }
    uintptr_t sync = (uintptr_t)sync_ptr;

    int32_t pool_capacity = 0;
    int32_t pool_count = 0;
    if (!read_i32_at(sync, SYNC_POOL_CAPACITY_OFFSET, &pool_capacity) ||
        !read_i32_at(sync, SYNC_POOL_COUNT_OFFSET, &pool_count) ||
        pool_capacity < 0 || pool_count < 0 || pool_count > pool_capacity ||
        (uint32_t)pool_count > MAX_REPLICATION_POOLS ||
        replication_index >= pool_count) {
        return false;
    }

    void *pools_ptr = NULL;
    if (!read_pointer_at(sync, SYNC_POOLS_OFFSET, &pools_ptr) || !pools_ptr) {
        return false;
    }

    uintptr_t pool = 0;
    uintptr_t pool_offset =
        (uintptr_t)(uint32_t)replication_index * REPLICATION_POOL_STRIDE;
    if (!checked_add((uintptr_t)pools_ptr, pool_offset, &pool)) {
        return false;
    }

    void *bucket_heads_ptr = NULL;
    void *next_indices_ptr = NULL;
    void *keys_ptr = NULL;
    void *values_ptr = NULL;
    int32_t bucket_count = 0;
    int32_t next_capacity = 0;
    int32_t next_count = 0;
    int32_t key_capacity = 0;
    int32_t key_count = 0;
    uint32_t value_count = 0;

    if (!read_pointer_at(pool, MAP_BUCKET_HEADS_OFFSET, &bucket_heads_ptr) ||
        !read_i32_at(pool, MAP_BUCKET_COUNT_OFFSET, &bucket_count) ||
        !read_pointer_at(pool, MAP_NEXT_INDICES_OFFSET, &next_indices_ptr) ||
        !read_i32_at(pool, MAP_NEXT_CAPACITY_OFFSET, &next_capacity) ||
        !read_i32_at(pool, MAP_NEXT_COUNT_OFFSET, &next_count) ||
        !read_pointer_at(pool, MAP_KEYS_OFFSET, &keys_ptr) ||
        !read_i32_at(pool, MAP_KEY_CAPACITY_OFFSET, &key_capacity) ||
        !read_i32_at(pool, MAP_KEY_COUNT_OFFSET, &key_count) ||
        !read_pointer_at(pool, MAP_VALUES_OFFSET, &values_ptr) ||
        !read_u32_at(pool, MAP_VALUE_COUNT_OFFSET, &value_count)) {
        return false;
    }

    if (bucket_count < 0 || next_capacity < 0 || next_count < 0 ||
        key_capacity < 0 || key_count < 0 || next_count > next_capacity ||
        key_count > key_capacity || (uint32_t)bucket_count > MAX_MAP_ENTRIES ||
        (uint32_t)next_count > MAX_MAP_ENTRIES ||
        (uint32_t)key_count > MAX_MAP_ENTRIES ||
        value_count > MAX_MAP_ENTRIES || next_count < key_count ||
        value_count < (uint32_t)key_count) {
        return false;
    }

    if (key_count == 0) {
        *out_flags = 0;
        return true;
    }

    if (bucket_count == 0 || !bucket_heads_ptr || !next_indices_ptr ||
        !keys_ptr || !values_ptr) {
        return false;
    }

    uint32_t bucket =
        (uint32_t)(entity_handle % (uint32_t)bucket_count);
    int32_t node = -1;
    if (!read_i32_at((uintptr_t)bucket_heads_ptr,
                     (uintptr_t)bucket * sizeof(int32_t), &node)) {
        return false;
    }

    for (uint32_t visited = 0; node >= 0; visited++) {
        if (visited >= (uint32_t)key_count || node >= key_count ||
            node >= next_count) {
            return false;
        }

        uint64_t key = 0;
        if (!read_u64_at((uintptr_t)keys_ptr,
                         (uintptr_t)(uint32_t)node * sizeof(uint64_t), &key)) {
            return false;
        }

        if (key == entity_handle) {
            uintptr_t bitset = 0;
            if ((uint32_t)node >= value_count ||
                !checked_add((uintptr_t)values_ptr,
                             (uintptr_t)(uint32_t)node * BITSET_STRIDE,
                             &bitset)) {
                return false;
            }

            uint32_t size = 0;
            uint32_t capacity = 0;
            if (!read_u32_at(bitset, BITSET_SIZE_OFFSET, &size) ||
                !read_u32_at(bitset, BITSET_CAPACITY_OFFSET, &capacity) ||
                size > capacity) {
                return false;
            }

            uint64_t qword_count = ((uint64_t)size + 63U) / 64U;
            if ((uint64_t)qword >= qword_count) {
                *out_flags = 0;
                return true;
            }

            if (capacity <= 64U) {
                return read_u64_at(bitset, BITSET_STORAGE_OFFSET, out_flags);
            }

            void *heap_ptr = NULL;
            if (!read_pointer_at(bitset, BITSET_STORAGE_OFFSET, &heap_ptr) ||
                !heap_ptr) {
                return false;
            }

            return read_u64_at((uintptr_t)heap_ptr,
                               (uintptr_t)qword * sizeof(uint64_t), out_flags);
        }

        if (!read_i32_at((uintptr_t)next_indices_ptr,
                         (uintptr_t)(uint32_t)node * sizeof(int32_t), &node)) {
            return false;
        }
    }

    *out_flags = 0;
    return true;
}
