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
 * Sizes are for the macOS build. The one that matters most is STDString:
 * upstream declares it as a std::basic_string, but the shipped struct stores it
 * in 16 bytes -- a pointer, a uint32 length and a uint32 capacity whose top bit
 * marks the long form, with short strings inline and their length in the last
 * byte. Reading it as a std::string (24 bytes on libc++, 32 on MSVC) put every
 * field after the first string in a struct at the wrong offset, in 35 of the
 * 106 types. See plans/staticdata-string-layout.md.
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
    RF_HASHSET_FIXEDSTRING,
    RF_ARRAY_STRUCT,            // Array of a nested struct; see elem/elem_size
    RF_VEC3,
    RF_VEC4,
    RF_OPT_GUID,
    RF_OPT_STDSTRING,
    RF_OPT_I32,
    RF_OPT_U8,
    RF_VARIANT_SCALAR,          // variant<NoValue, float, int, FixedString, bool>
    RF_HASHMAP_U8_FIXEDSTRING,
} ResourceFieldKind;

struct ResourceLayout;

typedef struct {
    const char *name;
    uint16_t offset;
    ResourceFieldKind kind;
    const struct ResourceLayout *elem;  // RF_ARRAY_STRUCT: element layout
    uint16_t elem_size;                 // RF_ARRAY_STRUCT: array stride
} ResourceField;

typedef struct ResourceLayout {
    const char *type_name;      // matches StaticDataTypeEntry::name
    const ResourceField *fields;
    int field_count;
    uint16_t size;              // sizeof the struct, for bounds checks
    bool embedded;              // a nested element type, not a GuidResource
} ResourceLayout;

extern const ResourceLayout g_resource_layouts[];
extern const int g_resource_layout_count;

/** Look up a type's layout by the name mods use. NULL if not modelled. */
const ResourceLayout *resource_layout_find(const char *type_name);

/** Look up one field within a layout. NULL if absent or truncated away. */
const ResourceField *resource_layout_field(const ResourceLayout *layout, const char *name);

/** True if the kind is one of the RF_ARRAY_* element kinds. */
bool resource_field_is_array(ResourceFieldKind kind);

/*
 * HashSet<T> is { StaticArray<int32> HashKeys; Array<int32> NextIds; Array<T> Keys; }.
 * The elements live in Keys, which is a plain Array at this offset; the two
 * index arrays only exist to make lookup fast.
 */
#define HASHSET_HASHKEYS_OFFSET 0x00
#define HASHSET_HASHSIZE_OFFSET 0x08
#define HASHSET_NEXTIDS_OFFSET  0x10
#define HASHSET_KEYS_OFFSET     0x20

/* HashMap<K,V> is a HashSet<K> with an Array<V> appended. */
#define HASHMAP_VALUES_OFFSET   0x30

/*
 * std::optional<T> stores T at +0 and its engaged flag just past it, so the
 * flag sits at sizeof(T) -- 16 for a Guid or an STDString, 4 for an int, 1 for
 * a byte-sized enum.
 */
#define OPTIONAL_FLAG_OFFSET(t_size) (t_size)

/*
 * libc++ lays a variant out as the union of its alternatives followed by a
 * one-byte index. For variant<NoValue, float, int, FixedString, bool> the
 * union is four bytes wide, so the index sits at +4.
 */
#define VARIANT_SCALAR_INDEX_OFFSET 4

#endif // BG3SE_STATICDATA_FIELDS_H
