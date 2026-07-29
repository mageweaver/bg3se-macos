#include "global_switches.h"
#include "../core/logging.h"
#include "../core/safe_memory.h"
#include "../core/offset_table.h"
#include <dispatch/dispatch.h>
#include <mach-o/dyld.h>
#include <string.h>

// The GlobalSwitches singleton slot (double pointer) is per-version:
// VersionOffsets.global_switches_ptr. It does NOT follow the uniform __DATA
// shift (moved -0x24000 between 6995620 and 7209685), so it must never be
// derived via component_data_shift.

// SkipSplashScreen field offset within EoCGlobalSwitches struct (ARM64 macOS)
// Found via RE: LDRB w8, [x8, #0x6ac] after SkipSplashScreen property registration.
// Re-verified on 7209685 (2026-07-29): ldr xN,[slot] ; ldrb/strb [xN, #0x6ac]
// appears at multiple sites, including a toggle (eor #1) sequence.
#define OFFSET_SKIP_SPLASH_SCREEN 0x6ac

static void *s_global_switches = NULL;
static bool s_initialized = false;

static uintptr_t get_base_address(void) {
    for (uint32_t i = 0; i < _dyld_image_count(); i++) {
        const char *name = _dyld_get_image_name(i);
        if (name && strstr(name, "Baldur")) {
            return (uintptr_t)_dyld_get_image_header(i);
        }
    }
    return 0;
}

bool global_switches_init(void) {
    if (s_initialized && s_global_switches) return true;

    uintptr_t base = get_base_address();
    if (!base) {
        LOG_CORE_ERROR("[GlobalSwitches] BG3 base address not found");
        return false;
    }

    // Fail closed: no table row for this version -> feature disabled.
    const VersionOffsets *vo = offset_table_get();
    if (!vo || !vo->global_switches_ptr) {
        LOG_CORE_INFO("[GlobalSwitches] No verified slot for this game version — disabled");
        return false;
    }
    uintptr_t ptr_addr = base + vo->global_switches_ptr;

    void *ptr_val = NULL;
    if (!safe_memory_read_pointer((mach_vm_address_t)ptr_addr, &ptr_val)) {
        LOG_CORE_ERROR("[GlobalSwitches] Failed to read ptr at 0x%lx", (unsigned long)ptr_addr);
        return false;
    }

    if (!ptr_val) {
        LOG_CORE_DEBUG("[GlobalSwitches] Singleton not yet allocated (ptr=NULL)");
        return false;
    }

    s_global_switches = ptr_val;
    s_initialized = true;
    LOG_CORE_INFO("[GlobalSwitches] Found singleton at 0x%lx (base=0x%lx)",
                  (unsigned long)s_global_switches, (unsigned long)base);
    return true;
}

bool global_switches_set_skip_splash_screen(bool value) {
    if (!s_global_switches && !global_switches_init()) return false;

    uint8_t *field = (uint8_t *)s_global_switches + OFFSET_SKIP_SPLASH_SCREEN;

    // Read-back guard before the raw write: the field must be readable and
    // already hold a bool (0/1). Anything else means the pointer or struct
    // layout is wrong for this build — refuse to write.
    uint8_t current = 0xFF;
    if (!safe_memory_read((mach_vm_address_t)field, &current, 1) || current > 1) {
        LOG_CORE_ERROR("[GlobalSwitches] Sanity check failed at %p (+0x%x, val=0x%x) — refusing write",
                       (void *)field, OFFSET_SKIP_SPLASH_SCREEN, current);
        return false;
    }

    *field = value ? 1 : 0;
    LOG_CORE_INFO("[GlobalSwitches] SkipSplashScreen = %s (at 0x%lx + 0x%x)",
                  value ? "true" : "false",
                  (unsigned long)s_global_switches, OFFSET_SKIP_SPLASH_SCREEN);
    return true;
}

bool global_switches_get_skip_splash_screen(void) {
    if (!s_global_switches && !global_switches_init()) return false;

    uint8_t *field = (uint8_t *)s_global_switches + OFFSET_SKIP_SPLASH_SCREEN;
    return *field != 0;
}

static int s_deferred_attempts = 0;
#define MAX_DEFERRED_ATTEMPTS 30

static void deferred_set_skip_splash(void *ctx __attribute__((unused))) {
    s_deferred_attempts++;

    if (global_switches_init()) {
        global_switches_set_skip_splash_screen(true);
        LOG_CORE_INFO("[GlobalSwitches] Deferred SkipSplashScreen set after %d attempts", s_deferred_attempts);
        return;
    }

    if (s_deferred_attempts >= MAX_DEFERRED_ATTEMPTS) {
        LOG_CORE_ERROR("[GlobalSwitches] Gave up after %d attempts — singleton never appeared", s_deferred_attempts);
        return;
    }

    dispatch_after(dispatch_time(DISPATCH_TIME_NOW, (int64_t)(500 * NSEC_PER_MSEC)),
                   dispatch_get_main_queue(),
                   ^{ deferred_set_skip_splash(NULL); });
}

void global_switches_deferred_set_skip_splash_screen(void) {
    s_deferred_attempts = 0;

    if (global_switches_init() && global_switches_set_skip_splash_screen(true)) {
        return;
    }

    LOG_CORE_INFO("[GlobalSwitches] Starting deferred SkipSplashScreen polling (500ms intervals, max %d)", MAX_DEFERRED_ATTEMPTS);
    dispatch_after(dispatch_time(DISPATCH_TIME_NOW, (int64_t)(500 * NSEC_PER_MSEC)),
                   dispatch_get_main_queue(),
                   ^{ deferred_set_skip_splash(NULL); });
}
