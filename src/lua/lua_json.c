/**
 * BG3SE-macOS - Lua JSON Module Implementation
 *
 * Simple JSON parser and stringifier for Lua integration.
 */

#include "lua_json.h"
#include "logging.h"

#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <math.h>

// ============================================================================
// Internal Helpers
// ============================================================================

/* Whitespace, plus // line and /* block *\/ comments.
 *
 * Upstream parses with rapidjson under
 *   #define RAPIDJSON_PARSE_DEFAULT_FLAGS kParseCommentsFlag | kParseTrailingCommasFlag | kParseNanAndInfFlag
 * (CoreLib/JsonLibs.h:3), so every mod config that relies on those parses on
 * Windows. This parser accepted none of them and returned nil instead: AV Item
 * Shipment Framework rejected "All Dyes in the Camp Chest" and "Camp Clothes in
 * the Camp Chest" as "Invalid ISF config JSON" purely over a trailing
 * `// UUID for the Dye Rack.` comment, so their items never reached the camp
 * chest (2026-09-27). Comments, trailing commas and NaN/Inf now match upstream. */
static const char *json_skip_whitespace(const char *json) {
    for (;;) {
        while (*json && (*json == ' ' || *json == '\t' || *json == '\n' || *json == '\r')) {
            json++;
        }
        if (json[0] == '/' && json[1] == '/') {
            json += 2;
            while (*json && *json != '\n') json++;
            continue;
        }
        if (json[0] == '/' && json[1] == '*') {
            json += 2;
            while (*json && !(json[0] == '*' && json[1] == '/')) json++;
            if (*json) json += 2;
            continue;
        }
        return json;
    }
}

static int json_hex4(const char *h, unsigned *out) {
    unsigned v = 0;
    for (int i = 0; i < 4; i++) {
        char c = h[i];
        v <<= 4;
        if (c >= '0' && c <= '9') v |= (unsigned)(c - '0');
        else if (c >= 'a' && c <= 'f') v |= (unsigned)(c - 'a' + 10);
        else if (c >= 'A' && c <= 'F') v |= (unsigned)(c - 'A' + 10);
        else return 0;
    }
    *out = v;
    return 1;
}

static void json_add_utf8(luaL_Buffer *b, unsigned cp) {
    if (cp < 0x80) {
        luaL_addchar(b, (char)cp);
    } else if (cp < 0x800) {
        luaL_addchar(b, (char)(0xC0 | (cp >> 6)));
        luaL_addchar(b, (char)(0x80 | (cp & 0x3F)));
    } else if (cp < 0x10000) {
        luaL_addchar(b, (char)(0xE0 | (cp >> 12)));
        luaL_addchar(b, (char)(0x80 | ((cp >> 6) & 0x3F)));
        luaL_addchar(b, (char)(0x80 | (cp & 0x3F)));
    } else {
        luaL_addchar(b, (char)(0xF0 | (cp >> 18)));
        luaL_addchar(b, (char)(0x80 | ((cp >> 12) & 0x3F)));
        luaL_addchar(b, (char)(0x80 | ((cp >> 6) & 0x3F)));
        luaL_addchar(b, (char)(0x80 | (cp & 0x3F)));
    }
}

static const char *json_parse_string(lua_State *L, const char *json) {
    if (*json != '"') return NULL;
    json++;  // skip opening quote

    luaL_Buffer b;
    luaL_buffinit(L, &b);

    while (*json && *json != '"') {
        if (*json == '\\' && json[1]) {
            json++;
            switch (*json) {
                case '"': luaL_addchar(&b, '"'); break;
                case '\\': luaL_addchar(&b, '\\'); break;
                case '/': luaL_addchar(&b, '/'); break;
                case 'b': luaL_addchar(&b, '\b'); break;
                case 'f': luaL_addchar(&b, '\f'); break;
                case 'n': luaL_addchar(&b, '\n'); break;
                case 'r': luaL_addchar(&b, '\r'); break;
                case 't': luaL_addchar(&b, '\t'); break;
                case 'u': {
                    /* \uXXXX, including a surrogate pair, decoded to UTF-8 as
                     * rapidjson does. It used to fall to the default case and
                     * yield a literal 'u' followed by the hex digits. */
                    unsigned cp;
                    if (!json_hex4(json + 1, &cp)) return NULL;
                    json += 4;
                    if (cp >= 0xD800 && cp <= 0xDBFF && json[1] == '\\' && json[2] == 'u') {
                        unsigned lo;
                        if (json_hex4(json + 3, &lo) && lo >= 0xDC00 && lo <= 0xDFFF) {
                            cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                            json += 6;
                        }
                    }
                    json_add_utf8(&b, cp);
                    break;
                }
                default: luaL_addchar(&b, *json); break;
            }
        } else {
            luaL_addchar(&b, *json);
        }
        json++;
    }

    if (*json != '"') return NULL;
    luaL_pushresult(&b);
    return json + 1;  // skip closing quote
}

static const char *json_parse_number(lua_State *L, const char *json) {
    const char *start = json;
    if (*json == '-') json++;
    while (*json >= '0' && *json <= '9') json++;
    if (*json == '.') {
        json++;
        while (*json >= '0' && *json <= '9') json++;
    }
    if (*json == 'e' || *json == 'E') {
        json++;
        if (*json == '+' || *json == '-') json++;
        while (*json >= '0' && *json <= '9') json++;
    }

    /* Integers stay integers, as upstream does (rapidjson IsInt64 -> push
     * int64). Pushing every number as a double turned `"FileVersion": 1` into
     * 1.0 -- equal under ==, but different under tostring() and as a table key. */
    int is_int = 1;
    for (const char *c = start; c < json; c++) {
        if (*c == '.' || *c == 'e' || *c == 'E') { is_int = 0; break; }
    }
    char *endptr;
    if (is_int) {
        long long iv = strtoll(start, &endptr, 10);
        if (endptr == json) { lua_pushinteger(L, (lua_Integer)iv); return json; }
    }
    double num = strtod(start, &endptr);
    lua_pushnumber(L, num);
    return json;
}

static const char *json_parse_object(lua_State *L, const char *json) {
    if (*json != '{') return NULL;
    json = json_skip_whitespace(json + 1);

    lua_newtable(L);

    if (*json == '}') return json + 1;

    while (1) {
        json = json_skip_whitespace(json);
        if (*json != '"') return NULL;

        // Parse key
        json = json_parse_string(L, json);
        if (!json) return NULL;

        json = json_skip_whitespace(json);
        if (*json != ':') return NULL;
        json = json_skip_whitespace(json + 1);

        // Parse value
        json = json_parse_value(L, json);
        if (!json) return NULL;

        // Set table[key] = value
        lua_settable(L, -3);

        json = json_skip_whitespace(json);
        if (*json == '}') return json + 1;
        if (*json != ',') return NULL;
        json = json_skip_whitespace(json + 1);
        if (*json == '}') return json + 1;   /* trailing comma, as upstream allows */
    }
}

static const char *json_parse_array(lua_State *L, const char *json) {
    if (*json != '[') return NULL;
    json = json_skip_whitespace(json + 1);

    lua_newtable(L);
    int index = 1;

    if (*json == ']') return json + 1;

    while (1) {
        json = json_skip_whitespace(json);
        json = json_parse_value(L, json);
        if (!json) return NULL;

        lua_rawseti(L, -2, index++);

        json = json_skip_whitespace(json);
        if (*json == ']') return json + 1;
        if (*json != ',') return NULL;
        json = json_skip_whitespace(json + 1);
        if (*json == ']') return json + 1;   /* trailing comma, as upstream allows */
    }
}

// ============================================================================
// Public Parsing Functions
// ============================================================================

const char *json_parse_value(lua_State *L, const char *json) {
    json = json_skip_whitespace(json);

    if (*json == '"') {
        return json_parse_string(L, json);
    } else if (*json == '{') {
        return json_parse_object(L, json);
    } else if (*json == '[') {
        return json_parse_array(L, json);
    } else if (*json == 't' && strncmp(json, "true", 4) == 0) {
        lua_pushboolean(L, 1);
        return json + 4;
    } else if (*json == 'f' && strncmp(json, "false", 5) == 0) {
        lua_pushboolean(L, 0);
        return json + 5;
    } else if (*json == 'n' && strncmp(json, "null", 4) == 0) {
        lua_pushnil(L);
        return json + 4;
    } else if (strncmp(json, "NaN", 3) == 0) {
        lua_pushnumber(L, (lua_Number)NAN);
        return json + 3;
    } else if (strncmp(json, "Infinity", 8) == 0 || strncmp(json, "-Infinity", 9) == 0) {
        int neg = (*json == '-');
        lua_pushnumber(L, neg ? -(lua_Number)INFINITY : (lua_Number)INFINITY);
        return json + (neg ? 9 : 8);
    } else if (strncmp(json, "Inf", 3) == 0 || strncmp(json, "-Inf", 4) == 0) {
        int neg = (*json == '-');
        lua_pushnumber(L, neg ? -(lua_Number)INFINITY : (lua_Number)INFINITY);
        return json + (neg ? 4 : 3);
    } else if (*json == '-' || (*json >= '0' && *json <= '9')) {
        return json_parse_number(L, json);
    }

    return NULL;
}

// ============================================================================
// Stringify Functions
// ============================================================================

// A plain growable byte buffer. The stringifier builds into this instead of a
// luaL_Buffer: luaL_Buffer keeps a box userdata on the Lua stack once it grows,
// and interleaving arbitrary stack pushes/pops (as table iteration requires) with
// buffer appends corrupts it (SIGSEGV in luaL_prepbuffsize). Building into a
// malloc'd buffer sidesteps that entirely; the result is copied into the caller's
// luaL_Buffer in a single, safe append at the top level.
typedef struct { char *data; size_t len; size_t cap; int oom; } JsonBuf;

// Cap recursion so a cyclic/self-referential table (possible now that mod tables
// carry an { __index = _G } metatable) fails gracefully instead of overflowing.
#define JSON_MAX_DEPTH 200

static void jb_init(JsonBuf *jb) {
    jb->cap = 256; jb->len = 0; jb->oom = 0;
    jb->data = (char *)malloc(jb->cap);
    if (!jb->data) jb->oom = 1; else jb->data[0] = '\0';
}
static void jb_free(JsonBuf *jb) { free(jb->data); jb->data = NULL; }
static void jb_reserve(JsonBuf *jb, size_t extra) {
    if (jb->oom) return;
    if (jb->len + extra + 1 <= jb->cap) return;
    size_t nc = jb->cap ? jb->cap : 256;
    while (nc < jb->len + extra + 1) nc *= 2;
    char *nd = (char *)realloc(jb->data, nc);
    if (!nd) { jb->oom = 1; return; }
    jb->data = nd; jb->cap = nc;
}
static void jb_addlstring(JsonBuf *jb, const char *s, size_t n) {
    if (jb->oom || !s || n == 0) return;
    jb_reserve(jb, n);
    if (jb->oom) return;
    memcpy(jb->data + jb->len, s, n);
    jb->len += n; jb->data[jb->len] = '\0';
}
static void jb_addstring(JsonBuf *jb, const char *s) { if (s) jb_addlstring(jb, s, strlen(s)); }
static void jb_addchar(JsonBuf *jb, char c) {
    jb_reserve(jb, 1);
    if (jb->oom) return;
    jb->data[jb->len++] = c; jb->data[jb->len] = '\0';
}

static void json_sb_value(lua_State *L, int index, JsonBuf *jb, int depth);

static void json_sb_table(lua_State *L, int index, JsonBuf *jb, int depth) {
    if (depth > JSON_MAX_DEPTH) { jb_addstring(jb, "null"); return; }

    // Check if it's an array (sequential integer keys starting from 1)
    int is_array = 1;
    int max_index = 0;

    lua_pushnil(L);
    while (lua_next(L, index) != 0) {
        if (lua_type(L, -2) != LUA_TNUMBER || lua_tointeger(L, -2) != max_index + 1) {
            is_array = 0;
        }
        max_index++;
        lua_pop(L, 1);
    }

    if (is_array && max_index > 0) {
        jb_addchar(jb, '[');
        for (int i = 1; i <= max_index; i++) {
            if (i > 1) jb_addchar(jb, ',');
            lua_rawgeti(L, index, i);
            json_sb_value(L, lua_gettop(L), jb, depth + 1);
            lua_pop(L, 1);
        }
        jb_addchar(jb, ']');
    } else {
        jb_addchar(jb, '{');
        int first = 1;
        lua_pushnil(L);
        while (lua_next(L, index) != 0) {
            if (!first) jb_addchar(jb, ',');
            first = 0;

            // Key (guard against lua_tostring returning NULL for non-string,
            // non-number keys, which would crash the buffer append).
            jb_addchar(jb, '"');
            if (lua_type(L, -2) == LUA_TSTRING) {
                jb_addstring(jb, lua_tostring(L, -2));
            } else {
                lua_pushvalue(L, -2);
                const char *ks = lua_tostring(L, -1);
                jb_addstring(jb, ks ? ks : "?");
                lua_pop(L, 1);
            }
            jb_addchar(jb, '"');
            jb_addchar(jb, ':');

            // Value
            json_sb_value(L, lua_gettop(L), jb, depth + 1);
            lua_pop(L, 1);
        }
        jb_addchar(jb, '}');
    }
}

static void json_sb_value(lua_State *L, int index, JsonBuf *jb, int depth) {
    int t = lua_type(L, index);
    switch (t) {
        case LUA_TSTRING: {
            const char *s = lua_tostring(L, index);
            jb_addchar(jb, '"');
            for (; s && *s; s++) {
                if (*s == '"' || *s == '\\') jb_addchar(jb, '\\');
                jb_addchar(jb, *s);
            }
            jb_addchar(jb, '"');
            break;
        }
        case LUA_TNUMBER: {
            char buf[64];
            snprintf(buf, sizeof(buf), "%g", lua_tonumber(L, index));
            jb_addstring(jb, buf);
            break;
        }
        case LUA_TBOOLEAN:
            jb_addstring(jb, lua_toboolean(L, index) ? "true" : "false");
            break;
        case LUA_TTABLE:
            json_sb_table(L, index, jb, depth);
            break;
        case LUA_TNIL:
        default:
            jb_addstring(jb, "null");
            break;
    }
}

// Public entry point kept for existing callers (user_variables, persistentvars,
// main). Builds into a private malloc buffer, then appends the whole result to
// the caller's luaL_Buffer in one safe operation.
void json_stringify_value(lua_State *L, int index, luaL_Buffer *b) {
    JsonBuf jb;
    jb_init(&jb);
    json_sb_value(L, index, &jb, 0);
    if (!jb.oom && jb.data && jb.len > 0) {
        luaL_addlstring(b, jb.data, jb.len);
    } else if (jb.oom) {
        luaL_addstring(b, "null");
    }
    jb_free(&jb);
}

// ============================================================================
// Lua C API Functions
// ============================================================================

int lua_ext_json_parse(lua_State *L) {
    const char *json = luaL_checkstring(L, 1);
    LOG_LUA_DEBUG("Ext.Json.Parse called (len: %zu)", strlen(json));

    const char *result = json_parse_value(L, json);
    if (!result) {
        lua_pushnil(L);
        LOG_LUA_DEBUG("Ext.Json.Parse failed");
    }
    return 1;
}

int lua_ext_json_stringify(lua_State *L) {
    luaL_Buffer b;
    luaL_buffinit(L, &b);
    json_stringify_value(L, 1, &b);
    luaL_pushresult(&b);
    return 1;
}

/**
 * Ext.DumpExport(value) -> string
 *
 * MCM uses this in its diagnostic paths, several of which concatenate the
 * result directly:
 *     "... (" .. Ext.DumpExport(config[settingID]) .. ") ..."
 * so it must ALWAYS return a string. Returning nil (or not existing at all)
 * raises inside MCM's blueprint preprocessing and aborts the whole load: the
 * server never reaches "Finished loading MCM blueprints", never ships the
 * blueprints to the client, and the MCM window is never created. That only
 * shows up on a profile large enough for some mod to have a setting its
 * blueprint does not declare, which is what takes MCM down that path.
 *
 * Non-serialisable values fall back to Lua's own tostring form rather than
 * failing, since every caller here is building a log line.
 */
static int lua_ext_dump_export(lua_State *L) {
    int t = lua_type(L, 1);

    if (t == LUA_TNONE || t == LUA_TNIL) {
        lua_pushliteral(L, "nil");
        return 1;
    }

    if (t == LUA_TSTRING || t == LUA_TNUMBER || t == LUA_TBOOLEAN || t == LUA_TTABLE) {
        luaL_Buffer b;
        luaL_buffinit(L, &b);
        json_stringify_value(L, 1, &b);
        luaL_pushresult(&b);
        return 1;
    }

    // Functions, userdata, threads: "function: 0x...", and so on.
    luaL_tolstring(L, 1, NULL);
    return 1;
}

// ============================================================================
// Registration
// ============================================================================

void lua_json_register(lua_State *L, int ext_table_index) {
    // Convert negative index to absolute since we'll be pushing onto stack
    if (ext_table_index < 0) {
        ext_table_index = lua_gettop(L) + ext_table_index + 1;
    }

    // Create Ext.Json table
    lua_newtable(L);
    lua_pushcfunction(L, lua_ext_json_parse);
    lua_setfield(L, -2, "Parse");
    lua_pushcfunction(L, lua_ext_json_stringify);
    lua_setfield(L, -2, "Stringify");
    lua_setfield(L, ext_table_index, "Json");

    // Top-level Ext.DumpExport - this is where MCM calls it from.
    lua_pushcfunction(L, lua_ext_dump_export);
    lua_setfield(L, ext_table_index, "DumpExport");
}
