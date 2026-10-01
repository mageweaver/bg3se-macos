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
/** Read-only structural dump of SyncBuffers and its pools (diagnostic). */
void replication_flags_debug_dump(void *entity_world);

/** Latching sampler: logs once if any replication pool is ever non-empty. */
void replication_flags_sample(void *entity_world);

bool replication_flags_set(void *entity_world, uint64_t entity_handle,
                           const char *component_name, uint32_t qword,
                           uint64_t flags, bool *out_changed);

/**
 * Replication event support (upstream ServerEntityReplicationEventHooks).
 * replication_type_index: SyncBuffers pool index for a replicated component
 * (short or engine name), or -1; out_short_name gets the short API name.
 * replication_pool_snapshot: copies up to max (entity handle, first flags
 * qword) pairs out of that pool. Read-only.
 */
int replication_type_index(const char *component_name, const char **out_short_name);
bool replication_sync_dirty(void *entity_world);
int replication_pool_snapshot(void *entity_world, int replication_index,
                              uint64_t *out_handles, uint64_t *out_fields, int max);

#ifdef __cplusplus
}
#endif

#endif /* BG3SE_REPLICATION_FLAGS_H */
