/**
 * pipeline_wait_guard.c — engine bugfix: unbounded pipeline-cache wait.
 *
 * rf::IAppStage::AddPipelineState caches pipeline entries by descriptor hash.
 * A losing racer that finds an existing entry waits for its compiled pipeline:
 *
 *   106410f44  ldr x8, [x20, #0x8]     ; entry->pipeline
 *   106410f48  cbnz x8, 1064114c4      ; ready -> return entry (x20)
 *   106410f4c  bl _pthread_yield_np
 *   106410f50  b 106410f44             ; wait forever
 *
 * But the producer stores the compile result UNCONDITIONALLY, null included
 * (106411250: str x0, [x20, #0x8] right after CreatePipelineState) — so a
 * failed compile leaves a permanently-null entry and every later request for
 * the same descriptor spins forever. Observed live: selecting a modded hair
 * whose material yields NullHandle shader IDs creates exactly such an entry
 * (CreatePipelineState correctly returns null for it), and the second request
 * freezes the render thread at ~300% CPU.
 *
 * Fix: retarget the loop's back-branch to the exit. The waiter yields once
 * and returns the entry regardless — exactly the state the first caller
 * returns when a compile fails, which the renderer already tolerates. An
 * entry whose compile completes later still self-heals: consumers hold the
 * entry and see the pipeline pointer once it lands.
 */

#include "pipeline_wait_guard.h"
#include "../core/logging.h"
#include "../hooks/arm64_hook.h"
#include <stdint.h>

#define ADDR_WAIT_BRANCH   0x106410f50ULL
#define ORIG_INSN          0x17FFFFFDu   /* b 0x106410f44 (back to the load) */
#define PATCHED_INSN       0x1400015Du   /* b 0x1064114c4 (exit, returns x20) */

static bool s_installed = false;

bool pipeline_wait_guard_init(void *binary_base) {
    if (s_installed) return true;
    uintptr_t addr = (uintptr_t)binary_base + (ADDR_WAIT_BRANCH - 0x100000000ULL);
    uint32_t found = *(const uint32_t *)addr;
    if (found != ORIG_INSN) {
        LOG_CORE_INFO("PipelineWaitGuard: NOT applied — 0x%llx reads 0x%08x, "
                      "expected 0x%08x (different build?)",
                      (unsigned long long)ADDR_WAIT_BRANCH, found, ORIG_INSN);
        return false;
    }
    if (!arm64_write_instruction((void *)addr, PATCHED_INSN)) {
        LOG_CORE_INFO("PipelineWaitGuard: write failed");
        return false;
    }
    s_installed = true;
    LOG_CORE_INFO("PipelineWaitGuard: unbounded pipeline-cache wait bounded "
                  "(failed compiles no longer freeze requesters)");
    return true;
}
