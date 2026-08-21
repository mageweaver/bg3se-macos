/**
 * BG3SE-macOS - Read-only entity replication flag lookup.
 * ReplicatedTypeContext preferred VAs come from generated_typeids.h.
 */

#ifndef BG3SE_REPLICATION_FLAGS_H
#define BG3SE_REPLICATION_FLAGS_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

bool replication_flags_get(void *entity_world, uint64_t entity_handle,
                           const char *component_name, uint32_t qword,
                           uint64_t *out_flags);

/**
 * OR flags into an entity's replication bitset and mark SyncBuffers dirty.
 *
 * Fails closed (returns false) when the entity has no replication entry for the
 * component, or when the requested qword lies beyond the bitset's current size:
 * both cases would require mutating engine-owned container storage, which is
 * not attempted. out_changed reports whether any bit actually flipped.
 */
bool replication_flags_set(void *entity_world, uint64_t entity_handle,
                           const char *component_name, uint32_t qword,
                           uint64_t flags, bool *out_changed);

#ifdef __cplusplus
}
#endif

#endif /* BG3SE_REPLICATION_FLAGS_H */
