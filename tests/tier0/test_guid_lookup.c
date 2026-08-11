/*
 * Tier 0 tests for BG3 GUID parsing and canonical formatting.
 */

#include "test_harness.h"
#include "guid_lookup.h"

typedef struct {
    const char *input;
    const char *canonical;
    const char *pre_fix_output;
} GuidFormatCase;

static const GuidFormatCase GUID_FORMAT_CASES[] = {
    {
        "b4d01c83-25e9-6156-d91b-84e619b3757d",
        "b4d01c83-25e9-6156-d91b-84e619b3757d",
        // The former hi-first formatter interpreted the parsed engine words
        // in this order, reproducing the exact live HandleToUuid failure.
        "757d19b3-84e6-d91b-6156-25e9b4d01c83",
    },
    {
        "00000000-0000-0000-0000-000000000000",
        "00000000-0000-0000-0000-000000000000",
        NULL,
    },
    {
        "ffffffff-ffff-ffff-ffff-ffffffffffff",
        "ffffffff-ffff-ffff-ffff-ffffffffffff",
        NULL,
    },
    {
        "A5EaEaFe-220D-bC4d-4Cc3-B94574d334C7",
        "a5eaeafe-220d-bc4d-4cc3-b94574d334c7",
        NULL,
    },
    {
        "00112233-4455-6677-8899-aabbccddeeff",
        "00112233-4455-6677-8899-aabbccddeeff",
        NULL,
    },
};

static size_t guid_format_case_count(void) {
    return sizeof(GUID_FORMAT_CASES) / sizeof(GUID_FORMAT_CASES[0]);
}

/* Reproduce the old field interpretation so the live regression is proved. */
static void format_pre_fix_field_order(const Guid *guid, char out_str[37]) {
    uint32_t a = (uint32_t)(guid->hi >> 32);
    uint16_t b = (uint16_t)((guid->hi >> 16) & 0xFFFFULL);
    uint16_t c = (uint16_t)(guid->hi & 0xFFFFULL);
    uint16_t d = (uint16_t)(guid->lo >> 48);
    uint64_t e = guid->lo & 0xFFFFFFFFFFFFULL;

    snprintf(out_str, 37, "%08x-%04x-%04x-%04x-%012llx",
             (unsigned int)a, (unsigned int)b, (unsigned int)c,
             (unsigned int)d, (unsigned long long)e);
}

TEST(format_parse_table_canonicalizes) {
    for (size_t i = 0; i < guid_format_case_count(); i++) {
        const GuidFormatCase *test_case = &GUID_FORMAT_CASES[i];
        Guid guid;
        char output[37];

        memset(output, 0xA5, sizeof(output));
        ASSERT_TRUE(guid_parse(test_case->input, &guid));
        guid_to_string(&guid, output);

        ASSERT_STR_EQ(output, test_case->canonical);
        ASSERT_EQ(strlen(output), 36u);
        ASSERT_EQ(output[36], '\0');
    }
}

TEST(live_host_pre_fix_output_regression) {
    const GuidFormatCase *host_case = &GUID_FORMAT_CASES[0];
    Guid guid;
    char pre_fix_output[37];
    char fixed_output[37];

    ASSERT_NOT_NULL(host_case->pre_fix_output);
    ASSERT_TRUE(guid_parse(host_case->input, &guid));

    format_pre_fix_field_order(&guid, pre_fix_output);
    ASSERT_STR_EQ(pre_fix_output, host_case->pre_fix_output);

    guid_to_string(&guid, fixed_output);
    ASSERT_STR_EQ(fixed_output, host_case->canonical);
    ASSERT_NE(strcmp(fixed_output, host_case->pre_fix_output), 0);
}

TEST(parse_format_parse_preserves_all_16_bytes) {
    ASSERT_EQ(sizeof(Guid), 16u);

    for (size_t i = 0; i < guid_format_case_count(); i++) {
        Guid first;
        Guid second;
        char canonical[37];

        ASSERT_TRUE(guid_parse(GUID_FORMAT_CASES[i].input, &first));
        guid_to_string(&first, canonical);
        ASSERT_TRUE(guid_parse(canonical, &second));

        ASSERT_EQ(memcmp(&first, &second, 16), 0);
    }
}

void register_guid_lookup_tests(void) {
    printf("[guid_lookup]\n");
    RUN_TEST(format_parse_table_canonicalizes);
    RUN_TEST(live_host_pre_fix_output_regression);
    RUN_TEST(parse_format_parse_preserves_all_16_bytes);
}
