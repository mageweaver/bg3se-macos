/**
 * BG3SE-macOS - Mod Path Helpers Implementation
 */

#include "mod_paths.h"

#include <stdio.h>
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

/**
 * Compare an LSX <attribute id="..." value="..."/> against an expected value,
 * searching only the [xml, xml_end) window. The search for value= is confined
 * to the tag its id= was found in, so an unrelated later attribute can never
 * satisfy the match.
 */
static int lsx_attr_equals(const char *xml, const char *xml_end,
                           const char *attr_id, const char *expected) {
    char needle[64];
    snprintf(needle, sizeof(needle), "id=\"%s\"", attr_id);

    size_t needle_len = strlen(needle);
    size_t expected_len = strlen(expected);

    for (const char *p = strstr(xml, needle); p && p < xml_end;
         p = strstr(p + needle_len, needle)) {
        const char *tag_end = strchr(p, '>');
        const char *val = strstr(p, "value=\"");
        if (!val || val >= xml_end) continue;
        if (tag_end && val > tag_end) continue;

        val += sizeof("value=\"") - 1;
        size_t val_len = strcspn(val, "\"");
        if (val_len == expected_len && strncmp(val, expected, val_len) == 0) return 1;
    }

    return 0;
}

int mod_meta_declares(const char *meta_xml, const char *mod_name) {
    if (!meta_xml || !mod_name || !*mod_name) return 0;

    const char *info = strstr(meta_xml, "id=\"ModuleInfo\"");
    if (!info) return 0;

    // ModuleInfo's own attributes end at its first child node or its close tag.
    const char *end = info + strlen(info);
    const char *children = strstr(info, "<children>");
    const char *close = strstr(info, "</node>");
    if (children && children < end) end = children;
    if (close && close < end) end = close;

    return lsx_attr_equals(info, end, "Folder", mod_name) ||
           lsx_attr_equals(info, end, "Name", mod_name);
}
