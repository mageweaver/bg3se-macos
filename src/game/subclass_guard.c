/**
 * subclass_guard.c - Defensive hook for eoc::character_creation::GetAvailableSubClassesForLevelUp.
 *
 * Root Cause Analysis:
 * In vanilla BG3, eoc::character_creation::GetAvailableSubClassesForLevelUp (arm64 offset 0x11e91f0)
 * performs two lookups in eoc::ClassDescriptions without checking for NULL before dereferencing:
 *
 * 1. Crash Site 1 at +320 (0x1011e9330):
 *      blr  x8                    ; eoc::ClassDescriptions::GetObjectByKey(classGuid)
 *      ...
 *      ldp  x1, x2, [x0, #0x70]   ; x0 is ClassDescription*. If not found, x0 == NULL -> SIGSEGV at 0x70!
 *    This happens when recruiting origins (such as Lae'zel during the Nautiloid cutscene)
 *    if the origin's ClassUUID or a modded class is not present in eoc::ClassDescriptions.
 *
 * 2. Crash Site 2 at +460 (0x1011e93bc):
 *      blr  x8                    ; eoc::ClassDescriptions::GetObjectByKey(subclassGuid)
 *      ldp  x1, x2, [x0, #0x70]   ; x0 is subclass ClassDescription*. If missing -> SIGSEGV at 0x70!
 *    This happens when mods (e.g., DoubleSubclass, subclass packs) inject dynamic or orphan
 *    subclass GUIDs into progression tables whose ClassDescriptions were not statically loaded.
 *
 * Fix:
 * Hook GetAvailableSubClassesForLevelUp via Dobby:
 * - If classGuid is not in ClassDescriptions, return an empty DynamicArray immediately (prevents +320).
 * - If any subclass in the progression table is missing from ClassDescriptions, prune it in-place
 *   before calling the original function (prevents +460).
 */

#include "subclass_guard.h"
#include "../core/logging.h"
/* Log GUIDs in Larian's byte order. Printing the raw words instead gives a
 * string that matches nothing in any mod's files, which makes the one thing
 * these warnings exist for -- tracing the bad GUID back to the mod that
 * references it -- impossible. See guid_format.h. */
#include "../core/guid_format.h"
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <dobby.h>

#if defined(__aarch64__) || defined(__arm64__)

typedef struct {
    uint64_t a;
    uint64_t b;
} NativeGuid;

typedef struct {
    void *data;
    int32_t capacity;
    int32_t size;
} NativeDynamicArray;

typedef struct {
    const void *begin;
    const void *end;
} NativeSpan;

typedef void (*GetAvailableSubClassesForLevelUpFn)(
    NativeDynamicArray *result,
    const NativeGuid *classGuid,
    const NativeSpan *levelUps,
    const void *env
);

/* Ceiling on how many subclasses one progression row can list and still be
 * pruned-then-restored from a stack copy. Real tables hold a handful; the
 * largest observed on a 122-mod class load order is 14 (Fighter). 64 leaves
 * generous headroom at 1 KB of stack. */
#define K_MAX_SUBCLASSES 64

static GetAvailableSubClassesForLevelUpFn s_orig_GetAvailableSubClasses = NULL;
static void *s_binary_base = NULL;

/* Per-build addresses, matched by the hook target's prologue so no version
 * lookup is needed at install time. The helper is only used by the build whose
 * target matched. */
typedef struct {
    uintptr_t target_rva;    /* eoc::character_creation::GetAvailableSubClassesForLevelUp */
    uintptr_t get_prog_rva;  /* eoc::ProgressionManager::GetProgressionByTableUUID */
    const char *build;
} SubclassGuardAddrs;

static const SubclassGuardAddrs k_builds[] = {
    { 0x11e91f0, 0x1c2b54c, "4.1.1.7398727" },
    { 0x11e91d8, 0x1c2b534, "4.1.1.7631656" },  /* hotfix: both moved -0x18 (nm) */
};
static uintptr_t s_get_prog_rva = 0;
static bool s_installed = false;

// Expected ARM64 instructions at eoc::character_creation::GetAvailableSubClassesForLevelUp (v4.1.1.7398727)
static const uint32_t k_expected_prologue[] = {
    0xa9bc5ff8u, // stp x24, x23, [sp, #-0x40]!
    0xa90157f6u, // stp x22, x21, [sp, #0x10]
    0xa9024ff4u, // stp x20, x19, [sp, #0x20]
    0xa9037bfdu, // stp x29, x30, [sp, #0x30]
};

static void hooked_GetAvailableSubClassesForLevelUp(
    NativeDynamicArray *result,
    const NativeGuid *classGuid,
    const NativeSpan *levelUps,
    const void *env
) {
    if (!result) return;

    if (!classGuid || !env) {
        result->data = NULL;
        result->capacity = 0;
        result->size = 0;
        return;
    }

    void *classDescs = *(void **)((const char *)env + 0x78);
    if (!classDescs) {
        result->data = NULL;
        result->capacity = 0;
        result->size = 0;
        return;
    }

    void **vt = *(void ***)classDescs;
    if (!vt) {
        result->data = NULL;
        result->capacity = 0;
        result->size = 0;
        return;
    }

    typedef void *(*GetObjectByKeyFn)(void *this_ptr, const NativeGuid *guid);
    GetObjectByKeyFn getObjectByKey = (GetObjectByKeyFn)vt[6]; // offset +0x30 in vtable
    if (!getObjectByKey) {
        result->data = NULL;
        result->capacity = 0;
        result->size = 0;
        return;
    }

    // Guard Site 1: Check if classGuid is registered in ClassDescriptions.
    void *classDesc = getObjectByKey(classDescs, classGuid);
    if (!classDesc) {
        char gs[GUID_STRING_SIZE];
        guid_bytes_to_string((const uint8_t *)classGuid, gs, sizeof(gs));
        LOG_CORE_WARN("[SubclassGuard] Class GUID %s not found in ClassDescriptions; "
                      "returning empty subclasses list (prevented engine crash at +320). "
                      "Grep the mods for that GUID to find what references it.", gs);
        result->data = NULL;
        result->capacity = 0;
        result->size = 0;
        return;
    }

    // Guard Site 2: Check subclasses listed in the progression table for this level.
    void *progMgr = *(void **)((const char *)env + 0xc0);
    if (progMgr && s_binary_base) {
        const NativeGuid *tableUuid = (const NativeGuid *)((const char *)classDesc + 0x70);
        int targetLevel = 1;
        bool isMulticlass = false;

        if (levelUps && levelUps->begin && levelUps->end && levelUps->begin != levelUps->end) {
            int count = 0;
            const char *begin = (const char *)levelUps->begin;
            const char *end = (const char *)levelUps->end;
            for (const char *p = begin; p + 0xe0 <= end; p += 0xe0) {
                if (memcmp(p, classGuid, sizeof(NativeGuid)) == 0) {
                    count++;
                }
            }
            targetLevel = count + 1;
            isMulticlass = (memcmp(begin, classGuid, sizeof(NativeGuid)) != 0);
        }

        /* Deliberately unvalidated, unlike the hook target: this address is only
         * ever reached when the prologue check below matched, so a build that
         * moved either function installs no hook at all and never gets here. */
        typedef void *(*GetProgressionFn)(void *this_ptr, uint64_t uuid_a, uint64_t uuid_b, int level, bool isMulticlass);
        GetProgressionFn getProg = (GetProgressionFn)((uintptr_t)s_binary_base + s_get_prog_rva);
        void *prog = getProg(progMgr, tableUuid->a, tableUuid->b, targetLevel, isMulticlass);
        if (prog) {
            int32_t count = *(int32_t *)((const char *)prog + 0x44);
            NativeGuid *subclasses = *(NativeGuid **)((const char *)prog + 0x38);
            if (count > 0 && count <= K_MAX_SUBCLASSES && subclasses) {
                /* The prune below edits the engine's own loaded Progression, which
                 * is shared: every character and every later level-up sees it. So
                 * the original contents are saved and put back once the original
                 * has read them.
                 *
                 * Without the restore this is a one-way door. GetObjectByKey only
                 * has to fail once -- called a moment too early, before the class
                 * banks are fully populated, which is exactly the timing bug that
                 * bit the GUIDSTRING guard -- and a legitimate subclass is gone
                 * from the level-up list for the rest of the session, with the
                 * player given no reason. Restoring keeps the crash protection and
                 * leaves data we do not own untouched. */
                NativeGuid saved[K_MAX_SUBCLASSES];
                memcpy(saved, subclasses, (size_t)count * sizeof(NativeGuid));

                int32_t valid = 0;
                for (int32_t i = 0; i < count; i++) {
                    if (getObjectByKey(classDescs, &subclasses[i]) != NULL) {
                        if (valid != i) {
                            subclasses[valid] = subclasses[i];
                        }
                        valid++;
                    } else {
                        char sub[GUID_STRING_SIZE], tab[GUID_STRING_SIZE];
                        guid_bytes_to_string((const uint8_t *)&subclasses[i], sub, sizeof(sub));
                        guid_bytes_to_string((const uint8_t *)tableUuid, tab, sizeof(tab));
                        LOG_CORE_WARN("[SubclassGuard] Subclass GUID %s not in ClassDescriptions; "
                                      "pruned from progression table %s for this call "
                                      "(prevented engine crash at +460)", sub, tab);
                    }
                }
                *(int32_t *)((char *)prog + 0x44) = valid;

                s_orig_GetAvailableSubClasses(result, classGuid, levelUps, env);

                /* Put the table back exactly as it was, pruned entries included. */
                memcpy(subclasses, saved, (size_t)count * sizeof(NativeGuid));
                *(int32_t *)((char *)prog + 0x44) = count;
                return;
            }
            if (count > K_MAX_SUBCLASSES) {
                /* Too many to save on the stack. Pruning without being able to
                 * restore is not worth a permanent edit, so leave it alone and let
                 * the engine decide -- site 1 is still guarded above. */
                LOG_CORE_WARN("[SubclassGuard] progression table %016llx%016llx lists %d subclasses "
                              "(> %d); left unpruned so it is not permanently modified",
                              (unsigned long long)tableUuid->b, (unsigned long long)tableUuid->a,
                              (int)count, (int)K_MAX_SUBCLASSES);
            }
        }
    }

    // Call the original function safely
    s_orig_GetAvailableSubClasses(result, classGuid, levelUps, env);
}

#endif /* defined(__aarch64__) || defined(__arm64__) */

bool subclass_guard_init(void *binary_base) {
#if !defined(__aarch64__) && !defined(__arm64__)
    (void)binary_base;
    LOG_CORE_INFO("[SubclassGuard] Skipped on non-ARM64 architecture");
    return true;
#else
    if (s_installed) return true;
    if (!binary_base) return false;

    s_binary_base = binary_base;

    // eoc::character_creation::GetAvailableSubClassesForLevelUp: the build
    // whose target carries the expected prologue.
    void *target = NULL;
    for (size_t i = 0; i < sizeof(k_builds) / sizeof(k_builds[0]); i++) {
        void *cand = (void *)((uintptr_t)binary_base + k_builds[i].target_rva);
        if (memcmp(cand, k_expected_prologue, sizeof(k_expected_prologue)) == 0) {
            target = cand;
            s_get_prog_rva = k_builds[i].get_prog_rva;
            LOG_CORE_INFO("[SubclassGuard] target matched for build %s", k_builds[i].build);
            break;
        }
    }
    if (!target) {
        LOG_CORE_WARN("[SubclassGuard] NOT applied — no known target address carries the "
                      "expected prologue (different game build?)");
        return false;
    }

    int ret = DobbyHook(target, (void *)hooked_GetAvailableSubClassesForLevelUp, (void **)&s_orig_GetAvailableSubClasses);
    if (ret != 0) {
        LOG_CORE_WARN("[SubclassGuard] DobbyHook failed (%d) at %p", ret, target);
        return false;
    }

    s_installed = true;
    LOG_CORE_INFO("[SubclassGuard] Installed defensive hook on GetAvailableSubClassesForLevelUp at %p", target);
    return true;
#endif
}
