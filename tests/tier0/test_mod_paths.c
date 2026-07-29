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
}
