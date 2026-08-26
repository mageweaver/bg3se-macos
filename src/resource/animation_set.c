/**
 * BG3SE-macOS - AnimationSet resource containers
 *
 * See animation_set.h and plans/animationset-writable-resources.md for where the
 * layout comes from and how it was verified.
 */

#include "animation_set.h"
#include "../core/logging.h"
#include "../core/offset_table.h"
#include "../core/safe_memory.h"

#include <string.h>
#include <stdlib.h>

// ============================================================================
// Layout (verified — see the plan document)
// ============================================================================

// ls::AnimationSetResource
#define ANIMSETRES_BANK_OFFSET      0x28   // AnimationSet* AnimationBank

// MapNode<FixedString, AnimationSubSet>
#define SUBSET_NODE_NEXT            0x00
#define SUBSET_NODE_KEY             0x08
#define SUBSET_NODE_ANIM_MAP        0x10   // Value.Animation (a LegacyRefMap)
#define SUBSET_NODE_FALLBACK        0x20   // Value.FallBackSubSet

// MapNode<FixedString, AnimationDesc>. AnimationDesc is { FixedString ID; u8
// flags; } and only needs 4-byte alignment, so Value sits directly after the key
// rather than being padded out to 8 like AnimationSubSet's is.
#define ANIM_NODE_NEXT              0x00
#define ANIM_NODE_KEY               0x08
#define ANIM_NODE_ID                0x0C
#define ANIM_NODE_FLAGS             0x10
#define ANIM_NODE_SIZE              0x18

// A map with more buckets than this is not one we understand; refuse rather
// than walk into whatever it actually is.
#define ANIMSET_MAX_BUCKETS         (1u << 20)

// ============================================================================
// Reading
// ============================================================================

static bool read_refmap(uintptr_t addr, AnimSetRefMap *out) {
    uint32_t count = 0, size = 0;
    void *table = NULL;

    if (!safe_memory_read_u32((mach_vm_address_t)addr, &count)) return false;
    if (!safe_memory_read_u32((mach_vm_address_t)addr + 4, &size)) return false;
    if (!safe_memory_read_pointer((mach_vm_address_t)addr + 8, &table)) return false;

    if (size > ANIMSET_MAX_BUCKETS) return false;
    if (size == 0 || table == NULL) {
        // A legitimately empty map: BG3SX ships its Animation maps this way.
        out->item_count = 0;
        out->hash_size = size;
        out->hash_table = table;
        return true;
    }

    out->item_count = count;
    out->hash_size = size;
    out->hash_table = table;
    return true;
}

bool animset_read_map(void *addr, AnimSetRefMap *out) {
    if (!addr || !out) return false;
    return read_refmap((uintptr_t)addr, out);
}

bool animset_get_bank(void *resource, void **out_bank) {
    if (!resource || !out_bank) return false;

    void *bank = NULL;
    if (!safe_memory_read_pointer((mach_vm_address_t)resource + ANIMSETRES_BANK_OFFSET,
                                  &bank) || !bank) {
        return false;
    }
    *out_bank = bank;
    return true;
}

bool animset_get_subsets(void *resource, AnimSetRefMap *out) {
    if (!resource || !out) return false;

    void *bank = NULL;
    if (!safe_memory_read_pointer((mach_vm_address_t)resource + ANIMSETRES_BANK_OFFSET,
                                  &bank) || !bank) {
        return false;
    }

    // AnimationSet holds exactly one member, the AnimationSubSets map, so the
    // bank pointer is the map.
    return read_refmap((uintptr_t)bank, out);
}

/** Walk buckets in order, invoking the caller for each node until it stops. */
static bool walk_nodes(const AnimSetRefMap *map, uint32_t want_index,
                       uint32_t want_key, bool by_key, void **out_node) {
    if (!map || !map->hash_table || map->hash_size == 0) return false;

    uint32_t seen = 0;
    for (uint32_t bucket = 0; bucket < map->hash_size; bucket++) {
        void *node = NULL;
        if (!safe_memory_read_pointer(
                (mach_vm_address_t)map->hash_table + (size_t)bucket * sizeof(void *),
                &node)) {
            continue;
        }

        // Chains are short; the bound only stops a corrupt map from hanging us.
        for (uint32_t hop = 0; node && hop < 4096; hop++) {
            if (by_key) {
                uint32_t key = 0;
                if (safe_memory_read_u32((mach_vm_address_t)node + SUBSET_NODE_KEY, &key)
                    && key == want_key) {
                    *out_node = node;
                    return true;
                }
            } else if (seen++ == want_index) {
                *out_node = node;
                return true;
            }

            void *next = NULL;
            if (!safe_memory_read_pointer((mach_vm_address_t)node + SUBSET_NODE_NEXT,
                                          &next)) {
                break;
            }
            node = next;
        }
    }

    return false;
}

static bool fill_subset(void *node, AnimSetSubSet *out) {
    if (!node || !out) return false;

    out->node = node;
    if (!safe_memory_read_u32((mach_vm_address_t)node + SUBSET_NODE_KEY, &out->key)) {
        return false;
    }
    if (!read_refmap((uintptr_t)node + SUBSET_NODE_ANIM_MAP, &out->animation)) {
        return false;
    }
    if (!safe_memory_read_u32((mach_vm_address_t)node + SUBSET_NODE_FALLBACK,
                              &out->fallback_subset)) {
        out->fallback_subset = 0xFFFFFFFFu;
    }
    return true;
}

bool animset_subset_at(const AnimSetRefMap *map, uint32_t index, AnimSetSubSet *out) {
    void *node = NULL;
    if (!walk_nodes(map, index, 0, false, &node)) return false;
    return fill_subset(node, out);
}

bool animset_subset_find(const AnimSetRefMap *map, uint32_t key, AnimSetSubSet *out) {
    void *node = NULL;
    if (!walk_nodes(map, 0, key, true, &node)) return false;
    return fill_subset(node, out);
}

static bool fill_animation(void *node, AnimSetAnimation *out) {
    if (!node || !out) return false;

    uint32_t flags = 0;
    if (!safe_memory_read_u32((mach_vm_address_t)node + ANIM_NODE_KEY, &out->key)) return false;
    if (!safe_memory_read_u32((mach_vm_address_t)node + ANIM_NODE_ID, &out->id)) return false;
    if (!safe_memory_read_u32((mach_vm_address_t)node + ANIM_NODE_FLAGS, &flags)) flags = 0;
    out->flags = (uint8_t)(flags & 0xFF);
    return true;
}

bool animset_animation_at(const AnimSetRefMap *map, uint32_t index, AnimSetAnimation *out) {
    void *node = NULL;
    if (!walk_nodes(map, index, 0, false, &node)) return false;
    return fill_animation(node, out);
}

bool animset_animation_find(const AnimSetRefMap *map, uint32_t key, AnimSetAnimation *out) {
    void *node = NULL;
    if (!walk_nodes(map, 0, key, true, &node)) return false;
    return fill_animation(node, out);
}

// ============================================================================
// Writing
// ============================================================================
//
// Mirrors LegacyMapBase::insert from upstream/CoreLib/Base/LegacyMap.h:
//
//     hash = &HashTable[Hash(key) % HashSize]
//     walk the chain; if the key is already there, overwrite Value and stop
//     otherwise allocate a Node, link it at the chain tail (or at *hash), ItemCount++
//
// Note that insert does NOT rehash or grow, so neither do we.
//
// The node must come from the game's allocator: the game frees these when the
// resource unloads, and handing it a malloc pointer would corrupt its heap.

typedef void *(*GameAllocateFunc)(size_t size, uint32_t alloc_type, int a3, size_t a4);

// On by default, since a gated write path means the mods that need it do not
// work. The allocator convention was read from call sites rather than a header,
// so it was gated until a session proved it: BG3AF drove 91 real inserts into
// BG3SX's set, a controlled probe took ItemCount 91 -> 92 and read the value
// back byte-identical, and no write-path error was logged. Set
// BG3SE_ANIMSET_WRITE=0 to turn it back off.
static bool animset_writes_enabled(void) {
    static int cached = -1;
    if (cached < 0) {
        const char *env = getenv("BG3SE_ANIMSET_WRITE");
        cached = (env && env[0] == '0') ? 0 : 1;
        if (!cached) {
            LOG_CORE_INFO("AnimationSet writes disabled by BG3SE_ANIMSET_WRITE=0; "
                          "reads are unaffected");
        }
    }
    return cached == 1;
}

static void *animset_game_alloc(size_t size) {
    GameAllocateFunc alloc = (GameAllocateFunc)offset_table_game_fn(GAME_FN_MEMORY_ALLOCATE);
    if (!alloc) {
        LOG_CORE_WARN("ls::MemoryManager::Allocate has no address for this build; "
                      "AnimationSet writes unavailable");
        return NULL;
    }
    return alloc(size, 0, 0, 0);
}

bool animset_animation_set(void *map_addr, uint32_t key, uint32_t id, uint8_t flags) {
    if (!map_addr) return false;
    if (!animset_writes_enabled()) return false;

    AnimSetRefMap map;
    if (!read_refmap((uintptr_t)map_addr, &map)) return false;

    // insert() divides by HashSize. A zero-bucket map would need its table
    // allocated first, which the game does at load; refuse rather than invent one.
    if (map.hash_size == 0 || !map.hash_table) {
        LOG_CORE_WARN("AnimationSet: refusing to insert into a map with no buckets "
                      "(HashSize=%u)", map.hash_size);
        return false;
    }

    uint32_t bucket = key % map.hash_size;
    mach_vm_address_t slot =
        (mach_vm_address_t)map.hash_table + (size_t)bucket * sizeof(void *);

    void *node = NULL;
    if (!safe_memory_read_pointer(slot, &node)) return false;

    // Existing key: overwrite the value in place, exactly as insert() does.
    void *last = NULL;
    for (uint32_t hop = 0; node && hop < 4096; hop++) {
        uint32_t node_key = 0;
        if (safe_memory_read_u32((mach_vm_address_t)node + ANIM_NODE_KEY, &node_key)
            && node_key == key) {
            if (!safe_memory_write_u32((mach_vm_address_t)node + ANIM_NODE_ID, id)) return false;
            safe_memory_write_u32((mach_vm_address_t)node + ANIM_NODE_FLAGS, flags);
            return true;
        }
        last = node;
        void *next = NULL;
        if (!safe_memory_read_pointer((mach_vm_address_t)node + ANIM_NODE_NEXT, &next)) break;
        node = next;
    }

    void *fresh = animset_game_alloc(ANIM_NODE_SIZE);
    if (!fresh) return false;

    // Fill the node completely BEFORE linking it. A half-initialised node that is
    // already reachable is a crash waiting for the next reader.
    bool ok = safe_memory_write_pointer((mach_vm_address_t)fresh + ANIM_NODE_NEXT, NULL)
           && safe_memory_write_u32((mach_vm_address_t)fresh + ANIM_NODE_KEY, key)
           && safe_memory_write_u32((mach_vm_address_t)fresh + ANIM_NODE_ID, id)
           && safe_memory_write_u32((mach_vm_address_t)fresh + ANIM_NODE_FLAGS, flags);
    if (!ok) {
        // Leaked rather than freed: we have no verified game free, and leaking one
        // node is better than handing the allocator a pointer it may not own.
        LOG_CORE_WARN("AnimationSet: failed to initialise a new node; not linking it");
        return false;
    }

    if (last) {
        if (!safe_memory_write_pointer((mach_vm_address_t)last + ANIM_NODE_NEXT, fresh)) {
            return false;
        }
    } else {
        if (!safe_memory_write_pointer(slot, fresh)) return false;
    }

    safe_memory_write_u32((mach_vm_address_t)map_addr, map.item_count + 1);
    return true;
}
