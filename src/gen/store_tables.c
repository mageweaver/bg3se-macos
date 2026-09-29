/**
 * store_tables.c - one store's system and replication tables.
 *
 * Compiled once per store, each time with that store's src/gen/<store> ahead of
 * the include path and with its two exports renamed (see CMakeLists.txt). The
 * dispatcher in generated_tables.c picks between the results at runtime.
 *
 * Kept separate from the consumers so the tables can be built per store without
 * duplicating the Lua registration and world-binding state those files hold.
 */

#include "generated_tables.h"
#include "generated_typeids.h"
#include "generated_remove_component.h"

#include <string.h>

#define SYSTEM_ENTRY(public, engine, va) { public, engine, va },
static const EcsSystemName kSystemNames[] = {
    GENERATED_SYSTEM_TYPEID_ENTRIES(SYSTEM_ENTRY)
};
#undef SYSTEM_ENTRY

_Static_assert(sizeof(kSystemNames) / sizeof(kSystemNames[0])
                   == GENERATED_SYSTEM_TYPEID_COUNT,
               "system table size does not match GENERATED_SYSTEM_TYPEID_COUNT");

#define REPLICATED_TYPE_ENTRY(api_name, component_name, mangled_symbol, context, \
                              build_id, preferred_va)                            \
    { api_name, component_name, mangled_symbol, context, build_id, preferred_va },
static const ReplicatedTypeGlobal kReplicatedTypes[] = {
    GENERATED_REPLICATED_TYPE_CONTEXT_ENTRIES(REPLICATED_TYPE_ENTRY)
};
#undef REPLICATED_TYPE_ENTRY

_Static_assert(sizeof(kReplicatedTypes) / sizeof(kReplicatedTypes[0])
                   == GENERATED_REPLICATED_TYPE_CONTEXT_COUNT,
               "replication table size does not match "
               "GENERATED_REPLICATED_TYPE_CONTEXT_COUNT");

const EcsSystemName *generated_system_names(size_t *count) {
    if (count) *count = sizeof(kSystemNames) / sizeof(kSystemNames[0]);
    return kSystemNames;
}

const ReplicatedTypeGlobal *generated_replicated_types(size_t *count) {
    if (count) *count = sizeof(kReplicatedTypes) / sizeof(kReplicatedTypes[0]);
    return kReplicatedTypes;
}

/*
 * The remove-component table is an if-chain rather than an array, so it is
 * exposed as a lookup. Returns the recorded VA for the engine class name, or 0
 * when this build has no specialization for it. The caller applies the slide.
 */
uintptr_t generated_remove_component_va(const char *class_name) {
    if (!class_name) return 0;
#define REMOVE_COMPONENT_ENTRY(name, va) \
    if (strcmp(class_name, (name)) == 0) return (uintptr_t)(va);
    GENERATED_REMOVE_COMPONENT_ENTRIES(REMOVE_COMPONENT_ENTRY)
#undef REMOVE_COMPONENT_ENTRY
    return 0;
}
