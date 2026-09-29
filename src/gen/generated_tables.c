/**
 * generated_tables.c - dispatch the system and replication tables by store.
 *
 * Mirrors generated_registry.c: the per-store copies are compiled with renamed
 * exports, and this picks between them using the store detected at runtime.
 */

#include "generated_tables.h"
#include "../core/version_detect.h"

#include <string.h>

const EcsSystemName *generated_system_names_steam(size_t *);
const ReplicatedTypeGlobal *generated_replicated_types_steam(size_t *);
const EcsSystemName *generated_system_names_gog(size_t *);
const ReplicatedTypeGlobal *generated_replicated_types_gog(size_t *);
uintptr_t generated_remove_component_va_steam(const char *);
uintptr_t generated_remove_component_va_gog(const char *);

typedef enum { STORE_NONE = 0, STORE_STEAM, STORE_GOG } ActiveStore;

static ActiveStore active_store(void) {
    static ActiveStore cached = STORE_NONE;
    if (cached != STORE_NONE) return cached;

    const char *store = version_detect_get_store();
    if (!store) return STORE_NONE;              /* too early; do not cache */

    if (strcmp(store, "gog") == 0)        cached = STORE_GOG;
    else if (strcmp(store, "steam") == 0) cached = STORE_STEAM;
    else return STORE_NONE;                     /* no table for this build */

    return cached;
}

const EcsSystemName *generated_system_names(size_t *count) {
    switch (active_store()) {
        case STORE_GOG:   return generated_system_names_gog(count);
        case STORE_STEAM: return generated_system_names_steam(count);
        default:          if (count) *count = 0; return NULL;
    }
}

const ReplicatedTypeGlobal *generated_replicated_types(size_t *count) {
    switch (active_store()) {
        case STORE_GOG:   return generated_replicated_types_gog(count);
        case STORE_STEAM: return generated_replicated_types_steam(count);
        default:          if (count) *count = 0; return NULL;
    }
}

uintptr_t generated_remove_component_va(const char *class_name) {
    switch (active_store()) {
        case STORE_GOG:   return generated_remove_component_va_gog(class_name);
        case STORE_STEAM: return generated_remove_component_va_steam(class_name);
        default:          return 0;
    }
}
