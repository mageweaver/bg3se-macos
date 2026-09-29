/*
 * Tier 0 tests: version_detect.c — build-identity helpers.
 *
 * The store suffix and the binary's LC_UUID are what gate address-dependent
 * features, because the version string cannot tell Steam and GOG apart (see
 * src/gen/build_identity.h). A regression here means a mismatched dylib
 * reads wrong memory instead of refusing to run.
 *
 * Mach-O headers are synthesised rather than read from an install: CI has no
 * copy of BG3.
 */

#include "test_harness.h"
#include "version_detect.h"
#include "build_identity.h"

#include <mach-o/loader.h>
#include <string.h>

/* ---------------------------------------------------------------- */
/* Store identification                                              */
/* ---------------------------------------------------------------- */

TEST(store_gog_suffix) {
    ASSERT_STR_EQ(version_detect_store_for_image_path(
        "/Users/x/Applications/Baldur's Gate 3.app/Contents/MacOS/Baldur's Gate 3 GOG"), "gog");
}

TEST(store_steam_suffix) {
    ASSERT_STR_EQ(version_detect_store_for_image_path(
        "/x/Baldur's Gate 3.app/Contents/MacOS/Baldur's Gate 3 Steam"), "steam");
}

/* The Steam layout has no suffix: CFBundleExecutable is the game binary. */
TEST(store_unsuffixed_is_steam) {
    ASSERT_STR_EQ(version_detect_store_for_image_path(
        "/x/Baldur's Gate 3.app/Contents/MacOS/Baldur's Gate 3"), "steam");
}

/* Install location must not matter: a GOG install reachable through a
 * Steam-shaped path is still GOG. */
TEST(store_ignores_install_path) {
    ASSERT_STR_EQ(version_detect_store_for_image_path(
        "/x/Steam/steamapps/common/Baldurs Gate 3/BG3.app/Contents/MacOS/Baldur's Gate 3 GOG"),
        "gog");
}

TEST(store_rejects_null_and_empty) {
    ASSERT_STR_EQ(version_detect_store_for_image_path(NULL), "unknown");
    ASSERT_STR_EQ(version_detect_store_for_image_path(""), "unknown");
}

/* " GOG" is a suffix, not a substring: neither a directory called "GOG" nor a
 * filename containing it is a store marker. */
TEST(store_suffix_not_substring) {
    ASSERT_STR_EQ(version_detect_store_for_image_path("/GOG/Games/Baldur's Gate 3"), "steam");
    ASSERT_STR_EQ(version_detect_store_for_image_path("/x/GOGLauncher"), "steam");
}

/* ---------------------------------------------------------------- */
/* Synthetic Mach-O images                                           */
/* ---------------------------------------------------------------- */

typedef struct {
    struct mach_header_64     mh;
    struct uuid_command       uuid;
    struct segment_command_64 text;
} FakeImage;

static void make_image_typed(FakeImage *img, uint64_t text_vmsize, uint32_t filetype) {
    memset(img, 0, sizeof(*img));
    img->mh.magic    = MH_MAGIC_64;
    img->mh.filetype = filetype;
    img->mh.ncmds  = 2;
    img->mh.sizeofcmds = sizeof(img->uuid) + sizeof(img->text);

    img->uuid.cmd     = LC_UUID;
    img->uuid.cmdsize = sizeof(img->uuid);
    for (int i = 0; i < 16; i++) img->uuid.uuid[i] = (uint8_t)(0x10 * i + i);

    img->text.cmd     = LC_SEGMENT_64;
    img->text.cmdsize = sizeof(img->text);
    strncpy(img->text.segname, SEG_TEXT, sizeof(img->text.segname));
    img->text.vmsize  = text_vmsize;
}

static void make_image(FakeImage *img, uint64_t text_vmsize) {
    make_image_typed(img, text_vmsize, MH_EXECUTE);
}

TEST(uuid_formats_uppercase_hyphenated) {
    FakeImage img; make_image(&img, 1024);
    char out[40] = {0};
    ASSERT_TRUE(version_detect_uuid_from_image(&img, out, sizeof(out)));
    /* bytes are 0x00,0x11,0x22,... — matches `dwarfdump --uuid` formatting */
    ASSERT_STR_EQ(out, "00112233-4455-6677-8899-AABBCCDDEEFF");
}

TEST(uuid_rejects_non_macho) {
    FakeImage img; make_image(&img, 1024);
    img.mh.magic = 0xDEADBEEF;
    char out[40] = {0};
    ASSERT_FALSE(version_detect_uuid_from_image(&img, out, sizeof(out)));
}

TEST(uuid_rejects_undersized_buffer) {
    FakeImage img; make_image(&img, 1024);
    char out[8] = {0};
    ASSERT_FALSE(version_detect_uuid_from_image(&img, out, sizeof(out)));
}

TEST(uuid_rejects_null) {
    char out[40] = {0};
    ASSERT_FALSE(version_detect_uuid_from_image(NULL, out, sizeof(out)));
}

/* A zero cmdsize would walk the load commands forever; bail instead. */
TEST(uuid_survives_malformed_load_command) {
    FakeImage img; make_image(&img, 1024);
    img.mh.ncmds  = 3;
    img.uuid.cmd  = LC_SEGMENT_64;   /* push LC_UUID out of reach */
    img.text.cmdsize = 0;
    char out[40] = {0};
    ASSERT_FALSE(version_detect_uuid_from_image(&img, out, sizeof(out)));
}

TEST(text_vmsize_reads_text_segment) {
    FakeImage img; make_image(&img, 138166272);  /* the real 4.1.1.7398727 game */
    ASSERT_EQ(version_detect_text_vmsize(&img), 138166272ULL);
}

/* Observed values: 32768 bytes of __TEXT for the stub, 138166272 for the game. */
TEST(stub_detected_and_game_is_not) {
    FakeImage stub; make_image(&stub, 32768);
    FakeImage game; make_image(&game, 138166272);
    ASSERT_TRUE(version_detect_is_launcher_stub(&stub));
    ASSERT_FALSE(version_detect_is_launcher_stub(&game));
}

/* Regression: the guard once read _dyld_get_image_header(0), which under
 * DYLD_INSERT_LIBRARIES is the INSERTED DYLIB, not the executable. libbg3se's
 * own __TEXT is ~3.5MB, so it matched the size test and the extender disabled
 * itself inside the very process it was meant to run in. Only an MH_EXECUTE
 * can be the launcher. */
TEST(a_dylib_is_never_a_stub) {
    FakeImage self; make_image_typed(&self, 3538944, MH_DYLIB);
    ASSERT_FALSE(version_detect_is_launcher_stub(&self));
    /* same size, but an executable: that IS a stub */
    FakeImage stub; make_image_typed(&stub, 3538944, MH_EXECUTE);
    ASSERT_TRUE(version_detect_is_launcher_stub(&stub));
}

TEST(unreadable_header_fails_open) {
    ASSERT_FALSE(version_detect_is_launcher_stub(NULL));
    FakeImage bad; make_image(&bad, 1024);
    bad.mh.magic = 0xFEEDFACE;
    ASSERT_FALSE(version_detect_is_launcher_stub(&bad));
    /* An MH_EXECUTE with no readable __TEXT must not be called a stub. */
    FakeImage noseg; make_image(&noseg, 0);
    ASSERT_FALSE(version_detect_is_launcher_stub(&noseg));
}

TEST(text_vmsize_zero_when_unreadable) {
    ASSERT_EQ(version_detect_text_vmsize(NULL), 0ULL);
    FakeImage img; make_image(&img, 1024);
    img.mh.magic = 0xFEEDFACE;   /* 32-bit magic: not a 64-bit image */
    ASSERT_EQ(version_detect_text_vmsize(&img), 0ULL);
}



/* ---------------------------------------------------------------- */
/* Build-id matching                                                 */
/* ---------------------------------------------------------------- */

/* Regression: generated tables stamp "<version>-<store>", while the detected
 * version from Info.plist never carries a suffix. Comparing them with strcmp
 * closed the TypeId gate on GOG, discovering 0 of 2004 components and making
 * every entity query return nothing, with the game otherwise running fine. */
TEST(build_id_matches_across_store_suffix) {
    /* Needs a detected version; version_detect_init() reads a plist we do not
     * have here, so this documents the shape rather than calling the real API.
     * The trimming rule: compare up to the last '-', if present. */
    const char *detected = "4.1.1.7398727";
    const char *ids[] = { "4.1.1.7398727", "4.1.1.7398727-gog", "4.1.1.7398727-steam" };
    for (size_t i = 0; i < sizeof(ids) / sizeof(ids[0]); i++) {
        const char *dash = strrchr(ids[i], '-');
        size_t len = dash ? (size_t)(dash - ids[i]) : strlen(ids[i]);
        ASSERT_TRUE(strlen(detected) == len && strncmp(detected, ids[i], len) == 0);
    }
}

/* A different game version must still be rejected, suffix or not. */
TEST(build_id_rejects_a_different_version) {
    const char *detected = "4.1.1.7398727";
    const char *ids[] = { "4.1.1.7209685", "4.1.1.7209685-gog" };
    for (size_t i = 0; i < sizeof(ids) / sizeof(ids[0]); i++) {
        const char *dash = strrchr(ids[i], '-');
        size_t len = dash ? (size_t)(dash - ids[i]) : strlen(ids[i]);
        ASSERT_FALSE(strlen(detected) == len && strncmp(detected, ids[i], len) == 0);
    }
}

TEST(build_id_rejects_null_and_empty) {
    ASSERT_FALSE(version_detect_build_id_matches(NULL));
    ASSERT_FALSE(version_detect_build_id_matches(""));
}

/* ---------------------------------------------------------------- */
/* Supported-build lookup                                            */
/* ---------------------------------------------------------------- */

/* One dylib carries several stores now, so the gate asks "is this store one of
 * mine", not "does this store equal my single compile-time target". */
TEST(identity_knows_both_shipped_stores) {
    ASSERT_TRUE(build_identity_supports_store("gog"));
    ASSERT_TRUE(build_identity_supports_store("steam"));
}

TEST(identity_rejects_unknown_stores) {
    ASSERT_FALSE(build_identity_supports_store("unknown"));
    ASSERT_FALSE(build_identity_supports_store("epic"));
    ASSERT_FALSE(build_identity_supports_store(""));
    ASSERT_FALSE(build_identity_supports_store(NULL));
}

/* Each store must carry its own UUID. Returning one store's UUID for the other
 * would let a mismatched binary pass the check that exists to catch it. */
TEST(identity_uuids_are_per_store_and_differ) {
    const char *gog = build_identity_uuid_for_store("gog");
    const char *steam = build_identity_uuid_for_store("steam");
    ASSERT_TRUE(gog != NULL && steam != NULL);
    ASSERT_TRUE(gog[0] != 0 && steam[0] != 0);
    ASSERT_FALSE(strcmp(gog, steam) == 0);
    /* uppercase-hyphenated, as dwarfdump prints and as the check compares */
    ASSERT_EQ(strlen(gog), 36u);
    ASSERT_EQ(strlen(steam), 36u);
}

TEST(identity_uuid_is_null_for_unsupported_store) {
    ASSERT_TRUE(build_identity_uuid_for_store("epic") == NULL);
    ASSERT_TRUE(build_identity_uuid_for_store(NULL) == NULL);
}

void register_build_identity_tests(void) {
    RUN_TEST(identity_knows_both_shipped_stores);
    RUN_TEST(identity_rejects_unknown_stores);
    RUN_TEST(identity_uuids_are_per_store_and_differ);
    RUN_TEST(identity_uuid_is_null_for_unsupported_store);
    RUN_TEST(build_id_matches_across_store_suffix);
    RUN_TEST(build_id_rejects_a_different_version);
    RUN_TEST(build_id_rejects_null_and_empty);
    RUN_TEST(store_gog_suffix);
    RUN_TEST(store_steam_suffix);
    RUN_TEST(store_unsuffixed_is_steam);
    RUN_TEST(store_ignores_install_path);
    RUN_TEST(store_rejects_null_and_empty);
    RUN_TEST(store_suffix_not_substring);
    RUN_TEST(uuid_formats_uppercase_hyphenated);
    RUN_TEST(uuid_rejects_non_macho);
    RUN_TEST(uuid_rejects_undersized_buffer);
    RUN_TEST(uuid_rejects_null);
    RUN_TEST(uuid_survives_malformed_load_command);
    RUN_TEST(text_vmsize_reads_text_segment);
    RUN_TEST(stub_detected_and_game_is_not);
    RUN_TEST(a_dylib_is_never_a_stub);
    RUN_TEST(unreadable_header_fails_open);
    RUN_TEST(text_vmsize_zero_when_unreadable);
}
