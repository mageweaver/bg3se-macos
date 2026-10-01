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
#include <dobby.h>
#include <stdint.h>
#include <stdio.h>
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

bool modsettings_probe_init(void *binary_base) {
#if defined(__aarch64__) || defined(__arm64__)
    if (!binary_base) return false;
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
