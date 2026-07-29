/**
 * BG3SE-macOS - Mod Path Helpers Implementation
 */

#include "mod_paths.h"

#include <string.h>
#include <strings.h>  // for strcasecmp

int mod_se_dir_from_pak_name(const char *pak_path, char *dir_out, size_t dir_size) {
    const char *base = strrchr(pak_path, '/');
    base = base ? base + 1 : pak_path;

    size_t len = strlen(base);
    if (len >= 4 && strcasecmp(base + len - 4, ".pak") == 0) {
        len -= 4;
    }
    if (len == 0 || len >= dir_size) return 0;

    memcpy(dir_out, base, len);
    dir_out[len] = '\0';
    return 1;
}

int mod_entry_se_config_dir(const char *entry_name, char *dir_out, size_t dir_size) {
    static const char prefix[] = "Mods/";
    static const char suffix[] = "/ScriptExtender/Config.json";

    if (strncmp(entry_name, prefix, sizeof(prefix) - 1) != 0) return 0;

    const char *dir_start = entry_name + sizeof(prefix) - 1;
    const char *suffix_pos = strstr(dir_start, suffix);
    if (!suffix_pos || suffix_pos == dir_start) return 0;

    // The suffix must terminate the entry path
    if (suffix_pos[sizeof(suffix) - 1] != '\0') return 0;

    // <dir> must be a single path component
    size_t len = (size_t)(suffix_pos - dir_start);
    if (memchr(dir_start, '/', len)) return 0;
    if (len >= dir_size) return 0;

    memcpy(dir_out, dir_start, len);
    dir_out[len] = '\0';
    return 1;
}
