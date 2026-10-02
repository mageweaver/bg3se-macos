/**
 * modsettings_probe.c - Name the module that makes BG3 reset the load order.
 *
 * ecl::ModManagerClient::LoadModSettingsFile (arm64 0x10305bb78) loads
 * modsettings.lsx, calls ls::ModManager::RefreshAvailableMods (+216), then
 * requires EVERY loaded entry's UUID to be present in AvailableMods. A single
 * miss branches to +380, which builds a ModuleSettings holding only GustavX
 * and SaveFile()s it over modsettings.lsx -- the "load order reset" players
 * see, with nothing logged anywhere to say which module was missing.
 *
 * Layout, read from that function's own loop (+232..+332):
 *   loaded entries  this+0x1a0 (buf) / +0x1ac (count), stride 0x60, UUID FixedString @ +0x00
 *   AvailableMods   this+0x140 (buf) / +0x14c (count), stride 0xf0, UUID FixedString @ +0x08
 * The comparison is a 32-bit FixedString compare on the UUID only.
 *
 * Diagnostic only: a call-through hook on RefreshAvailableMods that, when it
 * returns, repeats that exact check and logs every loaded UUID the engine is
 * about to reject. It changes nothing. Written 2026-09-27 after a verified
 * 1,045-entry modsettings.lsx -- every UUID of which is declared by an
 * installed pak -- was still reset 28 s into launch.
 */
#include "modsettings_probe.h"
#include "../core/logging.h"
#include "../strings/fixed_string.h"
#include "../core/stdstring.h"
#include <dobby.h>
#include <stdint.h>
#include <stdio.h>
#include <unistd.h>
#include <dirent.h>
#include <sys/stat.h>
#include <stdlib.h>
#include <string.h>

#define REFRESH_AVAILABLE_MODS_RVA 0x6045c48ULL

typedef void (*RefreshFn)(void *self);
static RefreshFn s_orig_refresh = NULL;
static unsigned s_calls = 0;

static const uint32_t k_prologue[] = {
    0xa9ba6ffcu, /* stp x28, x27, [sp, #-0x60]! */
    0xa90167fau, /* stp x26, x25, [sp, #0x10]   */
};

static const char *fs_text(uint32_t fs, char *buf, size_t n) {
    const char *s = fixed_string_resolve(fs);
    if (s && *s) return s;
    snprintf(buf, n, "<FixedString 0x%08x unresolved>", fs);
    return buf;
}

static void hooked_refresh(void *self) {
    if (self && s_calls < 8) {
        const uint8_t *mm = (const uint8_t *)self;
        LOG_CORE_WARN("[ModSettingsProbe] RefreshAvailableMods #%u BEFORE: %u loaded entries, ugc_flag=%u",
                      s_calls + 1, *(const uint32_t *)(mm + 0x1ac), (unsigned)mm[0x129]);
    }
    s_orig_refresh(self);
    if (!self || ++s_calls > 8) return;

    const uint8_t *m = (const uint8_t *)self;
    const uint8_t *loaded = *(const uint8_t * const *)(m + 0x1a0);
    uint32_t nLoaded = *(const uint32_t *)(m + 0x1ac);
    const uint8_t *avail = *(const uint8_t * const *)(m + 0x140);
    uint32_t nAvail = *(const uint32_t *)(m + 0x14c);

    LOG_CORE_WARN("[ModSettingsProbe] RefreshAvailableMods #%u: %u loaded entries, %u AvailableMods",
                  s_calls, nLoaded, nAvail);
    if (!loaded || !avail || nLoaded > 20000 || nAvail > 20000) return;

    /* A short list means the engine is not holding modsettings.lsx's order;
     * name what it holds instead. */
    if (nLoaded <= 32) {
        char nb[64];
        for (uint32_t i = 0; i < nLoaded; i++) {
            uint32_t u = *(const uint32_t *)(loaded + (size_t)i * 0x60);
            LOG_CORE_WARN("[ModSettingsProbe]   loaded[%u] %s", i, fs_text(u, nb, sizeof(nb)));
        }
    }

    unsigned missing = 0;
    char b[64];
    for (uint32_t i = 0; i < nLoaded; i++) {
        uint32_t u = *(const uint32_t *)(loaded + (size_t)i * 0x60);
        bool found = false;
        for (uint32_t j = 0; j < nAvail && !found; j++)
            found = *(const uint32_t *)(avail + (size_t)j * 0xf0 + 8) == u;
        if (!found) {
            missing++;
            if (missing <= 40)
                LOG_CORE_WARN("[ModSettingsProbe]   entry %u UUID %s is NOT in AvailableMods "
                              "-- the engine will reset the load order to GustavX",
                              i, fs_text(u, b, sizeof(b)));
        }
    }
    LOG_CORE_WARN("[ModSettingsProbe] %u of %u loaded entries missing from AvailableMods%s",
                  missing, nLoaded, missing ? " -> RESET" : " -> load order kept");
}

/* ls::ModuleSettings::LoadSettings(ls::Path const&) -- the call
 * LoadModSettingsFile makes right before RefreshAvailableMods. On launches
 * where the engine ends up holding 10 entries instead of modsettings.lsx's
 * ~1,046, log which path it read and what came back, to tell a wrong path
 * (file "missing" -> defaults) from a failed parse. */
#define LOAD_SETTINGS_RVA 0x6057d8cULL
static const uint32_t k_load_settings_prologue[] = {
    0xd10143ffu, 0xa9015ff8u, 0xa90257f6u, 0xa9034ff4u };
typedef uint64_t (*LoadSettingsFn)(void *self, const void *path);
static LoadSettingsFn s_orig_load_settings = NULL;

static uint64_t hooked_load_settings(void *self, const void *path) {
    uint64_t r = s_orig_load_settings(self, path);
    size_t len = 0;
    const char *text = path ? stdstring_read(path, &len) : NULL;
    const uint8_t *pb = (const uint8_t *)path;
    const uint8_t *sb = (const uint8_t *)self;
    LOG_CORE_WARN("[ModSettingsProbe] LoadSettings -> 0x%llx path=\"%.*s\" "
                  "path_raw=%016llx %016llx %016llx self=%016llx %016llx %016llx %016llx",
                  (unsigned long long)r, (int)(text ? (len > 300 ? 300 : len) : 0), text ? text : "",
                  pb ? *(const unsigned long long *)(pb) : 0ULL,
                  pb ? *(const unsigned long long *)(pb + 8) : 0ULL,
                  pb ? *(const unsigned long long *)(pb + 16) : 0ULL,
                  sb ? *(const unsigned long long *)(sb) : 0ULL,
                  sb ? *(const unsigned long long *)(sb + 8) : 0ULL,
                  sb ? *(const unsigned long long *)(sb + 16) : 0ULL,
                  sb ? *(const unsigned long long *)(sb + 24) : 0ULL);
    return r;
}

static void install_load_settings_probe(void *binary_base) {
    void *target = (void *)((uintptr_t)binary_base + LOAD_SETTINGS_RVA);
    if (memcmp(target, k_load_settings_prologue, sizeof(k_load_settings_prologue)) != 0) {
        LOG_CORE_WARN("[ModSettingsProbe] LoadSettings probe NOT applied: prologue mismatch at %p", target);
        return;
    }
    if (DobbyHook(target, (void *)hooked_load_settings, (void **)&s_orig_load_settings) != 0) {
        LOG_CORE_WARN("[ModSettingsProbe] DobbyHook(LoadSettings) failed");
        return;
    }
    LOG_CORE_WARN("[ModSettingsProbe] installed on ls::ModuleSettings::LoadSettings at %p", target);
}

/* Larian's mod crash guard. At startup App::LoadConfig checks for
 * <user folder>/ModCrashSanityCheck; the game creates it ~40 s into startup
 * and removes it once startup completes. If it is still there at the next
 * launch, ls::FileSystem::s_CrashOnStartupDueToMods is set and
 * ls::ModManager::RefreshAvailableMods drops every Mods-folder module
 * (ModuleSettings::RemoveMods), leaving the 10 built-in/non-UGC entries --
 * the "mods off" launches. A launch killed or crashed during startup leaves
 * the folder behind, and the mods-off launch that follows recreates it, so
 * quick retries cascade.
 *
 * On this setup the guard fires on kills and engine crashes far more often
 * than on mod-caused startup crashes, and a mods-off launch is never what the
 * player wants, so the marker is removed before the game reads it.
 * BG3SE_KEEP_MOD_CRASH_CHECK=1 leaves Larian's behaviour alone. Runs from the
 * dylib constructor (before App::LoadConfig); logged once logging is up. */
static int s_marker_cleared = 0;   /* 0 none, 1 removed, -1 present but kept/failed */

void modsettings_clear_mod_crash_marker(void) {
    const char *keep = getenv("BG3SE_KEEP_MOD_CRASH_CHECK");
    const char *home = getenv("HOME");
    if (!home) return;
    char path[1024];
    snprintf(path, sizeof(path),
             "%s/Documents/Larian Studios/Baldur's Gate 3/ModCrashSanityCheck", home);
    struct stat st;
    if (stat(path, &st) != 0 || !S_ISDIR(st.st_mode)) return;
    if (keep && *keep && *keep != '0') { s_marker_cleared = -1; return; }
    DIR *d = opendir(path);
    if (d) {
        struct dirent *e;
        while ((e = readdir(d)) != NULL) {
            if (strcmp(e->d_name, ".") == 0 || strcmp(e->d_name, "..") == 0) continue;
            char child[1300];
            snprintf(child, sizeof(child), "%s/%s", path, e->d_name);
            unlink(child);
        }
        closedir(d);
    }
    s_marker_cleared = (rmdir(path) == 0) ? 1 : -1;
}

bool modsettings_probe_init(void *binary_base) {
#if defined(__aarch64__) || defined(__arm64__)
    if (!binary_base) return false;
    if (s_marker_cleared == 1)
        LOG_CORE_WARN("[ModSettingsProbe] removed a leftover ModCrashSanityCheck marker "
                      "(previous launch ended during startup); mods stay enabled this launch");
    else if (s_marker_cleared == -1)
        LOG_CORE_WARN("[ModSettingsProbe] ModCrashSanityCheck marker present and kept: the game "
                      "will start with mods disabled this launch");
    install_load_settings_probe(binary_base);
    void *target = (void *)((uintptr_t)binary_base + REFRESH_AVAILABLE_MODS_RVA);
    if (memcmp(target, k_prologue, sizeof(k_prologue)) != 0) {
        LOG_CORE_WARN("[ModSettingsProbe] NOT applied: prologue mismatch at %p (different build?)", target);
        return false;
    }
    if (DobbyHook(target, (void *)hooked_refresh, (void **)&s_orig_refresh) != 0) {
        LOG_CORE_WARN("[ModSettingsProbe] DobbyHook failed at %p", target);
        return false;
    }
    LOG_CORE_WARN("[ModSettingsProbe] installed on ls::ModManager::RefreshAvailableMods at %p", target);
    return true;
#else
    (void)binary_base;
    return false;
#endif
}
