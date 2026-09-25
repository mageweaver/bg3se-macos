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

static GetAvailableSubClassesForLevelUpFn s_orig_GetAvailableSubClasses = NULL;
static void *s_binary_base = NULL;
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
        LOG_CORE_WARN("[SubclassGuard] Class GUID %016llx%016llx not found in ClassDescriptions; "
                      "returning empty subclasses list (prevented engine crash at +320)",
                      (unsigned long long)classGuid->b, (unsigned long long)classGuid->a);
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

        typedef void *(*GetProgressionFn)(void *this_ptr, uint64_t uuid_a, uint64_t uuid_b, int level, bool isMulticlass);
        GetProgressionFn getProg = (GetProgressionFn)((uintptr_t)s_binary_base + 0x1c2b54c);
        void *prog = getProg(progMgr, tableUuid->a, tableUuid->b, targetLevel, isMulticlass);
        if (prog) {
            int32_t count = *(int32_t *)((const char *)prog + 0x44);
            NativeGuid *subclasses = *(NativeGuid **)((const char *)prog + 0x38);
            if (count > 0 && subclasses) {
                int32_t valid = 0;
                for (int32_t i = 0; i < count; i++) {
                    if (getObjectByKey(classDescs, &subclasses[i]) != NULL) {
                        if (valid != i) {
                            subclasses[valid] = subclasses[i];
                        }
                        valid++;
                    } else {
                        LOG_CORE_WARN("[SubclassGuard] Subclass GUID %016llx%016llx not in ClassDescriptions; "
                                      "pruned from progression table %016llx%016llx (prevented engine crash at +460)",
                                      (unsigned long long)subclasses[i].b, (unsigned long long)subclasses[i].a,
                                      (unsigned long long)tableUuid->b, (unsigned long long)tableUuid->a);
                    }
                }
                *(int32_t *)((char *)prog + 0x44) = valid;
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

    // Address of eoc::character_creation::GetAvailableSubClassesForLevelUp
    void *target = (void *)((uintptr_t)binary_base + 0x11e91f0ULL);

    // Verify prologue before hooking
    if (memcmp(target, k_expected_prologue, sizeof(k_expected_prologue)) != 0) {
        LOG_CORE_WARN("[SubclassGuard] NOT applied — target at %p does not match expected prologue (different game build?)",
                      target);
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
