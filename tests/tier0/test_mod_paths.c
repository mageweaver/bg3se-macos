/*
 * Tier 0 tests: mod_paths.c — SE directory-name resolution helpers.
 * Covers the display-name/PAK-directory mismatch fixes (#87, #81).
 */

#include "test_harness.h"
#include "mod_paths.h"

TEST(pak_stem_basic) {
    char dir[64];
    ASSERT_TRUE(mod_se_dir_from_pak_name("/a/b/BG3MCM.pak", dir, sizeof(dir)));
    ASSERT_STR_EQ(dir, "BG3MCM");
}

TEST(pak_stem_case_insensitive_ext) {
    char dir[64];
    ASSERT_TRUE(mod_se_dir_from_pak_name("/mods/Trials.PAK", dir, sizeof(dir)));
    ASSERT_STR_EQ(dir, "Trials");
}

TEST(pak_stem_no_directory) {
    char dir[64];
    ASSERT_TRUE(mod_se_dir_from_pak_name("Solo.pak", dir, sizeof(dir)));
    ASSERT_STR_EQ(dir, "Solo");
}

TEST(pak_stem_no_extension) {
    char dir[64];
    ASSERT_TRUE(mod_se_dir_from_pak_name("/a/NoExt", dir, sizeof(dir)));
    ASSERT_STR_EQ(dir, "NoExt");
}

TEST(pak_stem_spaces_preserved) {
    char dir[64];
    ASSERT_TRUE(mod_se_dir_from_pak_name("/m/Trials of Tav - Reloaded.pak",
                                         dir, sizeof(dir)));
    ASSERT_STR_EQ(dir, "Trials of Tav - Reloaded");
}

TEST(pak_stem_rejects_empty) {
    char dir[64];
    ASSERT_FALSE(mod_se_dir_from_pak_name("/a/b/.pak", dir, sizeof(dir)));
    ASSERT_FALSE(mod_se_dir_from_pak_name("/a/b/", dir, sizeof(dir)));
}

TEST(pak_stem_rejects_overflow) {
    char dir[4];
    ASSERT_FALSE(mod_se_dir_from_pak_name("/a/LongName.pak", dir, sizeof(dir)));
}

TEST(entry_dir_match) {
    char dir[64];
    ASSERT_TRUE(mod_entry_se_config_dir(
        "Mods/BG3MCM/ScriptExtender/Config.json", dir, sizeof(dir)));
    ASSERT_STR_EQ(dir, "BG3MCM");
}

TEST(entry_dir_match_with_spaces) {
    char dir[64];
    ASSERT_TRUE(mod_entry_se_config_dir(
        "Mods/Trials of Tav/ScriptExtender/Config.json", dir, sizeof(dir)));
    ASSERT_STR_EQ(dir, "Trials of Tav");
}

TEST(entry_dir_rejects_wrong_prefix) {
    char dir[64];
    ASSERT_FALSE(mod_entry_se_config_dir(
        "Public/X/ScriptExtender/Config.json", dir, sizeof(dir)));
    ASSERT_FALSE(mod_entry_se_config_dir(
        "mods/X/ScriptExtender/Config.json", dir, sizeof(dir)));
}

TEST(entry_dir_rejects_nested_dir) {
    char dir[64];
    ASSERT_FALSE(mod_entry_se_config_dir(
        "Mods/A/B/ScriptExtender/Config.json", dir, sizeof(dir)));
}

TEST(entry_dir_rejects_deeper_path) {
    char dir[64];
    ASSERT_FALSE(mod_entry_se_config_dir(
        "Mods/A/ScriptExtender/Config.json.bak", dir, sizeof(dir)));
    ASSERT_FALSE(mod_entry_se_config_dir(
        "Mods/A/ScriptExtender/Lua/BootstrapServer.lua", dir, sizeof(dir)));
}

TEST(entry_dir_rejects_empty_dir) {
    char dir[64];
    ASSERT_FALSE(mod_entry_se_config_dir(
        "Mods//ScriptExtender/Config.json", dir, sizeof(dir)));
}

TEST(entry_dir_rejects_overflow) {
    char dir[4];
    ASSERT_FALSE(mod_entry_se_config_dir(
        "Mods/LongDirName/ScriptExtender/Config.json", dir, sizeof(dir)));
}

// ---------------------------------------------------------------------------
// mod_meta_declares — a PAK's meta.lsx must name the mod being resolved.
//
// Regression: the PAK-filename fallback in mod_pak_find_se_dir tests the PAK's
// own stem, not the requested mod, so it matched unconditionally and the first
// SE-capable PAK the directory scan reached claimed all 128 installed mods.
// MCM was resolved to chooseyourstats.pak and its BootstrapClient.lua never
// loaded, so the configuration menu never appeared.
// ---------------------------------------------------------------------------

// Shape taken verbatim from a real mod: a Dependencies node naming BG3MCM,
// followed by the ModuleInfo node that actually describes this mod.
static const char *META_DEPENDS_ON_MCM =
    "<?xml version=\"1.0\" encoding=\"utf-8\"?>\n"
    "<save><region id=\"Config\"><node id=\"root\"><children>\n"
    "  <node id=\"Dependencies\"><children>\n"
    "    <node id=\"ModuleShortDesc\">\n"
    "      <attribute id=\"Folder\" type=\"LSString\" value=\"BG3MCM\" />\n"
    "      <attribute id=\"Name\" type=\"LSString\" value=\"Mod Configuration Menu\" />\n"
    "    </node>\n"
    "  </children></node>\n"
    "  <node id=\"ModuleInfo\">\n"
    "    <attribute id=\"Author\" type=\"LSString\" value=\"Doya Solutions\" />\n"
    "    <attribute id=\"Folder\" type=\"LSString\" value=\"CleanMyHotbar\" />\n"
    "    <attribute id=\"Name\" type=\"LSString\" value=\"Clean My Hotbar\" />\n"
    "  </node>\n"
    "</children></node></region></save>\n";

static const char *META_MCM =
    "<save><region id=\"Config\"><node id=\"root\"><children>\n"
    "  <node id=\"ModuleInfo\">\n"
    "    <attribute id=\"Folder\" type=\"LSString\" value=\"BG3MCM\" />\n"
    "    <attribute id=\"Name\" type=\"LSString\" value=\"Mod Configuration Menu\" />\n"
    "  </node>\n"
    "</children></node></region></save>\n";

TEST(meta_declares_matches_folder) {
    ASSERT_TRUE(mod_meta_declares(META_MCM, "BG3MCM"));
}

TEST(meta_declares_matches_display_name) {
    // #87: modsettings.lsx may carry the display name rather than the folder.
    ASSERT_TRUE(mod_meta_declares(META_MCM, "Mod Configuration Menu"));
}

TEST(meta_declares_ignores_dependencies) {
    // The bug: this mod merely depends on MCM and must not claim to be it.
    ASSERT_FALSE(mod_meta_declares(META_DEPENDS_ON_MCM, "BG3MCM"));
    ASSERT_FALSE(mod_meta_declares(META_DEPENDS_ON_MCM, "Mod Configuration Menu"));
    // Its own identity still resolves.
    ASSERT_TRUE(mod_meta_declares(META_DEPENDS_ON_MCM, "CleanMyHotbar"));
    ASSERT_TRUE(mod_meta_declares(META_DEPENDS_ON_MCM, "Clean My Hotbar"));
}

TEST(meta_declares_rejects_unrelated_mod) {
    ASSERT_FALSE(mod_meta_declares(META_MCM, "chooseyourstats"));
}

TEST(meta_declares_requires_exact_value) {
    // Prefixes and suffixes of a real value must not match.
    ASSERT_FALSE(mod_meta_declares(META_MCM, "BG3"));
    ASSERT_FALSE(mod_meta_declares(META_MCM, "BG3MCMExtra"));
}

TEST(meta_declares_ignores_other_attributes) {
    // Author is not an identity attribute.
    ASSERT_FALSE(mod_meta_declares(META_DEPENDS_ON_MCM, "Doya Solutions"));
}

TEST(meta_declares_fails_closed) {
    ASSERT_FALSE(mod_meta_declares(NULL, "BG3MCM"));
    ASSERT_FALSE(mod_meta_declares(META_MCM, NULL));
    ASSERT_FALSE(mod_meta_declares(META_MCM, ""));
    // No ModuleInfo node at all - not evidence of anything.
    ASSERT_FALSE(mod_meta_declares("<save><node id=\"root\"/></save>", "BG3MCM"));
}

void register_mod_paths_tests(void) {
    printf("mod_paths:\n");
    RUN_TEST(pak_stem_basic);
    RUN_TEST(pak_stem_case_insensitive_ext);
    RUN_TEST(pak_stem_no_directory);
    RUN_TEST(pak_stem_no_extension);
    RUN_TEST(pak_stem_spaces_preserved);
    RUN_TEST(pak_stem_rejects_empty);
    RUN_TEST(pak_stem_rejects_overflow);
    RUN_TEST(entry_dir_match);
    RUN_TEST(entry_dir_match_with_spaces);
    RUN_TEST(entry_dir_rejects_wrong_prefix);
    RUN_TEST(entry_dir_rejects_nested_dir);
    RUN_TEST(entry_dir_rejects_deeper_path);
    RUN_TEST(entry_dir_rejects_empty_dir);
    RUN_TEST(entry_dir_rejects_overflow);
    RUN_TEST(meta_declares_matches_folder);
    RUN_TEST(meta_declares_matches_display_name);
    RUN_TEST(meta_declares_ignores_dependencies);
    RUN_TEST(meta_declares_rejects_unrelated_mod);
    RUN_TEST(meta_declares_requires_exact_value);
    RUN_TEST(meta_declares_ignores_other_attributes);
    RUN_TEST(meta_declares_fails_closed);
}
