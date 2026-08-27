# Ext.Stats: Make `ExecuteFunctors` Callable via a Lifetime-Scoped FunctorList Handle

**Project:** `/Users/jaredspigner/src/BG3SE/macos-port`
**Game build:** 4.1.1.7398727 (arm64)
**Status:** planned, not implemented. Scoped 2026-08-27 (~02:00) and deliberately
deferred to a fresh session — see *Why this was not implemented on discovery*.

## Context

`docs/deferrals.md` lists `ExecuteFunctors` under `Ext.Stats` as:

> | `ExecuteFunctors` | partial | Full param-block construction unverified for all
> functor types. | `src/stats/functor_hooks.c` | Extend per-functor param evidence |

**That diagnosis is wrong, and the stated unlock path solves a problem that is
not the blocker.** Investigation on 2026-08-27 found param blocks are already
handled: `Ext.Stats.PrepareFunctorParams(type)` builds a default-initialized
`ContextData` and returns it as `bg3se.FunctorContext` userdata
(`src/lua/lua_stats.c:1269`), and eight context structs are laid out in
`src/stats/functor_types.h` (`AttackTargetContextData` … `InterruptContextData`).

The actual blocker is stated by the implementation's own log line
(`src/lua/lua_stats.c`, `lua_stats_execute_functors`):

```c
LOG_STATS_DEBUG(
    "ExecuteFunctors: context type %d ready, but no bound "
    "StatsFunctorList/StatsFunctorBase object is available",
    (int)ctx_ud->type);
return 0;
```

The function resolves the context and the original proc
(`functor_hooks_get_original_proc`), then returns without calling anything,
because **Lua has no way to hold a `StatsFunctorList*`**.

## What already exists

Three of the four pieces are built and working; only the connection is missing.

| piece | location | state |
|---|---|---|
| Hooks receive genuine engine `StatsFunctorList*` | `src/stats/functor_hooks.c` (9 contexts: AttackTarget, AttackPosition, Move, Target, NearbyAttacked, NearbyAttacking, Equip, Source, Interrupt) | works |
| Pointer forwarded to the Lua event layer | `functor_hooks.c` → `events_fire_execute_functor(L, ctxType, (void*)functors, context)` | works |
| Exposed to Lua handlers | `src/lua/lua_events.c` — as a **bare integer**: `lua_pushinteger(L, (lua_Integer)(uintptr_t)functors)` → field `FunctorListPtr` | unsafe to consume |
| `ExecuteFunctors` accepts a list | `src/lua/lua_stats.c` | **missing** |

A raw integer cannot be consumed safely: the pointer is engine-owned and valid
only for the duration of that dispatch. A mod stashing `FunctorListPtr` and
calling later would hand a freed pointer to the engine's functor executor.

## Design

Mirror the two patterns the codebase already uses.

**1. `bg3se.FunctorList` userdata**
Same shape as the existing `FUNCTOR_CONTEXT_METATABLE` block
(`src/lua/lua_stats.c:1262`):

```c
#define FUNCTOR_LIST_METATABLE "bg3se.FunctorList"

typedef struct {
    const StatsFunctorList *list;
    uint32_t owner_generation;   /* copy taken at creation */
    int      ctx_type;           /* FunctorContextType it was dispatched for */
} LuaFunctorList;
```

**2. Lifetime tagging via the existing owner+generation mechanism**
`src/lua/lua_runtime.h` already documents exactly this contract — *"can be
validated against an owner + generation before use"*, with `generation` *"bumped
on every unregister; refs carry a copy"*. The handle records the generation at
creation and re-checks before use. This is the load-bearing safety property, not
an optimization.

**3. Emit the handle from the event**
In `events_fire_execute_functor` (and the `AfterExecuteFunctor` path), push the
userdata as a new field `FunctorList` **alongside** the existing
`FunctorListPtr` integer. Keep the integer: removing it is a breaking change to
any mod already reading it, and it costs nothing to leave.

**4. Accept it in `ExecuteFunctors`**
`Ext.Stats.ExecuteFunctors(functorContext, functorList)`:

- `functorList` absent → keep today's behavior (warn once, return `nil`). Do not
  silently change what existing callers get.
- generation mismatch → return `false` and warn. **Fail closed.**
- `ctx_type` mismatch between context and list → return `false`. Executing a
  list against the wrong context type shifts every register (see the
  `result_out` note in `functor_types.h` and
  `docs/bugs/wave2-functor-crash-analysis.md`).
- all checks pass → call the original proc for that context type, forwarding
  the hidden leading `result_out` argument.

**The `result_out` hazard.** `functor_hooks.c` warns in a comment block that
`result_out` is a hidden leading output argument (`esv::functor::Result`) not
visible in the demangled symbol, that it MUST be accepted and forwarded or every
subsequent register shifts, and that it must never be dereferenced. Any new call
path must obey the same contract. This has already caused one crash
(`docs/bugs/wave2-functor-crash-analysis.md`).

## Validation

Cannot be validated without a live session, and session loading is currently
unreliable — see `bg3se-spellsystem-load-crash` in the memory notes (one
successful load out of many attempts; cause unknown).

1. Subscribe to `ExecuteFunctor`, confirm `ev.FunctorList` arrives as userdata
   and `ev.FunctorListPtr` still arrives as an integer (no regression).
2. Inside the handler, call `ExecuteFunctors(PrepareFunctorParams(ev.ContextType),
   ev.FunctorList)` — expect the functors to run a second time with observable
   effect (damage/status applied twice).
3. **Stale-handle test (the important one):** stash `ev.FunctorList` in an
   upvalue, call it from a later `Ext.Timer` tick, expect `false` + warning and
   **no crash**.
4. Wrong-context test: pass a `Move` context with an `AttackTarget` list, expect
   `false`, no call.
5. Run with `BG3SE_LOG_LEVEL=INFO` — DEBUG writes ~86,000 lines/sec during load
   (272 MB in a single session) and will distort any timing observation.

## Definition of done

- `ExecuteFunctors` executes a real engine functor list from Lua, live-verified.
- Stale and mismatched handles return `false` without crashing, verified
  deliberately rather than assumed.
- `docs/deferrals.md` row rewritten: the "param-block construction" rationale is
  retired, replaced either by "implemented" or by an accurate blocker.
- No change to what existing `FunctorListPtr` consumers receive.

## Why this was not implemented on discovery

This creates a path where a Lua value drives a native call into the engine's
functor executor. Wrong validation is a crash, not a wrong answer. It was scoped
at ~02:00 after a session in which three confident causal conclusions were each
disproven by a later controlled test (see `bg3se-spellsystem-load-crash`).
Writing pointer-validation code under those conditions was judged a bad trade;
the analysis is captured here so implementation starts from evidence rather than
rediscovery.

## Related

- `docs/bugs/wave2-functor-crash-analysis.md` — the `result_out` register-shift crash
- `src/stats/functor_types.h` — context layouts, the eleven verified addresses,
  and the exact-build gate that keeps unknown builds fail-closed
- `docs/deferrals.md` — the registry row this plan retires
