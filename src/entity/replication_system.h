#ifndef BG3SE_REPLICATION_SYSTEM_H
#define BG3SE_REPLICATION_SYSTEM_H

#include <stdbool.h>
#include <stdint.h>

/** Resolve the esv::replication::ReplicationSystem instance, or NULL. */
void *replication_system_get(void *world);

/** Ask the engine to replicate an entity to a peer. */
bool replication_system_replicate_to_peer(void *world, uint64_t entity, int peer_id);

/** Stop replicating an entity. */
bool replication_system_stop_replicate(void *world, uint64_t entity);

#endif
