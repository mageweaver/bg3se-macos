#include "vt_unload_guard.h"
#include "../core/logging.h"
#include "../hooks/arm64_hook.h"
#include <stdint.h>

// ls::VirtualTextureManager::Unload(VirtualTextureLayerConfig::Type, ls::Path const&)
// Build 4.1.1.7398727, arm64. The not-found paths fall into an unconditional
// atomic refcount decrement on the (NULL) tileset:
//
//   10625e044  cbz w20, 0x10625e0fc   ; hash table empty  -> mov x20,#0 -> deref
//   10625e0a8  cbz x20, 0x10625e100   ; bucket chain miss -> x20 is NULL -> deref
//   ...
//   10625e100  add x8, x20, #0x1c
//   10625e108  ldaddal w9, w8, [x8]   ; SIGSEGV at 0x1c (observed 6x, 08-28/29,
//                                     ;   with and without the extender loaded)
//   10625e26c  <reload lock ptr, SRW write-unlock, epilogue>  ; never reads x20
//
// Fix: retarget both not-found branches to 0x10625e26c so unloading an
// unregistered tileset is a no-op (unlock + return). The found path (loop exit
// at 0x10625e0f8 -> 0x100) is untouched. Verified: the unlock block reloads its
// lock pointer from [sp,#0x8] and does not use x20.
//
// Every patch verifies the original encoding first and fails closed on any
// mismatch — an unknown build gets no patch, not a wrong one.

typedef struct {
    uint64_t va;        // unslid VA
    uint32_t original;  // expected encoding before patching
    uint32_t patched;   // encoding to write
    const char *what;
} VtPatch;

static const VtPatch k_patches[] = {
    { 0x10625e044ULL, 0x340005D4u, 0x34001154u, "empty-table cbz -> unlock" },
    { 0x10625e0a8ULL, 0xB40002D4u, 0xB4000E34u, "chain-miss cbz -> unlock" },
};
#define NUM_VT_PATCHES (sizeof(k_patches) / sizeof(k_patches[0]))

static bool s_installed = false;

bool vt_unload_guard_init(void *binary_base) {
    if (s_installed) return true;

    uintptr_t slide = (uintptr_t)binary_base - 0x100000000ULL;

    // Pass 1: verify every site before touching anything.
    for (unsigned i = 0; i < NUM_VT_PATCHES; i++) {
        const uint32_t *addr = (const uint32_t *)(k_patches[i].va + slide);
        uint32_t found = *addr;
        if (found != k_patches[i].original) {
            LOG_CORE_INFO(
                "VtUnloadGuard: NOT applied — %s at 0x%llx reads 0x%08x, "
                "expected 0x%08x (different build?)",
                k_patches[i].what, (unsigned long long)k_patches[i].va,
                found, k_patches[i].original);
            return false;
        }
    }

    // Pass 2: apply.
    for (unsigned i = 0; i < NUM_VT_PATCHES; i++) {
        void *addr = (void *)(k_patches[i].va + slide);
        if (!arm64_write_instruction(addr, k_patches[i].patched)) {
            LOG_CORE_INFO("VtUnloadGuard: write FAILED for %s at 0x%llx",
                          k_patches[i].what,
                          (unsigned long long)k_patches[i].va);
            return false;
        }
    }

    s_installed = true;
    LOG_CORE_INFO(
        "VtUnloadGuard: engine null-tileset fix applied "
        "(VirtualTextureManager::Unload, %u sites)",
        (unsigned)NUM_VT_PATCHES);
    return true;
}
