/**
 * BG3SE-macOS - Static data type registry lookup
 *
 * See staticdata_registry.h and plans/staticdata-generic-managers.md.
 */

#include "staticdata_registry.h"
#include "../core/logging.h"
#include "../core/safe_memory.h"

#include <string.h>

// ls::ImmutableDataHeadmaster::m_ptr — the singleton (build 4.1.1.7398727).
#define HEADMASTER_PTR_OFFSET   0x08ac13c8

// Manager HashMap inside the headmaster. Read out of
// ls::ImmutableDataHeadmaster::Get<ls::TagManager>() @ 0x10118616c:
//
//     slot = typeIndex % HashSize
//     idx  = HashKeys[slot]
//     while idx >= 0: if Keys[idx] == typeIndex -> Values[idx]; idx = NextIds[idx]
#define HM_HASHKEYS_OFFSET      0x00   // int32*
#define HM_HASHSIZE_OFFSET      0x08   // int32
#define HM_NEXTIDS_OFFSET       0x10   // int32*
#define HM_KEYS_OFFSET          0x20   // int32*
#define HM_KEYCOUNT_OFFSET      0x2C   // int32
#define HM_VALUES_OFFSET        0x30   // void**

// Chain bound: only guards against a corrupt map, real chains are short.
#define HM_MAX_PROBE            4096

// Set from staticdata_manager_init; the offsets above are image-relative.
static uintptr_t g_image_base = 0;

void staticdata_registry_init(void *main_binary_base) {
    g_image_base = (uintptr_t)main_binary_base;
}

const StaticDataTypeEntry *staticdata_registry_find(const char *name) {
    if (!name) return NULL;
    for (int i = 0; g_staticdata_types[i].name; i++) {
        if (strcasecmp(g_staticdata_types[i].name, name) == 0) {
            return &g_staticdata_types[i];
        }
    }
    return NULL;
}

static void *headmaster_instance(void) {
    uintptr_t base = g_image_base;
    if (!base) return NULL;

    void *hm = NULL;
    if (!safe_memory_read_pointer((mach_vm_address_t)(base + HEADMASTER_PTR_OFFSET), &hm)) {
        return NULL;
    }
    return hm;
}

void *staticdata_registry_get_manager(const StaticDataTypeEntry *entry) {
    if (!entry) return NULL;

    uintptr_t base = g_image_base;
    if (!base) return NULL;

    void *hm = headmaster_instance();
    if (!hm) return NULL;

    // The per-type index lives in a game global, assigned during type
    // registration; it is not a compile-time constant.
    int32_t type_index = 0;
    if (!safe_memory_read_i32((mach_vm_address_t)(base + entry->index_offset), &type_index)
        || type_index < 0) {
        return NULL;
    }

    int32_t hash_size = 0;
    if (!safe_memory_read_i32((mach_vm_address_t)hm + HM_HASHSIZE_OFFSET, &hash_size)
        || hash_size <= 0) {
        return NULL;
    }

    void *hash_keys = NULL, *next_ids = NULL, *keys = NULL, *values = NULL;
    if (!safe_memory_read_pointer((mach_vm_address_t)hm + HM_HASHKEYS_OFFSET, &hash_keys)
        || !safe_memory_read_pointer((mach_vm_address_t)hm + HM_NEXTIDS_OFFSET, &next_ids)
        || !safe_memory_read_pointer((mach_vm_address_t)hm + HM_KEYS_OFFSET, &keys)
        || !safe_memory_read_pointer((mach_vm_address_t)hm + HM_VALUES_OFFSET, &values)
        || !hash_keys || !next_ids || !keys || !values) {
        return NULL;
    }

    int32_t idx = 0;
    if (!safe_memory_read_i32((mach_vm_address_t)hash_keys
                              + (size_t)(type_index % hash_size) * 4, &idx)) {
        return NULL;
    }

    for (int probe = 0; idx >= 0 && probe < HM_MAX_PROBE; probe++) {
        int32_t candidate = 0;
        if (!safe_memory_read_i32((mach_vm_address_t)keys + (size_t)idx * 4, &candidate)) {
            return NULL;
        }
        if (candidate == type_index) {
            void *mgr = NULL;
            if (!safe_memory_read_pointer((mach_vm_address_t)values + (size_t)idx * 8, &mgr)) {
                return NULL;
            }
            return mgr;
        }
        if (!safe_memory_read_i32((mach_vm_address_t)next_ids + (size_t)idx * 4, &idx)) {
            return NULL;
        }
    }

    return NULL;
}
