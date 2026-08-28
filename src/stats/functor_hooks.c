/**
 * functor_hooks.c - Stats Functor Hook System Implementation
 *
 * Hooks into the game's functor execution system to fire Lua events
 * before and after each functor runs. This enables mods to:
 * - Monitor damage, healing, status effects
 * - Modify functor parameters
 * - Prevent functor execution
 * - Track combat mechanics
 *
 * Hook Pattern (from Windows BG3SE):
 * 1. Fire "ExecuteFunctor" event with functor + context
 * 2. Call original function
 * 3. Fire "AfterExecuteFunctor" event with functor + context + hit result
 */

#include "functor_hooks.h"
#include "functor_types.h"
#include "../core/logging.h"
#include "../core/offset_table.h"
#include "../lua/lua_events.h"
#include "../lua/lua_gate.h"
#include "../lua/lua_runtime.h"
#include "../entity/entity_storage.h"

#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wvariadic-macros"
#include <dobby.h>
#pragma clang diagnostic pop
#include <stdatomic.h>
#include <stdint.h>
#include <string.h>

// =============================================================================
// Module State
// =============================================================================

static bool g_HooksInstalled = false;
#define FUNCTOR_HOOK_TARGET_COUNT 10
static int g_InstalledCount = 0;
static uint64_t g_EventCount = 0;

// Dispatch barrier (replaces the old g_LuaState null check, E2.0). Dobby hooks
// stay patched after shutdown, so a hook firing post-shutdown must find the
// barrier down BEFORE it resolves the still-alive runtime. Cleared first in
// functor_hooks_shutdown(); checked before and after the gate in each
// dispatcher.
static _Atomic(bool) g_DispatchEnabled = false;

// Original function pointers (saved by Dobby)
static ExecuteFunctorsProc g_OrigAttackTarget = NULL;
static ExecuteFunctorsProc g_OrigAttackPosition = NULL;
static ExecuteFunctorsProc g_OrigMove = NULL;
static ExecuteFunctorsProc g_OrigTarget = NULL;
static ExecuteFunctorsProc g_OrigNearbyAttacked = NULL;
static ExecuteFunctorsProc g_OrigNearbyAttacking = NULL;
static ExecuteFunctorsProc g_OrigEquip = NULL;
static ExecuteFunctorsProc g_OrigSource = NULL;
static ExecuteInterruptFunctorsProc g_OrigInterrupt = NULL;
static ProcessDealDamageFunctorsProc g_OrigProcessDealDamage = NULL;

static uintptr_t get_runtime_addr(GameFunctionId id) {
    return (uintptr_t)offset_table_game_fn(id);
}

// =============================================================================
// Event Dispatch Helpers
// =============================================================================

// Returns non-zero if any subscriber exists for ExecuteFunctor or AfterExecuteFunctor.
// Called from each hook before touching the Lua state — zero cost when no mods subscribe.
static inline int has_functor_subscribers(void) {
    return events_get_handler_count(EVENT_EXECUTE_FUNCTOR) +
           events_get_handler_count(EVENT_AFTER_EXECUTE_FUNCTOR);
}

static inline int has_damage_subscribers(void) {
    return events_get_handler_count(EVENT_BEFORE_DEAL_DAMAGE) +
           events_get_handler_count(EVENT_DEAL_DAMAGE);
}

/*
 * Dispatch-window tracking for Ext.Stats.ExecuteFunctors.
 *
 * The StatsFunctorList* handed to the Lua events is engine-owned and valid
 * only while the enclosing hook frame is on the stack. A Lua handler may call
 * back into ExecuteFunctors DURING the dispatch (that is the feature); a
 * handler that stashes the handle and calls later must be refused. The window
 * is: seq increments at every dispatch start, depth is nonzero only while
 * handlers run, and a handle is valid iff its recorded seq matches AND depth
 * is nonzero. Events fire on the game thread and handlers run synchronously
 * on it, so plain statics suffice.
 */
static uint64_t g_FunctorDispatchSeq = 0;
static int g_FunctorDispatchDepth = 0;

uint64_t functor_dispatch_begin(void) {
    if (g_FunctorDispatchDepth == 0) g_FunctorDispatchSeq++;
    g_FunctorDispatchDepth++;
    return g_FunctorDispatchSeq;
}

void functor_dispatch_end(void) {
    if (g_FunctorDispatchDepth > 0) g_FunctorDispatchDepth--;
}

bool functor_dispatch_valid(uint64_t seq) {
    return g_FunctorDispatchDepth > 0 && seq == g_FunctorDispatchSeq;
}

/* Resolved in functor_hooks_init under the same exact-build gate as the hook
 * addresses; NULL on any other build. */
typedef void (*FunctorResultDtor)(void *result);
static FunctorResultDtor g_ResultDtor = NULL;
void *functor_hooks_result_dtor(void) { return (void *)g_ResultDtor; }

static void fire_execute_functor_event(const StatsFunctorList* functors, void* context, FunctorContextType ctxType) {
    // Stats functors are server-side (E2.0 audit 1.7) — resolve the server VM.
    if (!atomic_load_explicit(&g_DispatchEnabled, memory_order_acquire)) return;
    if (!lua_runtime_state_for(LUA_CONTEXT_SERVER)) return;
    // Hooked game execution thread entering the shared Lua state — serialize
    // and re-resolve under the gate (see lua_gate.h).
    lua_gate_lock();
    lua_State* L = lua_runtime_state_for(LUA_CONTEXT_SERVER);
    if (L && atomic_load_explicit(&g_DispatchEnabled, memory_order_acquire)) {
        events_fire_execute_functor(L, (int)ctxType, (void*)functors, context);
        g_EventCount++;
    }
    lua_gate_unlock();
}

static void fire_after_execute_functor_event(const StatsFunctorList* functors, void* context, FunctorContextType ctxType) {
    if (!atomic_load_explicit(&g_DispatchEnabled, memory_order_acquire)) return;
    if (!lua_runtime_state_for(LUA_CONTEXT_SERVER)) return;
    lua_gate_lock();
    lua_State* L = lua_runtime_state_for(LUA_CONTEXT_SERVER);
    if (L && atomic_load_explicit(&g_DispatchEnabled, memory_order_acquire)) {
        events_fire_after_execute_functor(L, (int)ctxType, (void*)functors, context);
        g_EventCount++;
    }
    lua_gate_unlock();
}

static void fire_damage_event(
    BG3SEEventType event,
    void* worldView,
    const StatsFunctorBase* functor,
    uint64_t entityHandle,
    const void* position,
    const void* spellState,
    const void* damageEffectFlags,
    const void* ability,
    const void* spellAttackType,
    const void* dependency1,
    const void* dependency2,
    int eventIndex,
    void* interruptEvents
) {
    if (!atomic_load_explicit(&g_DispatchEnabled, memory_order_acquire)) return;
    if (!lua_runtime_state_for(LUA_CONTEXT_SERVER)) return;
    lua_gate_lock();
    lua_State* L = lua_runtime_state_for(LUA_CONTEXT_SERVER);
    if (L && atomic_load_explicit(&g_DispatchEnabled, memory_order_acquire)) {
        events_fire_damage(
            L, event, worldView, (void*)functor, entityHandle,
            position, spellState, damageEffectFlags, ability, spellAttackType,
            dependency1, dependency2, eventIndex, interruptEvents);
        g_EventCount++;
    }
    lua_gate_unlock();
}

// =============================================================================
// Hook Implementations
// Each hook: listener-count guard → pre-event → original → post-event.
// If 0 subscribers for both functor events, original is called directly with no
// Lua overhead.
//
// result_out is the hidden leading output argument (esv::functor::Result) that
// the demangled symbol names do not show — it MUST be accepted and forwarded
// or every subsequent register shifts (see functor_types.h and
// docs/bugs/wave2-functor-crash-analysis.md). Never dereference it.
// =============================================================================

static void hook_ExecuteFunctors_AttackTarget(void* result_out, const StatsFunctorList* functors, AttackTargetContextData* ctx) {
    if (!has_functor_subscribers()) {
        if (g_OrigAttackTarget) g_OrigAttackTarget(result_out, functors, ctx);
        return;
    }
    fire_execute_functor_event(functors, ctx, FUNCTOR_CTX_ATTACK_TARGET);
    if (g_OrigAttackTarget) g_OrigAttackTarget(result_out, functors, ctx);
    fire_after_execute_functor_event(functors, ctx, FUNCTOR_CTX_ATTACK_TARGET);
}

static void hook_ExecuteFunctors_AttackPosition(void* result_out, const StatsFunctorList* functors, AttackPositionContextData* ctx) {
    if (!has_functor_subscribers()) {
        if (g_OrigAttackPosition) g_OrigAttackPosition(result_out, functors, ctx);
        return;
    }
    fire_execute_functor_event(functors, ctx, FUNCTOR_CTX_ATTACK_POSITION);
    if (g_OrigAttackPosition) g_OrigAttackPosition(result_out, functors, ctx);
    fire_after_execute_functor_event(functors, ctx, FUNCTOR_CTX_ATTACK_POSITION);
}

static void hook_ExecuteFunctors_Move(void* result_out, const StatsFunctorList* functors, MoveContextData* ctx) {
    if (!has_functor_subscribers()) {
        if (g_OrigMove) g_OrigMove(result_out, functors, ctx);
        return;
    }
    fire_execute_functor_event(functors, ctx, FUNCTOR_CTX_MOVE);
    if (g_OrigMove) g_OrigMove(result_out, functors, ctx);
    fire_after_execute_functor_event(functors, ctx, FUNCTOR_CTX_MOVE);
}

static void hook_ExecuteFunctors_Target(void* result_out, const StatsFunctorList* functors, TargetContextData* ctx) {
    if (!has_functor_subscribers()) {
        if (g_OrigTarget) g_OrigTarget(result_out, functors, ctx);
        return;
    }
    fire_execute_functor_event(functors, ctx, FUNCTOR_CTX_TARGET);
    if (g_OrigTarget) g_OrigTarget(result_out, functors, ctx);
    fire_after_execute_functor_event(functors, ctx, FUNCTOR_CTX_TARGET);
}

static void hook_ExecuteFunctors_NearbyAttacked(void* result_out, const StatsFunctorList* functors, NearbyAttackedContextData* ctx) {
    if (!has_functor_subscribers()) {
        if (g_OrigNearbyAttacked) g_OrigNearbyAttacked(result_out, functors, ctx);
        return;
    }
    fire_execute_functor_event(functors, ctx, FUNCTOR_CTX_NEARBY_ATTACKED);
    if (g_OrigNearbyAttacked) g_OrigNearbyAttacked(result_out, functors, ctx);
    fire_after_execute_functor_event(functors, ctx, FUNCTOR_CTX_NEARBY_ATTACKED);
}

static void hook_ExecuteFunctors_NearbyAttacking(void* result_out, const StatsFunctorList* functors, NearbyAttackingContextData* ctx) {
    if (!has_functor_subscribers()) {
        if (g_OrigNearbyAttacking) g_OrigNearbyAttacking(result_out, functors, ctx);
        return;
    }
    fire_execute_functor_event(functors, ctx, FUNCTOR_CTX_NEARBY_ATTACKING);
    if (g_OrigNearbyAttacking) g_OrigNearbyAttacking(result_out, functors, ctx);
    fire_after_execute_functor_event(functors, ctx, FUNCTOR_CTX_NEARBY_ATTACKING);
}

static void hook_ExecuteFunctors_Equip(void* result_out, const StatsFunctorList* functors, EquipContextData* ctx) {
    if (!has_functor_subscribers()) {
        if (g_OrigEquip) g_OrigEquip(result_out, functors, ctx);
        return;
    }
    fire_execute_functor_event(functors, ctx, FUNCTOR_CTX_EQUIP);
    if (g_OrigEquip) g_OrigEquip(result_out, functors, ctx);
    fire_after_execute_functor_event(functors, ctx, FUNCTOR_CTX_EQUIP);
}

static void hook_ExecuteFunctors_Source(void* result_out, const StatsFunctorList* functors, SourceContextData* ctx) {
    if (!has_functor_subscribers()) {
        if (g_OrigSource) g_OrigSource(result_out, functors, ctx);
        return;
    }
    fire_execute_functor_event(functors, ctx, FUNCTOR_CTX_SOURCE);
    if (g_OrigSource) g_OrigSource(result_out, functors, ctx);
    fire_after_execute_functor_event(functors, ctx, FUNCTOR_CTX_SOURCE);
}

static void hook_ExecuteFunctors_Interrupt(
    void* result_out,
    EntityWorld* entityWorld,
    const StatsFunctorList* functors,
    InterruptContextData* ctx
) {
    if (!has_functor_subscribers()) {
        if (g_OrigInterrupt) g_OrigInterrupt(result_out, entityWorld, functors, ctx);
        return;
    }
    fire_execute_functor_event(functors, ctx, FUNCTOR_CTX_INTERRUPT);
    if (g_OrigInterrupt) g_OrigInterrupt(result_out, entityWorld, functors, ctx);
    fire_after_execute_functor_event(functors, ctx, FUNCTOR_CTX_INTERRUPT);
}

static void hook_ProcessDealDamageFunctors(
    void* worldView,
    const StatsFunctorBase* functor,
    const uint64_t* entityHandle,
    const void* position,
    const void* spellState,
    const void* damageEffectFlags,
    const void* ability,
    const void* spellAttackType,
    const void* dependency1,
    const void* dependency2,
    int eventIndex,
    void* interruptEvents
) {
    if (!has_damage_subscribers()) {
        if (g_OrigProcessDealDamage) {
            g_OrigProcessDealDamage(
                worldView, functor, entityHandle, position, spellState,
                damageEffectFlags, ability, spellAttackType, dependency1,
                dependency2, eventIndex, interruptEvents);
        }
        return;
    }

    uint64_t entity = entityHandle ? *entityHandle : 0;
    if (events_get_handler_count(EVENT_BEFORE_DEAL_DAMAGE) > 0) {
        fire_damage_event(
            EVENT_BEFORE_DEAL_DAMAGE, worldView, functor, entity, position,
            spellState, damageEffectFlags, ability, spellAttackType, dependency1,
            dependency2, eventIndex, interruptEvents);
    }

    if (g_OrigProcessDealDamage) {
        g_OrigProcessDealDamage(
            worldView, functor, entityHandle, position, spellState,
            damageEffectFlags, ability, spellAttackType, dependency1,
            dependency2, eventIndex, interruptEvents);
    }

    if (events_get_handler_count(EVENT_DEAL_DAMAGE) > 0) {
        fire_damage_event(
            EVENT_DEAL_DAMAGE, worldView, functor, entity, position, spellState,
            damageEffectFlags, ability, spellAttackType, dependency1,
            dependency2, eventIndex, interruptEvents);
    }
}

// =============================================================================
// Public API
// =============================================================================

bool functor_hooks_init(void) {
    /* main.c only calls this under the FUNCTOR_ADDRS_VERIFIED_BUILD gate, so
     * the raw dtor address is safe to materialize here. */
    g_ResultDtor = (FunctorResultDtor)offset_table_fn(ADDR_FUNCTOR_RESULT_DTOR - 0x100000000ULL);

    if (g_HooksInstalled) {
        LOG_HOOKS_WARN("Functor hooks already installed");
        return true;
    }

    int success_count = 0;

    LOG_HOOKS_INFO("Installing functor execution hooks...");

    // Raise the dispatch barrier before any hook goes live so the first
    // firing never races the enable.
    atomic_store_explicit(&g_DispatchEnabled, true, memory_order_release);

    // Install AttackTarget hook
    uintptr_t addr = get_runtime_addr(GAME_FN_EXECUTE_FUNCTORS_ATTACK_TARGET);
    if (addr && DobbyHook((void*)addr, (void*)hook_ExecuteFunctors_AttackTarget, (void**)&g_OrigAttackTarget) == 0) {
        LOG_HOOKS_DEBUG("  AttackTarget hook @ 0x%llx", (unsigned long long)addr);
        success_count++;
    } else {
        LOG_HOOKS_ERROR("  Failed to hook AttackTarget @ 0x%llx", (unsigned long long)addr);
    }

    // Install AttackPosition hook
    addr = get_runtime_addr(GAME_FN_EXECUTE_FUNCTORS_ATTACK_POSITION);
    if (addr && DobbyHook((void*)addr, (void*)hook_ExecuteFunctors_AttackPosition, (void**)&g_OrigAttackPosition) == 0) {
        LOG_HOOKS_DEBUG("  AttackPosition hook @ 0x%llx", (unsigned long long)addr);
        success_count++;
    } else {
        LOG_HOOKS_ERROR("  Failed to hook AttackPosition @ 0x%llx", (unsigned long long)addr);
    }

    // Install Move hook
    addr = get_runtime_addr(GAME_FN_EXECUTE_FUNCTORS_MOVE);
    if (addr && DobbyHook((void*)addr, (void*)hook_ExecuteFunctors_Move, (void**)&g_OrigMove) == 0) {
        LOG_HOOKS_DEBUG("  Move hook @ 0x%llx", (unsigned long long)addr);
        success_count++;
    } else {
        LOG_HOOKS_ERROR("  Failed to hook Move @ 0x%llx", (unsigned long long)addr);
    }

    // Install Target hook
    addr = get_runtime_addr(GAME_FN_EXECUTE_FUNCTORS_TARGET);
    if (addr && DobbyHook((void*)addr, (void*)hook_ExecuteFunctors_Target, (void**)&g_OrigTarget) == 0) {
        LOG_HOOKS_DEBUG("  Target hook @ 0x%llx", (unsigned long long)addr);
        success_count++;
    } else {
        LOG_HOOKS_ERROR("  Failed to hook Target @ 0x%llx", (unsigned long long)addr);
    }

    // Install NearbyAttacked hook
    addr = get_runtime_addr(GAME_FN_EXECUTE_FUNCTORS_NEARBY_ATTACKED);
    if (addr && DobbyHook((void*)addr, (void*)hook_ExecuteFunctors_NearbyAttacked, (void**)&g_OrigNearbyAttacked) == 0) {
        LOG_HOOKS_DEBUG("  NearbyAttacked hook @ 0x%llx", (unsigned long long)addr);
        success_count++;
    } else {
        LOG_HOOKS_ERROR("  Failed to hook NearbyAttacked @ 0x%llx", (unsigned long long)addr);
    }

    // Install NearbyAttacking hook
    addr = get_runtime_addr(GAME_FN_EXECUTE_FUNCTORS_NEARBY_ATTACKING);
    if (addr && DobbyHook((void*)addr, (void*)hook_ExecuteFunctors_NearbyAttacking, (void**)&g_OrigNearbyAttacking) == 0) {
        LOG_HOOKS_DEBUG("  NearbyAttacking hook @ 0x%llx", (unsigned long long)addr);
        success_count++;
    } else {
        LOG_HOOKS_ERROR("  Failed to hook NearbyAttacking @ 0x%llx", (unsigned long long)addr);
    }

    // Install Equip hook
    addr = get_runtime_addr(GAME_FN_EXECUTE_FUNCTORS_EQUIP);
    if (addr && DobbyHook((void*)addr, (void*)hook_ExecuteFunctors_Equip, (void**)&g_OrigEquip) == 0) {
        LOG_HOOKS_DEBUG("  Equip hook @ 0x%llx", (unsigned long long)addr);
        success_count++;
    } else {
        LOG_HOOKS_ERROR("  Failed to hook Equip @ 0x%llx", (unsigned long long)addr);
    }

    // Install Source hook
    addr = get_runtime_addr(GAME_FN_EXECUTE_FUNCTORS_SOURCE);
    if (addr && DobbyHook((void*)addr, (void*)hook_ExecuteFunctors_Source, (void**)&g_OrigSource) == 0) {
        LOG_HOOKS_DEBUG("  Source hook @ 0x%llx", (unsigned long long)addr);
        success_count++;
    } else {
        LOG_HOOKS_ERROR("  Failed to hook Source @ 0x%llx", (unsigned long long)addr);
    }

    // Install Interrupt hook
    addr = get_runtime_addr(GAME_FN_EXECUTE_FUNCTORS_INTERRUPT);
    if (addr && DobbyHook((void*)addr, (void*)hook_ExecuteFunctors_Interrupt, (void**)&g_OrigInterrupt) == 0) {
        LOG_HOOKS_DEBUG("  Interrupt hook @ 0x%llx", (unsigned long long)addr);
        success_count++;
    } else {
        LOG_HOOKS_ERROR("  Failed to hook Interrupt @ 0x%llx", (unsigned long long)addr);
    }

    // Install ProcessDealDamageFunctors hook
    addr = get_runtime_addr(GAME_FN_PROCESS_DEAL_DAMAGE_FUNCTORS);
    if (addr && DobbyHook((void*)addr, (void*)hook_ProcessDealDamageFunctors,
                          (void**)&g_OrigProcessDealDamage) == 0) {
        LOG_HOOKS_DEBUG("  ProcessDealDamageFunctors hook @ 0x%llx",
                        (unsigned long long)addr);
        success_count++;
    } else {
        LOG_HOOKS_ERROR("  Failed to hook ProcessDealDamageFunctors @ 0x%llx",
                        (unsigned long long)addr);
    }

    g_HooksInstalled = (success_count > 0);
    g_InstalledCount = success_count;
    if (success_count > 0 && success_count < FUNCTOR_HOOK_TARGET_COUNT) {
        // Partial installs break paired event semantics silently (a missing
        // ProcessDealDamageFunctors hook means BeforeDealDamage/DealDamage
        // never fire despite successful subscriptions) — make it loud.
        LOG_HOOKS_ERROR("Functor hooks PARTIAL install: %d/%d — some functor "
                        "contexts will not fire events",
                        success_count, FUNCTOR_HOOK_TARGET_COUNT);
    }
    LOG_HOOKS_INFO("Functor hooks: %d/%d installed", success_count,
                   FUNCTOR_HOOK_TARGET_COUNT);

    return g_HooksInstalled;
}

int functor_hooks_get_installed_count(void) {
    return g_InstalledCount;
}

void functor_hooks_shutdown(void) {
    if (!g_HooksInstalled) return;

    // Barrier down FIRST: Dobby hooks stay patched, so any hook firing after
    // this point must bail out before touching the (still-alive) runtime.
    atomic_store_explicit(&g_DispatchEnabled, false, memory_order_release);

    LOG_HOOKS_INFO("Functor Lua dispatch disabled (hooks stay patched, originals forwarded)");

    // Dobby has no unhook, so the wrappers stay patched into the game for the
    // life of the process. The g_Orig* pointers are deliberately NOT cleared:
    // a post-shutdown firing must still forward to the original game function,
    // or stat functor execution (combat, equipment, spells) silently no-ops.
    // Only the Lua dispatch barrier above and the bookkeeping come down.
    g_HooksInstalled = false;
    g_InstalledCount = 0;
}

bool functor_hooks_is_active(void) {
    return g_HooksInstalled;
}

uint64_t functor_hooks_get_event_count(void) {
    return g_EventCount;
}

void* functor_hooks_get_original_proc(int ctx_type) {
    if (!g_HooksInstalled) return NULL;

    switch (ctx_type) {
    case FUNCTOR_CTX_ATTACK_TARGET:    return (void*)g_OrigAttackTarget;
    case FUNCTOR_CTX_ATTACK_POSITION:  return (void*)g_OrigAttackPosition;
    case FUNCTOR_CTX_MOVE:             return (void*)g_OrigMove;
    case FUNCTOR_CTX_TARGET:           return (void*)g_OrigTarget;
    case FUNCTOR_CTX_NEARBY_ATTACKED:  return (void*)g_OrigNearbyAttacked;
    case FUNCTOR_CTX_NEARBY_ATTACKING: return (void*)g_OrigNearbyAttacking;
    case FUNCTOR_CTX_EQUIP:            return (void*)g_OrigEquip;
    case FUNCTOR_CTX_SOURCE:           return (void*)g_OrigSource;
    case FUNCTOR_CTX_INTERRUPT:        return (void*)g_OrigInterrupt;
    default: return NULL;
    }
}
