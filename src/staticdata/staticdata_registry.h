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
#include <stddef.h>

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

/**
 * Look one resource up by GUID through the manager's own virtual
 * GetObjectByKey(Guid const&). `guid16` is the 16 raw bytes of the GUID as the
 * game stores them. NULL if the manager is unavailable or the GUID is absent.
 */
void *staticdata_registry_get_object(const StaticDataTypeEntry *entry, const void *guid16);

/**
 * Parse a GUID from its canonical string form into 16 raw bytes, in the byte
 * order ls::Guid uses. `out_guid16` must have room for 16 bytes.
 */
bool staticdata_registry_parse_guid(const char *guid_str, void *out_guid16);

/** As above, parsing the GUID from its string form. */
void *staticdata_registry_get_object_by_guid_string(const StaticDataTypeEntry *entry,
                                                    const char *guid_str);

/**
 * Fetch the bank's key array — every GUID it holds.
 * `out_buf` receives the Array<Guid> buffer, `out_count` its length.
 */
bool staticdata_registry_get_keys(const StaticDataTypeEntry *entry,
                                  void **out_buf, uint32_t *out_count);

/** Format the GUID at `index` in a key buffer into `out` (>= 40 bytes). */
bool staticdata_registry_format_key(void *keys_buf, uint32_t index, char *out, size_t out_size);

/**
 * Create a new resource of this type and add it to the bank.
 *
 * `guid_str` may be NULL to generate one. `out_guid` (>= 40 bytes, optional)
 * receives the GUID actually used. Returns the stored resource, or NULL.
 *
 * Requires at least two existing resources of the type: the element size is
 * measured from their spacing and the VMT is copied from one of them, neither
 * of which can be known otherwise.
 */
void *staticdata_registry_create(const StaticDataTypeEntry *entry,
                                 const char *guid_str,
                                 char *out_guid, size_t out_guid_size);

#endif // BG3SE_STATICDATA_REGISTRY_H
