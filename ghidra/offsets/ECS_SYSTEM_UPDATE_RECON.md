# ECS system-update dispatch recon

**Date:** 2026-08-03

**Game build:** 4.1.1.7209685

**Scope:** Static reconnaissance for `Ext.Entity.OnSystemUpdate` and
`Ext.Entity.OnSystemPostUpdate`; no game launch and no runtime patching.

## Verdict

**Implementation-ready.** The macOS binary has the same useful boundary as
Windows BG3SE: every registered ECS system has a writable update-function
pointer in its `SystemTypeEntry`. The central worker executor loads that pointer
from `entry + 0x18` and calls it as:

```c
void UpdateProc(void *system, void *entityWorld, GameTime const *time);
//               x0            x1                 x2
```

The preferred implementation is therefore to replace the selected registry
entry's `UpdateProc`, save the original, and run `pre -> original -> post`.
No executable-text hook is required. A central inline hook is also technically
possible at `ecs::core::SystemDependencyExecutor::ExecuteWTKernel()`
(`0x1063788cc`), but it is a higher-risk diagnostic/fallback path rather than
the Windows-parity implementation.

The remaining engineering requirement is worker-thread-safe Lua entry. System
updates are dispatched by worker jobs. The existing port has a recursive
process-wide Lua gate (`src/lua/lua_gate.c`), but a future implementation must
also select/pin the correct client or server Lua state and keep callback
execution synchronous; deferring the callback would lose the advertised
before/after boundary.

## Inputs and address convention

The game executable was resolved through the existing launch/deploy scripts to:

```text
/Users/tomdimino/Library/Application Support/Steam/steamapps/common/
Baldurs Gate 3/Baldur's Gate 3.app/Contents/MacOS/Baldur's Gate 3
```

The executable was inspected only with static tools. `otool -f` reports the
arm64 architecture (`cputype 16777228`) at decimal fat-file offset `257261568`,
or `0xf558000`. The arm64 image base used below is `0x100000000`:

```text
fat_file_offset = 0xf558000 + (virtual_address - 0x100000000)
```

The first required Ghidra bridge query was attempted:

```sh
curl -s "http://127.0.0.1:8080/searchFunctions?query=SystemUpdate"
```

The bridge was down (connection refused, HTTP `000`), so the evidence below
comes from `nm -arch arm64 -n`, `c++filt`, `strings`, `otool`, and
`xcrun llvm-objdump` against a temporary arm64 slice. Addresses are preferred
virtual addresses from the build's Mach-O symbol table; at runtime, apply the
normal ASLR slide/base conversion used elsewhere in the port.

## Windows reference: exact API mechanism

The read-only Windows clone establishes the intended semantics and rules out an
outer `EntityWorld::Update` detour as the per-system mechanism:

1. `BG3Extender/Lua/Libs/Entity.inl:219-229` sends both Lua APIs to
   `EntityEventHelpers::SubscribeSystemUpdate`; pre/post is a boolean and
   `once` is optional.
2. `Lua/Shared/EntityEventHelpers.inl:46-61` resolves an `ExtSystemType` to a
   `SystemTypeEntry`, checks `System`, and subscribes using `SystemIndex0`.
3. `GameDefinitions/EntitySystem.cpp:1387-1403` obtains
   `GetEntityWorld()->Systems.Systems[systemType]`, saves `system.UpdateProc`,
   records `System* -> SystemTypeIndex`, then assigns
   `system.UpdateProc = &StaticSystemUpdateHook`.
4. `EntitySystem.cpp:1408-1432` implements the wrapper in exact order:
   pre callback, saved original update proc, post callback.
5. `Lua/Shared/SystemEvents.inl:103-146` receives `BaseSystem*`, `GameTime`, and
   `SystemTypeIndex` internally, but calls the Lua function with an empty tuple.
   Thus the public Lua callback receives **no arguments**. `once`
   unsubscriptions are deferred safely.
6. `GameDefinitions/EntitySystem.cpp:711-801,841-849` builds the name mapping
   from the engine's system TypeId context and maps known `EngineClass` strings
   to `ExtSystemType` values.
7. The separate `EntityWorld::Update` detour in
   `Extender/ScriptExtender.cpp:280-300` handles outer ECS lifecycle/deferred
   events. It is not how Windows implements named per-system callbacks.

The macOS dispatch and registry described below match items 2-4 directly.

## Symbol-first candidate inventory

Demangling the arm64 symbol table produced these query counts:

```text
SystemUpdate: 909
UpdateSystems: 0
GameLoop:      0
```

All 909 `SystemUpdate` hits are concrete instances of
`ecs::_private::SystemRegistrationHelper::SystemUpdate<T>(ecs::System&,
ecs::EntityWorld&, ls::GameTime const&)`. They are the per-type thunks stored in
registry entries, not 909 loop functions.

| Candidate | VA | Fat file offset | Assessment |
|---|---:|---:|---|
| `ecs::EntityWorld::Update(ls::GameTime const&)` | `0x106366f58` | `0x158bef58` | Outer ECS cycle; schedules executors and waits, but has no current-system identity |
| `ecs::core::SystemDependencyExecutor::ExecuteWTKernel()` | `0x1063788cc` | `0x158d08cc` | **Central per-system dispatch**; loads entry, update proc, system, world, and time |
| `ecs::core::SystemECBFlushJob::ExecuteWTKernel()` | `0x106378a84` | `0x158d0a84` | Calls the entry's ECB-flush proc, not its normal update proc |
| `App::Update(ls::GameTime const&)` | `0x100bd40e8` | `0x1012c0e8` | Outer application loop |
| `esv::GameStateMachine::Update(ls::GameTime const&)` | `0x104a15e20` | `0x13f6de20` | Server game-state loop, not generic ECS system dispatch |
| `BaseApp::Update(ls::GameTime const&)` | `0x105d0f33c` | `0x1526733c` | Outer application loop |

Representative concrete `SystemUpdate<T>` thunks:

| System thunk | VA | Fat file offset |
|---|---:|---:|
| `ecl::PickingHelperManager` | `0x101053a8c` | `0x105aba8c` |
| `ls::AnimationBlueprintSystem` | `0x101ef85d4` | `0x114505d4` |
| `esv::DialogSystem` | `0x103d2ea28` | `0x13286a28` |
| `esv::combat::System` | `0x103dcd418` | `0x13325418` |
| `ls::SoundRoutingSystem` | `0x105f27834` | `0x1547f834` |

## Exact dispatch path

### 1. `ecs::EntityWorld::Update` — outer scheduler

At `0x106366f58`:

```asm
106366f58  sub  sp, sp, #0x60
106366f5c  stp  x26, x25, [sp, #0x10]
106366f60  stp  x24, x23, [sp, #0x20]
106366f64  stp  x22, x21, [sp, #0x30]
...
106366f74  ldrb w8, [x0, #0x251]
106366f78  cbz  w8, <return>
106366f7c  mov  x19, x0
106366f80  ldr  q0, [x1]
106366f84  ldr  w8, [x1, #0x10]
106366f88  str  w8, [x0, #0x210]
106366f8c  str  q0, [x0, #0x200]       ; copy GameTime into world
106366f90  ldr  x0, [x0, #0x218]       ; dependency executor/batch
...
106366fb4  ldr  x0, [x19, #0x68]
106366fb8  bl   0x1063788cc             ; ExecuteWTKernel
...
106367070  bl   0x1063788cc             ; another queued executor
...
1063671b8  ldr  x0, [x19, #0x218]
1063671c0  bl   <WorkerThreadBatch::WaitForCompletion>
...
10636734c  b    <ecs::EntityWorld::FlushECBs>
```

This function owns the per-frame ECS cycle and copies the `GameTime` to
`EntityWorld + 0x200`, but its boundary identifies only the world. It schedules
dependency executors rather than iterating a simple `(index++)` loop. Hooking it
alone cannot implement subscriptions by system name.

### 2. `SystemDependencyExecutor::ExecuteWTKernel` — normal update call site

At `0x1063788cc`:

```asm
1063788cc  sub  sp, sp, #0x70
1063788d0  stp  x28, x27, [sp, #0x10]
1063788d4  stp  x26, x25, [sp, #0x20]
1063788d8  stp  x24, x23, [sp, #0x30]
1063788dc  stp  x22, x21, [sp, #0x40]
...
1063788ec  mov  x19, x0
1063788f0  ldr  w8, [x0, #0x34]
1063788f4  str  w8, [x0, #0x30]
1063788f8  ldr  x9, [x0, #0x20]       ; SystemTypeEntry *entry
1063788fc  cbz  x9, 0x106378920
106378900  ldr  x8, [x9, #0x18]       ; entry->UpdateProc
106378904  cbz  x8, 0x106378920
106378908  ldrb w10, [x9, #0x12]      ; entry->Activated
10637890c  cbz  w10, 0x106378920
106378910  ldr  x0, [x9]              ; entry->System
106378914  ldr  x1, [x19, #0x28]      ; executor->EntityWorld
106378918  add  x2, x1, #0x200        ; &world->GameTime
10637891c  blr  x8                     ; UpdateProc(system, world, time)
```

This is the best central candidate. At function entry, the executor object is
in `x0`; `executor + 0x20` is the system entry and `executor + 0x28` is the
world. At the indirect call, system identity is available both as the entry
pointer (`x9`) and system object (`x0`). The registry index is not passed as a
separate argument, but each entry contains two copies at `+0x08` and `+0x0c`.
There is no name argument at the call boundary.

### 3. ECB flush is a distinct phase

At `0x106378a84`:

```asm
106378a84  ldr  x8, [x0, #0x18]
106378a88  ldr  x2, [x8, #0x30]       ; entry->ECBFlushProc
106378a8c  cbz  x2, <return>
106378a90  ldr  x8, [x8]              ; entry->System
106378a94  ldr  x9, [x0, #0x10]       ; EntityWorld
106378a98  add  x1, x9, #0x200        ; &world->GameTime
106378a9c  mov  x0, x8
106378aa0  br   x2
```

The `+0x30` callback is the ECB-flush job. It must not be confused with
`UpdateProc +0x18`, and `OnSystemPostUpdate` should run immediately after the
normal `UpdateProc` returns, not after this later flush phase.

### 4. Concrete thunk confirms the ABI

`SystemUpdate<esv::combat::System>` at `0x103dcd418` begins with a normal
prologue, uses the world argument in `x1`, prepares a derived system method's
arguments, and calls the concrete update implementation. This corroborates the
central caller's AAPCS64 interpretation: `x0 = System*`, `x1 = EntityWorld*`,
and `x2 = GameTime const*`.

## System registry chain

### EntityWorld to entry

The statically proven chain is:

```text
EntityWorld
  +0x28  embedded DEPRECATED_Array<SystemTypeEntry> vtable
  +0x30  SystemTypeEntry *buffer
  +0x38  allocated/capacity (u32)
  +0x3c  used/high-water (u32)
  +0x40  non-null count (u32)
  +0x44  grow size (u32; initialized to 1)

entry(index) = *(SystemTypeEntry **)(world + 0x30) + index * 0xf8
```

Evidence:

- The `EntityWorld` constructor at `0x106362640` initializes the embedded array
  at `world + 0x28`, writes capacity 16 and grow size 1, allocates `0xf90`
  bytes (16 entries of `0xf8`, plus the allocation header), and stores the
  resulting buffer at `world + 0x30`.
- `DEPRECATED_Array<SystemTypeEntry>::SetSize` at `0x100c80d74` manipulates the
  buffer and the count fields above.
- `DEPRECATED_Array<SystemTypeEntry>::SetAt` at `0x100c81208` computes the
  destination with an explicit stride:

```asm
100c8126c  ldr  x10, [x2]             ; source entry->System
100c81270  ldr  x8, [x0, #0x8]        ; array buffer
100c81274  mov  w9, #0xf8
100c81278  madd x8, x1, x9, x8        ; buffer + index * 0xf8
100c8127c  ldr  x9, [x8]              ; destination entry->System
```

The array object is passed as `world + 0x28`, so its `+0x08` buffer is
`world + 0x30`.

### Proven `SystemTypeEntry` prefix

```c
struct SystemTypeEntryPrefix {
    void *System;                    // +0x00
    int32_t SystemIndex0;            // +0x08
    int32_t SystemIndex1;            // +0x0c
    uint8_t field_10;                // +0x10
    uint8_t field_11;                // +0x11
    uint8_t Activated;               // +0x12
    uint8_t padding_13[5];           // +0x13
    void (*UpdateProc)(void *,
        void *, GameTime const *);   // +0x18
    void *SomeProc2;                 // +0x20
    void *SomeProc3;                 // +0x28
    void *ECBFlushProc;              // +0x30
    // dependency/dependent sets and other data follow
};                                  // full stride: 0xf8
```

Independent checks on this prefix:

- `RegisterSystem<ls::LevelInstanceLoadRequestSystem>` at `0x100f77540`
  reads its runtime TypeId, duplicates that index into `+0x08/+0x0c`, stores
  the system pointer at `+0x00`, and stores the concrete `SystemUpdate<T>` thunk
  at `+0x18` before inserting the entry into the `world + 0x28` array.

```asm
100f77560  adrp x8, <TypeId global page>
100f77564  ldr  x8, [x8, #0xcd0]
100f7756c  ldr  w19, [x8]             ; runtime system index
...
100f7766c  str  x21, [sp, #0x30]      ; entry +0x00: System
100f77670  dup.2s v0, w19
100f77678  ldr  x8, [x8, #0x988]      ; concrete SystemUpdate<T>
100f7767c  str  d0, [sp, #0x38]       ; entry +0x08/+0x0c: indices
100f77680  str  x8, [sp, #0x48]       ; entry +0x18: UpdateProc
```

- The normal executor reads exactly `System +0x00`, `Activated +0x12`, and
  `UpdateProc +0x18` before its `blr`.
- `EntityWorld::ActivateSystemInternal` at `0x106366658` tests and sets
  `entry + 0x12`.
- The system-entry comparer at `0x1063669e0` compares the indices at `+0x0c`
  and then `+0x08`.
- The ECB worker reads the distinct function pointer at `+0x30`.

### Name to dynamic index

The entry prefix does not expose a name pointer, and Windows BG3SE also does
not rely on one (the later `Name` field in its reference struct is disabled
under `#if 0`). Name resolution happens through the engine's TypeId machinery:

```text
engine system class name
  -> TypeId<T, ecs::SystemsContext>::m_TypeIndex global
  -> runtime int32 system index
  -> EntityWorld system array[index]
```

Static symbols expose 932 non-guard system-index globals (1,864 symbols when
their adjacent guard variables are included) of the form:

```text
ls::TypeId<T, ecs::SystemsContext>::m_TypeIndex
```

The context itself is visible as:

| Symbol | VA |
|---|---:|
| `ls::TypeContext<ecs::SystemsContext>::m_State` | `0x108948570` |
| systems-context registration record | `0x108948628` |
| `ls::TypeContext<ecs::SystemsContext>::RegisterContext` | `0x100c43a08` |

`RegisterType<ls::AnimationBlueprintSystem>` at `0x100c40ce4` associates an
`int32_t*` TypeId global with generated type-name text in a TypeInfo record. The
binary also contains generated names such as these:

| Generated type-name string | VA |
|---|---:|
| `ecs::SystemsContext` | `0x107b513e2` |
| `ls::AnimationBlueprintSystem` | `0x107b5172d` |
| `esv::combat::System` | `0x107b536a7` |
| `ecl::PickingHelperManager` | `0x107b65c36` |
| `ls::SoundRoutingSystem` | `0x107b667f8` |
| `esv::DialogSystem` | `0x107c744f4` |

The full linked `TypeContext` node layout was not proved in this session. It is
not required for the initial implementation because the local symbol table
gives a direct, build-specific `name -> TypeId global` table. Representative
globals for supported/public systems include:

| Engine class | TypeId global VA |
|---|---:|
| `ecl::PickingHelperManager` | `0x1088a4908` |
| `ecl::DialogSystem` | `0x1088ea958` |
| `ls::AnimationBlueprintSystem` | `0x108948560` |
| `ls::AnimationSetSystem` | `0x1088b4840` |
| `ls::EffectsManager` | `0x108940080` |
| `ls::VisualChangeRequestSystem` | `0x10893fd30` |
| `ls::VisualChangedSystem` | `0x10893fbb0` |
| `ls::VisualSystem` | `0x108947bb8` |
| `ls::LightSystem` | `0x108940110` |
| `ls::SoundRoutingSystem` | `0x1089401c0` |
| `esv::ActionResourceSystem` | `0x108915f18` |
| `esv::BoostSystem` | `0x108902ef8` |
| `esv::combat::System` | `0x108912270` |
| `esv::TurnOrderSystem` | `0x108915ef8` |
| `esv::PassiveSystem` | `0x108911098` |
| `esv::shapeshift::System` | `0x108911190` |
| `esv::spell_cast::CastRequestSystem` | `0x108911210` |
| `esv::StatsSystem` | `0x108912230` |
| `ecl::EquipmentVisualsSystem` | `0x1088acbb0` |
| `ecl::VisualSystem` | `0x1088acc30` |
| `ecl::CharacterManager` | `0x1088ab820` |
| `ecl::effect::HandlerSystem` (BSS) | `0x10898b508` |

These are addresses of mutable runtime index integers, not literal indices.
Read the `int32_t` after applying the image slide, validate it against the
captured world's array bounds, and then derive the entry.

The existing macOS port already supplies both roots needed for this chain:
`src/entity/entity_system.c` captures the server world from
`esv::EocServer + 0x288` and the client world from `ecl::EocClient + 0x1d0`,
with `entity_get_world_for_context(bool is_server)` exposing the selection.

## Hookability assessment

### Preferred: registry pointer substitution

`SystemTypeEntry` instances are heap-backed data. Replacing `entry + 0x18`
does not modify `__TEXT`, avoids instruction relocation, and is the mechanism
used by Windows BG3SE. The replacement must be installed per world because
client and server have distinct arrays and may assign different dynamic
indices.

Required validation before any write:

1. `world`, array buffer, and TypeId global are readable.
2. Index is nonnegative and below the array's used/high-water value at
   `world + 0x3c` (and below capacity at `+0x38`).
3. `entry->System` and `entry->UpdateProc` are non-null.
4. `entry->SystemIndex0 == index` and preferably `SystemIndex1 == index`.
5. The same `(world,index)` is not already wrapped; preserve the true original
   once and restore it during teardown/world replacement.

Pointer publication/restoration must be synchronized with worker readers. A
64-bit aligned pointer store is naturally single-copy atomic on arm64, but the
hook registry and Lua subscription state still require explicit locking and a
well-defined world-transition teardown order.

### Optional central text hook

Both principal text candidates have reachable, non-inlined symbol boundaries
and ordinary entry prologues. Their first 16 bytes contain only stack/frame
setup (`sub sp` and `stp`), with no PC-relative instruction:

- `EntityWorld::Update` at `0x106366f58`
- `SystemDependencyExecutor::ExecuteWTKernel` at `0x1063788cc`

That is compatible with the existing far-hook path in
`src/hooks/arm64_hook.c`, whose 16-byte absolute jump overwrites the entry and
whose MAP_JIT trampoline copies those instructions before returning to
`target + 16`. A near four-byte branch also fits if the replacement is within
the branch range.

Use an **explicit offset 0** call (`arm64_hook_at_offset(target, 0, ...)`) if
this fallback is ever implemented, because this recon has verified that exact
boundary. The current `arm64_safe_hook()` analysis should also select offset 0
for these two candidates: none of the first 16 bytes is marked PC-relative, so
`arm64_analyze_prologue()` resets `safe_hook_offset` to zero. An explicit
offset avoids making that behavior an implicit dependency. The data-pointer
substitution avoids executable-text patching entirely.

`EntityWorld::Update` remains unsuitable for named system callbacks even
though it is hookable: its entry arguments identify only `(world, time)`.
`SystemDependencyExecutor::ExecuteWTKernel` is suitable for tracing all
dispatches because its executor gives both entry and world, but wrapping every
executor invocation is unnecessary for selective subscriptions.

The dispatch uses a plain `blr x8`, not an authenticated branch instruction,
so the replacement function pointer does not require pointer-authentication
signing on this build.

## Implementation sketch

1. Define the build-4.1.1.7209685 system TypeId globals for the supported
   `ExtSystemType`/engine-class names, preferably generated from `nm` and kept
   alongside the public enum mapping.
2. When each client/server `EntityWorld` is captured, read the runtime
   `int32_t` index from the corresponding TypeId global and resolve
   `*(world + 0x30) + index * 0xf8` with all validations above.
3. Maintain a per-`(world,index)` hook record containing the system pointer,
   original `UpdateProc`, client/server context, pre/post subscriber lists, and
   once/deferred-removal state.
4. On the first subscriber, atomically replace `entry->UpdateProc` with a common
   AAPCS64 wrapper. Resolve the hook record by `(world,system)` at entry; do not
   assume the client and server use the same index.
5. In the wrapper, pin/select the context-specific Lua state and invoke pre
   subscribers with **zero Lua arguments** while holding the existing recursive
   Lua gate. Release the Lua gate before calling
   `original(system, world, time)` exactly once, then reacquire it for the post
   subscribers. Protect the hook record's lifetime separately across the whole
   wrapper. Ensure the original call still occurs if Lua is unavailable or
   event dispatch is transition-suppressed.
6. Mark `once` subscriptions inactive before invocation and remove them only
   after iteration, matching the Windows behavior.
7. Restore the exact original pointer when the final subscriber is removed and
   before a world is destroyed/replaced. Clear the `System*` lookup at the same
   transition boundary.
8. Use the central executor hook at `0x1063788cc` only as a diagnostic/fallback:
   at entry read `executor + 0x20` and `executor + 0x28`; install it with
   `arm64_hook_at_offset(..., 0)`.

## Confidence and remaining runtime checks

| Finding | Confidence | Runtime check still needed during implementation |
|---|---|---|
| `ExecuteWTKernel` is the normal per-system dispatch | High | Trace a few entry/system/index triples without invoking Lua |
| `SystemTypeEntry::UpdateProc` is `+0x18` | High; independently shown by registration and dispatch | Confirm original pointer lies in executable game image before replacement |
| Entry stride is `0xf8` | High; explicit `madd` in `SetAt` | Validate index fields and array bounds in both worlds |
| EntityWorld system array begins at `+0x28`, buffer `+0x30` | High; constructor and array methods agree | Observe plausible counts after session load |
| Name -> TypeId-global -> dynamic index chain | High for symbol-backed supported systems | Compare a sample runtime index to its entry's two stored indices |
| Full runtime TypeContext linked-list layout | Incomplete | Not needed for the first build-specific implementation; defer generic enumeration |
| Synchronous worker-thread Lua callbacks are safe with current primitives | Medium | Stress nested callbacks, transition teardown, and client/server state selection |

This evidence supports implementation rather than registry-entry deferral. The
unproved generic TypeContext enumeration can be deferred independently; it is
not a blocker for the named systems already represented by build-specific
TypeId globals.
