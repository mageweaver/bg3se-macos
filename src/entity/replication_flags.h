/**
 * BG3SE-macOS - Read-only entity replication flag lookup.
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

#ifdef __cplusplus
}
#endif

#endif /* BG3SE_REPLICATION_FLAGS_H */
