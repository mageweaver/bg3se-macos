/**
 * BG3SE-macOS - Mod Loader Implementation
 *
 * Parses modsettings.lsx and detects Script Extender mods.
 */

#include "mod_loader.h"
#include "mod_paths.h"
#include "logging.h"
#include "pak_reader.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dirent.h>
#include <strings.h>  // for strcasecmp

#include <lauxlib.h>

// ============================================================================
// Internal State
// ============================================================================

// Detected mods from modsettings.lsx
static char detected_mods[MAX_MODS][MAX_MOD_NAME_LEN];
static char detected_uuids[MAX_MODS][64];  // parallel to detected_mods (mod UUIDs)
static int detected_mod_count = 0;

// Detected SE mods (mods with ScriptExtender/Config.json containing "Lua").
// se_mods holds the modsettings.lsx Folder name; se_mod_dirs holds the
// resolved internal PAK directory name, which is what every Mods/<dir>/...
// path must be built from (#87, #81). se_mod_uuids parallels se_mods.
static char se_mods[MAX_MODS][MAX_MOD_NAME_LEN];
static char se_mod_dirs[MAX_MODS][MAX_MOD_NAME_LEN];
static char se_mod_uuids[MAX_MODS][64];
static int se_mod_count = 0;

// Current mod context (for Ext.Require)
static char current_mod_name[256] = "";
static char current_mod_lua_base[MAX_PATH_LEN] = "";
static char current_mod_pak_path[MAX_PATH_LEN] = "";

// ============================================================================
// Internal Helpers
// ============================================================================

/**
 * Check if a file contains a specific string.
 */
static int file_contains_string(const char *filepath, const char *search_str) {
    FILE *f = fopen(filepath, "r");
    if (!f) return 0;

    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);

    if (size <= 0 || size > 1024 * 1024) {  // Sanity check: max 1MB
        fclose(f);
        return 0;
    }

    char *content = (char *)malloc(size + 1);
    if (!content) {
        fclose(f);
        return 0;
    }

    fread(content, 1, size, f);
    content[size] = '\0';
    fclose(f);

    int found = (strstr(content, search_str) != NULL);
    free(content);
    return found;
}

/**
 * Check if a mod has ScriptExtender support.
 * On success writes the mod's resolved internal directory name to dir_out
 * (equal to mod_name except when a PAK resolves via its filename stem).
 */
static int check_mod_has_script_extender(const char *mod_name,
                                         char *dir_out, size_t dir_size) {
    char config_path[MAX_PATH_LEN];

    // Default resolution: directory named after the mod
    snprintf(dir_out, dir_size, "%s", mod_name);

    // Location 1: Extracted mod in /tmp/<ModName>_extracted/
    snprintf(config_path, sizeof(config_path),
             "/tmp/%s_extracted/Mods/%s/ScriptExtender/Config.json",
             mod_name, mod_name);
    if (file_contains_string(config_path, "\"Lua\"")) {
        LOG_MOD_INFO("Found Config.json with Lua for %s at: %s", mod_name, config_path);
        return 1;
    }

    // Location 2: Short extracted name (e.g., mrc_extracted for MoreReactiveCompanions_Configurable)
    const char *short_names[] = {"mrc", "se", "mod", NULL};
    for (int i = 0; short_names[i] != NULL; i++) {
        snprintf(config_path, sizeof(config_path),
                 "/tmp/%s_extracted/Mods/%s/ScriptExtender/Config.json",
                 short_names[i], mod_name);
        if (file_contains_string(config_path, "\"Lua\"")) {
            LOG_MOD_INFO("Found Config.json with Lua for %s at: %s", mod_name, config_path);
            return 1;
        }
    }

    // Location 3: User's Mods folder (unpacked mod)
    const char *home = getenv("HOME");
    if (home) {
        snprintf(config_path, sizeof(config_path),
                 "%s/Documents/Larian Studios/Baldur's Gate 3/Mods/%s/ScriptExtender/Config.json",
                 home, mod_name);
        if (file_contains_string(config_path, "\"Lua\"")) {
            LOG_MOD_INFO("Found Config.json with Lua for %s at: %s", mod_name, config_path);
            return 1;
        }
    }

    // Location 4: PAK file in Mods folder
    if (home) {
        char mods_dir[MAX_PATH_LEN];
        snprintf(mods_dir, sizeof(mods_dir),
                 "%s/Documents/Larian Studios/Baldur's Gate 3/Mods", home);

        DIR *dir = opendir(mods_dir);
        if (dir) {
            struct dirent *entry;
            while ((entry = readdir(dir)) != NULL) {
                size_t name_len = strlen(entry->d_name);
                if (name_len > 4 && strcasecmp(entry->d_name + name_len - 4, ".pak") == 0) {
                    char pak_path[MAX_PATH_LEN];
                    snprintf(pak_path, sizeof(pak_path), "%s/%s", mods_dir, entry->d_name);

                    if (mod_pak_find_se_dir(pak_path, mod_name, dir_out, dir_size)) {
                        LOG_MOD_INFO("Found SE mod %s in PAK: %s (dir: %s)",
                                     mod_name, pak_path, dir_out);
                        closedir(dir);
                        return 1;
                    }
                }
            }
            closedir(dir);
        }
    }

    return 0;
}

// ============================================================================
// PAK File Helpers
// ============================================================================

/**
 * Read a PAK entry and check it declares the "Lua" feature flag.
 */
static int pak_entry_has_lua(PakFile *pak, int entry_idx) {
    size_t size;
    char *content = pak_read_file(pak, entry_idx, &size);
    if (!content) return 0;

    int has_lua = (strstr(content, "\"Lua\"") != NULL);
    free(content);
    return has_lua;
}

int mod_pak_find_se_dir(const char *pak_path, const char *mod_name,
                        char *dir_out, size_t dir_size) {
    PakFile *pak = pak_open(pak_path);
    if (!pak) return 0;

    // Candidate 1: the modsettings.lsx display name.
    // Candidate 2: the PAK filename stem — the internal directory frequently
    // matches the file, not the display name (#87: "Mod Configuration Menu"
    // in modsettings vs Mods/BG3MCM/ inside BG3MCM.pak).
    char pak_stem[MAX_MOD_NAME_LEN];
    const char *candidates[2] = { mod_name, NULL };
    if (mod_se_dir_from_pak_name(pak_path, pak_stem, sizeof(pak_stem)) &&
        strcmp(pak_stem, mod_name) != 0) {
        candidates[1] = pak_stem;
    }

    for (int c = 0; c < 2; c++) {
        if (!candidates[c]) continue;

        char config_path[512];
        snprintf(config_path, sizeof(config_path),
                 "Mods/%s/ScriptExtender/Config.json", candidates[c]);

        int entry_idx = pak_find_entry(pak, config_path);
        if (entry_idx >= 0 && pak_entry_has_lua(pak, entry_idx)) {
            if (dir_out && dir_size) {
                snprintf(dir_out, dir_size, "%s", candidates[c]);
            }
            if (c > 0) {
                LOG_MOD_INFO("SE dir for '%s' resolved via PAK filename: %s",
                             mod_name, candidates[c]);
            }
            pak_close(pak);
            return 1;
        }
    }

    pak_close(pak);
    return 0;
}

int mod_pak_has_script_extender(const char *pak_path, const char *mod_name) {
    return mod_pak_find_se_dir(pak_path, mod_name, NULL, 0);
}

char *mod_pak_get_config_json(const char *dir_name) {
    char pak_path[MAX_PATH_LEN];
    if (!mod_find_pak(dir_name, pak_path, sizeof(pak_path))) return NULL;

    PakFile *pak = pak_open(pak_path);
    if (!pak) return NULL;

    char config_path[512];
    snprintf(config_path, sizeof(config_path),
             "Mods/%s/ScriptExtender/Config.json", dir_name);

    int entry_idx = pak_find_entry(pak, config_path);
    if (entry_idx < 0) {
        pak_close(pak);
        return NULL;
    }

    size_t size;
    char *content = pak_read_file(pak, entry_idx, &size);
    pak_close(pak);
    return content;
}

int mod_find_pak(const char *mod_name, char *pak_path_out, size_t pak_path_size) {
    const char *home = getenv("HOME");
    if (!home) return 0;

    char mods_dir[MAX_PATH_LEN];
    snprintf(mods_dir, sizeof(mods_dir),
             "%s/Documents/Larian Studios/Baldur's Gate 3/Mods", home);

    DIR *dir = opendir(mods_dir);
    if (!dir) return 0;

    struct dirent *entry;
    while ((entry = readdir(dir)) != NULL) {
        size_t name_len = strlen(entry->d_name);
        if (name_len > 4 && strcasecmp(entry->d_name + name_len - 4, ".pak") == 0) {
            char pak_path[MAX_PATH_LEN];
            snprintf(pak_path, sizeof(pak_path), "%s/%s", mods_dir, entry->d_name);

            // Check if this PAK contains our mod
            PakFile *pak = pak_open(pak_path);
            if (pak) {
                // Look for any file with our mod name in the path
                char mod_prefix[512];
                snprintf(mod_prefix, sizeof(mod_prefix), "Mods/%s/", mod_name);

                for (uint32_t i = 0; i < pak->num_files; i++) {
                    if (strncmp(pak->entries[i].name, mod_prefix, strlen(mod_prefix)) == 0) {
                        pak_close(pak);
                        closedir(dir);
                        strncpy(pak_path_out, pak_path, pak_path_size - 1);
                        pak_path_out[pak_path_size - 1] = '\0';
                        return 1;
                    }
                }
                pak_close(pak);
            }
        }
    }

    closedir(dir);
    return 0;
}

static mod_chunk_env_hook_t g_chunk_env_hook = NULL;

void mod_loader_set_chunk_env_hook(mod_chunk_env_hook_t hook) {
    g_chunk_env_hook = hook;
}

int mod_load_lua_from_pak(lua_State *L, const char *pak_path, const char *lua_path) {
    PakFile *pak = pak_open(pak_path);
    if (!pak) return 0;

    int entry_idx = pak_find_entry(pak, lua_path);
    if (entry_idx < 0) {
        pak_close(pak);
        return 0;
    }

    size_t size;
    char *content = pak_read_file(pak, entry_idx, &size);
    pak_close(pak);

    if (!content) return 0;

    // Load the chunk (do NOT execute yet): we must install the per-mod _ENV on the
    // loaded function before running it, so mods like MCM whose API lives on their
    // ModTable (Mods.<ModTable>) can reference it as a bare global.
    if (luaL_loadbuffer(L, content, size, lua_path) != LUA_OK) {
        const char *error = lua_tostring(L, -1);
        LOG_LUA_ERROR("PAK compile error (%s): %s", lua_path, error);
        lua_pop(L, 1);
        free(content);
        return 0;
    }
    free(content);

    // Install per-mod _ENV on the freshly loaded chunk (no-op if none is active).
    if (g_chunk_env_hook) g_chunk_env_hook(L);

    if (lua_pcall(L, 0, LUA_MULTRET, 0) != LUA_OK) {
        const char *error = lua_tostring(L, -1);
        LOG_LUA_ERROR("PAK load error (%s): %s", lua_path, error);
        lua_pop(L, 1);
        return 0;
    }

    LOG_LUA_INFO("Loaded from PAK: %s", lua_path);
    return 1;
}

// ============================================================================
// Mod Detection API
// ============================================================================

void mod_detect_enabled(void) {
    // Reset detected mods
    detected_mod_count = 0;
    se_mod_count = 0;
    memset(se_mod_dirs, 0, sizeof(se_mod_dirs));

    // Build path to modsettings.lsx
    const char *home = getenv("HOME");
    if (!home) {
        LOG_MOD_ERROR("Could not get HOME environment variable");
        return;
    }

    char path[1024];
    snprintf(path, sizeof(path),
             "%s/Documents/Larian Studios/Baldur's Gate 3/PlayerProfiles/Public/modsettings.lsx",
             home);

    FILE *f = fopen(path, "r");
    if (!f) {
        LOG_MOD_ERROR("Could not open modsettings.lsx at: %s", path);
        return;
    }

    // Read entire file
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);

    char *content = (char *)malloc(size + 1);
    if (!content) {
        fclose(f);
        LOG_MOD_ERROR("Out of memory reading modsettings.lsx");
        return;
    }

    fread(content, 1, size, f);
    content[size] = '\0';
    fclose(f);

    // Count and extract mod names
    LOG_MOD_INFO("=== Enabled Mods ===");

    int mod_count = 0;
    char *ptr = content;
    // Key on the mod's Folder, NOT its Name. ScriptExtender files inside a pak
    // live at Mods/<Folder>/ScriptExtender/..., and the Folder can differ from
    // the display Name (e.g. "Sit This One Out 2" -> folder "Sit This One
    // Out"; MCM's modsettings Folder is "Mod Configuration Menu" and only the
    // PAK-stem/se_mod_dirs fallback resolves its real "BG3MCM" directory).
    // Parsing Name here caused such mods' bootstraps to never load. Folder
    // values are also unescaped identifiers, avoiding &apos;-style HTML
    // entities present in Names.
    const char *name_marker = "attribute id=\"Folder\" type=\"LSString\" value=\"";
    size_t marker_len = strlen(name_marker);

    while ((ptr = strstr(ptr, name_marker)) != NULL) {
        ptr += marker_len;

        // Find the closing quote
        char *end = strchr(ptr, '"');
        if (end) {
            size_t name_len = end - ptr;
            if (name_len < MAX_MOD_NAME_LEN && detected_mod_count < MAX_MODS) {
                char mod_name[MAX_MOD_NAME_LEN];
                strncpy(mod_name, ptr, name_len);
                mod_name[name_len] = '\0';

                // Store in detected mods array
                strncpy(detected_mods[detected_mod_count], mod_name, MAX_MOD_NAME_LEN - 1);
                detected_mods[detected_mod_count][MAX_MOD_NAME_LEN - 1] = '\0';

                // Capture this ModuleShortDesc's UUID (appears after Folder, before
                // the next node) so we can set the ModuleUUID Lua global per mod.
                detected_uuids[detected_mod_count][0] = '\0';
                {
                    const char *uuid_marker = "id=\"UUID\" type=\"guid\" value=\"";
                    char *next_folder = strstr(end, name_marker);
                    char *uuid_pos = strstr(end, uuid_marker);
                    if (uuid_pos && (!next_folder || uuid_pos < next_folder)) {
                        uuid_pos += strlen(uuid_marker);
                        char *uend = strchr(uuid_pos, '"');
                        if (uend) {
                            size_t ulen = (size_t)(uend - uuid_pos);
                            if (ulen < sizeof(detected_uuids[0])) {
                                strncpy(detected_uuids[detected_mod_count], uuid_pos, ulen);
                                detected_uuids[detected_mod_count][ulen] = '\0';
                            }
                        }
                    }
                }
                detected_mod_count++;

                mod_count++;
                if (strcmp(mod_name, "GustavX") == 0) {
                    LOG_MOD_INFO("  [%d] %s (base game)", mod_count, mod_name);
                } else {
                    LOG_MOD_INFO("  [%d] %s", mod_count, mod_name);
                }
            }
            ptr = end;
        }
    }

    LOG_MOD_INFO("Total mods: %d (%d user mods)", mod_count, mod_count > 0 ? mod_count - 1 : 0);
    LOG_MOD_INFO("====================");

    free(content);

    // Now check which mods have Script Extender support
    LOG_MOD_INFO("=== Scanning for SE Mods ===");
    for (int i = 0; i < detected_mod_count; i++) {
        // Skip base game
        if (strcmp(detected_mods[i], "GustavX") == 0) continue;

        char mod_dir[MAX_MOD_NAME_LEN];
        if (check_mod_has_script_extender(detected_mods[i], mod_dir, sizeof(mod_dir))) {
            if (se_mod_count < MAX_MODS) {
                strncpy(se_mods[se_mod_count], detected_mods[i], MAX_MOD_NAME_LEN - 1);
                se_mods[se_mod_count][MAX_MOD_NAME_LEN - 1] = '\0';
                strncpy(se_mod_dirs[se_mod_count], mod_dir, MAX_MOD_NAME_LEN - 1);
                se_mod_dirs[se_mod_count][MAX_MOD_NAME_LEN - 1] = '\0';
                strncpy(se_mod_uuids[se_mod_count], detected_uuids[i], sizeof(se_mod_uuids[0]) - 1);
                se_mod_uuids[se_mod_count][sizeof(se_mod_uuids[0]) - 1] = '\0';
                se_mod_count++;
                LOG_MOD_INFO("  [SE] %s", detected_mods[i]);
            }
        }
    }

    if (se_mod_count == 0) {
        LOG_MOD_INFO("  No SE mods in modsettings.lsx");
    } else {
        LOG_MOD_INFO("Total SE mods from modsettings: %d", se_mod_count);
    }
    LOG_MOD_INFO("============================");

    // Scan ~/Documents/Larian Studios/Baldur's Gate 3/Mods/ for SE mods not in modsettings.lsx
    // Windows BG3SE only bootstraps SE mods that are in the active load order.
    // The macOS port additionally scanned the Mods folder and loaded anything
    // with a ScriptExtender/Config.json, whether or not the player had enabled
    // it. That is convenient with a handful of mods and catastrophic with a
    // large library: a 1205-PAK install bootstrapped 72 unenabled mods, many of
    // which threw during load (MCM's whole init chain among them), and the
    // resulting cascade aborted session start so no save or new game could be
    // entered. It also diverges from Windows, where a disabled mod is inert.
    //
    // Default to the Windows contract. Set BG3SE_LOAD_UNREGISTERED_MODS=1 to
    // restore the previous scan-everything behavior.
    bool load_unregistered = (getenv("BG3SE_LOAD_UNREGISTERED_MODS") != NULL);
    if (!load_unregistered) {
        LOG_MOD_INFO("=== Skipping Mods-folder scan (load order is authoritative) ===");
        LOG_MOD_INFO("  %d SE mod(s) from modsettings.lsx will load.", se_mod_count);
        LOG_MOD_INFO("  Set BG3SE_LOAD_UNREGISTERED_MODS=1 to also load mods "
                     "that are installed but not enabled.");
        LOG_MOD_INFO("=========================================");
        return;
    }

    LOG_MOD_INFO("=== Scanning Mods Folder for SE Mods ===");
    LOG_MOD_WARN("  BG3SE_LOAD_UNREGISTERED_MODS=1: loading mods that are NOT "
                 "in the load order. This diverges from Windows and can abort "
                 "session start if any of them fail to bootstrap.");
    if (home) {
        char mods_dir[MAX_PATH_LEN];
        snprintf(mods_dir, sizeof(mods_dir),
                 "%s/Documents/Larian Studios/Baldur's Gate 3/Mods", home);

        DIR *dir = opendir(mods_dir);
        if (dir) {
            struct dirent *entry;
            while ((entry = readdir(dir)) != NULL) {
                // Skip . and ..
                if (entry->d_name[0] == '.') continue;

                // Check if already in SE mods list — compare the resolved dir
                // too, or a Phase 1 entry detected under its display name
                // would be re-added here under its directory name
                int already_added = 0;
                for (int i = 0; i < se_mod_count; i++) {
                    if (strcmp(se_mods[i], entry->d_name) == 0 ||
                        strcmp(se_mod_dirs[i], entry->d_name) == 0) {
                        already_added = 1;
                        break;
                    }
                }
                if (already_added) continue;

                // Check if this mod has ScriptExtender support
                char config_path[MAX_PATH_LEN];
                snprintf(config_path, sizeof(config_path),
                         "%s/%s/ScriptExtender/Config.json",
                         mods_dir, entry->d_name);

                if (file_contains_string(config_path, "\"Lua\"")) {
                    if (se_mod_count < MAX_MODS) {
                        strncpy(se_mods[se_mod_count], entry->d_name, MAX_MOD_NAME_LEN - 1);
                        se_mods[se_mod_count][MAX_MOD_NAME_LEN - 1] = '\0';
                        strncpy(se_mod_dirs[se_mod_count], entry->d_name, MAX_MOD_NAME_LEN - 1);
                        se_mod_dirs[se_mod_count][MAX_MOD_NAME_LEN - 1] = '\0';
                        se_mod_uuids[se_mod_count][0] = '\0';  // UUID unknown for folder-scan mods
                        se_mod_count++;
                        LOG_MOD_INFO("  [SE] %s (from Mods folder)", entry->d_name);
                    }
                }
            }
            closedir(dir);
        } else {
            LOG_MOD_INFO("  Could not open Mods folder: %s", mods_dir);
        }
    }
    LOG_MOD_INFO("=========================================");

    // Phase 3: enumerate every PAK's Mods/<dir>/ScriptExtender/Config.json and
    // add any SE dir not attributed above. Catches mods whose internal
    // directory matches neither the display name nor the PAK filename (#81),
    // and PAKs bundling multiple SE mods.
    LOG_MOD_INFO("=== Scanning PAKs for Unattributed SE Mods ===");
    if (home) {
        char mods_dir[MAX_PATH_LEN];
        snprintf(mods_dir, sizeof(mods_dir),
                 "%s/Documents/Larian Studios/Baldur's Gate 3/Mods", home);

        DIR *dir = opendir(mods_dir);
        if (dir) {
            struct dirent *entry;
            while ((entry = readdir(dir)) != NULL && se_mod_count < MAX_MODS) {
                size_t name_len = strlen(entry->d_name);
                if (name_len <= 4 ||
                    strcasecmp(entry->d_name + name_len - 4, ".pak") != 0) {
                    continue;
                }

                char pak_path[MAX_PATH_LEN];
                snprintf(pak_path, sizeof(pak_path), "%s/%s", mods_dir, entry->d_name);

                PakFile *pak = pak_open(pak_path);
                if (!pak) continue;

                for (uint32_t i = 0; i < pak->num_files && se_mod_count < MAX_MODS; i++) {
                    char se_dir[MAX_MOD_NAME_LEN];
                    if (!mod_entry_se_config_dir(pak->entries[i].name,
                                                 se_dir, sizeof(se_dir))) {
                        continue;
                    }

                    int already_added = 0;
                    for (int j = 0; j < se_mod_count; j++) {
                        if (strcmp(se_mod_dirs[j], se_dir) == 0) {
                            already_added = 1;
                            break;
                        }
                    }
                    if (already_added) continue;

                    if (!pak_entry_has_lua(pak, (int)i)) continue;

                    strncpy(se_mods[se_mod_count], se_dir, MAX_MOD_NAME_LEN - 1);
                    se_mods[se_mod_count][MAX_MOD_NAME_LEN - 1] = '\0';
                    strncpy(se_mod_dirs[se_mod_count], se_dir, MAX_MOD_NAME_LEN - 1);
                    se_mod_dirs[se_mod_count][MAX_MOD_NAME_LEN - 1] = '\0';
                    se_mod_uuids[se_mod_count][0] = '\0';  // UUID unknown for PAK-scan mods
                    se_mod_count++;
                    LOG_MOD_INFO("  [SE] %s (from PAK %s)", se_dir, entry->d_name);
                }
                pak_close(pak);
            }
            closedir(dir);
        }
    }
    LOG_MOD_INFO("==============================================");
}

int mod_get_detected_count(void) {
    return detected_mod_count;
}

const char *mod_get_detected_name(int index) {
    if (index < 0 || index >= detected_mod_count) return NULL;
    return detected_mods[index];
}

const char *mod_get_detected_uuid(int index) {
    if (index < 0 || index >= detected_mod_count) return NULL;
    return detected_uuids[index];
}

int mod_get_se_count(void) {
    return se_mod_count;
}

const char *mod_get_se_name(int index) {
    if (index < 0 || index >= se_mod_count) return NULL;
    return se_mods[index];
}

const char *mod_get_se_uuid(int index) {
    if (index < 0 || index >= se_mod_count) return NULL;
    return se_mod_uuids[index];
}

const char *mod_get_se_dir(int index) {
    if (index < 0 || index >= se_mod_count) return NULL;
    return se_mod_dirs[index];
}

// ============================================================================
// Current Mod State
// ============================================================================

void mod_set_current(const char *mod_name, const char *lua_base_path, const char *pak_path) {
    if (mod_name) {
        strncpy(current_mod_name, mod_name, sizeof(current_mod_name) - 1);
        current_mod_name[sizeof(current_mod_name) - 1] = '\0';
    } else {
        current_mod_name[0] = '\0';
    }

    if (lua_base_path) {
        strncpy(current_mod_lua_base, lua_base_path, sizeof(current_mod_lua_base) - 1);
        current_mod_lua_base[sizeof(current_mod_lua_base) - 1] = '\0';
    } else {
        current_mod_lua_base[0] = '\0';
    }

    if (pak_path) {
        strncpy(current_mod_pak_path, pak_path, sizeof(current_mod_pak_path) - 1);
        current_mod_pak_path[sizeof(current_mod_pak_path) - 1] = '\0';
    } else {
        current_mod_pak_path[0] = '\0';
    }
}

const char *mod_get_current_name(void) {
    return current_mod_name;
}

const char *mod_get_current_lua_base(void) {
    return current_mod_lua_base;
}

const char *mod_get_current_pak_path(void) {
    return current_mod_pak_path;
}
