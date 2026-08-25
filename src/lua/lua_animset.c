/**
 * BG3SE-macOS - Lua surface for AnimationSet resources
 *
 * Exposes the shape BG3AF drives, and through it BG3SX / WickedAnims /
 * GrazztRing:
 *
 *     resource.AnimationBank.AnimationSubSets[key].Animation[mapKey]
 *         = { ID = animID, flags = {...} }
 *
 * Backed by src/resource/animation_set.c. See
 * plans/animationset-writable-resources.md for the layout and how it was
 * verified.
 */

#include "lua_animset.h"
#include "../resource/animation_set.h"
#include "../strings/fixed_string.h"
#include "../core/logging.h"

#include <lauxlib.h>
#include <string.h>

#define ANIMSET_MAP_MT "BG3AnimSetMap"

// Which map a proxy wraps. The two behave differently on write: subsets are
// read-only here (mods add links, not subsets), animations are writable.
typedef enum {
    ANIMSET_MAP_SUBSETS,
    ANIMSET_MAP_ANIMATION
} AnimSetMapKind;

typedef struct {
    void *map_addr;          // address of the LegacyRefMap inside the game object
    AnimSetMapKind kind;
} AnimSetMapUD;

static void push_map_proxy(lua_State *L, void *map_addr, AnimSetMapKind kind);

// The empty MapKey is stored as FS_NULL. BG3SX's only subset uses it, and
// BG3AF matches it by comparing against "", so it has to round-trip as "".
#define ANIMSET_EMPTY_KEY 0xFFFFFFFFu

static void push_key(lua_State *L, uint32_t key) {
    if (key == ANIMSET_EMPTY_KEY) {
        lua_pushliteral(L, "");
        return;
    }
    const char *s = fixed_string_resolve(key);
    lua_pushstring(L, s ? s : "");
}

static bool key_from_lua(lua_State *L, int idx, uint32_t *out) {
    const char *s = lua_tostring(L, idx);
    if (!s) return false;
    if (s[0] == '\0') {
        *out = ANIMSET_EMPTY_KEY;
        return true;
    }
    uint32_t fs = fixed_string_intern(s, -1);
    if (fs == 0xFFFFFFFFu) return false;
    *out = fs;
    return true;
}

/** One Animation entry as { ID = <string>, flags = <int> }. */
static void push_animation_value(lua_State *L, const AnimSetAnimation *a) {
    lua_newtable(L);
    const char *id = fixed_string_resolve(a->id);
    lua_pushstring(L, id ? id : "");
    lua_setfield(L, -2, "ID");
    lua_pushinteger(L, a->flags);
    lua_setfield(L, -2, "flags");
}

/** One subset as { Animation = <proxy>, FallBackSubSet = <string> }. */
static void push_subset_value(lua_State *L, const AnimSetSubSet *s) {
    lua_newtable(L);
    push_map_proxy(L, (char *)s->node + 0x10, ANIMSET_MAP_ANIMATION);
    lua_setfield(L, -2, "Animation");
    push_key(L, s->fallback_subset);
    lua_setfield(L, -2, "FallBackSubSet");
}

static int animset_map_index(lua_State *L) {
    AnimSetMapUD *ud = (AnimSetMapUD *)luaL_checkudata(L, 1, ANIMSET_MAP_MT);

    uint32_t key = 0;
    if (!key_from_lua(L, 2, &key)) {
        lua_pushnil(L);
        return 1;
    }

    AnimSetRefMap map;
    if (!animset_read_map(ud->map_addr, &map)) {
        lua_pushnil(L);
        return 1;
    }

    if (ud->kind == ANIMSET_MAP_SUBSETS) {
        AnimSetSubSet s;
        if (!animset_subset_find(&map, key, &s)) { lua_pushnil(L); return 1; }
        push_subset_value(L, &s);
    } else {
        AnimSetAnimation a;
        if (!animset_animation_find(&map, key, &a)) { lua_pushnil(L); return 1; }
        push_animation_value(L, &a);
    }
    return 1;
}

static int animset_map_newindex(lua_State *L) {
    AnimSetMapUD *ud = (AnimSetMapUD *)luaL_checkudata(L, 1, ANIMSET_MAP_MT);

    if (ud->kind != ANIMSET_MAP_ANIMATION) {
        return luaL_error(L, "AnimationSubSets is read-only; assign into a "
                             "subset's Animation table instead");
    }

    uint32_t key = 0;
    if (!key_from_lua(L, 2, &key)) {
        return luaL_error(L, "AnimationSet: key must be a string");
    }

    if (lua_isnil(L, 3)) {
        return luaL_error(L, "AnimationSet: removing entries is not supported");
    }
    luaL_checktype(L, 3, LUA_TTABLE);

    lua_getfield(L, 3, "ID");
    const char *id = lua_tostring(L, -1);
    if (!id || !id[0]) {
        return luaL_error(L, "AnimationSet: entry needs a non-empty ID");
    }
    uint32_t id_fs = fixed_string_intern(id, -1);
    lua_pop(L, 1);
    if (id_fs == 0xFFFFFFFFu) {
        return luaL_error(L, "AnimationSet: could not intern ID '%s'", id);
    }

    // flags is a table of names on Windows; accept an integer here and treat
    // anything else as none rather than guessing at the bitmask names.
    lua_getfield(L, 3, "flags");
    uint8_t flags = (uint8_t)(lua_isinteger(L, -1) ? lua_tointeger(L, -1) : 0);
    lua_pop(L, 1);

    if (!animset_animation_set(ud->map_addr, key, id_fs, flags)) {
        return luaL_error(L, "AnimationSet: write failed (writes are off unless "
                             "BG3SE_ANIMSET_WRITE=1)");
    }
    return 0;
}

/**
 * Iterator closure over (map userdata, cursor). Lua's generic for feeds the
 * first returned value back as the next control variable, and these maps are
 * keyed by string, so the cursor has to live in an upvalue rather than being
 * threaded through the loop.
 */
static int animset_map_iter(lua_State *L) {
    AnimSetMapUD *ud = (AnimSetMapUD *)lua_touserdata(L, lua_upvalueindex(1));
    if (!ud) { lua_pushnil(L); return 1; }

    uint32_t index = (uint32_t)lua_tointeger(L, lua_upvalueindex(2));

    AnimSetRefMap map;
    if (!animset_read_map(ud->map_addr, &map)) { lua_pushnil(L); return 1; }

    if (ud->kind == ANIMSET_MAP_SUBSETS) {
        AnimSetSubSet s;
        if (!animset_subset_at(&map, index, &s)) { lua_pushnil(L); return 1; }
        push_key(L, s.key);
        push_subset_value(L, &s);
    } else {
        AnimSetAnimation a;
        if (!animset_animation_at(&map, index, &a)) { lua_pushnil(L); return 1; }
        push_key(L, a.key);
        push_animation_value(L, &a);
    }

    lua_pushinteger(L, index + 1);
    lua_replace(L, lua_upvalueindex(2));
    return 2;
}

static int animset_map_pairs(lua_State *L) {
    luaL_checkudata(L, 1, ANIMSET_MAP_MT);
    lua_pushvalue(L, 1);
    lua_pushinteger(L, 0);
    lua_pushcclosure(L, animset_map_iter, 2);
    lua_pushvalue(L, 1);   // state (unused by the closure, but conventional)
    lua_pushnil(L);        // initial control
    return 3;
}

static int animset_map_len(lua_State *L) {
    AnimSetMapUD *ud = (AnimSetMapUD *)luaL_checkudata(L, 1, ANIMSET_MAP_MT);
    AnimSetRefMap map;
    lua_pushinteger(L, animset_read_map(ud->map_addr, &map) ? map.item_count : 0);
    return 1;
}

static void push_map_proxy(lua_State *L, void *map_addr, AnimSetMapKind kind) {
    AnimSetMapUD *ud = (AnimSetMapUD *)lua_newuserdatauv(L, sizeof(AnimSetMapUD), 0);
    ud->map_addr = map_addr;
    ud->kind = kind;

    if (luaL_newmetatable(L, ANIMSET_MAP_MT)) {
        lua_pushcfunction(L, animset_map_index);    lua_setfield(L, -2, "__index");
        lua_pushcfunction(L, animset_map_newindex); lua_setfield(L, -2, "__newindex");
        lua_pushcfunction(L, animset_map_pairs);    lua_setfield(L, -2, "__pairs");
        lua_pushcfunction(L, animset_map_len);      lua_setfield(L, -2, "__len");
        lua_pushliteral(L, "AnimationSetMap");      lua_setfield(L, -2, "__name");
    }
    lua_setmetatable(L, -2);
}

bool lua_animset_push_bank(lua_State *L, void *resource) {
    void *bank = NULL;
    if (!animset_get_bank(resource, &bank) || !bank) return false;

    // AnimationSet holds exactly one member, so the bank pointer IS the
    // AnimationSubSets map.
    lua_newtable(L);
    push_map_proxy(L, bank, ANIMSET_MAP_SUBSETS);
    lua_setfield(L, -2, "AnimationSubSets");
    return true;
}
