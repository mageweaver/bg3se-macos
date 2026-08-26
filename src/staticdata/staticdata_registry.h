/**
 * BG3SE-macOS - Static data type registry
 *
 * Every GuidResource manager hangs off ls::ImmutableDataHeadmaster, keyed by a
 * per-type index the game stores in a global:
 *
 *     ls::TypeId<eoc::ClassDescriptions, ls::ImmutableDataHeadmaster>::m_TypeIndex
 *
 * The generated table pairs the type names Windows BG3SE exposes to Lua with the
 * offset of that global, so any of them can be resolved without a per-type hook.
 * See plans/staticdata-generic-managers.md.
 */

#ifndef BG3SE_STATICDATA_REGISTRY_H
#define BG3SE_STATICDATA_REGISTRY_H

#include <stdint.h>
#include <stdbool.h>

typedef struct {
    const char *name;          // as mods write it, e.g. "Tag"
    const char *engine_class;  // e.g. "ls::TagManager"
    uint64_t index_offset;     // offset of m_TypeIndex from the image base
} StaticDataTypeEntry;

extern const StaticDataTypeEntry g_staticdata_types[];
extern const int g_staticdata_type_count;

/** Record the main binary base; the table's offsets are image-relative. */
void staticdata_registry_init(void *main_binary_base);

/** Look a type up by the name mods use. NULL if unknown. */
const StaticDataTypeEntry *staticdata_registry_find(const char *name);

/**
 * Resolve a type's manager through the headmaster.
 * Returns NULL if the headmaster is not up yet or the type is not registered
 * in this session (not every type is populated in every save).
 */
void *staticdata_registry_get_manager(const StaticDataTypeEntry *entry);

#endif // BG3SE_STATICDATA_REGISTRY_H
