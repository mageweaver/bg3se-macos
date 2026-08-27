#include "focus_hack.h"
#include "../core/logging.h"
#include "../core/offset_table.h"
#include "../core/safe_memory.h"
#include <dispatch/dispatch.h>
#include <mach-o/dyld.h>
#include <string.h>

// BaseApp::s_AppInstance slot (nm: __ZN7BaseApp13s_AppInstanceE) is carried
// per version in the offset table (baseapp_instance_ptr). This module WRITES
// the +0x142 focus flag through the pointer read from that slot, so a stale
// address is memory corruption, not just a failed read — an unknown build
// must resolve to nothing and fail closed.

// Focus flag offset within BaseApp (byte field)
// Found via Ghidra RE of BaseApp::OnFocusChange:
//   *(char *)((long)param_1 + 0x142) = (char)param_2;
#define BASEAPP_FOCUS_OFFSET 0x142

static void *s_baseapp = NULL;
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

bool focus_hack_init(void) {
    if (s_initialized && s_baseapp) return true;

    uintptr_t base = get_base_address();
    if (!base) {
        LOG_CORE_ERROR("[FocusHack] BG3 base address not found");
        return false;
    }

    const VersionOffsets *off = offset_table_get();
    if (!off || !off->baseapp_instance_ptr) {
        LOG_CORE_INFO("[FocusHack] BaseApp slot not verified for this game "
                      "version — focus hack disabled");
        return false;
    }

    uintptr_t slide = base - 0x100000000;
    uintptr_t ptr_addr = base + off->baseapp_instance_ptr;

    void *instance = NULL;
    if (!safe_memory_read_pointer((mach_vm_address_t)ptr_addr, &instance)) {
        LOG_CORE_ERROR("[FocusHack] Failed to read BaseApp ptr at 0x%lx", (unsigned long)ptr_addr);
        return false;
    }

    if (!instance) {
        LOG_CORE_DEBUG("[FocusHack] BaseApp::s_AppInstance not yet set (NULL)");
        return false;
    }

    /*
     * Shape-validate before trusting this pointer: this module WRITES through
     * it, so a wrong address is corruption rather than a bad read. The symbol
     * name alone is not enough -- the concrete app object is a BaseApp
     * subclass, so its vtable is not necessarily `vtable for BaseApp` and an
     * exact-vtable test would fail closed on a correct pointer. Check instead
     * that it looks like the object BaseApp::HasFocus reads from:
     *   - a vtable pointer that lands inside the main image
     *   - bool-valued bytes at +0x141 (IsStopRequested) and +0x142 (HasFocus)
     */
    void *vtable = NULL;
    if (!safe_memory_read_pointer((mach_vm_address_t)instance, &vtable) || !vtable) {
        LOG_CORE_ERROR("[FocusHack] BaseApp at 0x%lx has no readable vtable — refusing to write",
                       (unsigned long)instance);
        return false;
    }
    // The binary is ~500 MB; 0x20000000 bounds it with room to spare.
    if ((uintptr_t)vtable < base || (uintptr_t)vtable >= base + 0x20000000) {
        LOG_CORE_ERROR("[FocusHack] BaseApp vtable 0x%lx outside the game image "
                       "(base 0x%lx) — refusing to write",
                       (unsigned long)vtable, (unsigned long)base);
        return false;
    }

    uint8_t stop_flag = 0xff, focus_flag = 0xff;
    if (!safe_memory_read_u8((mach_vm_address_t)((uint8_t *)instance + 0x141), &stop_flag) ||
        !safe_memory_read_u8((mach_vm_address_t)((uint8_t *)instance + BASEAPP_FOCUS_OFFSET),
                             &focus_flag) ||
        stop_flag > 1 || focus_flag > 1) {
        LOG_CORE_ERROR("[FocusHack] BaseApp flags not bool-valued "
                       "(+0x141=%u +0x%x=%u) — refusing to write",
                       stop_flag, BASEAPP_FOCUS_OFFSET, focus_flag);
        return false;
    }

    s_baseapp = instance;
    s_initialized = true;
    LOG_CORE_INFO("[FocusHack] BaseApp instance at 0x%lx (slide=0x%lx)",
                  (unsigned long)s_baseapp, (unsigned long)slide);
    return true;
}

bool focus_hack_force_focused(void) {
    if (!s_baseapp && !focus_hack_init()) return false;

    uint8_t *focus_flag = (uint8_t *)s_baseapp + BASEAPP_FOCUS_OFFSET;
    uint8_t old_val = *focus_flag;
    *focus_flag = 1;

    LOG_CORE_INFO("[FocusHack] Forced focus: %d -> 1 (at BaseApp+0x%x = 0x%lx)",
                  old_val, BASEAPP_FOCUS_OFFSET,
                  (unsigned long)((uintptr_t)s_baseapp + BASEAPP_FOCUS_OFFSET));
    return true;
}

bool focus_hack_is_focused(void) {
    if (!s_baseapp && !focus_hack_init()) return false;

    uint8_t *focus_flag = (uint8_t *)s_baseapp + BASEAPP_FOCUS_OFFSET;
    return *focus_flag != 0;
}

static int s_deferred_attempts = 0;
#define MAX_DEFERRED_ATTEMPTS 30

static void deferred_force_focus(void *ctx __attribute__((unused))) {
    s_deferred_attempts++;

    if (focus_hack_init()) {
        focus_hack_force_focused();
        LOG_CORE_INFO("[FocusHack] Deferred force-focus succeeded after %d attempts",
                      s_deferred_attempts);
        return;
    }

    if (s_deferred_attempts >= MAX_DEFERRED_ATTEMPTS) {
        LOG_CORE_ERROR("[FocusHack] Gave up after %d attempts — BaseApp never appeared",
                       s_deferred_attempts);
        return;
    }

    dispatch_after(dispatch_time(DISPATCH_TIME_NOW, (int64_t)(500 * NSEC_PER_MSEC)),
                   dispatch_get_main_queue(),
                   ^{ deferred_force_focus(NULL); });
}

void focus_hack_deferred_force_focus(void) {
    s_deferred_attempts = 0;

    if (focus_hack_init() && focus_hack_force_focused()) {
        return;
    }

    LOG_CORE_INFO("[FocusHack] Starting deferred force-focus polling (500ms intervals, max %d)",
                  MAX_DEFERRED_ATTEMPTS);
    dispatch_after(dispatch_time(DISPATCH_TIME_NOW, (int64_t)(500 * NSEC_PER_MSEC)),
                   dispatch_get_main_queue(),
                   ^{ deferred_force_focus(NULL); });
}
