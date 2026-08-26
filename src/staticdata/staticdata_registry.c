/**
 * BG3SE-macOS - Static data type registry lookup
 *
 * See staticdata_registry.h and plans/staticdata-generic-managers.md.
 */

#include "staticdata_registry.h"
#include "../core/logging.h"
#include "../core/safe_memory.h"

#include <string.h>
#include <stdio.h>

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

// ls::ModdableFilesLoader<ls::Guid, T>::GetObjectByKey(Guid const&) const.
//
// Vtable slot 6 (+0x30). Slot order from GuidResourceBankBase in
// upstream/BG3Extender/GameDefinitions/GuidResources.h, remembering the virtual
// destructor occupies TWO slots under the Itanium ABI:
//
//   0,1 ~dtor   2 LoadModuleObjects   3 LEGACY_LoadModuleObjects
//   4 Clear     5 PostInit            6 GetObjectByKey
//
// Verified live rather than counted on faith: for the Background manager, slot 2
// and slot 6 matched the runtime addresses of LoadModuleObjects and
// GetObjectByKey exactly.
#define BANK_VT_GET_OBJECT_BY_KEY   0x30

typedef void *(*GetObjectByKeyFunc)(void *self, const void *guid);

void *staticdata_registry_get_object(const StaticDataTypeEntry *entry, const void *guid16) {
    if (!entry || !guid16) return NULL;

    void *mgr = staticdata_registry_get_manager(entry);
    if (!mgr) return NULL;

    void *vtable = NULL;
    if (!safe_memory_read_pointer((mach_vm_address_t)mgr, &vtable) || !vtable) {
        return NULL;
    }

    void *fn = NULL;
    if (!safe_memory_read_pointer((mach_vm_address_t)vtable + BANK_VT_GET_OBJECT_BY_KEY, &fn)
        || !fn) {
        return NULL;
    }

    return ((GetObjectByKeyFunc)fn)(mgr, guid16);
}

void *staticdata_registry_get_object_by_guid_string(const StaticDataTypeEntry *entry,
                                                    const char *guid_str) {
    if (!entry || !guid_str) return NULL;

    // Same field order the existing lookups compare against game memory, so the
    // 16 bytes land in the layout ls::Guid uses.
    unsigned int d[11];
    if (sscanf(guid_str, "%8x-%4x-%4x-%2x%2x-%2x%2x%2x%2x%2x%2x",
               &d[0], &d[1], &d[2], &d[3], &d[4], &d[5],
               &d[6], &d[7], &d[8], &d[9], &d[10]) != 11) {
        return NULL;
    }

    uint8_t guid[16];
    uint32_t p1 = (uint32_t)d[0];
    uint16_t p2 = (uint16_t)d[1], p3 = (uint16_t)d[2];
    memcpy(guid + 0, &p1, 4);
    memcpy(guid + 4, &p2, 2);
    memcpy(guid + 6, &p3, 2);
    for (int i = 0; i < 8; i++) guid[8 + i] = (uint8_t)d[3 + i];

    return staticdata_registry_get_object(entry, guid);
}

// GuidResourceBank<T> : GuidResourceBankBase { HashMap<Guid, T> Resources; ... }
//
// GuidResourceBankBase is vptr(8) + two FixedStrings(8) + a
// HashMap<Guid, Array<Guid>> ResourceGuidsByMod(0x40), so Resources starts at
// 0x50. HashMap is HashSet plus a values array, and HashSet is
//   StaticArray<int32> HashKeys @+0x00 (buf +0x00, size +0x08)
//   Array<int32>       NextIds  @+0x10
//   Array<TKey>        Keys     @+0x20 (buf +0x20, capacity +0x28, size +0x2C)
// which is the same shape the headmaster lookup above was derived from by
// disassembly - two independent routes to the same layout.
//
// Verified live: Background reports 28 keys, Race 203, Tag 1298 and
// CharacterCreationAppearanceVisual 9044, each with hashSize > keyCount.
#define BANK_RESOURCES_OFFSET   0x50
#define BANK_KEYS_BUF           (BANK_RESOURCES_OFFSET + 0x20)
#define BANK_KEYS_COUNT         (BANK_RESOURCES_OFFSET + 0x2C)

// Guard against a garbage count turning into a multi-gigabyte loop.
#define BANK_MAX_KEYS           (1u << 20)

bool staticdata_registry_get_keys(const StaticDataTypeEntry *entry,
                                  void **out_buf, uint32_t *out_count) {
    if (!entry || !out_buf || !out_count) return false;

    void *mgr = staticdata_registry_get_manager(entry);
    if (!mgr) return false;

    void *buf = NULL;
    uint32_t count = 0;
    if (!safe_memory_read_pointer((mach_vm_address_t)mgr + BANK_KEYS_BUF, &buf)) return false;
    if (!safe_memory_read_u32((mach_vm_address_t)mgr + BANK_KEYS_COUNT, &count)) return false;
    if (count > BANK_MAX_KEYS) return false;
    if (count > 0 && !buf) return false;

    *out_buf = buf;
    *out_count = count;
    return true;
}

bool staticdata_registry_format_key(void *keys_buf, uint32_t index, char *out, size_t out_size) {
    if (!keys_buf || !out || out_size < 40) return false;

    uint8_t g[16];
    for (int i = 0; i < 16; i++) {
        if (!safe_memory_read_u8((mach_vm_address_t)keys_buf + (size_t)index * 16 + i, &g[i])) {
            return false;
        }
    }

    uint32_t d1;
    uint16_t d2, d3;
    memcpy(&d1, g + 0, 4);
    memcpy(&d2, g + 4, 2);
    memcpy(&d3, g + 6, 2);
    snprintf(out, out_size, "%08x-%04x-%04x-%02x%02x-%02x%02x%02x%02x%02x%02x",
             d1, d2, d3, g[8], g[9], g[10], g[11], g[12], g[13], g[14], g[15]);
    return true;
}
