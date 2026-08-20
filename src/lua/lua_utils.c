/**
 * lua_utils.c - Ext.Utils Lua bindings (Windows parity surface)
 *
 * Ports the Windows Ext.Utils functions whose behavior is defined entirely by
 * the Lua VM, the process, or the app bundle -- i.e. everything that does not
 * require reverse-engineered engine state.
 *
 * Windows reference: BG3Extender/Lua/Libs/Utils.inl
 *
 * Implemented here:
 *   GameVersion, Include, LoadString, IsValidHandle, HandleToInteger,
 *   IntegerToHandle, GenerateGuid, ShowError, ShowErrorAndExitGame,
 *   GetCommandLineParams, GetMemoryUsage, ProfileBegin, ProfileEnd
 *
 * Deliberately NOT implemented here (engine RE required, see docs/deferrals.md):
 *   GetGlobalSwitches  - GlobalSwitches struct layout unrecovered on macOS
 *   GetDialogManager   - esv::DialogSystem layout unrecovered on macOS
 */

#include "lua_utils.h"

#include "../core/logging.h"
#include "../core/version_detect.h"
#include "../entity/entity_system.h"
#include "../mod/mod_loader.h"
#include "../pak/pak_reader.h"

#include <lauxlib.h>

#include <crt_externs.h>
#include <os/signpost.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>

#include <CoreFoundation/CoreFoundation.h>

#define UTILS_MAX_PATH_LEN 1024

// ============================================================================
// GameVersion
// ============================================================================

// Ext.Utils.GameVersion() -> string or nil
// Windows formats the engine GameVersionInfo as "vMajor.Minor.Revision.Build"
// (Utils.inl:84-95) and returns nil when the version cannot be read. macOS
// sources the same dotted quad from the app bundle's CFBundleShortVersionString
// via version_detect, so the returned shape matches.
static int lua_utils_gameversion(lua_State *L) {
    const char *version = version_detect_get_version();
    if (!version || version[0] == '\0') {
        lua_pushnil(L);
        return 1;
    }

    char buf[64];
    snprintf(buf, sizeof(buf), "v%s", version);
    lua_pushstring(L, buf);
    return 1;
}

// ============================================================================
// LoadString
// ============================================================================

// Ext.Utils.LoadString(source[, globalsTable]) -> chunk | (nil, error)
// Mirrors Utils.inl:46-77 exactly, including the temporary swap of
// LUA_RIDX_GLOBALS so the compiled chunk captures the caller-supplied globals
// table as its _ENV upvalue, and the (nil, message) error shape.
static int lua_utils_loadstring(lua_State *L) {
    size_t len = 0;
    const char *source = luaL_checklstring(L, 1, &len);

    bool replaceGlobals = lua_gettop(L) > 1 && !lua_isnil(L, 2);
    int globalsIdx = lua_gettop(L) + 1;

    if (replaceGlobals) {
        luaL_checktype(L, 2, LUA_TTABLE);
        // Save the current globals, then install the caller's table.
        lua_rawgeti(L, LUA_REGISTRYINDEX, LUA_RIDX_GLOBALS);
        lua_pushvalue(L, 2);
        lua_rawseti(L, LUA_REGISTRYINDEX, LUA_RIDX_GLOBALS);
    }

    // "t" => text chunks only, matching the Windows loader mode.
    int status = luaL_loadbufferx(L, source, len, NULL, "t");

    if (replaceGlobals) {
        lua_pushvalue(L, globalsIdx);
        lua_rawseti(L, LUA_REGISTRYINDEX, LUA_RIDX_GLOBALS);
        lua_remove(L, globalsIdx);
    }

    if (status == LUA_OK) {
        return 1;
    }

    // Error: message is on top; return nil followed by the message.
    lua_pushnil(L);
    lua_insert(L, -2);
    return 2;
}

// ============================================================================
// Include
// ============================================================================

// Resolve a mod UUID to its registered mod name. Returns NULL when unknown.
static const char *utils_mod_name_for_uuid(const char *uuid) {
    if (!uuid || uuid[0] == '\0') {
        return NULL;
    }

    int count = mod_get_se_count();
    for (int i = 0; i < count; i++) {
        const char *modUuid = mod_get_se_uuid(i);
        if (modUuid && strcasecmp(modUuid, uuid) == 0) {
            return mod_get_se_name(i);
        }
    }
    return NULL;
}

// Load (without running) a mod script into a chunk on the Lua stack.
// Mirrors require_resolve()'s resolution order: extracted filesystem base
// first, then the mod PAK. Returns true with the chunk pushed.
static bool utils_load_mod_chunk(lua_State *L, const char *modName,
                                 const char *fileName) {
    char pakPath[UTILS_MAX_PATH_LEN];
    char inPakPath[UTILS_MAX_PATH_LEN];

    // Filesystem base is only meaningful for the currently-executing mod.
    const char *currentName = mod_get_current_name();
    if (currentName && modName && strcmp(currentName, modName) == 0) {
        const char *luaBase = mod_get_current_lua_base();
        if (luaBase && luaBase[0] != '\0') {
            char fullPath[UTILS_MAX_PATH_LEN];
            snprintf(fullPath, sizeof(fullPath), "%s/%s", luaBase, fileName);
            if (luaL_loadfile(L, fullPath) == LUA_OK) {
                return true;
            }
            lua_pop(L, 1);  // discard the load error; fall through to the PAK
        }
    }

    if (!modName || modName[0] == '\0') {
        return false;
    }
    if (!mod_find_pak(modName, pakPath, sizeof(pakPath))) {
        return false;
    }

    snprintf(inPakPath, sizeof(inPakPath), "Mods/%s/ScriptExtender/Lua/%s",
             modName, fileName);

    PakFile *pak = pak_open(pakPath);
    if (!pak) {
        return false;
    }

    int entry = pak_find_entry(pak, inPakPath);
    if (entry < 0) {
        pak_close(pak);
        return false;
    }

    size_t size = 0;
    char *content = pak_read_file(pak, entry, &size);
    pak_close(pak);
    if (!content) {
        return false;
    }

    int status = luaL_loadbuffer(L, content, size, inPakPath);
    free(content);

    if (status != LUA_OK) {
        LOG_LUA_ERROR("Include compile error (%s): %s", inPakPath,
                      lua_tostring(L, -1));
        lua_pop(L, 1);
        return false;
    }
    return true;
}

// Ext.Utils.Include(modGuid, fileName[, globalsTable]) -> script return values
// Mirrors Utils.inl:7-44. modGuid may be nil to load relative to the current
// mod. The optional globals table is installed for the duration of the load
// and the call, so the included chunk sees it as its environment.
static int lua_utils_include(lua_State *L) {
    const char *modGuid = lua_isnoneornil(L, 1) ? NULL : luaL_checkstring(L, 1);
    const char *fileName = luaL_checkstring(L, 2);

    bool replaceGlobals = lua_gettop(L) > 2 && !lua_isnil(L, 3);
    int globalsIdx = lua_gettop(L) + 1;

    if (replaceGlobals) {
        luaL_checktype(L, 3, LUA_TTABLE);
        lua_rawgeti(L, LUA_REGISTRYINDEX, LUA_RIDX_GLOBALS);
        lua_pushvalue(L, 3);
        lua_rawseti(L, LUA_REGISTRYINDEX, LUA_RIDX_GLOBALS);
    }

    const char *modName = modGuid ? utils_mod_name_for_uuid(modGuid)
                                  : mod_get_current_name();

    int base = lua_gettop(L);
    bool loaded = modName && utils_load_mod_chunk(L, modName, fileName);

    if (replaceGlobals) {
        // Restore globals before running, matching the Windows ordering where
        // the swap brackets the load and the call together.
        lua_pushvalue(L, globalsIdx);
        lua_rawseti(L, LUA_REGISTRYINDEX, LUA_RIDX_GLOBALS);
    }

    if (!loaded) {
        if (replaceGlobals) {
            lua_remove(L, globalsIdx);
        }
        LOG_LUA_WARN("Ext.Utils.Include: could not resolve '%s' (mod '%s')",
                     fileName, modName ? modName : "<unknown>");
        return 0;
    }

    // Apply the caller's globals table as the chunk's _ENV upvalue directly,
    // which is more precise than relying on the registry swap alone.
    if (replaceGlobals) {
        lua_pushvalue(L, 3);
        if (lua_setupvalue(L, -2, 1) == NULL) {
            lua_pop(L, 1);
        }
    }

    if (lua_pcall(L, 0, LUA_MULTRET, 0) != LUA_OK) {
        const char *error = lua_tostring(L, -1);
        LOG_LUA_ERROR("Ext.Utils.Include runtime error (%s): %s", fileName,
                      error ? error : "?");
        lua_pop(L, 1);
        if (replaceGlobals) {
            lua_remove(L, globalsIdx);
        }
        return 0;
    }

    int results = lua_gettop(L) - base;
    if (replaceGlobals) {
        lua_remove(L, globalsIdx);
    }
    return results;
}

// ============================================================================
// Entity handle helpers
// ============================================================================

// Ext.Utils.IsValidHandle(value) -> boolean
// Windows (Utils.inl:96-110) returns true only for an entity proxy carrying a
// non-zero handle, and false for every other Lua type.
static int lua_utils_isvalidhandle(lua_State *L) {
    EntityUserdata *ud = (EntityUserdata *)luaL_testudata(L, 1, "BG3Entity");
    lua_pushboolean(L, ud != NULL && ud->handle != 0);
    return 1;
}

// Ext.Utils.HandleToInteger(handle) -> integer
// Windows (Utils.inl:115-118) returns the raw 64-bit handle for serialization.
static int lua_utils_handletointeger(lua_State *L) {
    EntityUserdata *ud = (EntityUserdata *)luaL_testudata(L, 1, "BG3Entity");
    if (!ud) {
        // Accept an already-integral handle so round-tripping is total.
        if (lua_isinteger(L, 1)) {
            lua_pushinteger(L, lua_tointeger(L, 1));
            return 1;
        }
        return luaL_argerror(L, 1, "expected entity handle");
    }
    lua_pushinteger(L, (lua_Integer)ud->handle);
    return 1;
}

// Ext.Utils.IntegerToHandle(integer) -> entity handle
// Windows (Utils.inl:124-127) constructs the handle without validating it, so
// the inverse of HandleToInteger is total. We mirror that: no liveness check.
static int lua_utils_integertohandle(lua_State *L) {
    lua_Integer raw = luaL_checkinteger(L, 1);

    EntityUserdata *ud =
        (EntityUserdata *)lua_newuserdata(L, sizeof(EntityUserdata));
    ud->handle = (EntityHandle)raw;
    ud->lifetime = lifetime_lua_get_current(L);

    luaL_getmetatable(L, "BG3Entity");
    lua_setmetatable(L, -2);
    return 1;
}

// ============================================================================
// GenerateGuid
// ============================================================================

// Ext.Utils.GenerateGuid() -> string
// Windows returns a Guid value (Utils.inl:129-132); the Lua surface renders it
// in the canonical dashed form, which is what mods consume. We emit an RFC 4122
// version-4 UUID in the same lowercase dashed shape BG3 uses.
static int lua_utils_generateguid(lua_State *L) {
    uint8_t bytes[16];
    arc4random_buf(bytes, sizeof(bytes));

    bytes[6] = (uint8_t)((bytes[6] & 0x0F) | 0x40);  // version 4
    bytes[8] = (uint8_t)((bytes[8] & 0x3F) | 0x80);  // RFC 4122 variant

    char out[37];
    snprintf(out, sizeof(out),
             "%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-"
             "%02x%02x%02x%02x%02x%02x",
             bytes[0], bytes[1], bytes[2], bytes[3], bytes[4], bytes[5],
             bytes[6], bytes[7], bytes[8], bytes[9], bytes[10], bytes[11],
             bytes[12], bytes[13], bytes[14], bytes[15]);

    lua_pushstring(L, out);
    return 1;
}

// ============================================================================
// Error dialogs
// ============================================================================

// Show a modal alert. Windows routes both entry points through
// LibraryManager::ShowStartupError (Utils.inl:139-149); the macOS analogue is
// CFUserNotification, which works before and after the window server is up.
static void utils_show_alert(const char *message) {
    CFStringRef title = CFSTR("Baldur's Gate 3 Script Extender");
    CFStringRef body = CFStringCreateWithCString(
        NULL, message ? message : "", kCFStringEncodingUTF8);
    if (!body) {
        return;
    }

    CFOptionFlags response = 0;
    CFUserNotificationDisplayAlert(0.0, kCFUserNotificationStopAlertLevel, NULL,
                                   NULL, NULL, title, body, NULL, NULL, NULL,
                                   &response);
    CFRelease(body);
}

// Ext.Utils.ShowError(message)
static int lua_utils_showerror(lua_State *L) {
    const char *message = luaL_checkstring(L, 1);
    LOG_LUA_ERROR("Ext.Utils.ShowError: %s", message);
    utils_show_alert(message);
    return 0;
}

// Ext.Utils.ShowErrorAndExitGame(message)
// Windows passes exitGame=true, which tears the process down after the dialog
// is dismissed.
static int lua_utils_showerrorandexitgame(lua_State *L) {
    const char *message = luaL_checkstring(L, 1);
    LOG_LUA_ERROR("Ext.Utils.ShowErrorAndExitGame: %s", message);
    utils_show_alert(message);
    _exit(1);
    return 0;  // unreachable
}

// ============================================================================
// Process introspection
// ============================================================================

// Ext.Utils.GetCommandLineParams() -> array of strings
// Windows splits GetCommandLineW() on spaces (Utils.inl:154-175). On macOS the
// kernel hands us a pre-split argv, which is strictly more accurate (quoted
// arguments containing spaces survive intact).
static int lua_utils_getcommandlineparams(lua_State *L) {
    int *argcp = _NSGetArgc();
    char ***argvp = _NSGetArgv();

    lua_newtable(L);
    if (!argcp || !argvp || !*argvp) {
        return 1;
    }

    int argc = *argcp;
    char **argv = *argvp;
    int n = 0;

    for (int i = 0; i < argc; i++) {
        if (!argv[i] || argv[i][0] == '\0') {
            continue;  // Windows drops empty tokens
        }
        lua_pushstring(L, argv[i]);
        lua_rawseti(L, -2, ++n);
    }
    return 1;
}

// Ext.Utils.GetMemoryUsage() -> integer (bytes)
// Windows reports the Lua allocator's tracked AllocatedMemory (Utils.inl:197).
// The macOS build uses the stock allocator, so we report the VM's own accounting
// of live bytes, which measures the same quantity.
static int lua_utils_getmemoryusage(lua_State *L) {
    int64_t kb = (int64_t)lua_gc(L, LUA_GCCOUNT);
    int64_t bytes = (int64_t)lua_gc(L, LUA_GCCOUNTB);
    lua_pushinteger(L, (lua_Integer)(kb * 1024 + bytes));
    return 1;
}

// ============================================================================
// Profiling
// ============================================================================

// Windows compiles ProfileBegin/ProfileEnd against Optick, which is a
// Windows-only dependency (Utils.inl:202-228 are wrapped in #if USE_OPTICK).
// The faithful macOS analogue is os_signpost, which Instruments consumes the
// same way Optick consumes its own scopes. Intervals are kept on a per-thread
// stack so nesting behaves like the Windows profiler.

#define UTILS_SIGNPOST_MAX_DEPTH 32

static os_log_t g_signpost_log;
static pthread_once_t g_signpost_once = PTHREAD_ONCE_INIT;

static void utils_signpost_log_init(void) {
    g_signpost_log = os_log_create("com.bg3se.macos", "LuaProfile");
}

static os_log_t utils_signpost_log(void) {
    pthread_once(&g_signpost_once, utils_signpost_log_init);
    return g_signpost_log;
}

typedef struct {
    os_signpost_id_t ids[UTILS_SIGNPOST_MAX_DEPTH];
    int depth;
} UtilsProfileStack;

static _Thread_local UtilsProfileStack g_profile_stack;

// Ext.Utils.ProfileBegin([nameOrFunction])
static int lua_utils_profilebegin(lua_State *L) {
    const char *name = "";
    if (lua_type(L, 1) == LUA_TSTRING) {
        name = lua_tostring(L, 1);
    } else if (lua_isfunction(L, 1)) {
        lua_Debug ar;
        lua_pushvalue(L, 1);
        if (lua_getinfo(L, ">S", &ar) && ar.short_src[0]) {
            name = ar.short_src;
        }
    }

    UtilsProfileStack *stack = &g_profile_stack;
    if (stack->depth >= UTILS_SIGNPOST_MAX_DEPTH) {
        LOG_LUA_WARN("Ext.Utils.ProfileBegin: nesting deeper than %d ignored",
                     UTILS_SIGNPOST_MAX_DEPTH);
        return 0;
    }

    os_log_t log = utils_signpost_log();
    os_signpost_id_t id = os_signpost_id_generate(log);
    stack->ids[stack->depth++] = id;
    os_signpost_interval_begin(log, id, "LuaScope", "%{public}s", name);
    return 0;
}

// Ext.Utils.ProfileEnd()
static int lua_utils_profileend(lua_State *L) {
    (void)L;
    UtilsProfileStack *stack = &g_profile_stack;
    if (stack->depth <= 0) {
        LOG_LUA_WARN("Ext.Utils.ProfileEnd: no matching ProfileBegin");
        return 0;
    }

    os_signpost_id_t id = stack->ids[--stack->depth];
    os_signpost_interval_end(utils_signpost_log(), id, "LuaScope");
    return 0;
}

// ============================================================================
// Registration
// ============================================================================

void lua_utils_register(lua_State *L) {
    static const luaL_Reg fns[] = {
        {"GameVersion", lua_utils_gameversion},
        {"Include", lua_utils_include},
        {"LoadString", lua_utils_loadstring},
        {"IsValidHandle", lua_utils_isvalidhandle},
        {"HandleToInteger", lua_utils_handletointeger},
        {"IntegerToHandle", lua_utils_integertohandle},
        {"GenerateGuid", lua_utils_generateguid},
        {"ShowError", lua_utils_showerror},
        {"ShowErrorAndExitGame", lua_utils_showerrorandexitgame},
        {"GetCommandLineParams", lua_utils_getcommandlineparams},
        {"GetMemoryUsage", lua_utils_getmemoryusage},
        {"ProfileBegin", lua_utils_profilebegin},
        {"ProfileEnd", lua_utils_profileend},
        {NULL, NULL},
    };

    luaL_setfuncs(L, fns, 0);
    LOG_LUA_INFO("Ext.Utils: registered %zu ported functions",
                 (sizeof(fns) / sizeof(fns[0])) - 1);
}
