#ifndef ENTITY_LIFECYCLE_H
#define ENTITY_LIFECYCLE_H

#include <stdbool.h>
#include <stdint.h>

/** True when the command-buffer entry points are resolvable on this build. */
bool entity_lifecycle_available(void);

/**
 * Resolve the calling thread's EntityCommandBuffer.
 * Must be called on the thread that submits the command; the buffer is
 * per-thread and indexed by ls::ThreadRegistry::RequestThreadIndex().
 */
void *entity_lifecycle_get_ecb(void *entity_world);

/** Queue creation of a new entity. Returns its handle, or 0 on failure. */
uint64_t entity_lifecycle_create(void *entity_world);

/** Queue destruction of an entity. Returns false if it could not be queued. */
bool entity_lifecycle_destroy(void *entity_world, uint64_t entity);

#endif /* ENTITY_LIFECYCLE_H */
