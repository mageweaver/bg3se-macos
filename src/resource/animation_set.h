/**
 * BG3SE-macOS - AnimationSet resource containers
 *
 * Typed access to ls::AnimationSetResource's subset and animation maps, which
 * BG3AF (and through it BG3SX, WickedAnims, GrazztRing) reads AND writes:
 *
 *     self[1].AnimationBank.AnimationSubSets[key].Animation[mapKey] = {ID=..}
 *
 * Layout is taken from Norbyte's headers, which are vendored in this repo -
 * upstream/BG3Extender/GameDefinitions/Resources.h and
 * upstream/CoreLib/Base/LegacyMap.h - and verified live against BG3SX's own
 * declared data. See plans/animationset-writable-resources.md.
 */

#ifndef BG3SE_ANIMATION_SET_H
#define BG3SE_ANIMATION_SET_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

// LegacyRefMap<K, V> == RefMapInternals: { u32 ItemCount; u32 HashSize; MapNode** HashTable; }
typedef struct {
    uint32_t item_count;
    uint32_t hash_size;
    void *hash_table;      // MapNode**
} AnimSetRefMap;

// A resolved AnimationSubSet entry.
typedef struct {
    void *node;            // the owning MapNode, for writes
    uint32_t key;          // FixedString; 0xFFFFFFFF is the empty MapKey
    AnimSetRefMap animation;
    uint32_t fallback_subset;
} AnimSetSubSet;

// One Animation entry: AnimationDesc { FixedString ID; uint8_t flags; }
typedef struct {
    uint32_t key;          // MapKey
    uint32_t id;           // AnimationDesc::ID
    uint8_t flags;
} AnimSetAnimation;

/** Read a LegacyRefMap header at `addr`. False if it does not look like one. */
bool animset_read_map(void *addr, AnimSetRefMap *out);

/** Fetch a resource's AnimationBank pointer (the AnimationSubSets map). */
bool animset_get_bank(void *resource, void **out_bank);

/** Read a resource's AnimationBank -> AnimationSubSets map. False if absent. */
bool animset_get_subsets(void *resource, AnimSetRefMap *out);

/**
 * Enumerate subsets. `index` walks 0..item_count-1 in bucket order.
 * Returns false once exhausted.
 */
bool animset_subset_at(const AnimSetRefMap *map, uint32_t index, AnimSetSubSet *out);

/** Find a subset by MapKey FixedString. */
bool animset_subset_find(const AnimSetRefMap *map, uint32_t key, AnimSetSubSet *out);

/** Enumerate a subset's Animation entries. */
bool animset_animation_at(const AnimSetRefMap *map, uint32_t index, AnimSetAnimation *out);

/** Find one Animation entry by MapKey. */
bool animset_animation_find(const AnimSetRefMap *map, uint32_t key, AnimSetAnimation *out);

/**
 * Insert or overwrite Animation[key] = { id, flags }, in the game's own map.
 *
 * Allocates the node from ls::MemoryManager so the game can free it on unload.
 * `map_addr` is the address of the AnimSetRefMap inside the game object, since
 * inserting mutates ItemCount and may replace HashTable.
 *
 * Returns false if the game allocator is unavailable or the map looks invalid;
 * never partially links a node.
 */
bool animset_animation_set(void *map_addr, uint32_t key, uint32_t id, uint8_t flags);

#ifdef __cplusplus
}
#endif

#endif // BG3SE_ANIMATION_SET_H
