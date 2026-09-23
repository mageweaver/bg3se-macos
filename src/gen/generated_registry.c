/**
 * generated_registry.c - dispatch the generated component tables by store.
 *
 * One dylib carries a table per supported build. Each store's
 * generated_component_registry.c is compiled with its own src/gen/<store> on
 * the include path, and with its four public functions renamed to a
 * store-suffixed symbol (see CMakeLists.txt). This file picks between them
 * using the store detected at runtime.
 *
 * Doing it in the build rather than in the generated files means
 * tools/extract_typeids.py keeps emitting exactly what it always has.
 */

#include "component_registry.h"
#include "component_typeid.h"
#include "../core/logging.h"
#include "../core/version_detect.h"

#include <string.h>

/* Provided by the per-store translation units. */
bool component_typeid_generated_lookup_steam(const char *, const char *, uint64_t *);
void component_registry_register_all_generated_steam(void);
int  component_registry_generated_count_steam(void);
int  component_typeid_discover_all_generated_steam(void);

bool component_typeid_generated_lookup_gog(const char *, const char *, uint64_t *);
void component_registry_register_all_generated_gog(void);
int  component_registry_generated_count_gog(void);
int  component_typeid_discover_all_generated_gog(void);

typedef enum { STORE_NONE = 0, STORE_STEAM, STORE_GOG } ActiveStore;

static ActiveStore active_store(void) {
    static ActiveStore cached = STORE_NONE;
    if (cached != STORE_NONE) return cached;

    const char *store = version_detect_get_store();
    if (!store) return STORE_NONE;              /* too early; do not cache */

    if (strcmp(store, "gog") == 0)        cached = STORE_GOG;
    else if (strcmp(store, "steam") == 0) cached = STORE_STEAM;
    else {
        /*
         * No table for this build. Returning nothing is correct: the callers
         * treat an empty registry as "no TypeIds", which is what the identity
         * gate in version_detect already reports. Guessing a table here would
         * read wrong objects at plausible-looking addresses.
         */
        static bool warned = false;
        if (!warned) {
            warned = true;
            LOG_ENTITY_INFO("No generated component table for store '%s'; "
                            "component discovery disabled", store);
        }
        return STORE_NONE;
    }
    return cached;
}

bool component_typeid_generated_lookup(const char *name, const char *context,
                                       uint64_t *out_va) {
    switch (active_store()) {
        case STORE_GOG:   return component_typeid_generated_lookup_gog(name, context, out_va);
        case STORE_STEAM: return component_typeid_generated_lookup_steam(name, context, out_va);
        default:          return false;
    }
}

void component_registry_register_all_generated(void) {
    switch (active_store()) {
        case STORE_GOG:   component_registry_register_all_generated_gog();   break;
        case STORE_STEAM: component_registry_register_all_generated_steam(); break;
        default: break;
    }
}

int component_registry_generated_count(void) {
    switch (active_store()) {
        case STORE_GOG:   return component_registry_generated_count_gog();
        case STORE_STEAM: return component_registry_generated_count_steam();
        default:          return 0;
    }
}

int component_typeid_discover_all_generated(void) {
    switch (active_store()) {
        case STORE_GOG:   return component_typeid_discover_all_generated_gog();
        case STORE_STEAM: return component_typeid_discover_all_generated_steam();
        default:          return 0;
    }
}
