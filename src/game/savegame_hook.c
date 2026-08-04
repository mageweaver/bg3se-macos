#include "savegame_hook.h"

#include "../core/crashlog.h"
#include "../core/logging.h"
#include "../core/safe_memory.h"
#include "../core/version_detect.h"
#include "../hooks/arm64_hook.h"

#include <inttypes.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

/* Evidence: ghidra/offsets/SAVEGAME_HOOK_SURFACE.md, build 4.1.1.7209685. */
#define SAVEGAME_HOOK_GHIDRA_BASE UINT64_C(0x100000000)
#define SAVEGAME_HOOK_VISIT_VA UINT64_C(0x104b51a9c)
#define SAVEGAME_HOOK_VISITOR_NESTED_OFFSET UINT64_C(0xb0)
#define SAVEGAME_HOOK_NESTED_MODE_OFFSET UINT64_C(0x8)
#define SAVEGAME_HOOK_VERIFIED_BUILD "4.1.1.7209685"

typedef bool (*SavegameVisitProc)(void *self, void *visitor);

static const uint8_t s_expected_prologue[] = {
    0xff, 0x03, 0x01, 0xd1,
    0xf6, 0x57, 0x01, 0xa9,
    0xf4, 0x4f, 0x02, 0xa9,
    0xfd, 0x7b, 0x03, 0xa9,
};

static ARM64HookHandle *s_hook;
static void *s_original_address;
static uintptr_t s_binary_base;
static _Atomic uint64_t s_call_count;
static _Thread_local bool s_observing;

_Static_assert(sizeof(SavegameVisitProc) == sizeof(void *),
               "SavegameVisit function pointer must fit in a code address");

static SavegameVisitProc savegame_hook_original(void) {
    SavegameVisitProc original = NULL;
    memcpy(&original, &s_original_address, sizeof(original));
    return original;
}

static void *savegame_hook_replacement_address(void);

static bool savegame_hook_visit(void *self, void *visitor) {
    uint64_t call = atomic_fetch_add_explicit(
                        &s_call_count, UINT64_C(1), memory_order_relaxed)
        + UINT64_C(1);
    uint32_t breadcrumb_call = (uint32_t)call;

    breadcrumb_mark("savegame_hook_entry", breadcrumb_call);

    if (!s_observing) {
        void *nested = NULL;
        uint32_t mode = UINT32_MAX;
        bool nested_read = false;
        bool mode_read = false;
        uint64_t thread_id = 0;
        uintptr_t caller = (uintptr_t)__builtin_return_address(0);
        uint64_t caller_rva = caller >= s_binary_base
            ? (uint64_t)(caller - s_binary_base)
            : UINT64_MAX;

        s_observing = true;
        if (visitor) {
            nested_read = safe_memory_read_pointer(
                (mach_vm_address_t)((uintptr_t)visitor
                                    + SAVEGAME_HOOK_VISITOR_NESTED_OFFSET),
                &nested);
        }
        if (nested_read && nested) {
            mode_read = safe_memory_read_u32(
                (mach_vm_address_t)((uintptr_t)nested
                                    + SAVEGAME_HOOK_NESTED_MODE_OFFSET),
                &mode);
        }
        (void)pthread_threadid_np(NULL, &thread_id);

        const char *direction = !mode_read ? "UNKNOWN"
            : (mode == 0 ? "READ" : "WRITE");
        LOG_GAME_INFO(
            "[SavegameHook] entry call=%" PRIu64
            " self=%p visitor=%p nested=%p nested_read=%s"
            " mode=%" PRIu32 " mode_read=%s direction=%s"
            " caller_rva=0x%" PRIx64 " thread=%" PRIu64,
            call, self, visitor, nested, nested_read ? "ok" : "failed",
            mode, mode_read ? "ok" : "failed", direction, caller_rva,
            thread_id);
        s_observing = false;
    }

    breadcrumb_mark("savegame_hook_tailcall", breadcrumb_call);
    return savegame_hook_original()(self, visitor);
}

static void *savegame_hook_replacement_address(void) {
    SavegameVisitProc replacement = savegame_hook_visit;
    void *address = NULL;
    memcpy(&address, &replacement, sizeof(address));
    return address;
}

void savegame_hook_init(void) {
    if (s_hook) {
        LOG_GAME_INFO("[SavegameHook] armed: hook already installed");
        return;
    }

    const char *enabled = getenv("BG3SE_SAVEGAME_SPIKE");
    if (!enabled || strcmp(enabled, "1") != 0) {
        LOG_GAME_INFO(
            "[SavegameHook] disarmed: BG3SE_SAVEGAME_SPIKE is not exactly 1");
        return;
    }

    const char *version = version_detect_get_version();
    if (!version || !version_detect_matches()
        || strcmp(version, SAVEGAME_HOOK_VERIFIED_BUILD) != 0) {
        LOG_GAME_INFO(
            "[SavegameHook] disarmed: build is %s; requires %s",
            version ? version : "unknown", SAVEGAME_HOOK_VERIFIED_BUILD);
        return;
    }

    void *binary_base = version_detect_get_binary_base();
    if (!binary_base) {
        LOG_GAME_INFO("[SavegameHook] disarmed: main-image base is unavailable");
        return;
    }

    uintptr_t target_address = (uintptr_t)binary_base
        + (SAVEGAME_HOOK_VISIT_VA - SAVEGAME_HOOK_GHIDRA_BASE);
    uint8_t prologue[sizeof(s_expected_prologue)];
    if (!safe_memory_read((mach_vm_address_t)target_address,
                          prologue, sizeof(prologue))) {
        LOG_GAME_INFO(
            "[SavegameHook] disarmed: prologue at %p is unreadable",
            (void *)target_address);
        return;
    }
    if (memcmp(prologue, s_expected_prologue, sizeof(prologue)) != 0) {
        LOG_GAME_INFO(
            "[SavegameHook] disarmed: prologue byte gate failed at %p",
            (void *)target_address);
        return;
    }

    atomic_store_explicit(&s_call_count, UINT64_C(0), memory_order_relaxed);
    s_binary_base = (uintptr_t)binary_base;
    s_original_address = NULL;

    /* Offset 0 is mandatory; the evidence doc proves automatic +4 is unsafe. */
    s_hook = arm64_hook_at_offset((void *)target_address, 0,
                                  savegame_hook_replacement_address(),
                                  &s_original_address);
    if (!s_hook || !s_original_address) {
        if (s_hook) {
            (void)arm64_unhook(s_hook);
            s_hook = NULL;
        }
        s_binary_base = 0;
        s_original_address = NULL;
        LOG_GAME_INFO("[SavegameHook] disarmed: hook installation failed");
        return;
    }

    LOG_GAME_INFO(
        "[SavegameHook] armed: build=%s target=%p env=BG3SE_SAVEGAME_SPIKE=1",
        version, (void *)target_address);
}

void savegame_hook_shutdown(void) {
    if (!s_hook) return;

    if (!arm64_unhook(s_hook)) {
        LOG_GAME_ERROR("[SavegameHook] shutdown: arm64_unhook failed; hook remains armed");
        return;
    }

    s_hook = NULL;
    s_original_address = NULL;
    s_binary_base = 0;
    LOG_GAME_INFO("[SavegameHook] shutdown: hook uninstalled cleanly");
}
