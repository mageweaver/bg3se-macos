/**
 * localization.c - Localization System Implementation
 *
 * Verified 4.1.1.7209685 offsets (macOS ARM64):
 *   ls::TranslatedStringRepository::m_ptr at 0x8af5088 (from module base)
 *   ls::TranslatedStringRepository::TryGet at 0x10652390c
 *   ls::TranslatedStringRepository::AddTranslatedString at 0x106521148
 *
 * TranslatedStringRepository structure (from Windows BG3SE):
 *   +0x00: int field_0
 *   +0x08: TextPool* TranslatedStrings[9]  (9 language pools)
 *   +0x50: TextPool* FallbackPool
 *   +0x58: TextPool* VersionedFallbackPool
 *   ...
 *   TextPool contains: HashMap<RuntimeStringHandle, LSStringView> Texts
 *
 * RuntimeStringHandle = { FixedString Handle; uint16_t Version; }
 */

#include "localization.h"
#include "logging.h"
#include "fixed_string.h"
#include "../core/offset_table.h"
#include "../core/safe_memory.h"
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <dirent.h>
#include <dlfcn.h>
#include <mach-o/dyld.h>

// ============================================================================
// Offset Constants (macOS ARM64)
// ============================================================================

// TranslatedStringRepository structure offsets. +0x08 is verified by the
// installed ARM64 TryGet implementation for identity 0.
#define REPO_OFFSET_TRANSLATED_STRINGS  0x08   // TextPool* TranslatedStrings[9]
#define REPO_OFFSET_FALLBACK_POOL       0x50   // TextPool* FallbackPool
#define REPO_OFFSET_VERSIONED_FALLBACK  0x58   // TextPool* VersionedFallbackPool

// TextPool structure offsets
#define TEXTPOOL_OFFSET_TEXTS           0x10   // HashMap<RuntimeStringHandle, LSStringView>

// HashMap structure (similar to other BG3 HashMaps)
// HashMap { buf*, capacity, size, ... }

// ============================================================================
// Function Pointer Types
// ============================================================================

// ls::FixedString::Create(uint32_t* out_index, char const*, int) - writes to output parameter
// NOTE: The result is written to the first parameter, not returned!
typedef void (*FixedStringCreateFn)(uint32_t *out_index, const char *str, int len);

// RuntimeStringHandle structure (8 bytes)
// { FixedString Handle (4 bytes), uint16_t Version (2 bytes), padding (2 bytes) }
typedef struct __attribute__((packed)) {
    uint32_t handle;     // FixedString index
    uint16_t version;    // Version number
    uint16_t _padding;
} RuntimeStringHandle;

// LSStringView structure (16 bytes on ARM64)
typedef struct {
    const char *data;    // Pointer to string data
    uint64_t size;       // String length
} LSStringView;

// TryGet writes a Result<StringView> to an explicit x0 output parameter.
// Disassembly of 4.1.1.7209685 shows value at +0x00/+0x08 and an error flag
// at +0x10 (0 = value present, 1 = no translation).
typedef struct __attribute__((aligned(16))) {
    LSStringView value;      // 0x00: The StringView if present
    uint8_t has_error;       // 0x10: 0 on success, 1 when no value exists
    uint8_t _pad[15];        // Padding to 32 bytes
} TryGetResult;

// TryGet signature after lowering:
// x0=result, x1=this, x2=&handle, w3=language, w4=fallback language.
typedef void (*TryGetFn)(void *result, void *repo, const RuntimeStringHandle *handle, int lang1, int lang2);

// AddTranslatedString signature (from Ghidra decompilation Dec 2025):
// void ls::TranslatedStringRepository::AddTranslatedString(
//     RuntimeStringHandle* out_handle,     // x0: output handle
//     TranslatedStringRepository* repo,    // x1: this (repository)
//     RuntimeStringHandle const* in_handle,// x2: input handle
//     char* str_data,                      // x3: StringView.data (passed by value in x3)
//     uint64_t str_size,                   // x4: StringView.size (passed by value in x4)
//     TextPool* text_pool,                 // x5: TranslatedStrings[0]
//     uint32_t flags                       // x6: EAddFlags (0 = default)
// )
// NOTE: On ARM64, 16-byte structs passed by value use two registers (x3+x4)
typedef void (*AddTranslatedStringFn)(
    RuntimeStringHandle *out_handle,
    void *repo,
    const RuntimeStringHandle *in_handle,
    const char *str_data,
    uint64_t str_size,
    void *text_pool,
    uint32_t flags
);

// ============================================================================
// Module State
// ============================================================================

static struct {
    bool initialized;
    void *binary_base;
    void **repo_ptr_addr;     // Address of the m_ptr global
    void *repo;               // Cached repository pointer
    FixedStringCreateFn fs_create;  // FixedString::Create function
    void *tryget_fn;          // TryGet function address (explicit x0 output)
    AddTranslatedStringFn add_string_fn;  // AddTranslatedString function
} s_loca = {0};

// ============================================================================
// Initialization
// ============================================================================

void localization_init(void *main_binary_base) {
    if (s_loca.initialized) {
        return;
    }

    s_loca.binary_base = main_binary_base;

    // The repository singleton and callable functions are selected from the
    // active per-version table. No row or zero field means fail closed.
    const VersionOffsets *vo = offset_table_get();
    if (!vo) {
        LOG_CORE_INFO("LOCA: No offset-table row for this game version — localization disabled");
        s_loca.initialized = true;  // don't retry; ready() stays false (repo_ptr_addr NULL)
        return;
    }
    s_loca.repo_ptr_addr =
        (void **)offset_table_resolve(vo->translated_string_repo_ptr);

    s_loca.fs_create = (FixedStringCreateFn)offset_table_game_fn(
        GAME_FN_FIXED_STRING_CREATE);
    s_loca.tryget_fn = offset_table_game_fn(
        GAME_FN_TRANSLATED_STRING_TRY_GET);
    s_loca.add_string_fn = (AddTranslatedStringFn)offset_table_game_fn(
        GAME_FN_TRANSLATED_STRING_ADD);

    LOG_CORE_INFO("LOCA: Initialized - repo_ptr at %p", (void*)s_loca.repo_ptr_addr);
    LOG_CORE_DEBUG("LOCA: FixedString::Create at %p", (void*)s_loca.fs_create);
    LOG_CORE_DEBUG("LOCA: TryGet at %p", s_loca.tryget_fn);
    LOG_CORE_DEBUG("LOCA: AddTranslatedString at %p", (void*)s_loca.add_string_fn);

    s_loca.initialized = true;
}

bool localization_ready(void) {
    if (!s_loca.initialized || !s_loca.repo_ptr_addr) {
        return false;
    }

    // Read the repository pointer (double-indirection like RPGStats).
    // safe_memory guards against a bad data_shift landing on an unmapped page.
    void *repo = NULL;
    if (!safe_memory_read_pointer((mach_vm_address_t)s_loca.repo_ptr_addr,
                                  &repo)) {
        return false;
    }
    if (repo != NULL) {
        s_loca.repo = repo;
        return true;
    }

    return false;
}

void* localization_get_raw(void) {
    if (!localization_ready()) {
        return NULL;
    }
    return s_loca.repo;
}

// ============================================================================
// Native TryGet Result Helper
// ============================================================================

/**
 * TryGet is compiled with an explicit output pointer, not an AAPCS64 x8
 * indirect result. This is verified by the function prologue saving x0 as the
 * output buffer before reading the repository from x1.
 */
static bool call_tryget(void *fn, void *repo,
                        const RuntimeStringHandle *handle,
                        int lang1, int lang2, TryGetResult *result) {
    if (!fn || !repo || !handle || !result) {
        return false;
    }

    memset(result, 0, sizeof(TryGetResult));
    ((TryGetFn)fn)(result, repo, handle, lang1, lang2);
    return result->has_error == 0;
}

// ============================================================================
// RuntimeStringHandle Helpers
// ============================================================================

/**
 * Create a FixedString from a handle string using the game's FixedString::Create.
 * Handle format: "h12345678g1234g4567g8901g123456789012"
 */
static uint32_t create_fixedstring_from_handle(const char *handle) {
    if (!handle || !s_loca.fs_create) {
        return 0xFFFFFFFF;
    }

    // Call ls::FixedString::Create(uint32_t* out, char const*, int)
    // The function writes the result to the output parameter
    uint32_t fs_index = 0xFFFFFFFF;
    s_loca.fs_create(&fs_index, handle, (int)strlen(handle));

    LOG_CORE_DEBUG("LOCA: FixedString::Create('%s') = 0x%x", handle, fs_index);
    return fs_index;
}

// ============================================================================
// Handle Creation
// ============================================================================

// Monotonically increasing counter for dynamic handles (like Windows BG3SE's
// NextDynamicStringHandleId). Protected by no lock because it's init-time only
// in practice. Use _Atomic to be correct if ever called from multiple threads.
#include <stdatomic.h>
static _Atomic uint32_t s_next_dynamic_handle_id = 1;

/**
 * Create a new unique localization handle string.
 *
 * Windows BG3SE format: "h{8hex}g{4hex}g{4hex}g{4hex}g{12hex}"
 * We use a simple monotonically-increasing counter packed into the first field
 * with the rest zeroed, matching BG3's "hXXXXXXXXg0000g0000g0000g000000000000"
 * pattern for dynamically generated handles.
 *
 * The returned handle string is written to `out` (caller-supplied buffer of
 * at least LOCA_HANDLE_BUF_SIZE bytes).
 */
bool localization_create_handle(char *out, size_t out_size) {
    if (!out || out_size < LOCA_HANDLE_BUF_SIZE) {
        return false;
    }

    uint32_t id = atomic_fetch_add(&s_next_dynamic_handle_id, 1);

    // Format: "h%08Xg0000g0000g0000g000000000000"
    int written = snprintf(out, out_size,
        "h%08Xg0000g0000g0000g000000000000", id);

    return (written > 0 && (size_t)written < out_size);
}

// ============================================================================
// String Access
// ============================================================================

// Thread-local storage decouples returned text from the game's map lifetime.
// Grow on demand so translations are not silently truncated.
static __thread char *s_result_buffer = NULL;
static __thread size_t s_result_capacity = 0;

static bool ensure_result_capacity(size_t required) {
    if (required <= s_result_capacity) {
        return true;
    }

    size_t capacity = s_result_capacity ? s_result_capacity : 256;
    while (capacity < required) {
        if (capacity > SIZE_MAX / 2) {
            capacity = required;
            break;
        }
        capacity *= 2;
    }

    char *buffer = (char *)realloc(s_result_buffer, capacity);
    if (!buffer) {
        return false;
    }
    s_result_buffer = buffer;
    s_result_capacity = capacity;
    return true;
}

const char* localization_get(const char *handle, const char *fallback) {
    const char *fallback_value = fallback ? fallback : "";
    if (!handle || !*handle) {
        return fallback_value;
    }
    if (!localization_ready() || !s_loca.fs_create || !s_loca.tryget_fn) {
        /* Distinguish subsystem-offline fallbacks (systemic, warn once) from
         * per-handle misses below (normal, silent). */
        static bool warned = false;
        if (!warned) {
            LOG_CORE_WARN(
                "LOCA: Get falling back to defaults - localization subsystem "
                "is not ready");
            warned = true;
        }
        return fallback_value;
    }

    uint32_t fixed_string = create_fixedstring_from_handle(handle);
    if (fixed_string == UINT32_MAX) {
        return fallback_value;
    }

    RuntimeStringHandle runtime_handle = {
        .handle = fixed_string,
        .version = 0,
        ._padding = 0
    };
    TryGetResult result;
    if (!call_tryget(
            s_loca.tryget_fn, s_loca.repo, &runtime_handle, 0, 0, &result)) {
        return fallback_value;
    }

    if (result.value.size == 0) {
        if (!ensure_result_capacity(1)) {
            return fallback_value;
        }
        s_result_buffer[0] = '\0';
        return s_result_buffer;
    }
    if (!result.value.data) {
        return fallback_value;
    }

    if (result.value.size > SIZE_MAX - 1) {
        return fallback_value;
    }
    size_t copy_size = (size_t)result.value.size;
    if (!ensure_result_capacity(copy_size + 1)) {
        return fallback_value;
    }
    if (!safe_memory_read(
            (mach_vm_address_t)result.value.data, s_result_buffer, copy_size)) {
        return fallback_value;
    }

    s_result_buffer[copy_size] = '\0';
    return s_result_buffer;
}

bool localization_set(const char *handle, const char *value) {
    LOG_CORE_DEBUG("LOCA: UpdateTranslatedString called with handle='%s' value='%s'",
                   handle ? handle : "(null)", value ? value : "(null)");

    if (!localization_ready()) {
        LOG_CORE_DEBUG("LOCA: Repository not ready");
        return false;
    }

    if (!handle || !*handle || !value) {
        return false;
    }

    if (!s_loca.fs_create || !s_loca.tryget_fn || !s_loca.add_string_fn) {
        LOG_CORE_INFO(
            "LOCA: UpdateTranslatedString deferred for this game version; "
            "one or more verified entry points are unavailable");
        return false;
    }

    uint32_t fixed_string = create_fixedstring_from_handle(handle);
    if (fixed_string == UINT32_MAX) {
        return false;
    }

    void *translated_pool = NULL;
    if (!safe_memory_read_pointer(
            (mach_vm_address_t)((uintptr_t)s_loca.repo
                + REPO_OFFSET_TRANSLATED_STRINGS),
            &translated_pool)
        || !translated_pool) {
        LOG_CORE_INFO("LOCA: default translated-string pool is unavailable");
        return false;
    }

    RuntimeStringHandle input = {
        .handle = fixed_string,
        .version = 0,
        ._padding = 0
    };
    RuntimeStringHandle output = {
        .handle = UINT32_MAX,
        .version = 0,
        ._padding = 0
    };
    size_t value_size = strlen(value);

    s_loca.add_string_fn(
        &output, s_loca.repo, &input, value, value_size,
        translated_pool, 0);

    // The pool's storage handle in `output` resolves immediately; the input
    // handle only becomes visible to TryGet on a later repository merge
    // (observed live: input-handle verify misses synchronously, hits seconds
    // later). Verify against output first, falling back to input.
    TryGetResult verify;
    bool verified = false;
    if (output.handle != UINT32_MAX
        && call_tryget(
               s_loca.tryget_fn, s_loca.repo, &output, 0, 0, &verify)
        && verify.value.size == value_size
        && (value_size == 0 || verify.value.data)) {
        verified = true;
    }
    if (!verified
        && call_tryget(
               s_loca.tryget_fn, s_loca.repo, &input, 0, 0, &verify)
        && verify.value.size == value_size
        && (value_size == 0 || verify.value.data)) {
        verified = true;
    }
    if (!verified) {
        LOG_CORE_INFO(
            "LOCA: UpdateTranslatedString('%s') could not be verified "
            "(output handle 0x%x)",
            handle, output.handle);
        return false;
    }

    if (value_size > 0) {
        if (!ensure_result_capacity(value_size)
            || !safe_memory_read(
                (mach_vm_address_t)verify.value.data,
                s_result_buffer, value_size)
            || memcmp(s_result_buffer, value, value_size) != 0) {
            LOG_CORE_INFO(
                "LOCA: UpdateTranslatedString('%s') verification mismatch",
                handle);
            return false;
        }
    }

    return true;
}

// ============================================================================
// Language Info
// ============================================================================

// Cached language string (detected once, cached forever)
static char s_detected_language[64] = {0};
static bool s_language_detected = false;

/**
 * Try to read language from Larian's profile options.
 * Path: ~/Library/Application Support/Larian Studios/Baldur's Gate 3/PlayerProfiles/<profile>/profileOptions.lsx
 *
 * The file contains XML like:
 *   <attribute id="ActiveLanguage" value="English" type="LSString"/>
 */
static bool try_read_language_from_profile(void) {
    const char *home = getenv("HOME");
    if (!home) return false;

    // Build path to profile directory
    char profile_dir[512];
    snprintf(profile_dir, sizeof(profile_dir),
             "%s/Library/Application Support/Larian Studios/Baldur's Gate 3/PlayerProfiles",
             home);

    // Try to open directory and find profile options
    DIR *dir = opendir(profile_dir);
    if (!dir) {
        LOG_CORE_DEBUG("LOCA: Could not open profile directory: %s", profile_dir);
        return false;
    }

    struct dirent *entry;
    bool found = false;

    while ((entry = readdir(dir)) != NULL && !found) {
        if (entry->d_name[0] == '.') continue;  // Skip . and ..

        // Build path to profileOptions.lsx
        char options_path[768];
        snprintf(options_path, sizeof(options_path),
                 "%s/%s/profileOptions.lsx", profile_dir, entry->d_name);

        FILE *f = fopen(options_path, "r");
        if (!f) continue;

        // Read file and search for ActiveLanguage
        char line[512];
        while (fgets(line, sizeof(line), f)) {
            // Look for: <attribute id="ActiveLanguage" value="..." type="LSString"/>
            const char *lang_attr = strstr(line, "id=\"ActiveLanguage\"");
            if (lang_attr) {
                const char *value_start = strstr(lang_attr, "value=\"");
                if (value_start) {
                    value_start += 7;  // Skip 'value="'
                    const char *value_end = strchr(value_start, '"');
                    if (value_end && value_end > value_start) {
                        size_t len = value_end - value_start;
                        if (len < sizeof(s_detected_language)) {
                            memcpy(s_detected_language, value_start, len);
                            s_detected_language[len] = '\0';
                            LOG_CORE_INFO("LOCA: Detected language from profile: %s", s_detected_language);
                            found = true;
                        }
                    }
                }
            }
        }
        fclose(f);
    }

    closedir(dir);
    return found;
}

/**
 * Try to detect system language from macOS locale settings.
 */
static bool try_read_system_language(void) {
    // Check LANG environment variable (e.g., "en_US.UTF-8", "de_DE.UTF-8")
    const char *lang = getenv("LANG");
    if (lang && *lang) {
        // Map common locale prefixes to BG3 language names
        static const struct { const char *prefix; const char *name; } locale_map[] = {
            {"en", "English"},
            {"de", "German"},
            {"fr", "French"},
            {"it", "Italian"},
            {"es", "Spanish"},
            {"pt_BR", "Brazilian Portuguese"},
            {"pt", "Portuguese"},
            {"pl", "Polish"},
            {"ru", "Russian"},
            {"zh_CN", "Chinese Simplified"},
            {"zh_TW", "Chinese Traditional"},
            {"zh", "Chinese Simplified"},
            {"ja", "Japanese"},
            {"ko", "Korean"},
            {"tr", "Turkish"},
            {"uk", "Ukrainian"},
            {NULL, NULL}
        };

        for (int i = 0; locale_map[i].prefix; i++) {
            if (strncmp(lang, locale_map[i].prefix, strlen(locale_map[i].prefix)) == 0) {
                strncpy(s_detected_language, locale_map[i].name, sizeof(s_detected_language) - 1);
                s_detected_language[sizeof(s_detected_language) - 1] = '\0';
                LOG_CORE_INFO("LOCA: Detected language from system locale: %s (LANG=%s)",
                             s_detected_language, lang);
                return true;
            }
        }
    }

    return false;
}

const char* localization_get_language(void) {
    // Return cached result if available
    if (s_language_detected && s_detected_language[0]) {
        return s_detected_language;
    }

    // Try detection methods in order of preference
    if (!s_language_detected) {
        s_language_detected = true;  // Mark as attempted

        // 1. Try to read from game profile (most accurate)
        if (try_read_language_from_profile()) {
            return s_detected_language;
        }

        // 2. Try to detect from system locale
        if (try_read_system_language()) {
            return s_detected_language;
        }

        // 3. Default to English
        LOG_CORE_INFO("LOCA: Could not detect language, defaulting to English");
        strncpy(s_detected_language, "English", sizeof(s_detected_language) - 1);
    }

    return s_detected_language;
}

// ============================================================================
// Debugging
// ============================================================================

void localization_dump_info(void) {
    LOG_CORE_INFO("LOCA: === Localization System Info ===");
    LOG_CORE_INFO("LOCA: Initialized: %s", s_loca.initialized ? "yes" : "no");
    LOG_CORE_INFO("LOCA: Binary base: %p", s_loca.binary_base);
    LOG_CORE_INFO("LOCA: Repo ptr addr: %p", (void*)s_loca.repo_ptr_addr);

    if (localization_ready()) {
        LOG_CORE_INFO("LOCA: Repository: %p", s_loca.repo);

        // Dump some structure info for verification
        void *repo = s_loca.repo;

        // Read TranslatedStrings[0] pointer
        void *ts0 = *(void**)((uintptr_t)repo + REPO_OFFSET_TRANSLATED_STRINGS);
        LOG_CORE_INFO("LOCA: TranslatedStrings[0]: %p", ts0);

        // Read FallbackPool pointer
        void *fallback = *(void**)((uintptr_t)repo + REPO_OFFSET_FALLBACK_POOL);
        LOG_CORE_INFO("LOCA: FallbackPool: %p", fallback);
    } else {
        LOG_CORE_INFO("LOCA: Repository: NOT READY");
    }

    LOG_CORE_INFO("LOCA: ================================");
}
