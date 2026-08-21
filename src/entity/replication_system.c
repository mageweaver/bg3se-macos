/*
 * BG3SE-macOS — esv::replication::ReplicationSystem access.
 *
 * Why this exists
 * ---------------
 * Windows implements entity:Replicate by writing SyncBuffers component pools
 * (LuaEntityProxy.inl:357). On 4.1.1.7398727 those pools are reachable and
 * structurally sound — 582 of them, offsets consistent with the engine HashMap
 * layout — but a latching sampler running from a hot hook never observed a
 * single populated pool, including with a console client connected over
 * crossplay and replication visibly working on both devices. So that is not the
 * mechanism this build replicates through.
 *
 * The binary instead exports a server replication system:
 *
 *   esv::replication::ReplicationSystem::ReplicateToPeer(
 *       ls::ID<ecs::EntityHandleTraits>, ls::TypeWrap<int, net::PeerIDClassname, true>)
 *   esv::replication::ReplicationSystem::StopReplicateWith(
 *       ecs::EntityWorld*, ls::ID<ecs::EntityHandleTraits>)
 *
 * which is a plain member call: x0 = system, x1 = entity handle, w2 = peer id.
 *
 * The system instance is resolved the same way ecs_system_update.c resolves the
 * other 82 systems: read the SystemsContext type index from the type's TypeId
 * global, then index the world's system array.
 */

#include "replication_system.h"

#include "../core/logging.h"
#include "../core/safe_memory.h"
#include "../core/version_detect.h"

#include <string.h>

#define REPL_GHIDRA_BASE 0x100000000ULL

/* ls::TypeId<esv::replication::ReplicationSystem, ecs::SystemsContext>::m_TypeIndex */
#define VA_REPLICATION_SYSTEM_TYPEID 0x10892fcf8ULL
/* esv::replication::ReplicationSystem::ReplicateToPeer(EntityHandle, PeerID) */
#define VA_REPLICATE_TO_PEER         0x105630d70ULL
/* esv::replication::ReplicationSystem::StopReplicateWith(EntityWorld*, EntityHandle) */
#define VA_STOP_REPLICATE_WITH       0x1056442f0ULL

/* Shared with ecs_system_update.c — the world's system array. */
#define ECS_WORLD_SYSTEM_BUFFER_OFFSET   0x30u
#define ECS_WORLD_SYSTEM_CAPACITY_OFFSET 0x38u
#define ECS_WORLD_SYSTEM_USED_OFFSET     0x3cu
#define ECS_SYSTEM_ENTRY_STRIDE          0xf8u
#define ECS_SYSTEM_ENTRY_SYSTEM_OFFSET   0x00u
#define ECS_SYSTEM_ENTRY_INDEX0_OFFSET   0x08u
#define ECS_SYSTEM_ENTRY_INDEX1_OFFSET   0x0cu

#define REPLICATION_VERIFIED_BUILD "4.1.1.7398727"

typedef void (*ReplicateToPeerFn)(void *system, uint64_t entity, int peer_id);
typedef void (*StopReplicateWithFn)(void *system, void *world, uint64_t entity);

static void *resolve_va(uint64_t va) {
    void *base = version_detect_get_binary_base();
    if (!base || !version_detect_matches()) return NULL;
    const char *v = version_detect_get_version();
    if (!v || strcmp(v, REPLICATION_VERIFIED_BUILD) != 0) return NULL;
    return (void *)((uintptr_t)base + (uintptr_t)(va - REPL_GHIDRA_BASE));
}

void *replication_system_get(void *world) {
    if (!world) return NULL;

    void *type_id_addr = resolve_va(VA_REPLICATION_SYSTEM_TYPEID);
    if (!type_id_addr) return NULL;

    int32_t system_index = -1;
    if (!safe_memory_read_i32((mach_vm_address_t)type_id_addr, &system_index) ||
        system_index < 0) {
        return NULL;
    }

    void *buffer = NULL;
    uint32_t capacity = 0, used = 0;
    if (!safe_memory_read_pointer(
            (mach_vm_address_t)((uintptr_t)world + ECS_WORLD_SYSTEM_BUFFER_OFFSET), &buffer) ||
        !safe_memory_read_u32(
            (mach_vm_address_t)((uintptr_t)world + ECS_WORLD_SYSTEM_CAPACITY_OFFSET), &capacity) ||
        !safe_memory_read_u32(
            (mach_vm_address_t)((uintptr_t)world + ECS_WORLD_SYSTEM_USED_OFFSET), &used) ||
        !buffer || capacity == 0 || capacity > 8192 || used == 0 || used > capacity ||
        (uint32_t)system_index >= used) {
        return NULL;
    }

    uintptr_t entry = (uintptr_t)buffer +
                      (uintptr_t)(uint32_t)system_index * ECS_SYSTEM_ENTRY_STRIDE;
    void *system = NULL;
    int32_t index0 = -1, index1 = -1;
    if (!safe_memory_read_pointer(
            (mach_vm_address_t)(entry + ECS_SYSTEM_ENTRY_SYSTEM_OFFSET), &system) ||
        !safe_memory_read_i32(
            (mach_vm_address_t)(entry + ECS_SYSTEM_ENTRY_INDEX0_OFFSET), &index0) ||
        !safe_memory_read_i32(
            (mach_vm_address_t)(entry + ECS_SYSTEM_ENTRY_INDEX1_OFFSET), &index1) ||
        !system) {
        return NULL;
    }

    /* Same self-consistency check the system-update path uses: the entry stores
     * its own index twice, so a wrong stride or index fails here rather than
     * handing back an unrelated object. */
    if (index0 != system_index || index1 != system_index) {
        LOG_ENTITY_DEBUG("ReplicationSystem: entry index mismatch (%d/%d vs %d)",
                         index0, index1, system_index);
        return NULL;
    }
    return system;
}

bool replication_system_replicate_to_peer(void *world, uint64_t entity, int peer_id) {
    if (!entity) return false;
    void *system = replication_system_get(world);
    if (!system) return false;
    ReplicateToPeerFn fn = (ReplicateToPeerFn)resolve_va(VA_REPLICATE_TO_PEER);
    if (!fn) return false;
    fn(system, entity, peer_id);
    LOG_ENTITY_DEBUG("ReplicateToPeer(entity=0x%llx, peer=%d) dispatched",
                     (unsigned long long)entity, peer_id);
    return true;
}

bool replication_system_stop_replicate(void *world, uint64_t entity) {
    if (!entity || !world) return false;
    void *system = replication_system_get(world);
    if (!system) return false;
    StopReplicateWithFn fn = (StopReplicateWithFn)resolve_va(VA_STOP_REPLICATE_WITH);
    if (!fn) return false;
    fn(system, world, entity);
    return true;
}
