/**
 * lua_resource_object.c - Field proxies for GuidResource objects
 *
 * See lua_resource_object.h. The layout table is generated; everything here is
 * the marshalling between a raw field and a Lua value.
 */

#include "lua_resource_object.h"

#include "../staticdata/staticdata_fields.h"
#include "../staticdata/staticdata_registry.h"
#include "../strings/fixed_string.h"
#include "../localization/localization.h"
#include "../core/offset_table.h"
#include "../core/safe_memory.h"
#include "../core/logging.h"

#include <lauxlib.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

#define RESOURCE_OBJECT_MT "BG3SE.ResourceObject"
#define RESOURCE_ARRAY_MT  "BG3SE.ResourceArray"

// GuidResource is { void* VMT; Guid ResourceUUID; }
#define RESOURCE_UUID_OFFSET 0x08

// Array<T> is { T* buf; uint32_t capacity; uint32_t size; }
#define ARRAY_BUF_OFFSET      0x00
#define ARRAY_CAPACITY_OFFSET 0x08
#define ARRAY_SIZE_OFFSET     0x0c

typedef struct {
    void *obj;
    const ResourceLayout *layout;
} ResourceObjectUD;

typedef struct {
    void *array;                 // address of the Array<T> itself
    ResourceFieldKind kind;      // RF_ARRAY_*
    bool readonly;               // set for a HashSet's element array
    const ResourceLayout *elem;  // RF_ARRAY_STRUCT: element layout
    uint16_t elem_size;          // RF_ARRAY_STRUCT: stride
} ResourceArrayUD;

// ============================================================================
// std::string (libc++)
// ============================================================================
//
// STDString is 16 bytes, not a std::string. Read off a live Progression:
//
//   short:  characters inline at +0, length in (byte[15] & 0x7f)
//   long:   pointer at +0, uint32 length at +8, uint32 capacity at +12
//           whose top bit marks the long form
//
// The discriminator is the top bit of the last byte either way, so the two
// forms are told apart the same way. Upstream declares this field as a
// std::basic_string, which would be 24 bytes on libc++ and 32 on MSVC; both
// readings put every field after the first string in a struct at the wrong
// offset. The sample that settled it held "Barbarian" inline with 0x09 at
// byte 15, and a 231-character Boosts string out of line with its capacity
// at +12 carrying the 0x80000000 flag.

#define STDSTRING_SIZE            16
#define STDSTRING_INLINE_MAX      15   // characters that fit inline
#define STDSTRING_WRITE_INLINE_MAX 14  // leave room for a terminator when we write

/** Returns a pointer to the characters and the length, or NULL. */
static const char *read_stdstring(const void *addr, size_t *out_len) {
    if (!addr) return NULL;
    const uint8_t *p = (const uint8_t *)addr;

    uint8_t flag_byte = 0;
    if (!safe_memory_read_u8((mach_vm_address_t)(p + 15), &flag_byte)) return NULL;

    if (!(flag_byte & 0x80)) {
        size_t len = flag_byte & 0x7f;
        if (len > STDSTRING_INLINE_MAX) return NULL;
        *out_len = len;
        return (const char *)p;
    }

    void *data = NULL;
    uint32_t size = 0, cap = 0;
    if (!safe_memory_read_u64((mach_vm_address_t)(p + 0), (uint64_t *)&data)) return NULL;
    if (!safe_memory_read_u32((mach_vm_address_t)(p + 8), &size)) return NULL;
    if (!safe_memory_read_u32((mach_vm_address_t)(p + 12), &cap)) return NULL;
    cap &= 0x7fffffffu;

    if (!data || size > cap || cap > (1u << 24)) return NULL;
    uint8_t probe = 0;
    if (!safe_memory_read_u8((mach_vm_address_t)data, &probe)) return NULL;

    *out_len = (size_t)size;
    return (const char *)data;
}

typedef void *(*ResourceAllocateFunc)(size_t size, uint32_t alloc_type, int a3, size_t a4);

static void *resource_game_alloc(size_t size) {
    ResourceAllocateFunc alloc =
        (ResourceAllocateFunc)offset_table_game_fn(GAME_FN_MEMORY_ALLOCATE);
    if (!alloc) {
        LOG_CORE_WARN("ls::MemoryManager::Allocate has no address for this build; "
                      "static data writes unavailable");
        return NULL;
    }
    return alloc(size, 0, 0, 0);
}

/** Overwrite an STDString in place. Short strings need no allocation. */
static bool write_stdstring(void *addr, const char *str, size_t len) {
    if (!addr) return false;
    uint8_t *p = (uint8_t *)addr;

    if (len <= STDSTRING_WRITE_INLINE_MAX) {
        memset(p, 0, STDSTRING_SIZE);
        memcpy(p, str, len);
        p[15] = (uint8_t)(len & 0x7f);
        return true;
    }

    char *buf = (char *)resource_game_alloc(len + 1);
    if (!buf) return false;
    memcpy(buf, str, len);
    buf[len] = '\0';
    *(void **)(p + 0) = buf;
    *(uint32_t *)(p + 8) = (uint32_t)len;
    *(uint32_t *)(p + 12) = (uint32_t)(len + 1) | 0x80000000u;
    return true;
}

// ============================================================================
// Field marshalling
// ============================================================================

static size_t array_element_size_of(const ResourceArrayUD *ud);

static size_t array_element_size(ResourceFieldKind kind) {
    switch (kind) {
        case RF_ARRAY_GUID:        return 16;
        case RF_ARRAY_FIXEDSTRING: return 4;
        case RF_ARRAY_I32:         return 4;
        case RF_ARRAY_U8:          return 1;
        default:                   return 0;
    }
}

static size_t array_element_size_of(const ResourceArrayUD *ud) {
    if (ud->kind == RF_ARRAY_STRUCT) return ud->elem_size;
    return array_element_size(ud->kind);
}

static void push_guid(lua_State *L, const void *addr) {
    char buf[40];
    // The registry already knows how to format a raw 16-byte GUID; a key buffer
    // of one element is exactly that.
    if (staticdata_registry_format_key((void *)addr, 0, buf, sizeof(buf))) {
        lua_pushstring(L, buf);
    } else {
        lua_pushnil(L);
    }
}

static void push_fixedstring(lua_State *L, uint32_t index) {
    const char *s = fixed_string_resolve(index);
    if (s) {
        lua_pushstring(L, s);
    } else {
        lua_pushnil(L);
    }
}

/** Ext.Loca.GetTranslatedString equivalent for a handle captured as an upvalue. */
static int translated_string_get(lua_State *L) {
    const char *handle = lua_tostring(L, lua_upvalueindex(1));
    const char *text = handle ? localization_get(handle, NULL) : NULL;
    if (text) {
        lua_pushstring(L, text);
    } else {
        lua_pushnil(L);
    }
    return 1;
}

/** Push one RuntimeStringHandle as { Handle = <FixedString>, Version = n }. */
static void push_runtime_string_handle(lua_State *L, const void *addr) {
    uint32_t index = 0;
    uint8_t lo = 0, hi = 0;
    safe_memory_read_u32((mach_vm_address_t)addr, &index);
    safe_memory_read_u8((mach_vm_address_t)((const uint8_t *)addr + 4), &lo);
    safe_memory_read_u8((mach_vm_address_t)((const uint8_t *)addr + 5), &hi);

    lua_newtable(L);
    const char *handle = fixed_string_resolve(index);
    if (handle) {
        lua_pushstring(L, handle);
    } else {
        lua_pushstring(L, "");
    }
    lua_setfield(L, -2, "Handle");
    lua_pushinteger(L, (lua_Integer)lo | ((lua_Integer)hi << 8));
    lua_setfield(L, -2, "Version");
}

/*
 * TranslatedString is { RuntimeStringHandle Handle; RuntimeStringHandle ArgumentString; }
 * and Lua sees it that way -- nested, not flattened. Mods reach the string
 * through ts.Handle.Handle (VolitionCabinet's Loca helper does exactly that),
 * so collapsing it into a single Handle string breaks them one level in.
 */
static void push_translated_string(lua_State *L, const void *addr) {
    lua_newtable(L);

    push_runtime_string_handle(L, addr);
    lua_setfield(L, -2, "Handle");
    push_runtime_string_handle(L, (const uint8_t *)addr + 8);
    lua_setfield(L, -2, "ArgumentString");

    uint32_t index = 0;
    safe_memory_read_u32((mach_vm_address_t)addr, &index);
    const char *handle = fixed_string_resolve(index);
    lua_pushstring(L, handle ? handle : "");
    lua_pushcclosure(L, translated_string_get, 1);
    lua_setfield(L, -2, "Get");
}

static void ensure_array_metatable(lua_State *L);


/** glm vectors read as plain arrays of floats. */
static void push_vec(lua_State *L, const void *addr, int n) {
    lua_createtable(L, n, 0);
    for (int i = 0; i < n; i++) {
        uint32_t bits = 0;
        if (!safe_memory_read_u32((mach_vm_address_t)((const uint8_t *)addr + i * 4), &bits)) {
            lua_pop(L, 1);
            lua_pushnil(L);
            return;
        }
        float f;
        memcpy(&f, &bits, sizeof(f));
        lua_pushnumber(L, f);
        lua_rawseti(L, -2, i + 1);
    }
}

/** True if a std::optional at `addr` holds a value. */
static bool optional_engaged(const void *addr, size_t value_size) {
    uint8_t flag = 0;
    if (!safe_memory_read_u8((mach_vm_address_t)((const uint8_t *)addr
                                                 + OPTIONAL_FLAG_OFFSET(value_size)), &flag)) {
        return false;
    }
    return flag != 0;
}

/*
 * variant<NoValue, float, int, FixedString, bool> as the alternative it holds.
 *
 * The index is validated against the alternative count rather than trusted: an
 * out-of-range index means the layout is wrong, and returning nil is a great
 * deal better than reading four bytes as whichever type we guessed.
 */
static void push_variant_scalar(lua_State *L, const void *addr) {
    uint8_t which = 0;
    if (!safe_memory_read_u8((mach_vm_address_t)((const uint8_t *)addr
                                                 + VARIANT_SCALAR_INDEX_OFFSET), &which)) {
        lua_pushnil(L);
        return;
    }

    uint32_t bits = 0;
    if (!safe_memory_read_u32((mach_vm_address_t)addr, &bits)) {
        lua_pushnil(L);
        return;
    }

    switch (which) {
        case 0:                       // NoValue
            lua_pushnil(L);
            return;
        case 1: {                     // float
            float f;
            memcpy(&f, &bits, sizeof(f));
            lua_pushnumber(L, f);
            return;
        }
        case 2:                       // int
            lua_pushinteger(L, (int32_t)bits);
            return;
        case 3:                       // FixedString
            push_fixedstring(L, bits);
            return;
        case 4:                       // bool
            lua_pushboolean(L, (bits & 0xff) != 0);
            return;
        default:
            lua_pushnil(L);
            return;
    }
}

/** HashMap<uint8_t, FixedString> as { [key] = name }. */
static void push_hashmap_u8_fixedstring(lua_State *L, const void *addr) {
    const uint8_t *p = (const uint8_t *)addr;
    uint64_t keys = 0, values = 0;
    uint32_t kcount = 0, vcount = 0;

    if (!safe_memory_read_u64((mach_vm_address_t)(p + HASHSET_KEYS_OFFSET), &keys)
        || !safe_memory_read_u32((mach_vm_address_t)(p + HASHSET_KEYS_OFFSET + 12), &kcount)
        || !safe_memory_read_u64((mach_vm_address_t)(p + HASHMAP_VALUES_OFFSET), &values)
        || !safe_memory_read_u32((mach_vm_address_t)(p + HASHMAP_VALUES_OFFSET + 12), &vcount)
        || kcount != vcount) {
        lua_newtable(L);
        return;
    }

    lua_createtable(L, 0, (int)kcount);
    for (uint32_t i = 0; i < kcount; i++) {
        uint8_t k = 0;
        uint32_t v = 0;
        if (!safe_memory_read_u8((mach_vm_address_t)(uintptr_t)(keys + i), &k)) break;
        if (!safe_memory_read_u32((mach_vm_address_t)(uintptr_t)(values + (uint64_t)i * 4), &v)) break;
        const char *name = fixed_string_resolve(v);
        if (!name) continue;
        lua_pushstring(L, name);
        lua_rawseti(L, -2, k);
    }
}

static void push_array_proxy_ex(lua_State *L, void *array_addr, ResourceFieldKind kind,
                                bool readonly) {
    ResourceArrayUD *ud = (ResourceArrayUD *)lua_newuserdatauv(L, sizeof(ResourceArrayUD), 0);
    ud->array = array_addr;
    ud->kind = kind;
    ud->readonly = readonly;
    ud->elem = NULL;
    ud->elem_size = 0;
    ensure_array_metatable(L);
    lua_setmetatable(L, -2);
}

static void push_array_proxy(lua_State *L, void *array_addr, ResourceFieldKind kind) {
    push_array_proxy_ex(L, array_addr, kind, false);
}

static int push_field(lua_State *L, void *obj, const ResourceField *field) {
    void *addr = (uint8_t *)obj + field->offset;

    switch (field->kind) {
        case RF_BOOL: {
            uint8_t v = 0;
            if (!safe_memory_read_u8((mach_vm_address_t)addr, &v)) { lua_pushnil(L); return 1; }
            lua_pushboolean(L, v != 0);
            return 1;
        }
        case RF_U8: {
            uint8_t v = 0;
            if (!safe_memory_read_u8((mach_vm_address_t)addr, &v)) { lua_pushnil(L); return 1; }
            lua_pushinteger(L, v);
            return 1;
        }
        case RF_U16: {
            uint8_t lo = 0, hi = 0;
            if (!safe_memory_read_u8((mach_vm_address_t)addr, &lo)
                || !safe_memory_read_u8((mach_vm_address_t)addr + 1, &hi)) {
                lua_pushnil(L);
                return 1;
            }
            lua_pushinteger(L, (lua_Integer)lo | ((lua_Integer)hi << 8));
            return 1;
        }
        case RF_U32: {
            uint32_t v = 0;
            if (!safe_memory_read_u32((mach_vm_address_t)addr, &v)) { lua_pushnil(L); return 1; }
            lua_pushinteger(L, v);
            return 1;
        }
        case RF_I32: {
            uint32_t v = 0;
            if (!safe_memory_read_u32((mach_vm_address_t)addr, &v)) { lua_pushnil(L); return 1; }
            lua_pushinteger(L, (int32_t)v);
            return 1;
        }
        case RF_F32: {
            uint32_t bits = 0;
            if (!safe_memory_read_u32((mach_vm_address_t)addr, &bits)) { lua_pushnil(L); return 1; }
            float f;
            memcpy(&f, &bits, sizeof(f));
            lua_pushnumber(L, f);
            return 1;
        }
        case RF_F64: {
            uint64_t bits = 0;
            if (!safe_memory_read_u64((mach_vm_address_t)addr, &bits)) { lua_pushnil(L); return 1; }
            double d;
            memcpy(&d, &bits, sizeof(d));
            lua_pushnumber(L, d);
            return 1;
        }
        case RF_FIXEDSTRING: {
            uint32_t idx = 0;
            if (!safe_memory_read_u32((mach_vm_address_t)addr, &idx)) { lua_pushnil(L); return 1; }
            push_fixedstring(L, idx);
            return 1;
        }
        case RF_GUID:
            push_guid(L, addr);
            return 1;

        case RF_STDSTRING: {
            size_t len = 0;
            const char *s = read_stdstring(addr, &len);
            if (s) {
                lua_pushlstring(L, s, len);
            } else {
                lua_pushnil(L);
            }
            return 1;
        }
        case RF_TRANSLATEDSTRING:
            push_translated_string(L, addr);
            return 1;

        case RF_VEC3:
            push_vec(L, addr, 3);
            return 1;
        case RF_VEC4:
            push_vec(L, addr, 4);
            return 1;

        case RF_OPT_GUID:
            if (!optional_engaged(addr, 16)) { lua_pushnil(L); return 1; }
            push_guid(L, addr);
            return 1;
        case RF_OPT_STDSTRING: {
            if (!optional_engaged(addr, 16)) { lua_pushnil(L); return 1; }
            size_t len = 0;
            const char *str = read_stdstring(addr, &len);
            if (str) lua_pushlstring(L, str, len); else lua_pushnil(L);
            return 1;
        }
        case RF_OPT_I32: {
            if (!optional_engaged(addr, 4)) { lua_pushnil(L); return 1; }
            uint32_t v = 0;
            if (safe_memory_read_u32((mach_vm_address_t)addr, &v)) {
                lua_pushinteger(L, (int32_t)v);
            } else {
                lua_pushnil(L);
            }
            return 1;
        }
        case RF_OPT_U8: {
            if (!optional_engaged(addr, 1)) { lua_pushnil(L); return 1; }
            uint8_t v = 0;
            if (safe_memory_read_u8((mach_vm_address_t)addr, &v)) {
                lua_pushinteger(L, v);
            } else {
                lua_pushnil(L);
            }
            return 1;
        }

        case RF_VARIANT_SCALAR:
            push_variant_scalar(L, addr);
            return 1;

        case RF_HASHMAP_U8_FIXEDSTRING:
            push_hashmap_u8_fixedstring(L, addr);
            return 1;

        case RF_HASHSET_FIXEDSTRING:
            // The members live in the Keys array; the index arrays beside it
            // only serve lookup, so iterate Keys and leave them alone.
            push_array_proxy_ex(L, (uint8_t *)addr + HASHSET_KEYS_OFFSET,
                                RF_ARRAY_FIXEDSTRING, true);
            return 1;

        case RF_ARRAY_STRUCT: {
            push_array_proxy(L, addr, field->kind);
            ResourceArrayUD *ud = (ResourceArrayUD *)lua_touserdata(L, -1);
            ud->elem = field->elem;
            ud->elem_size = field->elem_size;
            return 1;
        }

        default:
            if (resource_field_is_array(field->kind)) {
                push_array_proxy(L, addr, field->kind);
                return 1;
            }
            lua_pushnil(L);
            return 1;
    }
}


// ============================================================================
// HashSet<FixedString>
// ============================================================================
//
// Rebuilding a set means rebuilding its bucket index too, so this mirrors
// CoreLib's HashSet::ResizeHashMap / InsertToHashMap rather than trying to
// patch the existing one in place. The bucket counts are the game's own prime
// table; the hash of a FixedString is the murmur value the string table
// already stores for it.

static const uint32_t MULTI_HASHMAP_PRIMES[] = {
    0x35, 0x61, 0xC1, 0x185, 0x301, 0x607,
    0xC07, 0x1807, 0x3001, 0x6011, 0xC005,
    0x1800D, 0x30005, 0x60019, 0xC0001, 0x180005,
    0x30000B, 0x60000D, 0xC00005, 0x1800013,
    0x3000005, 0x6000017, 0x0C000013, 0x18000005,
    0x30000059, 0x60000005, 0x400CCCCD
};

static uint32_t nearest_hashmap_prime(uint32_t n) {
    for (size_t i = 0; i < sizeof(MULTI_HASHMAP_PRIMES) / sizeof(MULTI_HASHMAP_PRIMES[0]); i++) {
        if (MULTI_HASHMAP_PRIMES[i] >= n) return MULTI_HASHMAP_PRIMES[i];
    }
    return MULTI_HASHMAP_PRIMES[sizeof(MULTI_HASHMAP_PRIMES) / sizeof(MULTI_HASHMAP_PRIMES[0]) - 1];
}

/**
 * Replace a HashSet<FixedString>'s contents with the strings in a Lua table.
 *
 * Both shapes mods use are accepted: an array of strings, and a set written as
 * { [name] = true }. Duplicates are dropped, since that is what a set means.
 *
 * The previous buffers are not freed, for the same reason the array growth
 * path does not free: we do not resolve the matching deallocator.
 */
static int rebuild_fixedstring_hashset(lua_State *L, void *addr, int value_index) {
    // Collect the members first, so a bad element aborts before anything is
    // written into the game's memory.
    uint32_t stack_ids[256];
    uint32_t *ids = stack_ids;
    uint32_t count = 0, capacity = (uint32_t)(sizeof(stack_ids) / sizeof(stack_ids[0]));
    uint32_t *heap_ids = NULL;

    #define HASHSET_FAIL(...) do { free(heap_ids); return luaL_error(L, __VA_ARGS__); } while (0)

    lua_pushnil(L);
    while (lua_next(L, value_index) != 0) {
        // Array part gives us the string as the value; set part as the key.
        // Check the type exactly: lua_isstring also accepts numbers, and
        // lua_tostring on a numeric key rewrites it in place, which would
        // derail lua_next.
        const char *name = NULL;
        if (lua_type(L, -1) == LUA_TSTRING) {
            name = lua_tostring(L, -1);
        } else if (lua_type(L, -2) == LUA_TSTRING) {
            name = lua_tostring(L, -2);
        }
        if (!name) {
            lua_pop(L, 2);
            HASHSET_FAIL("a hash set takes strings, either as an array or as "
                         "{ [name] = true }");
        }

        uint32_t id = fixed_string_intern(name, (int)strlen(name));
        if (id == 0xffffffffu) {
            lua_pop(L, 2);
            HASHSET_FAIL("could not intern '%s' into the string table", name);
        }

        bool duplicate = false;
        for (uint32_t i = 0; i < count; i++) {
            if (ids[i] == id) { duplicate = true; break; }
        }
        if (!duplicate) {
            if (count == capacity) {
                uint32_t new_capacity = capacity * 2;
                uint32_t *grown = (uint32_t *)realloc(heap_ids, new_capacity * sizeof(uint32_t));
                if (!grown) {
                    lua_pop(L, 2);
                    HASHSET_FAIL("out of memory building the hash set");
                }
                if (!heap_ids) memcpy(grown, stack_ids, count * sizeof(uint32_t));
                heap_ids = grown;
                ids = grown;
                capacity = new_capacity;
            }
            ids[count++] = id;
        }
        lua_pop(L, 1);
    }

    uint32_t buckets = nearest_hashmap_prime(count ? count : 1);

    int32_t *hash_keys = (int32_t *)resource_game_alloc((size_t)buckets * sizeof(int32_t));
    int32_t *next_ids = count ? (int32_t *)resource_game_alloc((size_t)count * sizeof(int32_t)) : NULL;
    uint32_t *keys = count ? (uint32_t *)resource_game_alloc((size_t)count * sizeof(uint32_t)) : NULL;
    if (!hash_keys || (count && (!next_ids || !keys))) {
        HASHSET_FAIL("could not allocate the hash set");
    }

    for (uint32_t i = 0; i < buckets; i++) hash_keys[i] = -1;

    for (uint32_t k = 0; k < count; k++) {
        keys[k] = ids[k];

        uint32_t hash = fixed_string_get_hash(ids[k]);
        uint32_t bucket = hash % buckets;
        int32_t previous = hash_keys[bucket];
        if (previous < 0) {
            // An empty bucket stores an encoded terminator rather than -1, so
            // a walk can tell which bucket it ended in.
            previous = -2 - (int32_t)bucket;
        }
        next_ids[k] = previous;
        hash_keys[bucket] = (int32_t)k;
    }

    uint8_t *p = (uint8_t *)addr;
    *(void **)(p + HASHSET_HASHKEYS_OFFSET) = hash_keys;
    *(uint32_t *)(p + HASHSET_HASHSIZE_OFFSET) = buckets;
    *(void **)(p + HASHSET_NEXTIDS_OFFSET) = next_ids;
    *(uint32_t *)(p + HASHSET_NEXTIDS_OFFSET + 8) = count;
    *(uint32_t *)(p + HASHSET_NEXTIDS_OFFSET + 12) = count;
    *(void **)(p + HASHSET_KEYS_OFFSET) = keys;
    *(uint32_t *)(p + HASHSET_KEYS_OFFSET + 8) = count;
    *(uint32_t *)(p + HASHSET_KEYS_OFFSET + 12) = count;

    free(heap_ids);
    #undef HASHSET_FAIL
    return 0;
}



/*
 * Copy one struct element, giving the destination its own strings.
 *
 * A plain byte copy would leave both resources pointing at the same STDString
 * buffer, so whichever is torn down first takes the other's string with it.
 * The bytes are copied first -- that carries the fields the layout does not
 * model -- and then every string field is rewritten, which allocates afresh for
 * anything too long to sit inline.
 */
static void copy_struct_element(void *dst, const void *src, const ResourceLayout *layout,
                                size_t size) {
    memcpy(dst, src, size);
    if (!layout) return;

    for (int i = 0; i < layout->field_count; i++) {
        const ResourceField *f = &layout->fields[i];
        if (f->kind != RF_STDSTRING) continue;

        size_t len = 0;
        const char *str = read_stdstring((const uint8_t *)src + f->offset, &len);
        if (!str) continue;
        write_stdstring((uint8_t *)dst + f->offset, str, len);
    }
}

static int write_field(lua_State *L, void *obj, const ResourceField *field, int value_index);
static int write_element(lua_State *L, void *elem, ResourceFieldKind kind, int value_index);
static bool array_read_header(const ResourceArrayUD *ud, void **out_buf,
                              uint32_t *out_capacity, uint32_t *out_size);

/*
 * Replace a whole Array<T> from a Lua table.
 *
 * Refusing this and telling callers to assign elements one at a time was wrong:
 * building a new list and assigning it is how mods actually work with these
 * fields. DoubleSubclass merges subclass progressions by composing a fresh
 * table per field and writing it back, and failed on every one.
 *
 * The elements are built into a fresh buffer before anything is published, so a
 * bad entry leaves the field untouched rather than half-rewritten. The old
 * buffer is not freed, for the same reason growth does not free it: we do not
 * resolve the game's matching deallocator.
 */

/*
 * Write one struct element in place, from either a table or a proxy.
 *
 * Shared by whole-array assignment and by arr[i] = element, which merging uses
 * to replace a single entry.
 */
static int write_struct_element(lua_State *L, void *slot, const ResourceLayout *layout,
                                size_t esize, int value_index) {
    if (!layout) {
        return luaL_error(L, "this array's elements have no known layout");
    }

    ResourceObjectUD *src = (ResourceObjectUD *)luaL_testudata(L, value_index,
                                                               RESOURCE_OBJECT_MT);
    if (src) {
        if (src->layout != layout
            && (src->layout->size != layout->size
                || strcmp(src->layout->type_name, layout->type_name) != 0)) {
            return luaL_error(L, "expected a %s, got a %s",
                              layout->type_name, src->layout->type_name);
        }
        copy_struct_element(slot, src->obj, layout, esize);
        return 0;
    }

    if (!lua_istable(L, value_index)) {
        return luaL_error(L, "expected a table or a %s", layout->type_name);
    }

    memset(slot, 0, esize);
    for (int f = 0; f < layout->field_count; f++) {
        const ResourceField *sub = &layout->fields[f];
        lua_getfield(L, value_index, sub->name);
        if (!lua_isnil(L, -1)) {
            write_field(L, slot, sub, lua_gettop(L));
        }
        lua_pop(L, 1);
    }
    return 0;
}

static int write_array_from_table(lua_State *L, void *addr, const ResourceField *field,
                                  int value_index) {
    size_t esize = (field->kind == RF_ARRAY_STRUCT)
                 ? field->elem_size
                 : array_element_size(field->kind);

    // Assigning one resource's array straight onto another is how merging is
    // written -- target.SelectSpells = source.SelectSpells -- so the source is
    // usually one of our own array proxies rather than a table.
    ResourceArrayUD *src = (ResourceArrayUD *)luaL_testudata(L, value_index, RESOURCE_ARRAY_MT);
    if (src) {
        if (src->kind != field->kind || array_element_size_of(src) != esize) {
            return luaL_error(L, "field '%s' cannot take this array: element types differ",
                              field->name);
        }

        void *sbuf = NULL;
        uint32_t scap = 0, ssize = 0;
        if (!array_read_header(src, &sbuf, &scap, &ssize)) {
            return luaL_error(L, "the array assigned to '%s' is not readable", field->name);
        }

        void *fresh = NULL;
        if (ssize > 0) {
            fresh = resource_game_alloc((size_t)ssize * esize);
            if (!fresh) return luaL_error(L, "could not allocate for field '%s'", field->name);
            memset(fresh, 0, (size_t)ssize * esize);
            for (uint32_t i = 0; i < ssize; i++) {
                void *d = (uint8_t *)fresh + (size_t)i * esize;
                const void *o = (const uint8_t *)sbuf + (size_t)i * esize;
                if (field->kind == RF_ARRAY_STRUCT) {
                    copy_struct_element(d, o, field->elem, esize);
                } else {
                    memcpy(d, o, esize);
                }
            }
        }

        uint8_t *p = (uint8_t *)addr;
        *(void **)(p + ARRAY_BUF_OFFSET) = fresh;
        *(uint32_t *)(p + ARRAY_CAPACITY_OFFSET) = ssize;
        *(uint32_t *)(p + ARRAY_SIZE_OFFSET) = ssize;
        return 0;
    }

    luaL_checktype(L, value_index, LUA_TTABLE);

    lua_Integer count = (lua_Integer)lua_rawlen(L, value_index);
    if (count < 0) count = 0;

    if (esize == 0) {
        return luaL_error(L, "field '%s' holds elements we cannot write", field->name);
    }
    if (field->kind == RF_ARRAY_STRUCT && !field->elem) {
        return luaL_error(L, "field '%s' holds elements with no known layout", field->name);
    }

    void *buf = NULL;
    if (count > 0) {
        buf = resource_game_alloc((size_t)count * esize);
        if (!buf) return luaL_error(L, "could not allocate for field '%s'", field->name);
        memset(buf, 0, (size_t)count * esize);
    }

    for (lua_Integer i = 0; i < count; i++) {
        void *slot = (uint8_t *)buf + (size_t)i * esize;
        lua_rawgeti(L, value_index, i + 1);
        int elem_index = lua_gettop(L);

        if (field->kind == RF_ARRAY_STRUCT) {
            int rc = write_struct_element(L, slot, field->elem, esize, elem_index);
            if (rc != 0) {
                lua_pop(L, 1);
                return rc;
            }
        } else {
            write_element(L, slot, field->kind, elem_index);
        }
        lua_pop(L, 1);
    }

    // Publish only once every element is in place.
    uint8_t *p = (uint8_t *)addr;
    *(void **)(p + ARRAY_BUF_OFFSET) = buf;
    *(uint32_t *)(p + ARRAY_CAPACITY_OFFSET) = (uint32_t)count;
    *(uint32_t *)(p + ARRAY_SIZE_OFFSET) = (uint32_t)count;
    return 0;
}

static int write_field(lua_State *L, void *obj, const ResourceField *field, int value_index) {
    void *addr = (uint8_t *)obj + field->offset;

    switch (field->kind) {
        case RF_BOOL:
            *(uint8_t *)addr = (uint8_t)(lua_toboolean(L, value_index) ? 1 : 0);
            return 0;
        case RF_U8:
            *(uint8_t *)addr = (uint8_t)luaL_checkinteger(L, value_index);
            return 0;
        case RF_U16:
            *(uint16_t *)addr = (uint16_t)luaL_checkinteger(L, value_index);
            return 0;
        case RF_U32:
            *(uint32_t *)addr = (uint32_t)luaL_checkinteger(L, value_index);
            return 0;
        case RF_I32:
            *(int32_t *)addr = (int32_t)luaL_checkinteger(L, value_index);
            return 0;
        case RF_F32:
            *(float *)addr = (float)luaL_checknumber(L, value_index);
            return 0;
        case RF_F64:
            *(double *)addr = (double)luaL_checknumber(L, value_index);
            return 0;

        case RF_FIXEDSTRING: {
            const char *s = luaL_checkstring(L, value_index);
            uint32_t idx = fixed_string_intern(s, (int)strlen(s));
            if (idx == 0xffffffffu) {
                return luaL_error(L, "could not intern '%s' into the string table", s);
            }
            *(uint32_t *)addr = idx;
            return 0;
        }

        case RF_STDSTRING: {
            size_t len = 0;
            const char *s = luaL_checklstring(L, value_index, &len);
            if (!write_stdstring(addr, s, len)) {
                return luaL_error(L, "could not write field '%s'", field->name);
            }
            return 0;
        }

        case RF_GUID: {
            const char *s = luaL_checkstring(L, value_index);
            uint8_t raw[16];
            if (!staticdata_registry_parse_guid(s, raw)) {
                return luaL_error(L, "'%s' is not a GUID", s);
            }
            memcpy(addr, raw, sizeof(raw));
            return 0;
        }

        case RF_TRANSLATEDSTRING: {
            // Accept either a handle string or a { Handle = ..., Version = ... }
            // table, matching what a read hands back.
            const char *handle = NULL;
            lua_Integer version = 1;
            int popped = 0;
            if (lua_istable(L, value_index)) {
                // Either a whole TranslatedString ({ Handle = { Handle = ... } })
                // or a RuntimeStringHandle ({ Handle = "..." }).
                lua_getfield(L, value_index, "Handle");
                popped = 1;
                if (lua_istable(L, -1)) {
                    lua_getfield(L, -1, "Version");
                    if (lua_isinteger(L, -1)) version = lua_tointeger(L, -1);
                    lua_pop(L, 1);
                    lua_getfield(L, -1, "Handle");
                    popped = 2;
                }
                handle = lua_tostring(L, -1);
                if (version == 1) {
                    lua_getfield(L, value_index, "Version");
                    if (lua_isinteger(L, -1)) version = lua_tointeger(L, -1);
                    lua_pop(L, 1);
                }
            } else {
                handle = luaL_checkstring(L, value_index);
            }
            if (!handle) {
                return luaL_error(L, "field '%s' needs a handle string or a table "
                                     "with a Handle field", field->name);
            }
            uint32_t idx = handle ? fixed_string_intern(handle, (int)strlen(handle))
                                  : 0xffffffffu;
            lua_pop(L, popped);
            if (idx == 0xffffffffu) {
                return luaL_error(L, "could not intern handle for field '%s'", field->name);
            }
            *(uint32_t *)addr = idx;
            *(uint16_t *)((uint8_t *)addr + 4) = (uint16_t)version;
            return 0;
        }

        case RF_VEC3:
            push_vec(L, addr, 3);
            return 1;
        case RF_VEC4:
            push_vec(L, addr, 4);
            return 1;

        case RF_OPT_GUID:
            if (!optional_engaged(addr, 16)) { lua_pushnil(L); return 1; }
            push_guid(L, addr);
            return 1;
        case RF_OPT_STDSTRING: {
            if (!optional_engaged(addr, 16)) { lua_pushnil(L); return 1; }
            size_t len = 0;
            const char *str = read_stdstring(addr, &len);
            if (str) lua_pushlstring(L, str, len); else lua_pushnil(L);
            return 1;
        }
        case RF_OPT_I32: {
            if (!optional_engaged(addr, 4)) { lua_pushnil(L); return 1; }
            uint32_t v = 0;
            if (safe_memory_read_u32((mach_vm_address_t)addr, &v)) {
                lua_pushinteger(L, (int32_t)v);
            } else {
                lua_pushnil(L);
            }
            return 1;
        }
        case RF_OPT_U8: {
            if (!optional_engaged(addr, 1)) { lua_pushnil(L); return 1; }
            uint8_t v = 0;
            if (safe_memory_read_u8((mach_vm_address_t)addr, &v)) {
                lua_pushinteger(L, v);
            } else {
                lua_pushnil(L);
            }
            return 1;
        }

        case RF_VARIANT_SCALAR:
            push_variant_scalar(L, addr);
            return 1;

        case RF_HASHMAP_U8_FIXEDSTRING:
            push_hashmap_u8_fixedstring(L, addr);
            return 1;

        case RF_HASHSET_FIXEDSTRING: {
            luaL_checktype(L, value_index, LUA_TTABLE);
            return rebuild_fixedstring_hashset(L, addr, value_index);
        }

        default:
            if (resource_field_is_array(field->kind)) {
                return write_array_from_table(L, addr, field, value_index);
            }
            return luaL_error(L, "field '%s' is not writable", field->name);
    }
}

// ============================================================================
// Object metatable
// ============================================================================

static ResourceObjectUD *check_object(lua_State *L, int index) {
    return (ResourceObjectUD *)luaL_checkudata(L, index, RESOURCE_OBJECT_MT);
}

static int resource_object_index(lua_State *L) {
    ResourceObjectUD *ud = check_object(L, 1);
    const char *key = luaL_checkstring(L, 2);

    if (!ud->layout->embedded
        && (strcmp(key, "ResourceUUID") == 0 || strcmp(key, "UUID") == 0)) {
        push_guid(L, (uint8_t *)ud->obj + RESOURCE_UUID_OFFSET);
        return 1;
    }
    if (strcmp(key, "Type") == 0) {
        lua_pushstring(L, ud->layout->type_name);
        return 1;
    }
    if (strcmp(key, "_ptr") == 0) {
        lua_pushlightuserdata(L, ud->obj);
        return 1;
    }

    const ResourceField *field = resource_layout_field(ud->layout, key);
    if (!field) {
        lua_pushnil(L);
        return 1;
    }
    return push_field(L, ud->obj, field);
}

static int resource_object_newindex(lua_State *L) {
    ResourceObjectUD *ud = check_object(L, 1);
    const char *key = luaL_checkstring(L, 2);

    const ResourceField *field = resource_layout_field(ud->layout, key);
    if (!field) {
        return luaL_error(L, "%s has no field '%s'", ud->layout->type_name, key);
    }
    write_field(L, ud->obj, field, 3);
    return 0;
}

/** Iterate the modelled fields, so Debug.Dump and pairs() see the object. */
static int resource_object_next(lua_State *L) {
    ResourceObjectUD *ud = check_object(L, 1);
    int i = (int)lua_tointeger(L, lua_upvalueindex(1));

    if (i >= ud->layout->field_count) {
        lua_pushnil(L);
        return 1;
    }
    lua_pushinteger(L, i + 1);
    lua_replace(L, lua_upvalueindex(1));

    lua_pushstring(L, ud->layout->fields[i].name);
    push_field(L, ud->obj, &ud->layout->fields[i]);
    return 2;
}

static int resource_object_pairs(lua_State *L) {
    check_object(L, 1);
    lua_pushinteger(L, 0);
    lua_pushcclosure(L, resource_object_next, 1);
    lua_pushvalue(L, 1);
    lua_pushnil(L);
    return 3;
}

static int resource_object_tostring(lua_State *L) {
    ResourceObjectUD *ud = check_object(L, 1);
    if (ud->layout->embedded) {
        lua_pushfstring(L, "%s", ud->layout->type_name);
        return 1;
    }
    char guid[40] = "?";
    staticdata_registry_format_key((uint8_t *)ud->obj + RESOURCE_UUID_OFFSET, 0,
                                   guid, sizeof(guid));
    lua_pushfstring(L, "%s(%s)", ud->layout->type_name, guid);
    return 1;
}

// ============================================================================
// Array metatable
// ============================================================================

static ResourceArrayUD *check_array(lua_State *L, int index) {
    return (ResourceArrayUD *)luaL_checkudata(L, index, RESOURCE_ARRAY_MT);
}

static bool array_read_header(const ResourceArrayUD *ud, void **out_buf,
                              uint32_t *out_capacity, uint32_t *out_size) {
    const uint8_t *p = (const uint8_t *)ud->array;
    uint64_t buf = 0;
    if (!safe_memory_read_u64((mach_vm_address_t)(p + ARRAY_BUF_OFFSET), &buf)) return false;
    if (!safe_memory_read_u32((mach_vm_address_t)(p + ARRAY_CAPACITY_OFFSET), out_capacity)) return false;
    if (!safe_memory_read_u32((mach_vm_address_t)(p + ARRAY_SIZE_OFFSET), out_size)) return false;
    if (*out_size > *out_capacity) return false;   // not a live Array
    *out_buf = (void *)(uintptr_t)buf;
    return true;
}

static int resource_array_len(lua_State *L) {
    ResourceArrayUD *ud = check_array(L, 1);
    void *buf = NULL;
    uint32_t cap = 0, size = 0;
    if (!array_read_header(ud, &buf, &cap, &size)) {
        lua_pushinteger(L, 0);
        return 1;
    }
    lua_pushinteger(L, size);
    return 1;
}

static void push_object_proxy(lua_State *L, void *obj, const ResourceLayout *layout);
static void push_element(lua_State *L, const void *elem, ResourceFieldKind kind);

/*
 * One array element. Struct elements get the same field proxy a resource does,
 * pointing straight at the element inside the array -- so writing a field on
 * one writes through to the array, with nothing copied.
 */
static void push_element_at(lua_State *L, const ResourceArrayUD *ud, void *elem) {
    if (ud->kind == RF_ARRAY_STRUCT) {
        if (ud->elem) {
            push_object_proxy(L, elem, ud->elem);
        } else {
            lua_pushnil(L);
        }
        return;
    }
    push_element(L, elem, ud->kind);
}

static void push_element(lua_State *L, const void *elem, ResourceFieldKind kind) {
    switch (kind) {
        case RF_ARRAY_GUID:
            push_guid(L, elem);
            break;
        case RF_ARRAY_FIXEDSTRING: {
            uint32_t idx = 0;
            if (safe_memory_read_u32((mach_vm_address_t)elem, &idx)) {
                push_fixedstring(L, idx);
            } else {
                lua_pushnil(L);
            }
            break;
        }
        case RF_ARRAY_I32: {
            uint32_t v = 0;
            if (safe_memory_read_u32((mach_vm_address_t)elem, &v)) {
                lua_pushinteger(L, (int32_t)v);
            } else {
                lua_pushnil(L);
            }
            break;
        }
        case RF_ARRAY_U8: {
            uint8_t v = 0;
            if (safe_memory_read_u8((mach_vm_address_t)elem, &v)) {
                lua_pushinteger(L, v);
            } else {
                lua_pushnil(L);
            }
            break;
        }
        default:
            lua_pushnil(L);
            break;
    }
}

static int write_element(lua_State *L, void *elem, ResourceFieldKind kind, int value_index) {
    switch (kind) {
        case RF_ARRAY_GUID: {
            const char *s = luaL_checkstring(L, value_index);
            uint8_t raw[16];
            if (!staticdata_registry_parse_guid(s, raw)) {
                return luaL_error(L, "'%s' is not a GUID", s);
            }
            memcpy(elem, raw, sizeof(raw));
            return 0;
        }
        case RF_ARRAY_FIXEDSTRING: {
            const char *s = luaL_checkstring(L, value_index);
            uint32_t idx = fixed_string_intern(s, (int)strlen(s));
            if (idx == 0xffffffffu) {
                return luaL_error(L, "could not intern '%s'", s);
            }
            *(uint32_t *)elem = idx;
            return 0;
        }
        case RF_ARRAY_I32:
            *(int32_t *)elem = (int32_t)luaL_checkinteger(L, value_index);
            return 0;
        case RF_ARRAY_U8:
            *(uint8_t *)elem = (uint8_t)luaL_checkinteger(L, value_index);
            return 0;
        case RF_ARRAY_STRUCT:
            return luaL_error(L, "assign to the fields of this element rather "
                                 "than replacing it");
        default:
            return luaL_error(L, "this array's elements are not writable");
    }
}

static int resource_array_index(lua_State *L) {
    ResourceArrayUD *ud = check_array(L, 1);
    lua_Integer i = luaL_checkinteger(L, 2);

    void *buf = NULL;
    uint32_t cap = 0, size = 0;
    if (!array_read_header(ud, &buf, &cap, &size) || i < 1 || i > (lua_Integer)size) {
        lua_pushnil(L);
        return 1;
    }
    size_t esize = array_element_size_of(ud);
    push_element_at(L, ud, (uint8_t *)buf + (size_t)(i - 1) * esize);
    return 1;
}

/**
 * Grow the array to hold at least `needed` elements.
 *
 * The old buffer is not freed: the game's matching deallocator is not one of
 * the functions we resolve, and growth doubles, so a resource that is appended
 * to N times leaks O(N) elements once rather than per append.
 */
static bool array_reserve(ResourceArrayUD *ud, uint32_t needed) {
    void *buf = NULL;
    uint32_t cap = 0, size = 0;
    if (!array_read_header(ud, &buf, &cap, &size)) return false;
    if (needed <= cap) return true;

    uint32_t new_cap = cap ? cap * 2 : 4;
    while (new_cap < needed) new_cap *= 2;

    size_t esize = array_element_size_of(ud);
    void *fresh = resource_game_alloc((size_t)new_cap * esize);
    if (!fresh) return false;

    memset(fresh, 0, (size_t)new_cap * esize);
    if (buf && size) {
        memcpy(fresh, buf, (size_t)size * esize);
    }

    uint8_t *p = (uint8_t *)ud->array;
    *(void **)(p + ARRAY_BUF_OFFSET) = fresh;
    *(uint32_t *)(p + ARRAY_CAPACITY_OFFSET) = new_cap;
    return true;
}

static int resource_array_newindex(lua_State *L) {
    ResourceArrayUD *ud = check_array(L, 1);
    lua_Integer i = luaL_checkinteger(L, 2);

    if (ud->readonly) {
        return luaL_error(L, "this is the element array of a hash set; assign a "
                             "whole table to the field instead of changing it "
                             "in place");
    }

    void *buf = NULL;
    uint32_t cap = 0, size = 0;
    if (!array_read_header(ud, &buf, &cap, &size)) {
        return luaL_error(L, "this array is not readable");
    }
    if (i < 1) {
        return luaL_error(L, "array index %d is out of range", (int)i);
    }

    uint8_t *p = (uint8_t *)ud->array;
    size_t esize = array_element_size_of(ud);

    // arr[#arr] = nil is how Lua code pops the last element.
    if (lua_isnil(L, 3)) {
        if (i != (lua_Integer)size) {
            return luaL_error(L, "only the last element can be removed "
                                 "(tried %d of %d)", (int)i, (int)size);
        }
        *(uint32_t *)(p + ARRAY_SIZE_OFFSET) = size - 1;
        return 0;
    }

    if (i > (lua_Integer)size + 1) {
        return luaL_error(L, "array index %d leaves a hole past the end (%d)",
                          (int)i, (int)size);
    }

    if (i == (lua_Integer)size + 1) {
        if (!array_reserve(ud, size + 1)) {
            return luaL_error(L, "could not grow the array");
        }
        if (!array_read_header(ud, &buf, &cap, &size)) {
            return luaL_error(L, "array became unreadable while growing");
        }
        void *slot = (uint8_t *)buf + (size_t)size * esize;
        int rc = (ud->kind == RF_ARRAY_STRUCT)
               ? write_struct_element(L, slot, ud->elem, esize, 3)
               : write_element(L, slot, ud->kind, 3);
        if (rc == 0) {
            *(uint32_t *)(p + ARRAY_SIZE_OFFSET) = size + 1;
        }
        return 0;
    }

    void *slot = (uint8_t *)buf + (size_t)(i - 1) * esize;
    if (ud->kind == RF_ARRAY_STRUCT) {
        return write_struct_element(L, slot, ud->elem, esize, 3);
    }
    return write_element(L, slot, ud->kind, 3);
}

static int resource_array_next(lua_State *L) {
    ResourceArrayUD *ud = check_array(L, 1);
    lua_Integer i = lua_tointeger(L, 2) + 1;

    void *buf = NULL;
    uint32_t cap = 0, size = 0;
    if (!array_read_header(ud, &buf, &cap, &size) || i > (lua_Integer)size) {
        lua_pushnil(L);
        return 1;
    }
    lua_pushinteger(L, i);
    push_element_at(L, ud, (uint8_t *)buf + (size_t)(i - 1) * array_element_size_of(ud));
    return 2;
}

static int resource_array_pairs(lua_State *L) {
    check_array(L, 1);
    lua_pushcfunction(L, resource_array_next);
    lua_pushvalue(L, 1);
    lua_pushinteger(L, 0);
    return 3;
}

static int resource_array_tostring(lua_State *L) {
    ResourceArrayUD *ud = check_array(L, 1);
    void *buf = NULL;
    uint32_t cap = 0, size = 0;
    if (!array_read_header(ud, &buf, &cap, &size)) {
        lua_pushstring(L, "Array(unreadable)");
        return 1;
    }
    lua_pushfstring(L, "Array(%d)", (int)size);
    return 1;
}

// ============================================================================
// Registration
// ============================================================================

/** Build the metatable on first use, as the other proxies here do. */
static void ensure_metatable(lua_State *L, const char *name, const luaL_Reg *methods) {
    if (luaL_newmetatable(L, name)) {
        for (const luaL_Reg *m = methods; m->name; m++) {
            lua_pushcfunction(L, m->func);
            lua_setfield(L, -2, m->name);
        }
        // Keep the metatable itself out of reach of mod code.
        lua_pushboolean(L, 0);
        lua_setfield(L, -2, "__metatable");
    }
}

static void ensure_object_metatable(lua_State *L) {
    static const luaL_Reg methods[] = {
        { "__index",    resource_object_index },
        { "__newindex", resource_object_newindex },
        { "__pairs",    resource_object_pairs },
        { "__tostring", resource_object_tostring },
        { NULL, NULL }
    };
    ensure_metatable(L, RESOURCE_OBJECT_MT, methods);
}

static void ensure_array_metatable(lua_State *L) {
    static const luaL_Reg methods[] = {
        { "__index",    resource_array_index },
        { "__newindex", resource_array_newindex },
        { "__len",      resource_array_len },
        { "__pairs",    resource_array_pairs },
        { "__tostring", resource_array_tostring },
        { NULL, NULL }
    };
    ensure_metatable(L, RESOURCE_ARRAY_MT, methods);
}


// ============================================================================
// Layout self-check
// ============================================================================
//
// The field offsets are computed, not observed, so the first time a proxy is
// built we sanity-check one live object of that type and log the verdict. A
// mis-sized field shifts everything after it, and the shift is obvious in the
// data: strings stop being text, arrays claim more elements than they have room
// for. This is what turns "the generator says 0x48" into something checked.

static bool looks_like_text(const char *s, size_t len) {
    if (len == 0) return true;
    if (len > 512) return false;
    for (size_t i = 0; i < len; i++) {
        unsigned char c = (unsigned char)s[i];
        if (c < 0x09 || (c > 0x0d && c < 0x20) || c == 0x7f) return false;
    }
    return true;
}

/**
 * Returns the number of fields that do not look like their declared type.
 *
 * Every failing field is named, not just the first: a computed layout goes
 * wrong at one field and stays wrong, so the whole list shows where the drift
 * starts and whether it is one field or all of them.
 */
static int check_layout(void *obj, const ResourceLayout *layout,
                        char *note, size_t note_size) {
    int bad = 0;
    size_t used = 0;
    note[0] = '\0';

    for (int i = 0; i < layout->field_count; i++) {
        const ResourceField *f = &layout->fields[i];
        void *addr = (uint8_t *)obj + f->offset;
        const char *why = NULL;

        if (f->kind == RF_STDSTRING) {
            size_t len = 0;
            const char *str = read_stdstring(addr, &len);
            if (!str) {
                why = "unreadable string";
            } else if (!looks_like_text(str, len)) {
                why = "string is not text";
            } else if (len == 0) {
                // An empty string passes any text test, so a wrong string size
                // reads as "fine" here while every later field is shifted. Note
                // it so the verdict is not quietly built on nothing: if the
                // bytes just past it are printable, we are probably reading the
                // wrong end of a string that does have content.
                const uint8_t *p = (const uint8_t *)addr;
                uint8_t first = 0;
                if (safe_memory_read_u8((mach_vm_address_t)p, &first)
                    && first >= 0x20 && first < 0x7f) {
                    why = "string reads as empty but starts with text";
                }
            }
        } else if (f->kind == RF_ARRAY_STRUCT && !f->elem) {
            // Sized so later fields are right, but nothing to validate.
            continue;
        } else if (resource_field_is_array(f->kind)) {
            uint64_t buf = 0;
            uint32_t cap = 0, size = 0;
            if (!safe_memory_read_u64((mach_vm_address_t)addr, &buf)
                || !safe_memory_read_u32((mach_vm_address_t)((uint8_t *)addr + 8), &cap)
                || !safe_memory_read_u32((mach_vm_address_t)((uint8_t *)addr + 12), &size)) {
                why = "unreadable array";
            } else if (size > cap) {
                why = "array size exceeds capacity";
            } else if (size > 0 && buf == 0) {
                why = "array has elements but no buffer";
            }
        } else if (f->kind == RF_HASHSET_FIXEDSTRING) {
            uint64_t hash_keys = 0, keys_buf = 0;
            uint32_t buckets = 0, cap = 0, size = 0;
            if (!safe_memory_read_u64((mach_vm_address_t)((uint8_t *)addr + HASHSET_HASHKEYS_OFFSET), &hash_keys)
                || !safe_memory_read_u32((mach_vm_address_t)((uint8_t *)addr + HASHSET_HASHSIZE_OFFSET), &buckets)
                || !safe_memory_read_u64((mach_vm_address_t)((uint8_t *)addr + HASHSET_KEYS_OFFSET), &keys_buf)
                || !safe_memory_read_u32((mach_vm_address_t)((uint8_t *)addr + HASHSET_KEYS_OFFSET + 8), &cap)
                || !safe_memory_read_u32((mach_vm_address_t)((uint8_t *)addr + HASHSET_KEYS_OFFSET + 12), &size)) {
                why = "unreadable hash set";
            } else if (size > cap) {
                why = "hash set size exceeds capacity";
            } else if (size > 0 && (keys_buf == 0 || buckets == 0 || hash_keys == 0)) {
                why = "hash set has members but no index";
            }
        } else if (f->kind == RF_FIXEDSTRING) {
            uint32_t idx = 0;
            if (!safe_memory_read_u32((mach_vm_address_t)addr, &idx)) {
                why = "unreadable string handle";
            } else if (idx != 0xffffffffu && !fixed_string_is_valid(idx)) {
                why = "string handle is not in the table";
            }
        }

        if (why) {
            bad++;
            if (used + 1 < note_size) {
                int n = snprintf(note + used, note_size - used, "%s%s at +0x%02x: %s",
                                 used ? "; " : "", f->name, f->offset, why);
                if (n > 0) used += (size_t)n;
                if (used >= note_size) used = note_size - 1;
            }
        }
    }
    return bad;
}

/**
 * Check a type's layout once, against the first resource in its bank.
 * Logs a single line either way so a bad layout is visible in the log rather
 * than showing up later as nonsense field values.
 */
static void selftest_layout_once(const ResourceLayout *layout) {
    static const char *checked[64];
    static int checked_count = 0;

    for (int i = 0; i < checked_count; i++) {
        if (checked[i] == layout->type_name) return;
    }
    if (checked_count < (int)(sizeof(checked) / sizeof(checked[0]))) {
        checked[checked_count++] = layout->type_name;
    }

    const StaticDataTypeEntry *entry = staticdata_registry_find(layout->type_name);
    if (!entry) return;

    void *keys = NULL;
    uint32_t count = 0;
    if (!staticdata_registry_get_keys(entry, &keys, &count) || count == 0) return;

    char guid[40];
    if (!staticdata_registry_format_key(keys, 0, guid, sizeof(guid))) return;
    void *sample = staticdata_registry_get_object_by_guid_string(entry, guid);
    if (!sample) return;

    char note[512];
    int bad = check_layout(sample, layout, note, sizeof(note));
    if (bad == 0) {
        LOG_CORE_INFO("static data layout for %s checks out (%d fields, size 0x%x)",
                      layout->type_name, layout->field_count, layout->size);
    } else {
        // 256 bytes with ASCII: the field boundaries are legible directly,
        // and inline strings show both their text and the byte holding their
        // length -- which is what pins the string size.
        char dump[4200];
        size_t at = 0;
        for (int row = 0; row < 16; row++) {
            int base = row * 16;
            char hex[16 * 3 + 1];
            char asc[17];
            size_t hx = 0;
            int got = 0;
            for (int i = 0; i < 16; i++) {
                uint8_t b = 0;
                if (!safe_memory_read_u8((mach_vm_address_t)((uint8_t *)sample + base + i), &b)) break;
                hx += (size_t)snprintf(hex + hx, sizeof(hex) - hx, "%02x ", b);
                asc[i] = (b >= 0x20 && b < 0x7f) ? (char)b : '.';
                got++;
            }
            if (got == 0) break;
            asc[got] = '\0';
            hex[hx] = '\0';
            if (at + hx + 32 >= sizeof(dump)) break;
            at += (size_t)snprintf(dump + at, sizeof(dump) - at,
                                   "\n    +0x%02x  %-48s |%s|", base, hex, asc);
        }
        dump[at] = '\0';

        LOG_CORE_WARN("static data layout for %s looks wrong: %d of %d fields "
                      "failed their type check (%s). Field values for this type "
                      "may be nonsense. Sample at %p:%s",
                      layout->type_name, bad, layout->field_count, note,
                      sample, dump);
    }
}

static void push_object_proxy(lua_State *L, void *obj, const ResourceLayout *layout) {
    ResourceObjectUD *ud = (ResourceObjectUD *)lua_newuserdatauv(L, sizeof(ResourceObjectUD), 0);
    ud->obj = obj;
    ud->layout = layout;
    ensure_object_metatable(L);
    lua_setmetatable(L, -2);
}

void lua_resource_object_push(lua_State *L, void *obj, const char *type_name) {
    if (!obj) {
        lua_pushnil(L);
        return;
    }

    const ResourceLayout *layout = resource_layout_find(type_name);
    if (!layout || layout->field_count == 0) {
        // No modelled fields; hand back the pointer so callers that only test
        // for existence still work.
        lua_pushlightuserdata(L, obj);
        return;
    }

    selftest_layout_once(layout);
    push_object_proxy(L, obj, layout);
}
