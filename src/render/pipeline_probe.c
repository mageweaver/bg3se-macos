/**
 * pipeline_probe.c — diagnostic tap on the Metal pipeline factory.
 *
 * The modded-hair freeze is an unbounded wait in rf::IAppStage::
 * AddPipelineState on a cache entry whose compiled pipeline never arrives.
 * This probe logs every rf::metal::GPUDevice::CreatePipelineState call
 * (descriptor pointer, first descriptor words, result, wall time) so a live
 * reproduction shows whether the factory is ever invoked for the stuck
 * descriptor, what it returns, and how long it takes. Diagnostic only —
 * enabled with BG3SE_PIPELINE_PROBE=1.
 */

#include "pipeline_probe.h"
#include "../core/logging.h"
#include <dobby.h>
#include <mach/mach_time.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>

#define ADDR_CREATE_PIPELINE_STATE 0x10645c0d8ULL
#define CPS_PROLOGUE_0             0xD10203FFu   /* sub sp, sp, #0x80 */

typedef void *(*CreatePipelineStateFn)(void *device, void *desc);
static CreatePipelineStateFn s_orig = NULL;
static bool s_installed = false;

static void *fake_CreatePipelineState(void *device, void *desc) {
    uint64_t d0 = 0, d1 = 0, d2 = 0, d3 = 0;
    if (desc) {
        const uint64_t *w = (const uint64_t *)desc;
        d0 = w[0]; d1 = w[1]; d2 = w[2]; d3 = w[3];
    }
    uint64_t t0 = mach_absolute_time();
    void *result = s_orig(device, desc);
    uint64_t t1 = mach_absolute_time();

    static mach_timebase_info_data_t tb;
    if (tb.denom == 0) mach_timebase_info(&tb);
    uint64_t ms = (t1 - t0) * tb.numer / tb.denom / 1000000ULL;

    LOG_CORE_INFO("PipelineProbe: desc=%p [%llx %llx %llx %llx] -> %p (%llums)",
                  desc,
                  (unsigned long long)d0, (unsigned long long)d1,
                  (unsigned long long)d2, (unsigned long long)d3,
                  result, (unsigned long long)ms);
    return result;
}

bool pipeline_probe_init(void *binary_base) {
    if (s_installed) return true;
    const char *env = getenv("BG3SE_PIPELINE_PROBE");
    if (!env || env[0] != '1') return false;

    uintptr_t addr = (uintptr_t)binary_base
                   + (ADDR_CREATE_PIPELINE_STATE - 0x100000000ULL);
    if (*(const uint32_t *)addr != CPS_PROLOGUE_0) {
        LOG_CORE_INFO("PipelineProbe: prologue mismatch, not installed");
        return false;
    }
    if (DobbyHook((void *)addr, (void *)fake_CreatePipelineState,
                  (void **)&s_orig) != 0 || !s_orig) {
        LOG_CORE_INFO("PipelineProbe: hook failed");
        return false;
    }
    s_installed = true;
    LOG_CORE_INFO("PipelineProbe: logging every CreatePipelineState call");
    return true;
}
