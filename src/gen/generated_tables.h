/**
 * generated_tables.h - per-store system and replication tables.
 *
 * These expand from one store's generated_typeids.h, so their addresses belong
 * to that store alone: 4050 addresses differ between the two headers. One dylib
 * serves several stores, so the tables are compiled per store and selected from
 * the store detected at runtime, the same as the component registry.
 *
 * Both accessors return NULL and set *count to 0 when the running store has no
 * table. Callers must treat that as "feature unavailable" rather than reading
 * whatever is there.
 */

#ifndef BG3SE_GEN_GENERATED_TABLES_H
#define BG3SE_GEN_GENERATED_TABLES_H

#include <stddef.h>
#include <stdint.h>

typedef struct {
    const char *public_name;
    const char *engine_class;
    uintptr_t type_id_ghidra_address;
} EcsSystemName;

typedef struct {
    const char *name;
    const char *component_name;
    const char *mangled_symbol;
    const char *context;
    const char *build_id;
    uintptr_t replicated_type_va;
} ReplicatedTypeGlobal;

const EcsSystemName *generated_system_names(size_t *count);
const ReplicatedTypeGlobal *generated_replicated_types(size_t *count);

/**
 * Recorded VA of the RemoveComponent specialization for an engine class name,
 * or 0 when this build has none. The caller applies the image slide.
 */
uintptr_t generated_remove_component_va(const char *class_name);

#endif /* BG3SE_GEN_GENERATED_TABLES_H */
