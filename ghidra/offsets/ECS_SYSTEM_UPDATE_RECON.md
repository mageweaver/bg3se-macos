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

## 2026-08-20 — live probe: gate is OPEN, but the recon data is stale

`Ext.Entity.OnSystemUpdate` / `OnSystemPostUpdate` are registered and the build
gate passes on 4.1.1.7398727 (`ECS_SYSTEM_UPDATE_VERIFIED_BUILD` already equals
`BG3_KNOWN_VERSION`), so subscription attempts reach the resolver rather than
being refused. They still fail, for two distinct reasons:

    OnSystemUpdate("ServerCharacterManager") -> Unknown system type
    OnSystemUpdate("ClientCharacterManager") -> System not registered:
                                                system entry is absent or its
                                                stored indices disagree
    OnSystemUpdate("NoSuchSystemXYZ")        -> Unknown system type  (correct)

1. **Name coverage.** Several plausible names are absent from
   `GENERATED_SYSTEM_TYPEID_ENTRIES` entirely (`ServerCharacterManager`), so the
   table needs regenerating, not just re-addressing.

2. **The TypeId globals read garbage.** The recorded VAs *are* correct -- e.g.
   `ecl::CharacterManager` is `0x1088db750` in the table and `nm` agrees -- but
   reading them at runtime with the ASLR slide (`0xa20000`) yields nonsense:

   | symbol | value read |
   |---|---:|
   | `ecl::CharacterIconRenderSystem` | -1859477240 |
   | `ecl::CharacterManager` | -1459607556 |
   | `ecl::EquipmentVisualsSystem` | -1258290912 |
   | `ecl::PickingHelperManager` | 889192234 |

   The identical read technique returns valid, in-range, unique indices for the
   `ReplicatedTypeContext` globals on the same build, so the method is sound and
   these particular slots are simply not live TypeIndex storage.

3. **The EntityWorld system array offsets look stale.** The recon records the
   array/buffer at `+0x28`/`+0x30`, but on 7398727 that yields
   `count = 1433124880` -- plainly not a system count.

**Good news for whoever picks this up:** the SystemsContext TypeIds are exported
symbols, exactly like the replicated-type globals. 457 of them are recoverable
with no Ghidra at all:

    nm -gU <arm64 slice> | grep 'ecs14SystemsContext' \
      | grep '11m_TypeIndexE$' | grep -v '__ZGV'

So step 1 is mechanical: regenerate the whole table from symbols (names *and*
addresses) rather than patching entries. Step 2 -- re-deriving the EntityWorld
system array offsets and the `0xf8` entry stride for this build -- is the part
that still needs disassembly.

### CORRECTION (same day): the system TypeId table is NOT stale

The section above, and the "stale system-TypeIds" note in CLAUDE.md, both blame
the generated table. That is wrong. Re-running the project's own extractor
against the installed 4.1.1.7398727 binary:

    python3.12 tools/extract_typeids.py "<BG3 binary>" \
      --build-id 4.1.1.7398727 --header-out /tmp/regen.h

produces a `GENERATED_SYSTEM_TYPEID_ENTRIES` block that is **byte-identical** to
the shipped one: 73 entries, 73 exact matches, zero additions, zero removals.
Regenerating the table cannot fix `OnSystemUpdate`.

(Note the extractor needs Python 3.10+ for `zip(..., strict=True)`; the macOS
system `python3` is 3.9 and fails with `TypeError: zip() takes no keyword
arguments`.)

So the two real issues are:

1. **Coverage, not staleness.** The binary exports **454** `ecs::SystemsContext`
   TypeIndex symbols, but the table only carries **73**. Names like
   `ServerCharacterManager` return "Unknown system type" simply because they were
   never in the table -- not because their address drifted. Widening the
   extractor's system filter would add them, but see (2) before assuming that
   helps.

2. **The runtime lookup is what fails.** For the 73 covered systems the recorded
   addresses are provably correct, yet reading them live (slide `0xa20000`)
   returns implausible values (e.g. `ecl::CharacterManager` -> -1459607556),
   and `ClientCharacterManager` -- which *is* in the table -- fails with
   "system entry is absent or its stored indices disagree". The identical read
   technique returns valid, unique, in-range indices for the
   `ReplicatedTypeContext` globals on the same build, so the method is sound.
   Additionally `EntityWorld + 0x28/+0x30` yields `count = 1433124880`, which is
   not a system count.

**Revised next step.** Do not regenerate the table. Determine instead whether
these TypeIndex globals are populated only at system-registration time (so the
correct source is the live `EntityWorld` system array, not the statics), and
re-derive the array/buffer offsets and `0xf8` stride for 7398727. That is
disassembly work, and it is the only remaining blocker for both
`OnSystemUpdate` and `OnSystemPostUpdate`.

### Disassembly findings (2026-08-20): `+0x28/+0x30` is the wrong structure

`ecs::EntityWorld::Update(ls::GameTime const&)` is at `0x10638ec08` (Ghidra
rebase). Its dispatch loop is unambiguous:

```asm
ldp  x9, x10, [x19, #0x80]   ; x19 = EntityWorld: divisor/count @ +0x80,
                             ;                    buffer ptr    @ +0x88
udiv x12, x8, x9
msub x11, x12, x9, x8        ; x11 = index % count
add  x9, x10, x11, lsl #7    ; entry = buffer + index*0x80  (stride 128)
...
add  x10, x10, x11, lsl #7
ldr  x0, [x10, #0x8]         ; executor pointer at entry + 0x8
bl   ecs::core::SystemDependencyExecutor::ExecuteWTKernel   ; 0x1063a057c
```

Also visible: an atomic cursor at `EntityWorld + 0x180` (`ldapr`/`casal`), and
state words at `+0x218` / `+0x228`.

So the pair this recon records as the system array (`+0x28` / `+0x30`,
stride `0xf8`) does not describe what `Update` actually walks. What lives at
`+0x80`/`+0x88` is a **work queue of `SystemDependencyExecutor` pointers with a
0x80 stride**, which is why reading a "system count" from `+0x30` returned
`1433124880` in the live probe.

The `0xf8` figure does appear sound as the *descriptor* size:
`ls::RegisterSharedSystems(ecs::EntityWorld&)` (`0x105f23118`) zeroes a stack
descriptor spanning roughly `+0x58`..`+0xe8` before registering, consistent with
a ~0xf8-byte `SystemTypeEntry`.

**Why this stops here.** `RegisterSharedSystems` is heavily inlined: it builds
`ls::HashTable<unsigned>` and several `ls::DynamicArray` members in place rather
than calling a tidy `RegisterSystem(...)`, so the destination offsets are not
recoverable from a single function's disassembly. This is the point to use the
analyzed Ghidra project (`~/tools/ghidra-projects/BG3_7398727`) and decompile
`RegisterSharedSystems` / `RegisterClientSystems` properly, where the member
writes will render as structure assignments.

**Corrected task list for OnSystemUpdate/OnSystemPostUpdate:**
1. Do NOT regenerate the TypeId table -- it is byte-identical to a fresh
   extraction (see the correction above).
2. Decompile `RegisterSharedSystems` (`0x105f23118`) in Ghidra to recover where
   the ~0xf8 `SystemTypeEntry` descriptors are stored on `EntityWorld`.
3. Confirm the `UpdateProc` slot within that entry (recon says `+0x18`).
4. Only then revisit whether the TypeIndex statics or the live table is the
   correct lookup source.

## RESOLVED (2026-08-20): full layout recovered by decompilation

### Retraction of the previous section

The disassembly section above concluded that `EntityWorld + 0x28/+0x30` "is the
wrong structure" and that the real table was the `+0x80/+0x88` queue. **That was
wrong, and so was the live probe that motivated it.** The original recon was
correct. What follows supersedes it.

The `+0x80/+0x88` finding is still true as far as it goes -- `EntityWorld::Update`
does walk a 0x80-stride queue of `SystemDependencyExecutor` there -- but that is
the *scheduler*, not the system table, and it is not what `OnSystemUpdate` needs.

### Why the live probe misread it

The probe did `ReadPtr(world + 0x28)` and `ReadI32(world + 0x30)`, treating
`+0x28` as a buffer and `+0x30` as a count. In fact `+0x28` is the *start of the
array object* and `+0x30` is its **buffer pointer**. Reading a pointer as an
int32 produced `1433124880`, which is simply that pointer's low word. Nothing
was stale.

### Recovered layout (decompiled, not inferred)

`ls::RegisterSharedSystems(ecs::EntityWorld&)` at `0x105f23118` ends with:

```c
if (*(uint *)(param_1 + 0x38) <= uVar1) {
  ls::DEPRECATED_Array<ecs::core::SystemTypeEntry,...>::SetSize(
      param_1 + 0x28, *(uint *)(param_1 + 0x44) + uVar1);
}
ls::DEPRECATED_Array<ecs::core::SystemTypeEntry,...>::SetAt(
      param_1 + 0x28, uVar1, &descriptor);
```

`uVar1` is the system's `ecs::SystemsContext` TypeIndex, read through the GOT
slot for `ls::TypeId<T, ecs::SystemsContext>::m_TypeIndex`. **The TypeIndex is
the array index.**

`SetAt` (`0x100c81890`) gives the array's internals:

```c
plVar1 = (long *)(*(long *)(param_1 + 8) + param_2 * 0xf8);   // buffer @ +0x08, stride 0xf8
if (*(uint *)(param_1 + 0x14) <= param_2)
  *(int *)(param_1 + 0x14) = (int)param_2 + 1;                 // size   @ +0x14
*(int *)(param_1 + 0x18) += iVar2;                             // counter@ +0x18
```

Combining, relative to `EntityWorld`:

| Field | Offset | Notes |
|---|---:|---|
| `DEPRECATED_Array<SystemTypeEntry>` object | `+0x28` | array base |
| entry buffer pointer | `+0x30` | array `+0x08` |
| capacity | `+0x38` | array `+0x10`; guard in RegisterSharedSystems |
| size | `+0x3c` | array `+0x14`; bumped by SetAt |
| counter | `+0x40` | array `+0x18` |
| growth count | `+0x44` | array `+0x1c` |

`SystemTypeEntry` is **0xf8** bytes. Within it, the descriptor built by
`RegisterSharedSystems` places
`ecs::_private::SystemRegistrationHelper::SystemUpdate<T>` at **`+0x18`**,
confirming the original recon's `UpdateProc` slot. `+0x00` holds a vtable-like
pointer (the teardown path calls `(**(code **)(*entry + 8))()`), and `+0x08`
holds the TypeIndex duplicated into both halves of a 64-bit word
(`CONCAT44(uVar1, uVar1)`).

### Entry address formula

    entry = *(void **)(EntityWorld + 0x30) + TypeIndex * 0xf8
    updateProc = *(void **)(entry + 0x18)

valid when `TypeIndex < *(uint32 *)(EntityWorld + 0x3c)`.

### Remaining work

Everything needed to implement `OnSystemUpdate`/`OnSystemPostUpdate` is now
recovered. What is left is engineering, not RE: verify the formula live against
a known system, then wire the existing `ecs_system_update.c` swap logic to these
offsets. The name-coverage gap still stands separately -- the generated table
carries 73 of the 454 exported `SystemsContext` TypeIds, so uncovered names will
still report "Unknown system type" until the extractor's filter is widened.

### Tooling note: Ghidra ships no Apple Silicon decompiler

This was only reachable after building one. Ghidra 12.1.3 ships
`os/linux_x86_64` and `os/win_x86_64` decompiler binaries but **no
`mac_arm_64`**, so every decompile fails with
`os/mac_arm_64/decompile does not exist`. The bundled source builds it, but the
Makefile hardcodes Intel (it carries a literal
`TODO: need to revise to support arm64/aarch64 arch`):

    cd Ghidra/Features/Decompiler/src/decompile/cpp
    make ghidra_opt -j8 ARCH_TYPE="-arch arm64" \
         ADDITIONAL_FLAGS="-mmacosx-version-min=11.0 -w" OSDIR=mac_arm_64
    mkdir -p ../../../os/mac_arm_64
    cp ghidra_opt ../../../os/mac_arm_64/decompile

Headless decompilation works after that. This is plausibly why prior RE on this
project stopped at recon.

### Follow-up: the offsets in `ecs_system_update.c` were already correct

After recovering the layout, comparing it against the shipped implementation
shows **every constant already matches**:

| Constant in `ecs_system_update.c` | Value | Decompiled |
|---|---|---|
| `ECS_WORLD_SYSTEM_BUFFER_OFFSET` | `0x30` | matches |
| `ECS_WORLD_SYSTEM_CAPACITY_OFFSET` | `0x38` | matches |
| `ECS_WORLD_SYSTEM_USED_OFFSET` | `0x3c` | matches |
| `ECS_SYSTEM_ENTRY_STRIDE` | `0xf8` | matches |
| `ECS_SYSTEM_ENTRY_SYSTEM_OFFSET` | `0x00` | matches |
| `ECS_SYSTEM_ENTRY_INDEX0/1_OFFSET` | `0x08` / `0x0c` | matches (the CONCAT44 pair) |
| `ECS_SYSTEM_ENTRY_UPDATE_PROC_OFFSET` | `0x18` | matches |

So the walk is not the problem, and no code change is warranted from this
recon. The remaining failure is isolated to **resolving a system's TypeIndex**.

### What is known about the TypeIndex read

- The `ls::TypeId<T, ecs::SystemsContext>::m_TypeIndex` symbols are the real
  storage; the `PTR_..._10840a698` seen in the decompiler output is a `__got`
  slot (range `0x1083d0000` + `0x6ae58`) holding that symbol's address, not a
  second copy.
- Those globals live in `__DATA,__common`, i.e. BSS: **zero at load, assigned
  when the system registers.** A system that has not registered in the current
  session therefore has no meaningful index.
- The live probe read implausible values for four sampled systems, and
  `ClientCharacterManager` -- which *is* in the generated table -- reported
  "system entry is absent or its stored indices disagree", which is the expected
  symptom when the index fails the `< *(uint32*)(EntityWorld+0x3c)` bound.

### The one open question

Whether those reads were wrong (address/slide) or correct-but-unregistered.
This is a five-minute live check, not RE: with a session loaded, read the
TypeIndex for a system known to be registered in that world (a *server* system
when probing the server world, since `RegisterSharedSystems`/`RegisterClientSystems`
populate different sets), then confirm
`entry = *(void**)(world+0x30) + idx*0xf8` lands on an entry whose `+0x08`/`+0x0c`
pair equals that index.

If it does, `OnSystemUpdate` works today for covered systems and only the
name-coverage gap (73 of 454) remains. If it does not, the problem is the
TypeIndex source, not the table walk.

## VERIFIED WORKING (2026-08-20) — OnSystemUpdate/OnSystemPostUpdate credited

Live probe of the server `EntityWorld` on 4.1.1.7398727:

    buffer @ world+0x30 = 0x861a84010
    capacity @ +0x38    = 934
    size     @ +0x3c    = 934
    506 entries populated, first at slot 311
    491 of 506 have entry+0x08 == slot
    501 of 506 have a non-null UpdateProc at +0x18
    slot 0 reads +0x08 = 0xffffffff, the "unset" sentinel the decompiled
      registration initialises with

An earlier scan reported "populated=0" only because it stopped at slot 300 --
entirely inside the empty prefix. The layout was right all along.

**Subscription works.** 16 of 24 sampled system names subscribed successfully,
and a `ServerPassive` hook fired **462 times in ~12s** (~38Hz server tick)
before unsubscribing cleanly. Both APIs are credited.

### Three real limitations, none of them layout bugs

1. **`Client*` systems fail against the server world.** All seven sampled
   `Client*`/`PickingHelper` names returned "system entry is absent" while every
   `Server*` name but `ServerDialog` succeeded. Client systems live in the
   client `EntityWorld`; resolve them there.

2. **One hook kind per system.** Subscribing `OnSystemPostUpdate` to a system
   that already has an `OnSystemUpdate` hook fails with "original UpdateProc is
   outside the executable game image" -- the first hook replaced the proc with
   our trampoline and `pointer_is_game_text` correctly rejects it. Working as
   designed, but worth documenting for callers.

3. **`entry+0x0c` is not reliably a duplicate index.** 15 of 506 populated
   entries have `+0x08 == slot` but `+0x0c` holding something else
   (e.g. -8388605, 8388606). `resolve_system_entry` currently requires
   *both* to equal the index (`level_manager`-style guard at
   `ecs_system_update.c:296`), so those systems are rejected. Loosening that to
   validate only `+0x08` would widen coverage; it was left unchanged here
   because no sampled name needed it and tightening/loosening a safety guard
   deserves its own evidence.

### Still open: name coverage

The generated table carries **73** of the **454** exported `ecs::SystemsContext`
TypeIds, so most systems still report "Unknown system type". Widening the
extractor's system filter is mechanical (`nm | grep ecs14SystemsContext`).

## Entity tracing: verified 2026-08-20 (build 4.1.1.7398727, vanilla Tav fixture)

`Ext.Entity.EnableTracing` / `GetTrace` / `ClearTrace` were previously implemented
but never verified in-game. Live testing found two real defects, both now fixed.

### Defect 1 — tracing captured nothing (the significant one)

Signal connections are injected per component type, and `entity_events_bind`
installs hooks only for *types that already have a Lua subscription*. Because
`EnableTracing` merely set an observer callback and installed no hooks of its
own, `GetTrace()` returned an empty `Entities` table unless the mod happened to
independently subscribe to the very same component type. Measured: 0 entities
captured after applying a status; the same stimulus with a matching subscription
captured 1.

This is a genuine divergence from Windows, whose `ECSChangeTracer` hooks the ECS
framework itself and therefore sees the whole world.

Fixed with `entity_events_enable_global_capture(bool)`, which injects
create/destroy connections across every CCR type on enable. Ownership is tracked
in a bitmask so disable removes only our own connections and never tears down a
connection belonging to a real Lua subscription. Measured on the fixture:
1425 types hooked / 1290 skipped (NULL ComponentCallbacks) out of CCR 2715,
clean removal on disable, 11 entities and 33 component entries captured with no
subscriptions active.

### Defect 2 — Lua surface did not match Windows

Windows exposes (`PropertyMaps/CommonTypes.inl`, `ExtIdeHelpers.lua`):

    ECSEntityLog    { Entity : EntityHandle, Components, Create, Dead,
                      Destroy, Ignore, Immediate }
    ECSComponentLog { Name, Type, Create, Destroy, OneFrame, Replicate,
                      ReplicatedComponent }

`Flags` is `P_BITMASK`, so on Windows it surfaces as *named booleans*, not an
integer. The port emitted `Entity` as a bare integer and only integer `Flags` /
`ComponentType`, so Windows mod code reading `c.Name` or `log.Entities[h].Create`
got nil. `Entity` is now a real `BG3Entity` proxy (method calls verified) and both
logs carry the Windows boolean fields; `Name`/`Type` come from the ECS component
registry, which covers every CCR type, rather than the property-layout table,
which only describes the subset whose fields are mapped. The integer
`ComponentType`/`Flags` are retained as macOS extras alongside them.

### Known limitation — `OneFrame` is always false

One-frame status is encoded as bit `0x8000` of the component type index. The
index reaching us in the signal payload (`FunctionStorage + 0x18`) has that bit
already stripped, and the component registry stores the stripped value too —
confirmed by `esv::hit::HitNotificationEventOneFrameComponent`, which reports
`OneFrame=false` despite its name. Neither available source carries the bit, so
the field is emitted as false rather than inferred from a name suffix. This
affects the pre-existing registry consumers (`Ext.Entity` component info) equally
and is not introduced by tracing.

### Still divergent from Windows

Windows `ECSChangeTracerOptions` additionally follows the entity command buffer,
the immediate world cache, replication, and in-place component *modifications*.
macOS observes component add/remove signals only, so modification tracking
remains absent.

## entity:RemoveComponent — findings 2026-08-20

### It genuinely works

Live testing removed a real component from a live entity and the effect was
immediately observable (see the crash note below). That validates the whole
chain: the 666-entry specialization table, the slide correction, the build gate,
and the `EntityWorld + 0x3f0` ImmediateWorldCache pointer.

### DIVERGENCE — the port implements the wrong one of two Windows methods

Windows exposes two distinct methods (`LuaEntityProxy.inl:114-123`):

    RemoveComponent          -> EntityWorld->Deferred()->RemoveComponent(...)
                                queued on the EntityCommandBuffer, applied at
                                the next ECS flush
    RemoveComponentImmediate -> EntityWorld->Cache->RemoveComponent(...)
                                applied to the ImmediateWorldCache at once

The generated table holds `ecs::legacy::ImmediateWorldCache::RemoveComponent<T>`
specializations — the *immediate* path — but it was bound only to the name
`RemoveComponent`. Both names are now bound to it: `RemoveComponentImmediate` is
exact, while `RemoveComponent` still differs from Windows in timing (the removal
lands now rather than at the next flush). Mods depending on deferred ordering
will observe the change one flush early. Wiring the real deferred path requires
the EntityCommandBuffer layout, which is not mapped.

### Return value — RETRACTION

An earlier revision of this document claimed the specialization returns bool and
that the port was discarding it. That was wrong, and the "fix" that read a return
value has been reverted.

Template functions encode their return type in the mangled name, and every
specialization is `v` (void):

    _ZN3ecs6legacy19ImmediateWorldCache15RemoveComponentIN2ls12ConstructionEEEvNS3_2IDINS_18EntityHandleTraitsEEE
    -> void ecs::legacy::ImmediateWorldCache::RemoveComponent<ls::Construction>(ls::ID<ecs::EntityHandleTraits>)

The bool-returning `ImmediateWorldCache::RemoveComponent(EntityHandle,
ComponentTypeIndex)` seen in the Windows source is a *different*, non-template
overload. Reading a result from the macOS specialization returns whatever happens
to be left in w0. Confirmed in game: two consecutive removals of the same
component both reported "true".

`entity:RemoveComponent` therefore returns true to mean "a specialization existed
and was dispatched", and false only when the name is unknown or uncovered. It
cannot report whether the entity actually had the component.

### Effect is not immediately visible

Removal records a pending change in the ImmediateWorldCache; it does not edit the
committed storage class. `GetAllComponentNames` reads committed data, so a
removed component still appears in the list within the same tick, and the
component count does not change. Verified: removing
`esv::replication::ReplicationDependencyComponent` from a status entity returned
true, left the list at 10 components, and did not error. The removal is
nonetheless real — removing `ls::VisualComponent` from the host freed the visual
and crashed the render thread.

Consequence for tests: neither the return value nor `GetAllComponentNames` can
confirm a removal took effect. Only an observable side effect can.

### `HasRawComponent` false negative (separate pre-existing bug)

`HasRawComponent("ls::VisualComponent")` returned false for the host character,
which demonstrably *had* it: removing it freed the visual and the render thread
segfaulted in `ls::CascadedShadowBufferStage::Submit` -> Metal `setResource`.
The debug trace shows the cause:

    ComponentTypeToIndex lookup: type=1998 ... initial_idx=-1
    Type 1998 not found in this storage class
    Failed: Component type not in this storage class

`component_lookup_by_index` resolves through the entity's committed storage
class and reports absence when the type is not in that class, while the game's
own `GetCommittedComponent` finds it. HasRawComponent therefore cannot be used
as a safety predicate for destructive operations. Root-causing the storage-class
resolution is tracked separately; it is not introduced by RemoveComponent.

### Testing rule learned

Never choose a removal target by scanning for a component the entity appears to
lack, and never test removal against the player character. Confine mutation to a
spawned throwaway item and remove only a component created for the test.

## BUG — `entity:CreateComponent` aborts the process on most component types

Found 2026-08-20. Root-caused and fixed.

macOS ships 2647 `ecs::ComponentOps<T>::AddImmediateDefaultComponent`
specializations. Only **725 (27.4%)** are real. The other **1922 (72.6%)** are
compiled out to an identical three-instruction body:

    stp  x29, x30, [sp, #-0x10]!    ; 0xa9bf7bfd
    mov  x29, sp                    ; 0x910003fd
    bl   <__TEXT,__stubs>           ; -> __DATA,__la_symbol_ptr 0x10888CC78

and that lazy-bind slot resolves to `libc++/__ZSt9terminatev` — `std::terminate`.
Entering such a slot kills the process immediately.

`lua_entity_create_component` validated the build gate, the registry index, the
ComponentOps entry, the vtable pointer and the slot pointer — all of which pass
for a stubbed type, because the pointer is perfectly valid — and then branched
into it. So CreateComponent was roughly a 3-in-4 chance of killing the game for
any name that cleared the registry check. Reproduced with
`esv::status::DifficultyModifiersComponent`.

### A C++ try/catch does NOT fix this — retraction

An intermediate commit added a C++ shim so the call site would have an exception
handler. That was wrong and has been removed. The stub *calls* `std::terminate`
directly; there is no exception in flight, nothing to unwind, and nothing to
catch. Verified in game: with the shim in place the process still aborted, now
with `bg3se_add_immediate_default_component_guarded` in the crash frame above
`lua_entity_create_component`.

### Fix

Recognise the stub before branching. The three-instruction shape is checked at
the slot address and the call is refused with a clear message when it matches,
so the ~27% of component types that have real implementations still work while
the rest fail closed instead of terminating the process.


## Entity lifecycle unlocked — EntityCommandBuffer reached (2026-08-20)

`Ext.Entity.Create`/`Destroy` were deferred as "engine entity lifecycle
(allocator + handle mint) not recovered". Both are now implemented, because the
command buffer turned out to be directly reachable.

Symbols exist for the entry points:

    0x10636764c  ecs::EntityCommandBuffer::CreateEntity()
    0x10636769c  ecs::EntityCommandBuffer::DestroyEntity(ls::ID<EntityHandleTraits>)
    0x1063676ec  ecs::EntityCommandBuffer::Flush(ecs::ECBExecutor&)

`CreateEntity` disassembles to a short, unambiguous body:

    x0 = [this]                    ; +0x00 EntityHandleGenerator*
    bl EntityHandleGenerator::Create -> handle in x0
    x0 = this+0x10, x2 = [this+0x8]  ; +0x10 change map, +0x08 FrameAllocator*
    bl PagedHashMap::EnsureUniversal  -> ECBEntityChange*
    [x0+0x28] |= 1                   ; bit0 = Create
    ret handle

`DestroyEntity` is the same shape, sets bit1, returns void.

The missing piece was how to obtain the buffer. Scanning `__text` for BL
instructions targeting either function found exactly four call sites
(0x1036837a4, 0x1037812b4, 0x104a6455c, 0x105202fd4), and every one uses an
identical sequence:

    ldr    x20, [x27]              ; EntityWorld*
    bl     ls::ThreadRegistry::RequestThreadIndex()   ; 0x1065401c0
    ldr    x8,  [x20, #0x230]      ; per-thread EntityCommandBuffer array
    mov    w9,  #0xc0              ; stride 192
    smaddl x0,  w0, w9, x8         ; ecb = array + threadIndex * 0xC0
    bl     CreateEntity / DestroyEntity

So:

    EntityWorld + 0x230  -> EntityCommandBuffer[]   (per thread, stride 0xC0)

Because the buffer is per-thread, it must be resolved on the submitting thread;
a cached pointer would post commands into another thread's frame storage. The
port resolves it per call and shape-checks +0x00 and +0x08 before use.

Both operations are deferred and land at the next flush, which matches Windows,
where entity lifecycle goes through `EntityWorld->Deferred()`.

Note this also identifies the container Windows' *deferred* `RemoveComponent`
uses. `ecs::EntityCommandBuffer` exposes only CreateEntity, DestroyEntity,
`AccessAddStorage<T>` and `Flush` as symbols, so the deferred remove path is
inlined or templated and is not yet callable; the RemoveComponent timing
divergence therefore still stands.

## Entity lifecycle verified in-game (2026-08-20)

`Ext.Entity.Create` / `Destroy` confirmed on the vanilla Tav fixture.

Observing the result required care, because two existing APIs cannot see entity
liveness at all:

- `Ext.Entity.GetByHandle` resolves any well-formed handle. It returned an
  entity for a freshly created handle *before* any flush, so it proves nothing.
- `entity_is_alive` (`entity:IsAlive`) is a stub — its body is
  `// TODO: Check entity storage for validity` followed by `return true`. It
  reports true for any valid-looking handle, including one that was never
  created and one that was destroyed. **This is a separate pre-existing defect:
  `IsAlive` is currently meaningless.**

A real signal comes from the indexed component pool. Giving the new entity a
component and querying `GetAllEntitiesWithComponent`:

    CreateComponent("eoc::TagComponent") -> true
    after create + flush : entity present, pool = 20668
    after destroy + flush: entity absent,  pool = 20667

Both operations are therefore genuinely applied by the engine, and the pool
count moves by exactly one in each direction.

This also exercises the working side of the CreateComponent stub fix:
`eoc::TagComponent` is one of the 725 types with a real
AddImmediateDefaultComponent, and it succeeds, while
`esv::status::DifficultyModifiersComponent` (a std::terminate stub) now returns
false instead of killing the process.

## `entity:IsAlive` was a stub — now a real liveness query (2026-08-20)

`entity_is_alive` returned true for any well-formed handle:

    if (!entity_is_valid(handle) || !g_EntityWorld) return false;
    // TODO: Check entity storage for validity
    return true;

so `IsAlive` was wrong in *both* directions — true for a handle that was never
created, and still true after the engine destroyed the entity. This is worse
than a missing API, because a mod can reasonably use IsAlive as a guard before
touching an entity. It is also what made the first two attempts at verifying
Ext.Entity.Destroy meaningless.

It now routes through `component_lookup_get_storage_data`, which calls the
game's own `EntityStorageContainer::TryGet`, and additionally confirms the
handle has an entry in that storage class via InstanceToPageMap. Only the Lua
binding calls it, so there is no internal regression surface.

Caveat: an entity holding no components at all may belong to no storage class
and is reported not-alive. That is the conservative direction for a guard.

Note `Ext.Entity.GetByHandle` still resolves any well-formed handle and does not
imply the entity exists; it was returning proxies for freshly created handles
before any flush. Its semantics are left alone here.

## Terminate-stub audit of the RemoveComponent table

Since 1922 of 2647 AddImmediateDefaultComponent slots are compiled-out
`std::terminate` stubs, the 666 `ImmediateWorldCache::RemoveComponent<T>`
addresses the port dispatches through were audited the same way:

    666 entries -> 666 real bodies, 0 terminate stubs, 0 outside __text

So RemoveComponent is not exposed to this failure mode on 4.1.1.7398727. The
check was still generalized to `game_fn_is_terminate_stub()` and applied on that
path, because a future build could compile any specialization out and three word
reads is cheap next to branching into std::terminate.

### IsAlive verified in-game (2026-08-20)

    host character (long-lived) : true    (unchanged, no regression)
    handle never created        : false   (previously true)
    created + flushed           : true
    destroyed + flushed         : false   (previously true)

Both previously-wrong directions are now correct, and this is the probe that
should have been used to verify Ext.Entity.Destroy in the first place.
