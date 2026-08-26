/**
 * BG3SE-macOS - GuidResource field layouts
 *
 * Every static data type is a plain struct whose fields follow declaration
 * order, so `tools/generate_resource_fields.py` can compute the offsets from
 * upstream's GuidResources.h without a disassembler. The catch is that one
 * mis-sized field shifts every field after it, so the generator refuses to
 * guess: a struct is truncated at the first type it cannot size (the fields
 * before it stay exact). Today that is only the bare std::variant fields.
 *
 * Sizes are for the macOS build, which differs from upstream's MSVC in one
 * place that matters here: std::string is 24 bytes rather than 32.
 */

#ifndef BG3SE_STATICDATA_FIELDS_H
#define BG3SE_STATICDATA_FIELDS_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

typedef enum {
    RF_BOOL,
    RF_U8,
    RF_U16,
    RF_U32,
    RF_I32,
    RF_F32,
    RF_F64,
    RF_FIXEDSTRING,
    RF_GUID,
    RF_STDSTRING,
    RF_TRANSLATEDSTRING,
    RF_ARRAY_GUID,
    RF_ARRAY_FIXEDSTRING,
    RF_ARRAY_I32,
    RF_ARRAY_U8,
} ResourceFieldKind;

typedef struct {
    const char *name;
    uint16_t offset;
    ResourceFieldKind kind;
} ResourceField;

typedef struct {
    const char *type_name;      // matches StaticDataTypeEntry::name
    const ResourceField *fields;
    int field_count;
    uint16_t size;              // sizeof the struct, for bounds checks
} ResourceLayout;

extern const ResourceLayout g_resource_layouts[];
extern const int g_resource_layout_count;

/** Look up a type's layout by the name mods use. NULL if not modelled. */
const ResourceLayout *resource_layout_find(const char *type_name);

/** Look up one field within a layout. NULL if absent or truncated away. */
const ResourceField *resource_layout_field(const ResourceLayout *layout, const char *name);

/** True if the kind is one of the RF_ARRAY_* element kinds. */
bool resource_field_is_array(ResourceFieldKind kind);

#endif // BG3SE_STATICDATA_FIELDS_H
