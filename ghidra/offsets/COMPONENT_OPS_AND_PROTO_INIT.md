# ComponentOps Registry and Prototype Initialization (4.1.1.7209685)

## Scope and confidence

This is the salvage report for Wave 3 RE Agent MAX-C. It records only
instruction-level findings established before the interrupted analysis.

- Game build: `4.1.1.7209685`
- Ghidra/image base: `0x100000000`
- Installed universal binary ARM64 slice: **`0xf558000`**, verified with
  `lipo -detailed_info`
- Correct raw-read mapping for the installed file:
  `file_offset = 0xf558000 + (vaddr - 0x100000000)`

The originally supplied `0xf534000` slice offset is stale for the installed
file (it is `0x24000` too low). `nm -arch arm64` and `otool -arch arm64` were
slice-aware and unaffected; raw Python reads were repeated with `0xf558000`.

Confidence labels:

- **VERIFIED**: supported by at least two independent code/data sites, or by
  a named symbol plus matching instruction evidence.
- **PROVISIONAL**: supported by one site only.
- **OPEN**: not proved before the interrupted pass.

---

# Dig 1: ComponentOps registry and component mutation

## Evidence so far

### EntityWorld contains the ComponentOps array at `+0x390` — VERIFIED

The registry is not a pointer-sized field at the earlier `+0x368` estimate.
It is an embedded
`ls::DynamicArray<ls::UniquePtr<ecs::IComponentOps>>` beginning at
`EntityWorld+0x390`.

Independent registration site 1,
`ecl::RegisterComponents(ecs::EntityWorld&)`, around `0x100cff910`:

```asm
0x100cff90c  stp  xzr, x24, [sp, #0x10]  ; new IComponentOps owner
0x100cff910  add  x22, x19, #0x390       ; x19 = EntityWorld*
0x100cff91c  mov  x0, x22                ; DynamicArray this
0x100cff920  mov  x1, x23                ; masked component type index
0x100cff924  bl   DynamicArray<UniquePtr<IComponentOps>>::SetEnsure(...)
```

Independent registration site 2,
`ls::RegisterSharedComponents(ecs::EntityWorld&)`, around `0x105e74f50`:

```asm
0x105e74f4c  stp  xzr, x24, [sp, #0x10]
0x105e74f50  add  x22, x19, #0x390       ; x19 = EntityWorld*
0x105e74f5c  mov  x0, x22
0x105e74f60  mov  x1, x23
0x105e74f64  bl   DynamicArray<UniquePtr<IComponentOps>>::SetEnsure(...)
```

The `EntityWorld` constructor/unwind path independently identifies the type:

```asm
0x1063644b4  add  x0, x8, #0x390
0x1063644bc  bl   DynamicArray<UniquePtr<IComponentOps>>::~DynamicArray()
```

The `SetEnsure` implementation at `0x100c8597c` proves the embedded array
layout:

```c
struct ComponentOpsRegistry7209685 {
    ecs::IComponentOps **Buffer; // EntityWorld + 0x390
    int32_t Capacity;            // EntityWorld + 0x398
    int32_t Size;                // EntityWorld + 0x39c
};
```

Evidence:

```asm
0x100c8599c  ldr  w8, [x0, #0xc]  ; Size
0x100c859a8  ldr  x8, [x20]       ; Buffer
0x100c859dc  ldr  w8, [x20, #0x8] ; Capacity
0x100c85a84  str  w22, [x20, #0xc]
```

### Native registry lookup and add dispatch — VERIFIED

`ecs::EntityWorld::AttachImmediateComponentDependencies` at
`0x10636b85c` performs the exact lookup needed by
`Entity:CreateComponent` at two branches (`0x10636b93c` and
`0x10636b9b4`):

```asm
0x10636b93c  and  x8, x23, #0x7fff
0x10636b940  ldr  x9, [x20, #0x390] ; registry Buffer
0x10636b944  ldr  x0, [x9, x8, lsl #3] ; ComponentOps*
0x10636b948  ldr  x8, [x0]          ; vptr
0x10636b94c  ldr  x8, [x8, #0x28]   ; address-point slot 5
0x10636b950  mov  x1, x22           ; EntityHandle (uint64)
0x10636b954  mov  x2, x21           ; retryCount
0x10636b958  blr  x8
```

The second branch repeats the same sequence at
`0x10636b9b4`–`0x10636b9d0`.

Required caller checks:

1. `idx = componentId & 0x7fff`
2. `idx < *(int32_t *)(world + 0x39c)`
3. `Buffer != NULL`
4. `Buffer[idx] != NULL`
5. invoke address-point slot 5 with `x0=ops`, `x1=entityHandle`,
   `w2=retryCount` (normally zero)

### ARM64/Itanium ComponentOps vtable layout — VERIFIED

The Itanium ABI has two destructor entries. Consequently, Windows virtual
slot 4 (`AddImmediateDefaultComponent`) becomes macOS address-point slot 5,
at byte offset `+0x28` from the object vptr.

Concrete vtable 1:

- `vtable for ecs::ComponentOps<eoc::HealthComponent>`:
  `0x1086b1d68`
- Raw file offset with the verified slice: `0x17c09d68`
- Itanium address point: vtable symbol `+0x10`

```text
symbol+0x10 / vptr+0x00  0x101e88624  ~ComponentOps() (D1)
symbol+0x18 / vptr+0x08  0x101e88628  ~ComponentOps() (D0)
symbol+0x20 / vptr+0x10  0x101e886b0  SendComponentAttachedSignal
symbol+0x28 / vptr+0x18  0x101e88798  SendComponentDetachedSignal
symbol+0x30 / vptr+0x20  0x101e88880  DefaultConstructComponents
symbol+0x38 / vptr+0x28  0x101e88bd0  AddImmediateDefaultComponent
```

Concrete vtable 2:

- `vtable for ecs::ComponentOps<ls::TransformComponent>`:
  `0x108831df8`
- Raw file offset with the verified slice: `0x17d89df8`

```text
symbol+0x10 / vptr+0x00  0x105e8dda0  ~ComponentOps() (D1)
symbol+0x18 / vptr+0x08  0x105e8dda4  ~ComponentOps() (D0)
symbol+0x20 / vptr+0x10  0x105e8de2c  SendComponentAttachedSignal
symbol+0x28 / vptr+0x18  0x105e8df14  SendComponentDetachedSignal
symbol+0x30 / vptr+0x20  0x105e8dffc  DefaultConstructComponents
symbol+0x38 / vptr+0x28  0x105e8e368  AddImmediateDefaultComponent
```

The concrete Health implementation independently proves the call ABI:

```asm
0x101e88be0  mov  x19, x2          ; retryCount
0x101e88be4  str  x1, [sp, #0x8]  ; EntityHandle
0x101e88be8  ldr  x20, [x0, #0x20]; ComponentOps->EntityWorld
0x101e88bec  ldr  x0, [x20, #0x3f0]
```

Registered concrete `ComponentOps<T>` objects are allocated as `0x30` bytes.
Both registration sites initialize:

```c
struct IComponentOps7209685 {
    void **VMT;              // +0x00
    void *field_08;          // +0x08, OPEN name
    void *field_10;          // +0x10, OPEN name
    void *field_18;          // +0x18, OPEN name
    ecs::EntityWorld *World; // +0x20
    uint16_t TypeId;         // +0x28 (masked with 0x7fff)
    // size 0x30
};
```

### RemoveComponent symbol inventory and ABI — VERIFIED

The macOS binary does **not** contain a non-template
`ImmediateWorldCache::RemoveComponent(EntityHandle, ComponentId)` symbol.
Full local-symbol inventory found:

- 734 `ecs::legacy::ImmediateWorldCache::RemoveComponent<T>(EntityHandle)`
  instantiations
- zero non-template `ImmediateWorldCache::RemoveComponent(...)` entries

Representative exact symbols:

| Instantiation | Address |
|---|---:|
| `RemoveComponent<eoc::ActiveComponent>(EntityHandle)` | `0x1012e521c` |
| `RemoveComponent<eoc::HealthComponent>(EntityHandle)` | `0x101a1aafc` |
| `RemoveComponent<ls::TransformComponent>(EntityHandle)` | `0x102e22ed0` |

Per-instantiation AAPCS64 ABI:

```c
// Native return type is void, not bool.
void RemoveComponent_T(
    ecs::legacy::ImmediateWorldCache *cache, // x0
    uint64_t entityHandle                   // x1
);
```

Health proof:

```asm
0x101a1aafc  sub  sp, sp, #0x90
0x101a1ab1c  mov  x20, x0
0x101a1ab20  str  x1, [sp, #0x28]
0x101a1abdc  adrp x8, TypeId<HealthComponent>::m_TypeIndex@PAGE
0x101a1abe0  ldr  x8, [x8, #0x4c8]
0x101a1abe4  ldr  w10, [x8]
0x101a1abe8  and  w25, w10, #0x7fff
```

The template hard-codes its component type through
`TypeId<T>::m_TypeIndex`; therefore one specialization cannot safely remove
an arbitrary runtime-selected type.

Useful non-template cache symbols:

| Symbol | Address | ABI |
|---|---:|---|
| `ImmediateWorldCache::Flush(ECBExecutor&)` | `0x10636c668` | `x0=cache`, `x1=executor` |
| `ImmediateWorldCache::Reset()` | `0x10636d714` | `x0=cache` |
| `ImmediateWorldCache::GetChange(EntityHandle, ComponentId) const` | `0x10636d86c` | `x0=cache`, `x1=entity`, `w2=componentId`; `x0=change/null` |

The two independently inspected removal templates (Active and Health) agree
on these cache fields:

```c
struct ImmediateWorldCache7209685 {
    // component-type availability bitset begins at +0x00
    // ...
    ComponentChanges *ComponentsByType; // effective base +0x110,
                                         // stride 0x80 per type
    // ...
    void *Callbacks;                    // +0x240
    void *Allocator;                    // +0x248
    ecs::EntityWorld *EntityWorld;      // +0x250
};
```

The removal templates:

1. inspect `cache->EntityWorld` at `+0x250`;
2. use the hard-coded `TypeId<T>`;
3. create/find per-type change storage at `cache+0x110 + type*0x80`;
4. obtain the live/pending component;
5. invoke destroy callbacks through `cache+0x240`;
6. insert a null/default `ImmediateComponentChange` to represent removal.

No single native address with an `x2/w2` runtime type argument was found.

## Verified offsets and ABI recipes

Suggested `offset_manifest.json` fields:

| Suggested field | Value | Confidence |
|---|---:|---|
| `entity_world_component_ops_registry` | `0x390` | VERIFIED |
| `component_ops_registry_buffer` | `0x0` | VERIFIED |
| `component_ops_registry_capacity` | `0x8` | VERIFIED |
| `component_ops_registry_size` | `0xc` | VERIFIED |
| `component_ops_vptr_add_immediate_slot` | `5` | VERIFIED |
| `component_ops_vptr_add_immediate_byte_offset` | `0x28` | VERIFIED |
| `component_ops_entity_world` | `0x20` | VERIFIED |
| `component_ops_type_id` | `0x28` | VERIFIED |
| `entity_world_immediate_cache` | `0x3f0` | VERIFIED (prior work and current callers) |
| `immediate_cache_components_by_type` | `0x110` | VERIFIED |
| `immediate_cache_component_change_stride` | `0x80` | VERIFIED |
| `immediate_cache_callbacks` | `0x240` | VERIFIED |
| `immediate_cache_allocator` | `0x248` | VERIFIED |
| `immediate_cache_entity_world` | `0x250` | VERIFIED |
| `fn_immediate_cache_get_change` | `0x10636d86c` | VERIFIED |

Function recipe for create:

```c
typedef void (*AddImmediateDefaultComponentFn)(
    void *componentOps, uint64_t entityHandle, int retryCount);

registry = (char *)entityWorld + 0x390;
idx = componentTypeId & 0x7fff;
if (registry->Buffer && idx < registry->Size) {
    ops = registry->Buffer[idx];
    if (ops) {
        fn = *(AddImmediateDefaultComponentFn *)
            (*(uintptr_t *)ops + 0x28);
        fn(ops, entityHandle, 0);
    }
}
```

## Feasibility verdicts

### `Entity:CreateComponent` — UNLOCKED (offline evidence)

The registry offset, bounds layout, concrete vtable mapping, virtual slot,
and complete AAPCS64 argument shape are all verified across independent
sites. Implementation should still preserve existing game-version gating and
fail closed for missing/null registry entries.

### `Entity:RemoveComponent` — NOT GENERICALLY UNLOCKED

The cache and specialized ABI are mapped, but macOS emits only
`RemoveComponent<T>(EntityHandle)` instantiations. There is no verified native
`RemoveComponent(cache, entity, runtimeTypeId)` entry point to put in the
manifest. Unlock requires one of:

1. a generated type-index-to-specialization function table for all supported
   component types, version-gated per build; or
2. a carefully audited generic reimplementation of the common template body,
   including pending-change handling and destroy callbacks.

Calling a representative specialization with a different runtime type would
remove the specialization's hard-coded type and is unsafe.

---

# Dig 2: Passive/Interrupt prototype initialization

## Evidence so far

### PassivePrototype native symbols — VERIFIED

The complete local `nm` inventory established these exact class methods:

| Symbol | Address |
|---|---:|
| `eoc::PassivePrototype::PassivePrototype()` | `0x101c0d6c8` |
| `eoc::PassivePrototype::Clean()` | `0x101c0d964` |
| `eoc::PassivePrototype::~PassivePrototype()` | `0x101c0dc0c` |

No `eoc::PassivePrototype::Init(...)` symbol was present in either the
external or complete local symbol table inspected before interruption.
Therefore the current honest-false comment in
`src/stats/prototype_managers.c` is correct as to symbol presence, but it does
not describe the actual native construction path.

### Passive singleton and old `0x108aeccd8` claim — VERIFIED CORRECTION

`0x108aeccd8` has no `nm` symbol and was inherited from an older
`GetPassivePrototype` ADRP+LDR analysis. The old evidence used stale function
addresses. An ARM64-slice-aware ADRP scanner found **zero** references to
`0x108aeccd8` in build 7209685.

The actual native type is `eoc::Passives`, not a separately named
`PassivePrototypeManager`, and its singleton has an exact local BSS symbol:

```text
0x1089bc228 b __ZN3eoc8Passives5m_ptrE
              eoc::Passives::m_ptr
```

This is independently corroborated by many code references. Representative
ADRP+LDR pairs include:

```asm
; ecl::PassiveSystem update
0x10104b404  adrp  x?, 0x1089bc000
0x10104b408  ldr   x?, [x?, #0x228]

; ModuleLoadSystem::DoLoadStats passive population lambda
0x103062a08  adrp  x?, 0x1089bc000
0x103062a0c  ldr   x?, [x?, #0x228]
```

The scanner found 74 matching ADRP+LDR sites, including initialization,
shutdown, server/client systems, and stats consumers. This is substantially
stronger than the old single-site inference.

Verdict: **reject `0x108aeccd8`; use `eoc::Passives::m_ptr` at
`0x1089bc228` for build 7209685.**

Suggested field policy:

```text
passives_ptr: 0x1089bc228
```

If compatibility requires the old manifest field name,
`passive_prototype_manager_ptr` may alias this address, but the native type
should be documented as `eoc::Passives`.

### Passive construction/population path and size — VERIFIED

The actual equivalent of a per-object `Init` is inlined into the
`ecl::ModuleLoadSystem::DoLoadStats()` passive-loading lambda beginning at
`0x103061fbc`.

For a new entry:

```asm
0x103062c18  mov   w0, #0x220
0x103062c1c  bl    _malloc
0x103062c28  mov   x0, x28
0x103062c2c  str   xzr, [x0], #0x10  ; zero RefMap node link; x0 = node+0x10
0x103062c34  mov   w8, #-1
0x103062c38  str   w8, [x21, #0x8]!  ; node key
0x103062c3c  bl    eoc::PassivePrototype::PassivePrototype()
...
0x103062cdc  ldr   x8, [x23, #0x10]  ; bucket array
0x103062ce4  str   x10, [x28]        ; link old bucket head
0x103062ce8  str   x28, [x8, x9]     ; insert node
0x103062cf4  str   w8, [x23, #0x8]   ; increment map count
0x103062cf8  add   x24, x28, #0x10   ; prototype payload
0x103062d00  bl    eoc::PassivePrototype::Clean()
```

The node allocation is exactly `0x220`; its prototype payload begins at
node `+0x10`, so `sizeof(eoc::PassivePrototype) == 0x210`. This is
cross-checked by the constructor's final initialized scalar at object
`+0x20c` (`0x101c0d860`).

There is no top-level `PassivePrototype` vptr at object `+0`. The constructor
starts by writing scalar/aggregate fields there. It constructs nested
subobjects and explicitly installs the `eoc::StatsFunctorList` address-point
at object `+0xb8`:

```asm
0x101c0d720  adrp  x8, 0x10880a000
0x101c0d724  add   x8, x8, #0x580 ; vtable symbol
0x101c0d728  add   x8, x8, #0x10  ; Itanium address-point
0x101c0d730  str   x8, [x22, #0xb8]!
```

Therefore copying a template vptr into `PassivePrototype+0` (as the status
path does) would corrupt the passive object. Native construction must call
the constructor; subsequent field population is loader-specific and remains
to be mapped before sync can be enabled.

### InterruptPrototype and manager symbols — VERIFIED

| Symbol | Address |
|---|---:|
| `eoc::InterruptPrototype::~InterruptPrototype()` | `0x101b7a054` |
| `eoc::InterruptPrototypeManager::~InterruptPrototypeManager()` D1 | `0x101b7ab70` |
| `eoc::InterruptPrototypeManager::~InterruptPrototypeManager()` D2 | `0x101b7ab74` |
| `eoc::InterruptPrototypeManager::~InterruptPrototypeManager()` D0 | `0x101b7ac00` |
| `eoc::InterruptPrototypeManager::GetPrototype(FixedString const&) const` | `0x101b7adcc` |
| `eoc::InterruptPrototypeManager::m_ptr` | `0x1089ba8f0` |
| `vtable for eoc::InterruptPrototypeManager` | `0x1086aa9b8` |

`m_ptr` is a local BSS symbol in the installed binary, so
`0x1089ba8f0` supersedes the old `0x108aecce0` EvaluateInterrupt ADRP guess.

No `eoc::InterruptPrototype::Init(...)` or constructor symbol was established
in the class-method inventory before interruption. A stripped/inlined manager
population equivalent remains possible and was not yet ruled out.

### Interrupt manager lookup, storage, and object size — VERIFIED

`InterruptPrototypeManager::GetPrototype` supplies instruction-level ABI and
container evidence:

```asm
; x0 = manager, x1 = FixedString const*
0x101b7add8  ldr   w8, [x0, #0x10]       ; hash capacity
0x101b7ade8  ldr   w0, [x1]
0x101b7adec  bl    FixedString::GetHash()
0x101b7adfc  ldr   x9, [x19, #0x8]       ; hash buckets
0x101b7ae08  ldr   x9, [x19, #0x28]      ; key array
0x101b7ae1c  ldr   x11, [x19, #0x18]     ; collision links
...
0x101b7ae30  ldr   x9, [x19, #0x38]      ; contiguous prototype array
0x101b7ae34  mov   w10, #0x1f0
0x101b7ae38  madd  x0, x8, x10, x9       ; return &array[index]
```

Thus the ABI is `x0 = manager`, `x1 = FixedString const*`, return pointer in
`x0` or null, and `sizeof(eoc::InterruptPrototype) == 0x1f0`.

The size is independently confirmed by the inlined interrupt-loading/
reallocation path in the same `DoLoadStats` lambda:

```asm
0x103063f08  lsl   x9, x8, #9
0x103063f0c  sub   x23, x9, x8, lsl #4 ; count * (0x200 - 0x10)
0x103063f18  mov   x0, x23
0x103063f1c  bl    _malloc
...
0x103063f70  bl    eoc::InterruptPrototype::~InterruptPrototype()
0x103063f74  add   x21, x21, #0x1f0
```

The new-object move begins by copying a `FixedString` at object `+0`
(`0x103063f94`..`0x103063fa8`) and moving a `TranslatedString` at object
`+0x8` (`0x103063fac`..`0x103063fb4`). Therefore InterruptPrototype also has
no top-level vptr at `+0`; copying a template VMT there would corrupt its
first data field. Nested polymorphic members are initialized/moved by the
loader.

This also disproves the source helper's assumption that every prototype
manager is a `DEPRECATED_RefMap`: InterruptPrototypeManager uses a hash table
and a contiguous `0x1f0`-stride object array.

## Verified offsets and ABI recipes

Suggested `offset_manifest.json` fields that are safe now:

| Suggested field | Value | Confidence |
|---|---:|---|
| `interrupt_prototype_manager_ptr` | `0x1089ba8f0` | VERIFIED (`nm`, BSS) |
| `interrupt_prototype_manager_vtable` | `0x1086aa9b8` | VERIFIED (`nm`) |
| `fn_interrupt_prototype_manager_get` | `0x101b7adcc` | VERIFIED (`nm`) |
| `passives_ptr` | `0x1089bc228` | VERIFIED (`nm`, 74 ADRP+LDR sites) |
| `fn_passive_prototype_ctor` | `0x101c0d6c8` | VERIFIED (`nm`) |
| `fn_passive_prototype_clean` | `0x101c0d964` | VERIFIED (`nm`) |
| `fn_passive_prototype_dtor` | `0x101c0dc0c` | VERIFIED (`nm`) |
| `fn_interrupt_prototype_dtor` | `0x101b7a054` | VERIFIED (`nm`) |
| `passive_prototype_size` | `0x210` | VERIFIED (allocator + constructor extent) |
| `interrupt_prototype_size` | `0x1f0` | VERIFIED (lookup + loader allocation stride) |

AAPCS64 shapes proved only from the C++ symbol signatures so far:

```c
void PassivePrototype_ctor(PassivePrototype *self); // x0
void PassivePrototype_Clean(PassivePrototype *self); // x0
void PassivePrototype_dtor(PassivePrototype *self); // x0
void InterruptPrototype_dtor(InterruptPrototype *self); // x0

InterruptPrototype *InterruptPrototypeManager_GetPrototype(
    InterruptPrototypeManager const *self, // x0
    uint32_t const *fixedString            // x1, FixedString const&
);
```

The manager `GetPrototype` ABI and `x0` return are additionally confirmed by
the instruction sequence at `0x101b7adcc`..`0x101b7ae44`.

## Feasibility verdicts

### Passive sync — BLOCKED

The singleton, lookup container, constructor, cleanup call, allocation size,
and absence of a top-level VMT are now established. However, the native
per-stat field-population sequence after `Clean()` is inlined into a large
stats-loader lambda rather than exposed as `Init(FixedString const&)`.
Replaying only construction/insertion would create a semantically empty
prototype. Continue to fail closed until the post-`Clean` population region
is mapped or a narrower callable helper is found.

### Interrupt sync — BLOCKED

The singleton, lookup ABI, exact object size, manager storage form, and
absence of a top-level VMT are verified. However, there is no callable
constructor or `Init`, and the native loader builds/moves objects inline while
maintaining a hash table plus contiguous array. The existing generic RefMap
insert helper is structurally incompatible. Continue to fail closed until the
full inlined population/insertion sequence is mapped or a callable manager
helper is found.

---

# OPEN QUESTIONS

These are intentionally explicit so no unproved result is mistaken for a
manifest-ready recipe.

1. **Passive population equivalent:** map the field-copy/parse region after
   `0x103062d00` in the large `DoLoadStats` lambda and determine whether a
   narrower callable helper exists. Construction, size, insertion, singleton,
   and VMT policy are no longer open.
2. **Interrupt population equivalent:** map the remainder of the inlined
   object move/build path at `0x103063f94` onward and the hash-table update.
   Size, lookup ABI, storage family, and top-level VMT policy are no longer
   open.
3. **RemoveComponent generic path:** determine whether a stripped shared tail
   or callable helper exists below the 734 templates. No named generic entry
   exists; absent such a helper, removal needs a generated dispatcher or a
   separately audited generic implementation.
