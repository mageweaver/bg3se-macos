/*
 * Entity lifecycle: Ext.Entity.Create / Ext.Entity.Destroy
 *
 * Recovered 2026-08-20 for build 4.1.1.7398727 by disassembling every call site
 * of ecs::EntityCommandBuffer::CreateEntity/DestroyEntity. All four sites use an
 * identical sequence:
 *
 *     ldr    x20, [x27]                 ; EntityWorld*
 *     bl     ls::ThreadRegistry::RequestThreadIndex()
 *     ldr    x8,  [x20, #0x230]         ; per-thread EntityCommandBuffer array
 *     mov    w9,  #0xc0                 ; stride 192 bytes
 *     smaddl x0,  w0, w9, x8            ; ecb = array + threadIndex * 0xC0
 *     bl     ecs::EntityCommandBuffer::CreateEntity   (or DestroyEntity)
 *
 * CreateEntity disassembles to:
 *     x0 = [this]                       ; +0x00 EntityHandleGenerator*
 *     bl EntityHandleGenerator::Create   -> new handle in x0
 *     x0 = this + 0x10, x2 = [this+0x8] ; +0x10 change map, +0x08 FrameAllocator*
 *     bl PagedHashMap::EnsureUniversal   -> ECBEntityChange*
 *     [x0+0x28] |= 1                     ; bit0 = Create
 *     return handle
 *
 * DestroyEntity is the same shape but sets bit1 and returns void.
 *
 * Both are deferred: the change lands when the command buffer is flushed
 * (ecs::EntityCommandBuffer::Flush), which matches Windows, where Ext.Entity
 * lifecycle goes through EntityWorld->Deferred().
 */

#include "entity_lifecycle.h"

#include "entity_system.h"
#include "logging.h"
#include "../core/safe_memory.h"
#include "../core/version_detect.h"
#include "../core/offset_table.h"

#include <lauxlib.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

/* Verified only against the build whose call sites were disassembled. */
#define ENTITY_LIFECYCLE_VERIFIED_BUILD "4.1.1.7398727"

/* EntityWorld + this offset holds the per-thread command buffer array. */
#define ENTITYWORLD_ECB_ARRAY_OFFSET 0x230
/* sizeof(EntityCommandBuffer) as used by the smaddl stride. */
#define ECB_STRIDE 0xC0

/* Nominal-image-base VAs; corrected by the load slide before use. */
#define VA_ECB_CREATE_ENTITY      0x10636764cULL
#define VA_ECB_DESTROY_ENTITY     0x10636769cULL
#define VA_REQUEST_THREAD_INDEX   0x1065401c0ULL

typedef uint64_t (*EcbCreateEntityFn)(void *ecb);
typedef void     (*EcbDestroyEntityFn)(void *ecb, uint64_t entity);
typedef int      (*RequestThreadIndexFn)(void);

static void *lifecycle_resolve(uint64_t va) {
    void *base = version_detect_get_binary_base();
    if (!base || !version_detect_matches()) return NULL;
    const VersionOffsets *offsets = offset_table_get();
    if (!offsets ||
        strcmp(offsets->version, ENTITY_LIFECYCLE_VERIFIED_BUILD) != 0) {
        return NULL;
    }
    uintptr_t slide = (uintptr_t)base - 0x100000000ull;
    return (void *)((uintptr_t)va + slide);
}

/*
 * Resolve this thread's EntityCommandBuffer.
 *
 * The buffer is per-thread by construction, so it must be resolved on the very
 * thread that will submit the command -- caching one across threads would post
 * commands into another thread's frame storage.
 */
void *entity_lifecycle_get_ecb(void *entity_world) {
    if (!entity_world) return NULL;

    RequestThreadIndexFn request_index =
        (RequestThreadIndexFn)lifecycle_resolve(VA_REQUEST_THREAD_INDEX);
    if (!request_index) return NULL;

    void *array = NULL;
    if (!safe_memory_read_pointer(
            (mach_vm_address_t)((uintptr_t)entity_world +
                                ENTITYWORLD_ECB_ARRAY_OFFSET),
            &array) || !array) {
        return NULL;
    }

    int index = request_index();
    if (index < 0 || index > 256) {
        LOG_ENTITY_DEBUG("EntityLifecycle: implausible thread index %d", index);
        return NULL;
    }

    void *ecb = (void *)((uintptr_t)array + (uintptr_t)index * ECB_STRIDE);

    /* Shape check before we hand this to the engine: +0x00 must be the handle
     * generator and +0x08 the frame allocator. Both are heap pointers. */
    void *generator = NULL, *allocator = NULL;
    if (!safe_memory_read_pointer((mach_vm_address_t)ecb, &generator) ||
        !generator ||
        !safe_memory_read_pointer((mach_vm_address_t)((uintptr_t)ecb + 8),
                                  &allocator) || !allocator) {
        LOG_ENTITY_DEBUG("EntityLifecycle: ECB at %p failed shape check", ecb);
        return NULL;
    }
    return ecb;
}

bool entity_lifecycle_available(void) {
    return lifecycle_resolve(VA_ECB_CREATE_ENTITY) != NULL;
}

uint64_t entity_lifecycle_create(void *entity_world) {
    EcbCreateEntityFn fn =
        (EcbCreateEntityFn)lifecycle_resolve(VA_ECB_CREATE_ENTITY);
    if (!fn) return 0;
    void *ecb = entity_lifecycle_get_ecb(entity_world);
    if (!ecb) return 0;
    uint64_t handle = fn(ecb);
    LOG_ENTITY_DEBUG("EntityLifecycle: created entity 0x%llx (deferred)",
                     (unsigned long long)handle);
    return handle;
}

bool entity_lifecycle_destroy(void *entity_world, uint64_t entity) {
    if (!entity) return false;
    EcbDestroyEntityFn fn =
        (EcbDestroyEntityFn)lifecycle_resolve(VA_ECB_DESTROY_ENTITY);
    if (!fn) return false;
    void *ecb = entity_lifecycle_get_ecb(entity_world);
    if (!ecb) return false;
    fn(ecb, entity);
    LOG_ENTITY_DEBUG("EntityLifecycle: destroyed entity 0x%llx (deferred)",
                     (unsigned long long)entity);
    return true;
}
