/**
 * BG3SE-macOS - Game Binary Version Detection
 *
 * Detects BG3 version via Info.plist CFBundleShortVersionString.
 * Compares against the known-good version to gate address-dependent features.
 *
 * Issue #73: Game hotfixes shift 2,000+ hardcoded addresses. Without version
 * detection, dereferencing stale singleton pointers causes SIGSEGV.
 */

#include "version_detect.h"
#include "offset_table.h"
#include "logging.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <mach/mach.h>

// ============================================================================
// Constants
// ============================================================================

// Ghidra analysis base address for the BG3 binary
#define GHIDRA_BASE 0x100000000ULL

// Sentinel addresses: known data-segment globals that should be readable
// regardless of whether the game has initialized. These are static globals
// in BSS/DATA — readable even if the pointer value inside is NULL.
// If vm_read succeeds on all 3, the binary layout matches our addresses.
// Values re-derived via nm for game build 4.1.1.7209685 (2026-07-28
// migration) — they MUST move in lockstep with BG3_KNOWN_VERSION and the
// per-subsystem offsets (entity_system.c, prototype_managers.c). The
// harness offset audit (tests/harness/test_offset_audit.py) validates the
// same symbols against the installed binary.
static const uintptr_t g_sentinel_ghidra_addrs[] = {
    0x1089968b8,  // esv::EocServer::m_ptr (server singleton global)
    0x108994968,  // ecl::EocClient::m_ptr (client singleton global)
    0x1089c2c80,  // eoc::SpellPrototypeManager::m_ptr
};
#define NUM_SENTINELS (sizeof(g_sentinel_ghidra_addrs) / sizeof(g_sentinel_ghidra_addrs[0]))

// ============================================================================
// State
// ============================================================================

static char g_detected_version[64] = {0};
static bool g_initialized = false;
static bool g_version_matches = true;  // Optimistic default
static void *g_binary_base = NULL;     // Set by version_detect_set_binary_base()

// ============================================================================
// Info.plist Parsing (lightweight, no Foundation dependency)
// ============================================================================

/**
 * Extract a string value from an XML plist by key name.
 * Simple text scanning — avoids pulling in Foundation framework from C code.
 */
static bool plist_extract_string(const char *plist_path, const char *key,
                                  char *out, size_t out_size) {
    FILE *f = fopen(plist_path, "r");
    if (!f) return false;

    // Read entire file (Info.plist is small, typically <4KB)
    fseek(f, 0, SEEK_END);
    long file_size = ftell(f);
    if (file_size <= 0 || file_size > 65536) {
        fclose(f);
        return false;
    }
    fseek(f, 0, SEEK_SET);

    char *buf = malloc((size_t)file_size + 1);
    if (!buf) { fclose(f); return false; }
    size_t read = fread(buf, 1, (size_t)file_size, f);
    buf[read] = '\0';
    fclose(f);

    // Search for <key>KEY</key> followed by <string>VALUE</string>
    char key_tag[128];
    snprintf(key_tag, sizeof(key_tag), "<key>%s</key>", key);
    char *key_pos = strstr(buf, key_tag);
    if (!key_pos) { free(buf); return false; }

    // Bound search: <string> must appear before the next <key>
    char *next_key = strstr(key_pos + strlen(key_tag), "<key>");
    char *str_start = strstr(key_pos, "<string>");
    if (!str_start || (next_key && str_start > next_key)) { free(buf); return false; }
    str_start += 8;  // len("<string>")

    char *str_end = strstr(str_start, "</string>");
    if (!str_end) { free(buf); return false; }

    size_t len = (size_t)(str_end - str_start);
    if (len >= out_size) len = out_size - 1;
    memcpy(out, str_start, len);
    out[len] = '\0';

    free(buf);
    return true;
}

// ============================================================================
// Steam App Path Detection
// ============================================================================

/**
 * Check that bundle_path is a real app bundle (has Contents/Info.plist).
 */
static bool bundle_exists(const char *bundle_path) {
    char plist_check[1280];
    snprintf(plist_check, sizeof(plist_check), "%s/Contents/Info.plist", bundle_path);
    FILE *f = fopen(plist_check, "r");
    if (!f) return false;
    fclose(f);
    return true;
}

/**
 * Try both bundle spellings under a "Baldurs Gate 3" directory.
 * Note: the Steam folder omits the apostrophe, the .app bundle usually
 * keeps it ("Baldur's Gate 3.app").
 */
static bool try_bundle_in_dir(const char *dir, char *out, size_t out_size) {
    static const char *app_names[] = { "Baldur's Gate 3.app", "Baldurs Gate 3.app" };
    for (size_t i = 0; i < sizeof(app_names) / sizeof(app_names[0]); i++) {
        snprintf(out, out_size, "%s/%s", dir, app_names[i]);
        if (bundle_exists(out)) return true;
    }
    return false;
}

/**
 * Find the BG3 app bundle path (#90, #86). Resolution order:
 *   1. BG3SE_GAME_PATH env override (the bundle, or a directory containing it)
 *   2. The default Steam library
 *   3. Every additional library in steamapps/libraryfolders.vdf
 */
static const char *find_bg3_app_path(void) {
    static char path[1024] = {0};
    if (path[0]) return path;

    const char *env = getenv("BG3SE_GAME_PATH");
    if (env && env[0]) {
        size_t env_len = strlen(env);
        if (env_len > 4 && strcmp(env + env_len - 4, ".app") == 0) {
            if (bundle_exists(env)) {
                snprintf(path, sizeof(path), "%s", env);
                return path;
            }
        } else if (try_bundle_in_dir(env, path, sizeof(path))) {
            return path;
        }
        LOG_CORE_WARN("BG3SE_GAME_PATH set but no BG3 bundle found there: %s", env);
    }

    const char *home = getenv("HOME");
    if (!home) return NULL;

    char dir[1024];
    snprintf(dir, sizeof(dir),
             "%s/Library/Application Support/Steam/steamapps/common/"
             "Baldurs Gate 3", home);
    if (try_bundle_in_dir(dir, path, sizeof(path))) return path;

    // Additional Steam libraries (external drives): scan libraryfolders.vdf
    // for "path" values — a flat token scan is enough for this format.
    char vdf_path[1024];
    snprintf(vdf_path, sizeof(vdf_path),
             "%s/Library/Application Support/Steam/steamapps/libraryfolders.vdf",
             home);
    FILE *vf = fopen(vdf_path, "r");
    if (vf) {
        fseek(vf, 0, SEEK_END);
        long vdf_size = ftell(vf);
        fseek(vf, 0, SEEK_SET);

        if (vdf_size > 0 && vdf_size < 256 * 1024) {
            char *buf = (char *)malloc((size_t)vdf_size + 1);
            if (buf) {
                fread(buf, 1, (size_t)vdf_size, vf);
                buf[vdf_size] = '\0';

                const char *p = buf;
                while ((p = strstr(p, "\"path\"")) != NULL) {
                    p += strlen("\"path\"");
                    while (*p && *p != '"') p++;
                    if (*p != '"') break;
                    p++;
                    const char *end = strchr(p, '"');
                    if (!end) break;

                    snprintf(dir, sizeof(dir),
                             "%.*s/steamapps/common/Baldurs Gate 3",
                             (int)(end - p), p);
                    if (try_bundle_in_dir(dir, path, sizeof(path))) {
                        free(buf);
                        fclose(vf);
                        return path;
                    }
                    p = end + 1;
                }
                free(buf);
            }
        }
        fclose(vf);
    }

    path[0] = '\0';
    return NULL;
}

// ============================================================================
// Public API
// ============================================================================

bool version_detect_init(const char *app_bundle_path) {
    if (g_initialized) return g_detected_version[0] != '\0';

    g_initialized = true;

    // Find the app bundle
    const char *bundle = app_bundle_path;
    if (!bundle) bundle = find_bg3_app_path();
    if (!bundle) {
        log_message("[WARN] [VersionDetect] Could not find BG3 app bundle");
        return false;
    }

    // Read Info.plist
    char plist_path[1280];
    snprintf(plist_path, sizeof(plist_path), "%s/Contents/Info.plist", bundle);

    // Try CFBundleShortVersionString first (human-readable like "4.1.1.7209685")
    if (!plist_extract_string(plist_path, "CFBundleShortVersionString",
                               g_detected_version, sizeof(g_detected_version))) {
        // Fallback: CFBundleVersion
        if (!plist_extract_string(plist_path, "CFBundleVersion",
                                   g_detected_version, sizeof(g_detected_version))) {
            log_message("[WARN] [VersionDetect] Could not read version from %s", plist_path);
            return false;
        }
    }

    // Compare against known-good version
    g_version_matches = (strcmp(g_detected_version, BG3_KNOWN_VERSION) == 0);

    if (g_version_matches) {
        log_message("[INFO] [VersionDetect] Game version: %s (matches known-good)",
                    g_detected_version);
    } else {
        log_message("[WARN] [VersionDetect] Game version: %s (MISMATCH — expected %s). "
                    "Address-dependent features may not work correctly. "
                    "TypeId addresses, singleton pointers, and function offsets "
                    "were verified for %s.",
                    g_detected_version, BG3_KNOWN_VERSION, BG3_KNOWN_VERSION);
    }

    return true;
}

const char *version_detect_get_version(void) {
    if (!g_initialized || g_detected_version[0] == '\0') return NULL;
    return g_detected_version;
}

bool version_detect_matches(void) {
    return g_version_matches;
}

/**
 * Probe sentinel addresses to validate binary layout compatibility.
 * Reads known data-segment globals via vm_read. If all are readable,
 * the binary layout matches our hardcoded addresses even if the
 * version string changed (common for minor hotfix builds).
 *
 * Requires g_binary_base to be set via version_detect_set_binary_base().
 */
static bool probe_sentinel_addresses(void) {
    if (!g_binary_base) return false;

    uintptr_t base = (uintptr_t)g_binary_base;
    int pass = 0;

    for (int i = 0; i < (int)NUM_SENTINELS; i++) {
        uintptr_t runtime_addr = g_sentinel_ghidra_addrs[i] - GHIDRA_BASE + base;

        vm_size_t data_size = sizeof(void*);
        vm_offset_t data = 0;
        mach_msg_type_number_t count = 0;
        kern_return_t kr = vm_read(mach_task_self(), (vm_address_t)runtime_addr,
                                    data_size, &data, &count);
        if (kr == KERN_SUCCESS) {
            if (data) vm_deallocate(mach_task_self(), data, count);
            pass++;
        } else {
            log_message("[WARN] [VersionDetect] Sentinel probe %d failed at 0x%llx "
                        "(Ghidra: 0x%llx, kr=%d)",
                        i, (unsigned long long)runtime_addr,
                        (unsigned long long)g_sentinel_ghidra_addrs[i], kr);
        }
    }

    log_message("[INFO] [VersionDetect] Sentinel probe: %d/%d passed", pass, (int)NUM_SENTINELS);
    return pass == (int)NUM_SENTINELS;
}

void version_detect_set_binary_base(void *base) {
    g_binary_base = base;
    // Both version string and binary base are now available — initialize offset table.
    offset_table_init();
}

void *version_detect_get_binary_base(void) {
    return g_binary_base;
}

bool version_detect_addresses_safe(void) {
    // Manual override for power users
    const char *force = getenv("BG3SE_FORCE_ADDRESSES");
    if (force && force[0] && force[0] != '0') return true;

    // Fail CLOSED if version detection hasn't run
    if (!g_initialized || g_detected_version[0] == '\0') {
        static bool warned = false;
        if (!warned) {
            log_message("[WARN] [VersionDetect] Could not determine game version. "
                        "Address-dependent features disabled as safety precaution. "
                        "Set BG3SE_FORCE_ADDRESSES=1 to override.");
            warned = true;
        }
        return false;
    }

    // Exact version match — always safe
    if (g_version_matches) return true;

    // Version mismatch but same major.minor.patch — try sentinel probes.
    // Minor hotfix builds (4.1.1.NNNNNNN) often don't change the binary layout.
    if (g_binary_base) {
        static int probe_result = -1;  // -1 = not probed yet
        if (probe_result == -1) {
            probe_result = probe_sentinel_addresses() ? 1 : 0;
            if (probe_result == 1) {
                log_message("[INFO] [VersionDetect] Version mismatch (%s vs %s) but "
                            "sentinel probes PASSED — addresses appear compatible. "
                            "Enabling address-dependent features.",
                            g_detected_version, BG3_KNOWN_VERSION);
            } else {
                log_message("[WARN] [VersionDetect] Version mismatch AND sentinel probes "
                            "FAILED — binary layout has changed. Address-dependent "
                            "features disabled.");
            }
        }
        return probe_result == 1;
    }

    return false;
}
