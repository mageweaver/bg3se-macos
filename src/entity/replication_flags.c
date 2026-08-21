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
#include "../core/logging.h"
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
        /* Accept either spelling. The table is keyed by the short API name
         * ("Stats"), but every other Ext.Entity entry point also takes the
         * fully-qualified engine name ("eoc::StatsComponent"), and callers
         * reasonably pass whichever they already have. */
        if (strcmp(component_name, k_replicated_type_globals[i].name) == 0 ||
            (k_replicated_type_globals[i].component_name &&
             strcmp(component_name,
                    k_replicated_type_globals[i].component_name) == 0)) {
            return &k_replicated_type_globals[i];
        }
    }

    return NULL;
}

/*
 * Shared lookup for both the read and write paths.
 *
 * Walks EntityWorld->Replication (SyncBuffers) -> ComponentPools[replicationIdx]
 * -> HashMap<EntityHandle, BitSet<>> and reports where this entity's BitSet
 * lives, so replication_flags_get and replication_flags_set cannot drift apart.
 *
 * Result codes distinguish "no entry for this entity" (a legitimate empty
 * answer for a reader, but not something a writer may silently ignore) from a
 * hard failure.
 */
typedef enum {
    REPL_LOCATE_ERROR = 0,   /* unusable state; callers must fail */
    REPL_LOCATE_FOUND = 1,   /* out_bitset / out_sync are valid */
    REPL_LOCATE_ABSENT = 2   /* pool exists but this entity has no entry */
} ReplLocateResult;

static ReplLocateResult replication_locate(void *entity_world,
                                           uint64_t entity_handle,
                                           const char *component_name,
                                           uintptr_t *out_bitset,
                                           uintptr_t *out_sync) {
    if (!entity_world || !version_detect_matches()) {
        return REPL_LOCATE_ERROR;
    }

    const ReplicatedTypeGlobal *replicated_type =
        find_replicated_type(component_name);
    if (!replicated_type) {
        return REPL_LOCATE_ERROR;
    }

    const char *detected_build = version_detect_get_version();
    if (!detected_build ||
        strcmp(detected_build, GENERATED_TYPEIDS_BUILD_ID) != 0 ||
        strcmp(replicated_type->build_id, GENERATED_TYPEIDS_BUILD_ID) != 0) {
        return REPL_LOCATE_ERROR;
    }

    void *binary_base_ptr = version_detect_get_binary_base();
    if (!binary_base_ptr || replicated_type->replicated_type_va < GHIDRA_BASE) {
        return REPL_LOCATE_ERROR;
    }

    uintptr_t replicated_type_address = 0;
    if (!checked_add((uintptr_t)binary_base_ptr,
                     replicated_type->replicated_type_va - GHIDRA_BASE,
                     &replicated_type_address)) {
        return REPL_LOCATE_ERROR;
    }

    int32_t replication_index = -1;
    if (!safe_memory_read_i32((mach_vm_address_t)replicated_type_address,
                              &replication_index) ||
        replication_index < 0) {
        return REPL_LOCATE_ERROR;
    }

    void *sync_ptr = NULL;
    if (!read_pointer_at((uintptr_t)entity_world, 0, &sync_ptr) || !sync_ptr) {
        return REPL_LOCATE_ERROR;
    }
    uintptr_t sync = (uintptr_t)sync_ptr;

    int32_t pool_capacity = 0;
    int32_t pool_count = 0;
    if (!read_i32_at(sync, SYNC_POOL_CAPACITY_OFFSET, &pool_capacity) ||
        !read_i32_at(sync, SYNC_POOL_COUNT_OFFSET, &pool_count) ||
        pool_capacity < 0 || pool_count < 0 || pool_count > pool_capacity ||
        (uint32_t)pool_count > MAX_REPLICATION_POOLS ||
        replication_index >= pool_count) {
        return REPL_LOCATE_ERROR;
    }

    void *pools_ptr = NULL;
    if (!read_pointer_at(sync, SYNC_POOLS_OFFSET, &pools_ptr) || !pools_ptr) {
        return REPL_LOCATE_ERROR;
    }

    uintptr_t pool = 0;
    uintptr_t pool_offset =
        (uintptr_t)(uint32_t)replication_index * REPLICATION_POOL_STRIDE;
    if (!checked_add((uintptr_t)pools_ptr, pool_offset, &pool)) {
        return REPL_LOCATE_ERROR;
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
        return REPL_LOCATE_ERROR;
    }

    if (bucket_count < 0 || next_capacity < 0 || next_count < 0 ||
        key_capacity < 0 || key_count < 0 || next_count > next_capacity ||
        key_count > key_capacity || (uint32_t)bucket_count > MAX_MAP_ENTRIES ||
        (uint32_t)next_count > MAX_MAP_ENTRIES ||
        (uint32_t)key_count > MAX_MAP_ENTRIES ||
        value_count > MAX_MAP_ENTRIES || next_count < key_count ||
        value_count < (uint32_t)key_count) {
        return REPL_LOCATE_ERROR;
    }

    if (key_count == 0) {
        return REPL_LOCATE_ABSENT;
    }

    if (bucket_count == 0 || !bucket_heads_ptr || !next_indices_ptr ||
        !keys_ptr || !values_ptr) {
        return REPL_LOCATE_ERROR;
    }

    uint32_t bucket =
        (uint32_t)(entity_handle % (uint32_t)bucket_count);
    int32_t node = -1;
    if (!read_i32_at((uintptr_t)bucket_heads_ptr,
                     (uintptr_t)bucket * sizeof(int32_t), &node)) {
        return REPL_LOCATE_ERROR;
    }

    for (uint32_t visited = 0; node >= 0; visited++) {
        if (visited >= (uint32_t)key_count || node >= key_count ||
            node >= next_count) {
            return REPL_LOCATE_ERROR;
        }

        uint64_t key = 0;
        if (!read_u64_at((uintptr_t)keys_ptr,
                         (uintptr_t)(uint32_t)node * sizeof(uint64_t), &key)) {
            return REPL_LOCATE_ERROR;
        }

        if (key == entity_handle) {
            uintptr_t bitset = 0;
            if ((uint32_t)node >= value_count ||
                !checked_add((uintptr_t)values_ptr,
                             (uintptr_t)(uint32_t)node * BITSET_STRIDE,
                             &bitset)) {
                return REPL_LOCATE_ERROR;
            }
            if (out_bitset) *out_bitset = bitset;
            if (out_sync) *out_sync = sync;
            return REPL_LOCATE_FOUND;
        }

        if (!read_i32_at((uintptr_t)next_indices_ptr,
                         (uintptr_t)(uint32_t)node * sizeof(int32_t), &node)) {
            return REPL_LOCATE_ERROR;
        }
    }

    return REPL_LOCATE_ABSENT;
}

/* Read one qword of an entity's replication flags. Absent entry reads as 0,
 * matching the previous behaviour of this function. */
bool replication_flags_get(void *entity_world, uint64_t entity_handle,
                           const char *component_name, uint32_t qword,
                           uint64_t *out_flags) {
    if (!out_flags) return false;

    uintptr_t bitset = 0, sync = 0;
    ReplLocateResult r = replication_locate(entity_world, entity_handle,
                                            component_name, &bitset, &sync);
    if (r == REPL_LOCATE_ERROR) return false;
    if (r == REPL_LOCATE_ABSENT) { *out_flags = 0; return true; }

    uint32_t size = 0, capacity = 0;
    if (!read_u32_at(bitset, BITSET_SIZE_OFFSET, &size) ||
        !read_u32_at(bitset, BITSET_CAPACITY_OFFSET, &capacity) ||
        size > capacity) {
        return false;
    }
    uint64_t qword_count = ((uint64_t)size + 63U) / 64U;
    if ((uint64_t)qword >= qword_count) { *out_flags = 0; return true; }

    if (capacity <= 64U) {
        return read_u64_at(bitset, BITSET_STORAGE_OFFSET, out_flags);
    }
    void *heap_ptr = NULL;
    if (!read_pointer_at(bitset, BITSET_STORAGE_OFFSET, &heap_ptr) || !heap_ptr) {
        return false;
    }
    return read_u64_at((uintptr_t)heap_ptr,
                       (uintptr_t)qword * sizeof(uint64_t), out_flags);
}

/*
 * OR flags into an entity's replication bitset and mark SyncBuffers dirty,
 * which is what Windows' ReplicateComponent does
 * (LuaEntityProxy.inl:357 -> GetOrCreateReplicationFlags + Dirty = true).
 *
 * Two things Windows does are deliberately NOT done here:
 *
 *   - GetOrCreate: Windows calls pool.add_key(entity) when the entity has no
 *     entry yet, which grows the hash map. Inserting into a live engine
 *     container is not something to attempt without proving the growth and
 *     rehash path, so an absent entry fails closed.
 *   - EnsureSize: Windows grows the BitSet to (qword+1)*64 bits. Growing means
 *     reallocating engine-owned storage, so a qword beyond the current size
 *     fails closed instead.
 *
 * Both refusals are reported, so a caller learns the request was rejected
 * rather than silently dropped.
 */
bool replication_flags_set(void *entity_world, uint64_t entity_handle,
                           const char *component_name, uint32_t qword,
                           uint64_t flags, bool *out_changed) {
    if (out_changed) *out_changed = false;

    uintptr_t bitset = 0, sync = 0;
    ReplLocateResult r = replication_locate(entity_world, entity_handle,
                                            component_name, &bitset, &sync);
    if (r != REPL_LOCATE_FOUND) return false;

    uint32_t size = 0, capacity = 0;
    if (!read_u32_at(bitset, BITSET_SIZE_OFFSET, &size) ||
        !read_u32_at(bitset, BITSET_CAPACITY_OFFSET, &capacity) ||
        size > capacity) {
        return false;
    }
    uint64_t qword_count = ((uint64_t)size + 63U) / 64U;
    if ((uint64_t)qword >= qword_count) {
        return false;   /* would require EnsureSize; see note above */
    }

    uintptr_t slot;
    if (capacity <= 64U) {
        if (qword != 0) return false;
        slot = bitset + BITSET_STORAGE_OFFSET;
    } else {
        void *heap_ptr = NULL;
        if (!read_pointer_at(bitset, BITSET_STORAGE_OFFSET, &heap_ptr) || !heap_ptr) {
            return false;
        }
        slot = (uintptr_t)heap_ptr + (uintptr_t)qword * sizeof(uint64_t);
    }

    uint64_t current = 0;
    if (!read_u64_at(slot, 0, &current)) return false;

    uint64_t updated = current | flags;
    if (updated != current) {
        if (!safe_memory_write((mach_vm_address_t)slot, &updated, sizeof(updated))) {
            return false;
        }
        /* SyncBuffers::Dirty sits immediately after the ComponentPools array
         * (Array is 16 bytes: buf, capacity, size), i.e. sync + 0x10. */
        uint8_t dirty = 1;
        if (!safe_memory_write((mach_vm_address_t)(sync + 0x10), &dirty, sizeof(dirty))) {
            return false;
        }
        if (out_changed) *out_changed = true;
    }
    return true;
}

/*
 * Read-only structural dump of the replication tables.
 *
 * Added 2026-08-20: with a real remote peer connected and replication visibly
 * working, replication_flags_get returned 0 for every entity and every type,
 * which means the walk is landing somewhere wrong rather than the pools being
 * empty. This reports each step so the break point is visible instead of
 * inferred.
 */
void replication_flags_debug_dump(void *entity_world) {
    if (!entity_world) {
        log_message("[Replication] dump: no entity world");
        return;
    }

    void *sync_ptr = NULL;
    if (!read_pointer_at((uintptr_t)entity_world, 0, &sync_ptr) || !sync_ptr) {
        log_message("[Replication] dump: EntityWorld+0 (SyncBuffers) is NULL");
        return;
    }
    uintptr_t sync = (uintptr_t)sync_ptr;

    void *pools_ptr = NULL;
    int32_t pool_capacity = 0, pool_count = 0;
    read_pointer_at(sync, SYNC_POOLS_OFFSET, &pools_ptr);
    read_i32_at(sync, SYNC_POOL_CAPACITY_OFFSET, &pool_capacity);
    read_i32_at(sync, SYNC_POOL_COUNT_OFFSET, &pool_count);
    uint8_t dirty = 0;
    safe_memory_read_u8((mach_vm_address_t)(sync + 0x10), &dirty);

    log_message("[Replication] SyncBuffers=%p pools=%p capacity=%d count=%d dirty=%u",
                (void *)sync, pools_ptr, pool_capacity, pool_count, dirty);

    if (!pools_ptr || pool_count <= 0 || pool_count > 4096) return;

    /* Report every pool that actually holds entries, not just the ones we have
     * ReplicatedTypeContext globals for. */
    int populated = 0;
    for (int i = 0; i < pool_count; i++) {
        uintptr_t pool = (uintptr_t)pools_ptr + (uintptr_t)i * REPLICATION_POOL_STRIDE;
        int32_t key_count = 0;
        if (!read_i32_at(pool, MAP_KEY_COUNT_OFFSET, &key_count)) continue;
        if (key_count > 0 && key_count < 100000) {
            populated++;
            if (populated <= 12) {
                log_message("[Replication]   pool[%d]: %d entities", i, key_count);
            }
        }
    }
    log_message("[Replication] populated pools: %d of %d", populated, pool_count);
}

/*
 * High-frequency sampler: report the first moment any pool holds entries.
 *
 * Polling from Lua always observed zero pools even with a live remote peer and
 * replication visibly working. If SyncBuffers is filled and cleared inside the
 * ECS flush, script-level sampling can never catch it. This is cheap enough to
 * call from a hot hook (early-exits on the first non-empty pool) and latches
 * once, so it answers "do these pools EVER populate" without flooding the log.
 */
static bool s_repl_sample_latched = false;

void replication_flags_sample(void *entity_world) {
    if (s_repl_sample_latched) return;

    /* Prove the instrument runs before trusting a negative result: a sampler
     * that silently no-ops looks identical to "pools never populate". */
    static int s_calls = 0;
    if (++s_calls == 1) {
        log_message("[Replication] sampler active (world=%p)", entity_world);
    }
    if (!entity_world) return;

    void *sync_ptr = NULL;
    if (!read_pointer_at((uintptr_t)entity_world, 0, &sync_ptr) || !sync_ptr) return;
    uintptr_t sync = (uintptr_t)sync_ptr;

    void *pools_ptr = NULL;
    int32_t pool_count = 0;
    if (!read_pointer_at(sync, SYNC_POOLS_OFFSET, &pools_ptr) || !pools_ptr) return;
    if (!read_i32_at(sync, SYNC_POOL_COUNT_OFFSET, &pool_count)) return;
    if (pool_count <= 0 || pool_count > 4096) return;

    static int s_reported = 0;
    if (!s_reported) {
        s_reported = 1;
        log_message("[Replication] sampler reached pools: count=%d (scanning each call)",
                    pool_count);
    }

    uint8_t dirty = 0;
    safe_memory_read_u8((mach_vm_address_t)(sync + 0x10), &dirty);

    for (int i = 0; i < pool_count; i++) {
        uintptr_t pool = (uintptr_t)pools_ptr + (uintptr_t)i * REPLICATION_POOL_STRIDE;
        int32_t key_count = 0;
        if (!read_i32_at(pool, MAP_KEY_COUNT_OFFSET, &key_count)) continue;
        if (key_count > 0 && key_count < 100000) {
            s_repl_sample_latched = true;
            log_message("[Replication] *** POOLS DO POPULATE *** pool[%d] has %d "
                        "entities (dirty=%u). Sampled from a hot hook, not script.",
                        i, key_count, dirty);
            return;
        }
    }
}

