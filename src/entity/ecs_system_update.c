/**
 * BG3SE-macOS - Ext.Entity named system update events.
 *
 * Offsets and the eight-step implementation sequence come from
 * ghidra/offsets/ECS_SYSTEM_UPDATE_RECON.md (build 4.1.1.7398727).
 */

#include "ecs_system_update.h"

#include "entity_events.h"
#include "entity_system.h"
#include "generated_typeids.h"
#include "../core/logging.h"
#include "../core/safe_memory.h"
#include "../core/version_detect.h"
#include "../lua/lua_gate.h"
#include "../lua/lua_runtime.h"

#include <lauxlib.h>
#include <lua.h>

#include <mach-o/loader.h>
#include <mach/vm_prot.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#define ECS_SYSTEM_UPDATE_VERIFIED_BUILD "4.1.1.7398727"
#define ECS_GHIDRA_BASE UINT64_C(0x100000000)

/* ECS_SYSTEM_UPDATE_RECON.md: EntityWorld system array and entry layout. */
#define ECS_WORLD_SYSTEM_BUFFER_OFFSET 0x30u
#define ECS_WORLD_SYSTEM_CAPACITY_OFFSET 0x38u
#define ECS_WORLD_SYSTEM_USED_OFFSET 0x3cu
#define ECS_SYSTEM_ENTRY_STRIDE 0xf8u
#define ECS_SYSTEM_ENTRY_SYSTEM_OFFSET 0x00u
#define ECS_SYSTEM_ENTRY_INDEX0_OFFSET 0x08u
#define ECS_SYSTEM_ENTRY_INDEX1_OFFSET 0x0cu
#define ECS_SYSTEM_ENTRY_UPDATE_PROC_OFFSET 0x18u

#define ECS_SYSTEM_UPDATE_MAX_SUBSCRIPTIONS 256
#define ECS_SYSTEM_UPDATE_MAX_HOOK_RECORDS 1024
#define ECS_SYSTEM_UPDATE_INVALID_HOOK UINT16_MAX

typedef void (*EcsUpdateProc)(void *system, void *entity_world,
                              const void *game_time);

typedef struct {
    const char *public_name;
    const char *engine_class;
    uintptr_t type_id_ghidra_address;
} EcsSystemName;

#define SYSTEM_ENTRY(public, engine, va) { public, engine, va },
static const EcsSystemName g_system_names[] = {
    GENERATED_SYSTEM_TYPEID_ENTRIES(SYSTEM_ENTRY)
};
#undef SYSTEM_ENTRY

_Static_assert(
    sizeof(g_system_names) / sizeof(g_system_names[0]) == GENERATED_SYSTEM_TYPEID_COUNT,
    "g_system_names size does not match GENERATED_SYSTEM_TYPEID_COUNT"
);

#define ECS_SYSTEM_NAME_COUNT (sizeof(g_system_names) / sizeof(g_system_names[0]))

typedef struct {
    bool allocated;
    bool active;
    bool once;
    bool post_update;
    bool pending_once_removal;
    uint16_t salt;
    uint16_t hook_slot;
    uint16_t name_index;
    LuaContext context;
    int callback_ref;
} EcsSystemSubscription;

typedef struct {
    bool used;
    bool installed;
    void *world;
    void *entry;
    void *system;
    int32_t system_index;
    LuaContext context;
    EcsUpdateProc original;
    uint32_t subscriber_count;
} EcsSystemHook;

typedef struct {
    bool ok;
    int32_t system_index;
    uint32_t capacity;
    uint32_t used;
    int32_t entry_index0;
    int32_t entry_index1;
    void *entry;
    void *system;
    EcsUpdateProc original;
} EcsResolveResult;

typedef enum {
    ECS_ATTACH_OK = 0,
    ECS_ATTACH_NO_WORLD,
    ECS_ATTACH_BAD_TYPE_ID,
    ECS_ATTACH_BAD_ARRAY,
    ECS_ATTACH_BAD_ENTRY,
    ECS_ATTACH_BAD_ORIGINAL,
    ECS_ATTACH_POOL_FULL,
    ECS_ATTACH_SWAP_FAILED
} EcsAttachStatus;

static pthread_mutex_t g_ecs_system_update_mutex = PTHREAD_MUTEX_INITIALIZER;
static EcsSystemSubscription g_subscriptions[ECS_SYSTEM_UPDATE_MAX_SUBSCRIPTIONS];
static EcsSystemHook g_hooks[ECS_SYSTEM_UPDATE_MAX_HOOK_RECORDS];
static uint16_t g_next_salt = 1;
static _Atomic bool g_in_transition = false;
static void *g_worlds[2]; /* [0] server, [1] client */
static uintptr_t g_text_start;
static uintptr_t g_text_end;

static void ecs_system_update_wrapper(void *system, void *entity_world,
                                      const void *game_time);

static size_t context_world_slot(LuaContext context) {
    return context == LUA_CONTEXT_CLIENT ? 1u : 0u;
}

static bool build_is_supported(void) {
    return version_detect_matches()
        && version_detect_get_binary_base() != NULL
        && strcmp(BG3_KNOWN_VERSION, ECS_SYSTEM_UPDATE_VERIFIED_BUILD) == 0;
}

static const EcsSystemName *find_system_name(const char *name, size_t *index_out) {
    if (!name) return NULL;

    for (size_t i = 0; i < ECS_SYSTEM_NAME_COUNT; i++) {
        if (strcmp(name, g_system_names[i].public_name) == 0
            || strcmp(name, g_system_names[i].engine_class) == 0) {
            if (index_out) *index_out = i;
            return &g_system_names[i];
        }
    }

    return NULL;
}

static bool refresh_text_bounds(void) {
    if (g_text_start != 0 && g_text_end > g_text_start) return true;

    void *base = version_detect_get_binary_base();
    if (!base) return false;

    const struct mach_header_64 *header = (const struct mach_header_64 *)base;
    if (header->magic != MH_MAGIC_64) return false;

    const uint8_t *cursor = (const uint8_t *)base + sizeof(*header);
    const uint8_t *commands_end = cursor + header->sizeofcmds;
    for (uint32_t i = 0; i < header->ncmds; i++) {
        if (cursor + sizeof(struct load_command) > commands_end) return false;
        const struct load_command *command = (const struct load_command *)cursor;
        if (command->cmdsize < sizeof(*command) || cursor + command->cmdsize > commands_end) {
            return false;
        }

        if (command->cmd == LC_SEGMENT_64) {
            const struct segment_command_64 *segment =
                (const struct segment_command_64 *)cursor;
            if (strncmp(segment->segname, "__TEXT", sizeof(segment->segname)) == 0
                && (segment->initprot & VM_PROT_EXECUTE) != 0) {
                g_text_start = (uintptr_t)base + segment->vmaddr - ECS_GHIDRA_BASE;
                g_text_end = g_text_start + segment->vmsize;
                return g_text_end > g_text_start;
            }
        }

        cursor += command->cmdsize;
    }

    return false;
}

static bool pointer_is_game_text(EcsUpdateProc proc) {
    uintptr_t pointer = (uintptr_t)proc;
    return refresh_text_bounds() && pointer >= g_text_start && pointer < g_text_end;
}

static bool read_update_slot(void *entry, EcsUpdateProc *proc_out) {
    void *pointer = NULL;
    if (!safe_memory_read_pointer(
            (mach_vm_address_t)((uintptr_t)entry + ECS_SYSTEM_ENTRY_UPDATE_PROC_OFFSET),
            &pointer)) {
        return false;
    }

    *proc_out = (EcsUpdateProc)pointer;
    return true;
}

static bool atomic_replace_update_proc(void *entry, EcsUpdateProc expected_proc,
                                       EcsUpdateProc replacement) {
    uintptr_t slot_address = (uintptr_t)entry + ECS_SYSTEM_ENTRY_UPDATE_PROC_OFFSET;
    SafeMemoryInfo info = safe_memory_check_address((mach_vm_address_t)slot_address);
    if (!info.is_valid || !info.is_readable || !info.is_writable
        || slot_address + sizeof(uintptr_t) > info.region_start + info.region_size
        || (slot_address & (sizeof(uintptr_t) - 1u)) != 0) {
        return false;
    }

    _Atomic(uintptr_t) *slot = (_Atomic(uintptr_t) *)slot_address;
    uintptr_t expected = (uintptr_t)expected_proc;
    return atomic_compare_exchange_strong_explicit(
        slot, &expected, (uintptr_t)replacement,
        memory_order_release, memory_order_acquire);
}

static bool atomic_restore_update_proc(EcsSystemHook *hook) {
    if (!hook || !hook->installed || !hook->entry || !hook->original) return false;

    EcsUpdateProc current = NULL;
    if (!read_update_slot(hook->entry, &current)) {
        hook->installed = false;
        return false;
    }

    if (current == hook->original) {
        hook->installed = false;
        return true;
    }

    if (current != &ecs_system_update_wrapper) {
        hook->installed = false;
        return false;
    }

    bool restored = atomic_replace_update_proc(
        hook->entry, &ecs_system_update_wrapper, hook->original);
    hook->installed = false;
    return restored;
}

static EcsAttachStatus resolve_system_entry(void *world, size_t name_index,
                                            EcsResolveResult *result) {
    memset(result, 0, sizeof(*result));
    if (!world || name_index >= ECS_SYSTEM_NAME_COUNT) return ECS_ATTACH_NO_WORLD;

    void *base = version_detect_get_binary_base();
    uintptr_t type_id_address = (uintptr_t)base
        + g_system_names[name_index].type_id_ghidra_address - ECS_GHIDRA_BASE;
    int32_t system_index = -1;
    if (!safe_memory_read_i32((mach_vm_address_t)type_id_address, &system_index)
        || system_index < 0) {
        return ECS_ATTACH_BAD_TYPE_ID;
    }

    void *buffer = NULL;
    uint32_t capacity = 0;
    uint32_t used = 0;
    if (!safe_memory_read_pointer(
            (mach_vm_address_t)((uintptr_t)world + ECS_WORLD_SYSTEM_BUFFER_OFFSET),
            &buffer)
        || !safe_memory_read_u32(
            (mach_vm_address_t)((uintptr_t)world + ECS_WORLD_SYSTEM_CAPACITY_OFFSET),
            &capacity)
        || !safe_memory_read_u32(
            (mach_vm_address_t)((uintptr_t)world + ECS_WORLD_SYSTEM_USED_OFFSET),
            &used)
        || !buffer || capacity == 0 || capacity > 8192 || used == 0
        || used > capacity || (uint32_t)system_index >= used
        || (uint32_t)system_index >= capacity) {
        return ECS_ATTACH_BAD_ARRAY;
    }

    uintptr_t entry_address = (uintptr_t)buffer
        + (uintptr_t)(uint32_t)system_index * ECS_SYSTEM_ENTRY_STRIDE;
    void *system = NULL;
    int32_t index0 = -1;
    int32_t index1 = -1;
    EcsUpdateProc original = NULL;
    if (!safe_memory_read_pointer(
            (mach_vm_address_t)(entry_address + ECS_SYSTEM_ENTRY_SYSTEM_OFFSET),
            &system)
        || !safe_memory_read_i32(
            (mach_vm_address_t)(entry_address + ECS_SYSTEM_ENTRY_INDEX0_OFFSET),
            &index0)
        || !safe_memory_read_i32(
            (mach_vm_address_t)(entry_address + ECS_SYSTEM_ENTRY_INDEX1_OFFSET),
            &index1)
        || !read_update_slot((void *)entry_address, &original)
        || !system || !original || index0 != system_index || index1 != system_index) {
        return ECS_ATTACH_BAD_ENTRY;
    }

    if (!pointer_is_game_text(original)) return ECS_ATTACH_BAD_ORIGINAL;

    result->ok = true;
    result->system_index = system_index;
    result->capacity = capacity;
    result->used = used;
    result->entry_index0 = index0;
    result->entry_index1 = index1;
    result->entry = (void *)entry_address;
    result->system = system;
    result->original = original;
    return ECS_ATTACH_OK;
}

static int find_hook_by_identity(void *world, void *system) {
    for (int i = 0; i < ECS_SYSTEM_UPDATE_MAX_HOOK_RECORDS; i++) {
        if (g_hooks[i].used && g_hooks[i].world == world
            && g_hooks[i].system == system) {
            return i;
        }
    }
    return -1;
}

static int find_or_create_hook(void *world, LuaContext context,
                               const EcsResolveResult *resolved) {
    int slot = find_hook_by_identity(world, resolved->system);
    if (slot >= 0) {
        EcsSystemHook *hook = &g_hooks[slot];
        if (!hook->installed && hook->subscriber_count == 0) {
            hook->entry = resolved->entry;
            hook->system_index = resolved->system_index;
            hook->context = context;
            hook->original = resolved->original;
        }
        return slot;
    }

    for (int i = 0; i < ECS_SYSTEM_UPDATE_MAX_HOOK_RECORDS; i++) {
        if (!g_hooks[i].used) {
            g_hooks[i] = (EcsSystemHook){
                .used = true,
                .world = world,
                .entry = resolved->entry,
                .system = resolved->system,
                .system_index = resolved->system_index,
                .context = context,
                .original = resolved->original
            };
            return i;
        }
    }

    return -1;
}

static bool install_hook(EcsSystemHook *hook) {
    if (hook->installed) return true;
    if (!hook->original || !pointer_is_game_text(hook->original)) return false;

    if (!atomic_replace_update_proc(
            hook->entry, hook->original, &ecs_system_update_wrapper)) {
        EcsUpdateProc current = NULL;
        if (!read_update_slot(hook->entry, &current)
            || current != &ecs_system_update_wrapper) {
            return false;
        }
    }

    hook->installed = true;
    return true;
}

static uint32_t pack_subscription(int index) {
    return ((uint32_t)g_subscriptions[index].salt << 16)
        | (uint32_t)(uint16_t)index;
}

static bool validate_subscription(uint32_t packed, int *index_out) {
    uint16_t index = (uint16_t)(packed & UINT32_C(0xffff));
    uint16_t salt = (uint16_t)(packed >> 16);
    if (index >= ECS_SYSTEM_UPDATE_MAX_SUBSCRIPTIONS
        || !g_subscriptions[index].allocated
        || g_subscriptions[index].salt != salt) {
        return false;
    }

    if (index_out) *index_out = index;
    return true;
}

static int allocate_subscription(void) {
    for (int i = 0; i < ECS_SYSTEM_UPDATE_MAX_SUBSCRIPTIONS; i++) {
        if (!g_subscriptions[i].allocated) {
            uint16_t salt = g_next_salt++;
            if (salt == 0) salt = g_next_salt++;
            g_subscriptions[i] = (EcsSystemSubscription){
                .allocated = true,
                .active = true,
                .salt = salt,
                .hook_slot = ECS_SYSTEM_UPDATE_INVALID_HOOK,
                .callback_ref = LUA_NOREF
            };
            return i;
        }
    }

    return -1;
}

static void detach_subscription(EcsSystemSubscription *subscription) {
    if (subscription->hook_slot == ECS_SYSTEM_UPDATE_INVALID_HOOK
        || subscription->hook_slot >= ECS_SYSTEM_UPDATE_MAX_HOOK_RECORDS) {
        subscription->hook_slot = ECS_SYSTEM_UPDATE_INVALID_HOOK;
        return;
    }

    EcsSystemHook *hook = &g_hooks[subscription->hook_slot];
    if (hook->subscriber_count > 0) hook->subscriber_count--;
    subscription->hook_slot = ECS_SYSTEM_UPDATE_INVALID_HOOK;
    if (hook->subscriber_count == 0) atomic_restore_update_proc(hook);
}

static void release_subscription(EcsSystemSubscription *subscription,
                                 lua_State *state) {
    detach_subscription(subscription);
    if (state && subscription->callback_ref != LUA_NOREF
        && subscription->callback_ref != LUA_REFNIL) {
        luaL_unref(state, LUA_REGISTRYINDEX, subscription->callback_ref);
    }

    subscription->allocated = false;
    subscription->active = false;
    subscription->pending_once_removal = false;
    subscription->callback_ref = LUA_NOREF;
}

static EcsAttachStatus attach_subscription(EcsSystemSubscription *subscription,
                                           void *world,
                                           EcsResolveResult *resolved_out) {
    EcsResolveResult resolved;
    EcsAttachStatus status = resolve_system_entry(
        world, subscription->name_index, &resolved);
    if (status != ECS_ATTACH_OK) return status;

    int hook_slot = find_or_create_hook(world, subscription->context, &resolved);
    if (hook_slot < 0) return ECS_ATTACH_POOL_FULL;

    EcsSystemHook *hook = &g_hooks[hook_slot];
    subscription->hook_slot = (uint16_t)hook_slot;
    hook->subscriber_count++;

    /* Step 4: publication occurs only for the first subscriber on this hook. */
    if (hook->subscriber_count == 1 && !install_hook(hook)) {
        hook->subscriber_count--;
        subscription->hook_slot = ECS_SYSTEM_UPDATE_INVALID_HOOK;
        return ECS_ATTACH_SWAP_FAILED;
    }

    if (resolved_out) *resolved_out = resolved;
    return ECS_ATTACH_OK;
}

static int attach_context_subscriptions(LuaContext context, void *world) {
    int attached = 0;
    for (int i = 0; i < ECS_SYSTEM_UPDATE_MAX_SUBSCRIPTIONS; i++) {
        EcsSystemSubscription *subscription = &g_subscriptions[i];
        if (!subscription->allocated || !subscription->active
            || subscription->context != context
            || subscription->hook_slot != ECS_SYSTEM_UPDATE_INVALID_HOOK) {
            continue;
        }

        if (attach_subscription(subscription, world, NULL) == ECS_ATTACH_OK) {
            attached++;
        }
    }
    return attached;
}

static int restore_context_hooks(LuaContext context) {
    int restored = 0;
    for (int i = 0; i < ECS_SYSTEM_UPDATE_MAX_SUBSCRIPTIONS; i++) {
        if (g_subscriptions[i].allocated
            && g_subscriptions[i].context == context) {
            g_subscriptions[i].hook_slot = ECS_SYSTEM_UPDATE_INVALID_HOOK;
        }
    }

    for (int i = 0; i < ECS_SYSTEM_UPDATE_MAX_HOOK_RECORDS; i++) {
        EcsSystemHook *hook = &g_hooks[i];
        if (!hook->used || hook->context != context) continue;
        if (hook->installed && atomic_restore_update_proc(hook)) restored++;
        hook->subscriber_count = 0;
    }
    return restored;
}

static int unsubscribe_system_locked(uint64_t id, LuaContext caller_context,
                                     lua_State *state) {
    if (SUB_ID_TYPE(id) != SUB_TYPE_SYSTEM) return 0;

    int index = -1;
    if (!validate_subscription(SUB_ID_INDEX(id), &index)) return 0;
    EcsSystemSubscription *subscription = &g_subscriptions[index];
    if (subscription->context != caller_context) return 0;

    release_subscription(subscription, state);
    return 1;
}

static void dispatch_callbacks(uint16_t hook_slot, bool post_update,
                               LuaContext context) {
    /*
     * These callbacks are synchronous worker-thread Lua entry. Recon rates
     * that safety Medium until live stress testing; keep this body defensive,
     * bounded, and free of game-memory traversal beyond the saved registry.
     */
    lua_gate_lock();
    lua_State *state = lua_runtime_state_for(context); /* resolve under gate */
    if (!state || atomic_load_explicit(&g_in_transition, memory_order_acquire)) {
        lua_gate_unlock();
        return;
    }

    uint32_t snapshot[ECS_SYSTEM_UPDATE_MAX_SUBSCRIPTIONS];
    size_t snapshot_count = 0;

    pthread_mutex_lock(&g_ecs_system_update_mutex);
    if (!atomic_load_explicit(&g_in_transition, memory_order_acquire)) {
        for (int i = 0; i < ECS_SYSTEM_UPDATE_MAX_SUBSCRIPTIONS; i++) {
            EcsSystemSubscription *subscription = &g_subscriptions[i];
            if (subscription->allocated && subscription->active
                && subscription->hook_slot == hook_slot
                && subscription->post_update == post_update) {
                snapshot[snapshot_count++] = pack_subscription(i);
            }
        }
    }
    pthread_mutex_unlock(&g_ecs_system_update_mutex);

    for (size_t i = 0; i < snapshot_count; i++) {
        int index = -1;
        int callback_ref = LUA_NOREF;

        pthread_mutex_lock(&g_ecs_system_update_mutex);
        if (validate_subscription(snapshot[i], &index)) {
            EcsSystemSubscription *subscription = &g_subscriptions[index];
            if (subscription->active && subscription->hook_slot == hook_slot
                && subscription->post_update == post_update) {
                if (subscription->once) {
                    /* Step 6: deactivate before invocation; remove after iteration. */
                    subscription->active = false;
                    subscription->pending_once_removal = true;
                }
                callback_ref = subscription->callback_ref;
            }
        }
        pthread_mutex_unlock(&g_ecs_system_update_mutex);

        if (callback_ref == LUA_NOREF || callback_ref == LUA_REFNIL) continue;
        lua_rawgeti(state, LUA_REGISTRYINDEX, callback_ref);
        if (!lua_isfunction(state, -1)) {
            lua_pop(state, 1);
            continue;
        }

        if (lua_pcall(state, 0, 0, 0) != LUA_OK) {
            const char *error = lua_tostring(state, -1);
            LOG_ENTITY_ERROR("[SystemUpdate] callback failed: %s",
                             error ? error : "unknown Lua error");
            lua_pop(state, 1);
        }
    }

    pthread_mutex_lock(&g_ecs_system_update_mutex);
    for (int i = 0; i < ECS_SYSTEM_UPDATE_MAX_SUBSCRIPTIONS; i++) {
        EcsSystemSubscription *subscription = &g_subscriptions[i];
        if (subscription->allocated && subscription->pending_once_removal
            && subscription->hook_slot == hook_slot
            && subscription->post_update == post_update) {
            release_subscription(subscription, state);
        }
    }
    pthread_mutex_unlock(&g_ecs_system_update_mutex);
    lua_gate_unlock();
}

static void ecs_system_update_wrapper(void *system, void *entity_world,
                                      const void *game_time) {
    EcsUpdateProc original = NULL;
    LuaContext context = LUA_CONTEXT_NONE;
    uint16_t hook_slot = ECS_SYSTEM_UPDATE_INVALID_HOOK;

    pthread_mutex_lock(&g_ecs_system_update_mutex);
    int slot = find_hook_by_identity(entity_world, system);
    if (slot >= 0) {
        original = g_hooks[slot].original;
        context = g_hooks[slot].context;
        hook_slot = (uint16_t)slot;
    }
    pthread_mutex_unlock(&g_ecs_system_update_mutex);

    if (original) {
        if (hook_slot != ECS_SYSTEM_UPDATE_INVALID_HOOK) {
            dispatch_callbacks(hook_slot, false, context);
        }

        /* Step 5: the original is unconditional, including transition/no-Lua. */
        original(system, entity_world, game_time);

        if (hook_slot != ECS_SYSTEM_UPDATE_INVALID_HOOK) {
            dispatch_callbacks(hook_slot, true, context);
        }
    } else {
        LOG_ENTITY_ERROR("[SystemUpdate] wrapper could not resolve original for world=%p system=%p",
                         entity_world, system);
    }
}

static const char *attach_status_message(EcsAttachStatus status) {
    switch (status) {
        case ECS_ATTACH_NO_WORLD: return "EntityWorld is unavailable";
        case ECS_ATTACH_BAD_TYPE_ID: return "TypeId global is unreadable or uninitialized";
        case ECS_ATTACH_BAD_ARRAY: return "EntityWorld system array failed bounds validation";
        case ECS_ATTACH_BAD_ENTRY: return "system entry is absent or its stored indices disagree";
        case ECS_ATTACH_BAD_ORIGINAL: return "original UpdateProc is outside the executable game image";
        case ECS_ATTACH_POOL_FULL: return "system update hook pool is full";
        case ECS_ATTACH_SWAP_FAILED: return "UpdateProc changed before the atomic replacement";
        case ECS_ATTACH_OK: return "ok";
    }
    return "unknown failure";
}

static int lua_subscribe(lua_State *L, bool post_update) {
    lua_gate_lock();
    LuaContext context = lua_runtime_context_of(L);
    lua_State *owned_state = lua_runtime_state_for(context); /* resolve under gate */
    if (!owned_state) {
        lua_gate_unlock();
        return luaL_error(L, "No live Lua runtime owns this call");
    }

    if (!build_is_supported()) {
        lua_gate_unlock();
        return luaL_error(L,
            "ECS system update events are supported only on game build %s",
            ECS_SYSTEM_UPDATE_VERIFIED_BUILD);
    }

    const char *requested_name = lua_tostring(L, 1);
    if (!requested_name) {
        lua_gate_unlock();
        return luaL_error(L, "System type name must be a string");
    }
    size_t name_index = 0;
    const EcsSystemName *system_name = find_system_name(requested_name, &name_index);
    if (!system_name) {
        lua_gate_unlock();
        return luaL_error(L, "Unknown system type: %s", requested_name);
    }

    if (!lua_isfunction(L, 2)) {
        lua_gate_unlock();
        return luaL_error(L, "System update callback must be a function");
    }
    bool once = lua_gettop(L) >= 3 && lua_toboolean(L, 3);
    if (context != LUA_CONTEXT_SERVER && context != LUA_CONTEXT_CLIENT) {
        lua_gate_unlock();
        return luaL_error(L, "Cannot determine the Lua runtime context");
    }

    void *world = entity_get_world_for_context(context == LUA_CONTEXT_SERVER);
    if (atomic_load_explicit(&g_in_transition, memory_order_acquire)) {
        lua_gate_unlock();
        return luaL_error(L, "Cannot subscribe to system updates during a world transition");
    }

    lua_pushvalue(L, 2);
    int callback_ref = luaL_ref(L, LUA_REGISTRYINDEX);
    EcsResolveResult resolved;
    EcsAttachStatus status = ECS_ATTACH_POOL_FULL;
    uint64_t id = ENTITY_SUB_INVALID;

    pthread_mutex_lock(&g_ecs_system_update_mutex);
    int subscription_index = allocate_subscription();
    if (subscription_index >= 0) {
        EcsSystemSubscription *subscription = &g_subscriptions[subscription_index];
        subscription->once = once;
        subscription->post_update = post_update;
        subscription->name_index = (uint16_t)name_index;
        subscription->context = context;
        subscription->callback_ref = callback_ref;
        status = attach_subscription(subscription, world, &resolved);
        if (status == ECS_ATTACH_OK) {
            id = MAKE_SUB_ID(SUB_TYPE_SYSTEM, pack_subscription(subscription_index));
        } else {
            subscription->allocated = false;
            subscription->active = false;
            subscription->callback_ref = LUA_NOREF;
        }
    }
    pthread_mutex_unlock(&g_ecs_system_update_mutex);

    if (id == ENTITY_SUB_INVALID) {
        luaL_unref(L, LUA_REGISTRYINDEX, callback_ref);
        LOG_ENTITY_WARN("[SystemUpdate] subscribe rejected for %s: %s",
                        system_name->public_name, attach_status_message(status));
        lua_gate_unlock();
        return luaL_error(L, "System %s not registered: %s",
                          system_name->public_name, attach_status_message(status));
    }

    LOG_ENTITY_INFO("[SystemUpdate] validated %s: world=%p array=%u/%u index=%d "
                    "entryIndices=%d/%d entry=%p system=%p original=%p text=[%p,%p)",
                    system_name->public_name, world, resolved.used, resolved.capacity,
                    resolved.system_index, resolved.entry_index0, resolved.entry_index1,
                    resolved.entry, resolved.system, (void *)resolved.original,
                    (void *)g_text_start, (void *)g_text_end);
    LOG_ENTITY_INFO("[SystemUpdate] installed %s %s subscription handle=0x%llx",
                    system_name->public_name, post_update ? "post" : "pre",
                    (unsigned long long)id);

    lua_pushinteger(L, (lua_Integer)id);
    lua_gate_unlock();
    return 1;
}

static int lua_on_system_update(lua_State *L) {
    return lua_subscribe(L, false);
}

static int lua_on_system_post_update(lua_State *L) {
    return lua_subscribe(L, true);
}

static int lua_unsubscribe(lua_State *L) {
    lua_gate_lock();
    LuaContext context = lua_runtime_context_of(L);
    lua_State *owned_state = lua_runtime_state_for(context); /* resolve under gate */
    if (!owned_state) {
        lua_gate_unlock();
        lua_pushboolean(L, false);
        return 1;
    }

    if (!lua_isinteger(L, 1)) {
        lua_gate_unlock();
        return luaL_error(L, "Subscription handle must be an integer");
    }
    uint64_t id = (uint64_t)lua_tointeger(L, 1);
    bool unsubscribed = false;
    if (SUB_ID_TYPE(id) == SUB_TYPE_SYSTEM) {
        pthread_mutex_lock(&g_ecs_system_update_mutex);
        unsubscribed = unsubscribe_system_locked(id, context, owned_state) != 0;
        pthread_mutex_unlock(&g_ecs_system_update_mutex);
    } else {
        /* Preserve the pre-existing component subscription path. */
        unsubscribed = entity_events_unsubscribe(id, owned_state);
    }

    lua_pushboolean(L, unsubscribed);
    lua_gate_unlock();
    return 1;
}

void ecs_system_update_register_lua(lua_State *L, int entity_table_index) {
    if (!L) return;

    lua_gate_lock();
    LuaContext context = lua_runtime_context_of(L);
    lua_State *owned_state = lua_runtime_state_for(context); /* resolve under gate */
    if (!owned_state) {
        lua_gate_unlock();
        LOG_ENTITY_WARN("[SystemUpdate] registration skipped: no live Lua runtime");
        return;
    }

    int entity_index = lua_absindex(L, entity_table_index);
    if (!lua_istable(L, entity_index)) {
        lua_gate_unlock();
        LOG_ENTITY_WARN("[SystemUpdate] registration target is not a table");
        return;
    }

    lua_pushcfunction(L, lua_on_system_update);
    lua_setfield(L, entity_index, "OnSystemUpdate");
    lua_pushcfunction(L, lua_on_system_post_update);
    lua_setfield(L, entity_index, "OnSystemPostUpdate");
    lua_pushcfunction(L, lua_unsubscribe);
    lua_setfield(L, entity_index, "Unsubscribe");
    lua_gate_unlock();

    LOG_ENTITY_INFO("[SystemUpdate] registered Lua API (build gate: %s, supported=%s)",
                    ECS_SYSTEM_UPDATE_VERIFIED_BUILD,
                    build_is_supported() ? "yes" : "no");
    LOG_ENTITY_INFO("[SystemUpdate] using writable UpdateProc slots; "
                    "ExecuteWTKernel fallback 0x1063788cc is not installed");
}

void ecs_system_update_on_world_bound(void *entity_world, bool is_server) {
    if (!entity_world || !build_is_supported()) return;

    LuaContext context = is_server ? LUA_CONTEXT_SERVER : LUA_CONTEXT_CLIENT;
    int restored = 0;
    int attached = 0;

    pthread_mutex_lock(&g_ecs_system_update_mutex);
    size_t world_slot = context_world_slot(context);
    if (g_worlds[world_slot] != entity_world) {
        restored = restore_context_hooks(context);
        g_worlds[world_slot] = entity_world;
    }
    if (!atomic_load_explicit(&g_in_transition, memory_order_acquire)) {
        attached = attach_context_subscriptions(context, entity_world);
    }
    pthread_mutex_unlock(&g_ecs_system_update_mutex);

    LOG_ENTITY_INFO("[SystemUpdate] observed %s EntityWorld=%p (restored=%d, reattached=%d)",
                    is_server ? "server" : "client", entity_world,
                    restored, attached);
}

void ecs_system_update_set_transition(bool in_transition) {
    atomic_store_explicit(&g_in_transition, in_transition, memory_order_release);

    int restored = 0;
    int attached = 0;
    pthread_mutex_lock(&g_ecs_system_update_mutex);
    if (in_transition) {
        restored += restore_context_hooks(LUA_CONTEXT_SERVER);
        restored += restore_context_hooks(LUA_CONTEXT_CLIENT);
    } else if (build_is_supported()) {
        void *server_world = entity_get_world_for_context(true);
        void *client_world = entity_get_world_for_context(false);
        g_worlds[0] = server_world;
        g_worlds[1] = client_world;
        if (server_world) {
            attached += attach_context_subscriptions(LUA_CONTEXT_SERVER, server_world);
        }
        if (client_world) {
            attached += attach_context_subscriptions(LUA_CONTEXT_CLIENT, client_world);
        }
    }
    pthread_mutex_unlock(&g_ecs_system_update_mutex);

    LOG_ENTITY_INFO("[SystemUpdate] transition %s (restored=%d, reattached=%d)",
                    in_transition ? "ON" : "OFF", restored, attached);
}

void ecs_system_update_teardown(void) {
    lua_gate_lock();
    lua_State *server_state = lua_runtime_state_for(LUA_CONTEXT_SERVER);
    lua_State *client_state = lua_runtime_state_for(LUA_CONTEXT_CLIENT);

    int restored = 0;
    int released = 0;
    pthread_mutex_lock(&g_ecs_system_update_mutex);
    atomic_store_explicit(&g_in_transition, true, memory_order_release);
    restored += restore_context_hooks(LUA_CONTEXT_SERVER);
    restored += restore_context_hooks(LUA_CONTEXT_CLIENT);

    for (int i = 0; i < ECS_SYSTEM_UPDATE_MAX_SUBSCRIPTIONS; i++) {
        EcsSystemSubscription *subscription = &g_subscriptions[i];
        if (!subscription->allocated) continue;
        lua_State *state = subscription->context == LUA_CONTEXT_CLIENT
            ? client_state : server_state;
        release_subscription(subscription, state);
        released++;
    }
    g_worlds[0] = NULL;
    g_worlds[1] = NULL;
    pthread_mutex_unlock(&g_ecs_system_update_mutex);
    lua_gate_unlock();

    LOG_ENTITY_INFO("[SystemUpdate] teardown restored=%d released=%d",
                    restored, released);
}
