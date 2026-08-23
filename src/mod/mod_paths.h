/**
 * BG3SE-macOS - Mod Path Helpers
 *
 * Pure string helpers for resolving a mod's internal PAK directory name.
 * The display name in modsettings.lsx frequently differs from the directory
 * under Mods/ inside the PAK (e.g. "Mod Configuration Menu" vs "BG3MCM"),
 * so SE detection must derive candidates from more than the display name.
 *
 * No filesystem or PAK dependencies — unit-tested in Tier 0.
 */

#ifndef BG3SE_MOD_PATHS_H
#define BG3SE_MOD_PATHS_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Derive a mod directory candidate from a PAK file path: the basename with
 * any .pak extension (case-insensitive) stripped.
 * @return 1 if dir_out was written, 0 if the stem is empty or doesn't fit
 */
int mod_se_dir_from_pak_name(const char *pak_path, char *dir_out, size_t dir_size);

/**
 * If entry_name is exactly "Mods/<dir>/ScriptExtender/Config.json" for a
 * single path component <dir>, extract that directory name.
 * @return 1 if dir_out was written, 0 if the entry doesn't match
 */
int mod_entry_se_config_dir(const char *entry_name, char *dir_out, size_t dir_size);

/**
 * Decide whether a mod's meta.lsx describes the mod named mod_name, by
 * matching the Folder or Name attribute of its ModuleInfo node.
 *
 * Only ModuleInfo is consulted. The sibling Dependencies node lists other
 * mods' Folder and Name using identical markup, so a whole-document search
 * makes every dependent of X claim to be X.
 *
 * @param meta_xml Contents of meta.lsx (NUL-terminated); may be NULL
 * @param mod_name The modsettings.lsx Folder name being resolved
 * @return 1 if ModuleInfo declares this mod, 0 otherwise
 */
int mod_meta_declares(const char *meta_xml, const char *mod_name);

#ifdef __cplusplus
}
#endif

#endif // BG3SE_MOD_PATHS_H
