# Passives::Parse discrimination recon (4.1.1.7209685)

**Date:** 2026-08-03  
**Scope:** Wave 7 B2, evidence only; no game launch and no source changes.

## Verdict

**PARTIAL-GO for a future, build-gated implementation; NO-GO for changing the
current `sync_passive_prototype()` result. It must remain honestly `false`.**

The exact subset supported by static evidence is:

1. **Existing-entry scalar/description refresh: GO at the static-analysis
   level.** The loader reads the `StatsObject` directly, updates every scalar,
   `FixedString`, context mask, condition index, and `StatsDescription`, and
   does not touch any `StatsFunctorList` vptr.
2. **Existing-entry functor-list refresh: GO at the static-analysis level.**
   All three already-constructed nested lists are cleared and repopulated by
   virtual `Destroy`/`Insert` calls. Their vptrs are read for dispatch, never
   overwritten.
3. **`Boosts` refresh: mechanically feasible but not binding-ready.** The
   destination is a non-polymorphic `DynamicArray<Guid>` at prototype `+0x1f8`.
   The native parse chain and callback are identified, but the callable route
   requires synthesizing an LTO-specialized `ls::Function` closure and its
   predicate/capture storage. There is no standalone `ParseStaticBoosts`
   entry point.
4. **New-entry container construction/insertion: statically mapped, but not an
   end-to-end GO.** Allocation, construction, key ownership, bucket linking,
   and manager ownership are proved. A new entry is not semantically complete
   until the full refresh, including `Boosts`, is implementation- and
   runtime-verified with rollback behavior.

Consequently, a partial scalar/functor update must not be reported as a
successful `Ext.Stats.Sync`. `Passives::Parse` does not close the remaining
gap.

## Method and binary identity

The Ghidra HTTP bridge was tried first and was unavailable:

```text
curl: (7) Failed to connect to 127.0.0.1 port 8080
```

The fallback was static ARM64 disassembly of the installed universal binary:

```text
/Users/tomdimino/Library/Application Support/Steam/steamapps/common/
  Baldurs Gate 3/Baldur's Gate 3.app/Contents/MacOS/Baldur's Gate 3
```

`otool -f` re-derived the relevant fat member:

```text
architecture 1
    cputype 16777228
    offset 257261568
    size 243843216
```

`257261568 == 0xf558000`, so raw reads use:

```text
file_offset = 0xf558000 + (vaddr - 0x100000000)
```

Local-symbol inventory confirms:

| Symbol | Address |
|---|---:|
| `eoc::Passives::Get(ls::FixedString const&) const` | `0x101c0f27c` |
| `eoc::Passives::Parse(eoc::Passives const&, ls::STDString const&)` | `0x101c0f37c` |
| `eoc::PassivePrototype::PassivePrototype()` | `0x101c0d6c8` |
| `eoc::PassivePrototype::Clean()` | `0x101c0d964` |
| `eoc::PassivePrototype::~PassivePrototype()` | `0x101c0dc0c` |
| `eoc::Passives::~Passives()` | `0x101c0edc0` / `0x101c0ef20` |
| `vtable for eoc::Passives` | `0x1086ad020` |
| `eoc::Passives::m_ptr` | `0x1089bc228` |
| `DoLoadStats()::$_6` call body | `0x103061fbc` |

The required prior evidence was read first:
`src/stats/prototype_managers.c` and
`ghidra/offsets/COMPONENT_OPS_AND_PROTO_INIT.md`.

## 1. What `Passives::Parse` actually does

### Observed internal ABI

Like `Passives::Get`, this local function has an optimizer-specialized ABI.
The body and all callers agree on:

```c
// Observed build-7209685 machine ABI, not a portable declaration.
void Passives_Parse(
    DynamicArray<PassivePrototype const *> *result, // x0
    Passives const *passives,                       // x1
    STDString const *text);                         // x2
```

The non-trivial result buffer is passed in `x0`, not the normal standalone
AAPCS64 `x8` convention. Do not bind from the mangled signature alone.

Entry and input normalization:

```asm
0x101c0f398  stp   xzr, xzr, [x0]       ; zero result Buffer/Capacity/Size
0x101c0f39c  ldrb  w8, [x2, #0xf]       ; STDString short/long discriminator
0x101c0f3a4  ldr   x10, [x2]            ; long-string pointer
0x101c0f3a8  ldr   w11, [x2, #0x8]      ; long-string length
0x101c0f3c0  mov   x20, x1              ; Passives*
0x101c0f3c4  mov   x19, x0              ; output DynamicArray*
```

It trims the string, splits on `';'`, then splits/advances over whitespace:

```asm
0x101c0f464  adrp  x11, ...
0x101c0f468  add   x11, x11, #0xdda     ; "';'"
0x101c0f4b0  bl    0x100c2fe68          ; ls::Split<2>(...)
0x101c0f588  bl    0x100c2fe68          ; next split
```

For each token it interns a `FixedString`, looks up an already-existing map
entry, and appends the prototype pointer to the result:

```asm
0x101c0f5fc  add   x0, sp, #0x8
0x101c0f600  add   x1, sp, #0x50
0x101c0f604  bl    0x1064a8ae4          ; FixedString::Create(StringView const&)
0x101c0f608  ldr   w0, [x20, #0xc]      ; Passives bucket capacity
0x101c0f61c  ldr   x1, [x20, #0x10]     ; bucket array
0x101c0f624  ldr   x8, [x8]              ; chained node walk
0x101c0f62c  ldr   w9, [x8, #0x8]       ; node FixedString key
0x101c0f638  bl    0x101c0f2bc          ; RefMap operator[], returns node+0x10
0x101c0f648  ldp   w9, w8, [x19, #0x8]  ; result Capacity/Size
0x101c0f670  bl    0x10117f888          ; result DynamicArray::Reallocate
0x101c0f67c  str   w9, [x19, #0xc]      ; Size++
0x101c0f684  str   x22, [x9, w8, sxtw #3]
```

It also requires `Passives::Initialized`:

```asm
0x101c0f450  ldrb  w11, [x20, #0x18]
0x101c0f454  cmp   w11, #0
0x101c0f45c  b.eq  0x101c0f3fc          ; empty result when uninitialized
```

### Xref discrimination

A slice-aware scan found **17 direct `BL` callers**:

```text
0x10117e308  eoc::BackgroundManager::PostInit
0x1011c94e8  character_creation::_private::ApplyBasics
0x1011f6eb0  character_creation::_private::TryApplyTemplate
0x101b531a0  eoc::FeatManager::PostInit
0x101b53588  eoc::FeatManager::PostInit
0x101bfcdd0  eoc::OriginManager::PostInit
0x101c27120  eoc::ProgressionManager::PostInit
0x101c27500  eoc::ProgressionManager::PostInit
0x101ff88d0  eoc::StatusPrototype::GetPassivePrototypes
0x1028d3870  gui::VMTooltipItem::AddPassives lambda
0x1055ed590, 0x1055ed7b4, 0x1055eda48,
0x1055ee6c4, 0x1055ee77c, 0x1055eea4c, 0x1055eeca0
               esv::PassiveSystem::Update
```

Every inspected caller supplies a stack `DynamicArray` in `x0`, a `Passives*`
in `x1`, and an `STDString*` in `x2`. Representative caller:

```asm
; eoc::OriginManager::PostInit
0x101bfcdc0  adrp  x8, 0x1089bc000
0x101bfcdc4  ldr   x1, [x8, #0x228]    ; *Passives::m_ptr
0x101bfcdc8  add   x2, x24, #0xd8      ; serialized passive-name string
0x101bfcdcc  add   x0, sp, #0x20       ; output DynamicArray
0x101bfcdd0  bl    0x101c0f37c
```

There is no call from the `DoLoadStats` population lambda.

### Answer to question 1

**No: `Parse` does not consume loader-internal parser state, temporary
allocators, a `StatsObject`, or a stats-loader context.** Its material is easy
to synthesize: an initialized `Passives`, an `STDString`, and result storage.

That does not make it a sync helper. It is a consumer-side resolver for
semicolon-separated passive names and can only return pointers already in the
map. It cannot construct an entry, call `Clean`, update one field, parse
prototype `Boosts`, or populate functor lists. Binding it for sync is therefore
an explicit **NO-GO**.

## 2. Existing-entry refresh versus new-entry insertion

### Both paths share one population tail

The loader uses `x26 = StatsObject*`, `x23 = Passives*`, `x28 = map node`, and
`x24 = node + 0x10 = PassivePrototype*` at the common tail.

Existing-entry hit:

```asm
0x103062b6c  ldr   w10, [x23, #0xc]    ; capacity
0x103062b80  ldr   x11, [x23, #0x10]   ; buckets
0x103062b88  ldr   x12, [x12]          ; node / next
0x103062b90  ldr   w13, [x12, #0x8]    ; key
0x103062bc4  b     0x103062cf8         ; hit -> common tail
```

New-entry path:

```asm
0x103062c18  mov   w0, #0x220
0x103062c1c  bl    _malloc
0x103062c2c  str   xzr, [x0], #0x10    ; next = NULL; x0 = payload
0x103062c38  str   w8, [x21, #0x8]!    ; key = FixedString::Null
0x103062c3c  bl    0x101c0d6c8        ; PassivePrototype ctor(payload)
0x103062c98  ldr   w21, [x26, #0x20]   ; StatsObject::Name
0x103062ca8  bl    0x1064aa8f0        ; gst::Acquire(key)
0x103062cd4  str   w21, [x28, #0x8]
0x103062ce4  str   x10, [x28]          ; node->next = old bucket head
0x103062ce8  str   x28, [x8, x9]       ; bucket head = node
0x103062cf4  str   w8, [x23, #0x8]     ; manager Count++
```

Common refresh:

```asm
0x103062cf8  add   x24, x28, #0x10
0x103062cfc  mov   x0, x24
0x103062d00  bl    0x101c0d964        ; PassivePrototype::Clean()
```

Thus existing and new entries do not have different field-population logic.
Insertion is the extra prefix; all semantic population is shared.

### Recovered build-7209685 payload layout

Offsets below are relative to `PassivePrototype*` (node `+0x10`):

| Offset | Field / role | Loader evidence |
|---:|---|---|
| `0x000` | `Properties` (`uint32`) | `0x103062f24`-`0x103063d4c`, flag reconstruction |
| `0x004` | `Name` (`FixedString`) | `0x103062d04`-`0x103062d4c` |
| `0x008` | `StatsDescription` | `0x103062d50`-`0x103062d58` |
| `0x090` | `EnabledConditions` / condition index | `0x103063160`-`0x103063194` |
| `0x098` | `EnabledContext` (`uint64`) | `0x103062fcc`-`0x103063074` |
| `0x0a0` | `ToggleOnEffect` (`FixedString`) | `0x103062d5c`-`0x103062e28` |
| `0x0a4` | `ToggleOffEffect` (`FixedString`) | `0x103062e2c`-`0x103062ef8` |
| `0x0a8` | `StatsFunctorContext` (`uint64`) | `0x103063080`-`0x103063128` |
| `0x0b0` | `ConditionsIndex` | `0x103063190`-`0x1030631f0` |
| `0x0b8` | `StatsFunctors` (`StatsFunctorList`, `0x60`) | `0x10306332c`-`0x103063430` |
| `0x118` | `ToggleOnFunctors` (`StatsFunctorList`, `0x60`) | `0x1030634f8`-`0x1030635bc` |
| `0x178` | `ToggleOffFunctors` (`StatsFunctorList`, `0x60`) | `0x103063684`-`0x103063748` |
| `0x1d8` | `ToggleGroup` (`FixedString`) | `0x1030639dc`-`0x103063aa4` |
| `0x1e0` | `ToggleOffContext` (`uint64`) | `0x103063924`-`0x1030639d0` |
| `0x1e8` | `BoostContext` (`uint64`) | `0x103063748`-`0x1030637f4` |
| `0x1f0` | `BoostConditionsIndex` | `0x1030631f4`-`0x10306325c` |
| `0x1f8` | `DynamicArray<Guid> Boosts` | `0x103063800`-`0x103063920`; callback append at `0x10118f99c`-`0x10118f9e0` |
| `0x208` | `PriorityOrder` (`int32`) | store at node `+0x218`, `0x103063b20` |
| `0x20c` | `TooltipConditionalDamage` (`FixedString`) | node `+0x21c`, tail before `0x103062b38` |

`sizeof(PassivePrototype) == 0x210`; each of the three functor lists is `0x60`
bytes apart. The static attribute keys used by this loader region corroborate
the field names:

```text
0x1089bc030  strEnabledConditions
0x1089bc038  strEnabledContext
0x1089bc040  strToggleOnFunctors
0x1089bc048  strToggleOffFunctors
0x1089bc050  strToggleOnEffect
0x1089bc058  strToggleOffEffect
0x1089bc060  strToggleGroup
0x1089bc068  strToggleOffContext
0x1089bc070  strPriorityOrder
0x1089bc078  strTooltipConditionalDamage
0x1089bc080  EoCFS::strBoostConditions
0x1089bc084  EoCFS::strBoostContext
0x1089c36e8  eoc::strStatsFunctorContext
0x1089c4ed0  EoCFS::strConditions
0x1089c4ed4  EoCFS::strStatsFunctors
```

### Scalar/description refresh

Scalar population uses only `StatsObject*`, the global `RPGStats` attribute
metadata/value tables, fixed-string acquire/release, and one callable helper:

```asm
0x103062d50  add   x0, x28, #0x18     ; prototype +0x08
0x103062d54  mov   x1, x26            ; StatsObject*
0x103062d58  bl    0x101f8209c        ; StatsDescription::SetFromStatsObject
```

The repeated attribute path is visible throughout the tail:

```asm
ldr  w8, [x26, #0xe4]       ; object modifier-list index
ldr  x9, [RPGStats, #0x68]  ; modifier-list array
ldr  x21, [x9, x8, lsl #3]
blr  [modifier-list vptr +0x28/0x20] ; find/resolve attribute
ldr  w8, [x26 + value-index]
ldr  x9, [RPGStats, #0x348 or #0x358]
```

No loader closure field or temporary parser object participates. Numeric and
mask fields are direct stores. `FixedString` fields are also refreshable, but
must mirror the shown `gst::Acquire`/`gst::Map::Release` ownership sequence;
blind `uint32_t` replacement is not safe.

**Nested-vptr answer:** scalar/description refresh does not write any of the
three functor-list vptrs.

### Functor-list refresh

The constructor installs `StatsFunctorList` address points at prototype
`+0xb8`, `+0x118`, and `+0x178`. The loader preserves them and operates through
them. First-list excerpt (the next two are structurally identical):

```asm
0x10306332c  mov   x25, x28
0x103063330  ldr   x8, [x25, #0xc8]! ; node+0xc8 = prototype+0xb8
0x103063334  ldr   x8, [x8, #0x10]
0x103063338  mov   x0, x25
0x10306333c  blr   x8                 ; destination Destroy()
0x103063340  ldr   x8, [x24]
0x103063344  ldr   x8, [x8, #0x38]
0x10306334c  blr   x8                 ; source GetAmountOfEntries()
0x10306335c  ldr   x8, [x24]
0x103063360  ldr   x8, [x8, #0x48]
0x10306336c  blr   x8                 ; source GetEntry(index) const
0x103063370  ldr   x8, [x0]
0x103063374  ldr   x8, [x8, #0x18]
0x103063378  blr   x8                 ; clone source functor
0x103063380  ldr   x8, [x25]
0x103063384  ldr   x8, [x8, #0x18]
0x10306338c  blr   x8                 ; destination Insert(clone)
```

Raw vtable data identifies the destination slots from the list address point:

```text
vtable symbol                  0x10880a580
address point                  0x10880a590
[vptr + 0x10] 0x101b799d4     CNamedElementManager<StatsFunctorBase>::Destroy
[vptr + 0x18] 0x101b79ad4     CNamedElementManager<StatsFunctorBase>::Insert
[vptr + 0x38] 0x101b79e04     GetAmountOfEntries
[vptr + 0x48] 0x101b79e2c     GetEntry(unsigned long) const
```

`PassivePrototype::Clean()` independently clears owned functors and resets the
three list sizes, while preserving the list objects/vptrs. For example:

```asm
0x101c0d97c  ldr   w8, [x0, #0xcc]    ; first list count
0x101c0d99c  ldr   x8, [x19, #0xc0]   ; first list entries
0x101c0d9b0  blr   x8                 ; destroy owned functor
0x101c0d9b8  str   wzr, [x19, #0xcc]
```

Equivalent blocks handle the lists at `+0x118` and `+0x178`.

**Nested-vptr answer:** functor refresh reads the nested vptrs and calls their
methods; it never writes a vptr. A status-style template-vptr copy at prototype
`+0` remains corrupting and forbidden.

### `Boosts` refresh

The stat's `Boosts` value is a `FixedString` containing serialized boost text,
not an already-populated prototype array. The loader resolves it to a
`StringView`, clears the destination size, constructs an `ls::Function`
callback on the stack, and calls the generic parser:

```asm
0x10306382c  ... EoCFS::strBoosts
0x10306388c  bl    0x1064a9ddc        ; gst::Get
0x1030638a0  add   x8, x28, #0x208    ; node+0x208 = prototype+0x1f8
0x1030638b0  str   wzr, [x28, #0x214] ; DynamicArray Size = 0
0x1030638dc  ... callback Call = 0x10118f5e4
0x1030638e8  sub   x2, x29, #0xb0     ; ls::Function closure
0x1030638ec  bl    0x10118faac        ; (anonymous namespace)::ParseBoosts
```

The callback symbols are:

| Role | Address |
|---|---:|
| `ParseStaticBoosts::$_1::Call` | `0x10118f5e4` |
| `ParseStaticBoosts::$_1::Copy` | `0x10118fa68` |
| `ParseStaticBoosts::$_1::MoveDestroy` | `0x10118fa88` |
| `(anonymous namespace)::ParseBoosts` | `0x10118faac` |

The callback invokes
`BoostPrototypeManager::CreateBoostPrototype` at `0x101190ff0` and appends
the returned 16-byte `Guid` to the destination:

```asm
0x10118f99c  ldr   x19, [x19, #0x20]  ; captured DynamicArray<Guid>*
0x10118f9a0  ldp   w9, w8, [x19, #0x8]; Capacity/Size
0x10118f9c8  bl    0x100c018ac        ; DynamicArray<Guid>::Reallocate
0x10118f9d4  str   w9, [x19, #0xc]    ; Size++
0x10118f9e0  stp   x20, x21, [x8]     ; append Guid
```

This corrects the older generic assumption that the build-7209685 field is an
`Array<void*>`: the native callback is explicitly specialized for
`DynamicArray<Guid>`.

**Nested-vptr answer:** `Boosts` population writes only the non-polymorphic
array at `+0x1f8`; it does not touch a functor-list vptr. The remaining blocker
is callable closure/capture ABI and lifecycle proof, not object layout.

## 3. Exact path from `Passives::m_ptr` to an entry

`Passives` itself is polymorphic (unlike `PassivePrototype`). Its destructor
installs the address point `0x1086ad030` and proves that the embedded legacy
map begins at manager `+0x8`:

```asm
0x101c0ede4  adrp  x8, 0x1086ad000
0x101c0ede8  add   x8, x8, #0x30      ; Passives vtable +0x10
0x101c0edec  str   x8, [x0]
0x101c0edf0  strb  wzr, [x0, #0x18]   ; Initialized = false
0x101c0edf4  ldr   w8, [x0, #0x8]!    ; x0 = embedded RefMap at +0x8
```

Recovered layout:

```c
struct PassiveNode7209685 {
    PassiveNode7209685 *Next;       // +0x000
    uint32_t Key;                   // +0x008, ls::FixedString pool index
    uint32_t Padding;               // +0x00c
    PassivePrototype7209685 Value;  // +0x010, sizeof 0x210
};                                  // sizeof/allocation 0x220

struct Passives7209685 {
    void **Vptr;                    // +0x00, address point 0x1086ad030
    uint32_t Count;                 // +0x08
    uint32_t Capacity;              // +0x0c, number of buckets
    PassiveNode7209685 **Buckets;   // +0x10
    bool Initialized;              // +0x18
    uint8_t Padding[7];
};
```

Traversal recipe, as far as static analysis proves:

```text
runtime(0x1089bc228)
  -> load one pointer: eoc::Passives*
  -> Capacity = *(uint32_t *)(passives + 0x0c)
  -> Buckets  = *(Node ***)(passives + 0x10)
  -> bucket   = fixed_string_index % Capacity
  -> node     = Buckets[bucket]
  -> compare *(uint32_t *)(node + 0x08)
  -> miss: node = *(Node **)(node + 0x00)
  -> hit:  PassivePrototype* = node + 0x10
```

The key hash is the 32-bit `FixedString` index itself for this instantiation.
This is the same body-level ABI warning already established for
`Passives::Get`: its `w1` key is passed by value despite the mangled
`FixedString const&` signature.

Manager destruction confirms ownership of externally linked nodes: it walks
every bucket, calls `PassivePrototype::~PassivePrototype(node+0x10)`, releases
the key at node `+0x8`, and frees the `0x220` node (`0x101c0ee54`-
`0x101c0eeac`).

## Windows contract comparison

The read-only Windows reference uses the behavior expected by sync:

- `BG3Extender/GameDefinitions/Stats/Stats.cpp`, lines 107-134:
  `PassivePrototypeManager::SyncStat` clears properties/contexts and
  `Boosts_SV`, invokes mapped `PassivePrototype::Init(proto, stats::Object*)`,
  and uses `get_or_insert` for a miss.
- `BG3Extender/GameDefinitions/Stats/Prototype.h`, lines 179-212: declares the
  scalar, condition, functor, boost, priority, and tooltip fields and the
  manager's legacy map.

The macOS loader performs the same conceptual operation, but the population
body is inlined and its current-build representation includes three `0x60`
`StatsFunctorList` subobjects plus `DynamicArray<Guid>` boosts. There is no
callable macOS `PassivePrototype::Init` symbol.

## Answers to the wave questions

1. **Does `Parse` require unsynthesizable loader material?** No. It consumes
   only an initialized `Passives` and serialized name string, but it is the
   wrong operation: lookup/list resolution only. It cannot be used for sync.
2. **What can refresh without `Parse`?** All scalar/description fields and all
   three functor lists are statically implementable from the `StatsObject`
   without writing nested vptrs. `Boosts` also lives outside those vptrs and
   its parser chain is known, but its LTO closure ABI is not yet safe to bind.
   Existing entries and new entries share the same refresh tail.
3. **What is the manager-to-entry path?** One load from BSS `m_ptr`, then the
   embedded map at manager `+0x8`; capacity `+0xc`, buckets `+0x10`, chained
   node `next@0`, `FixedString key@+0x8`, inline `PassivePrototype@+0x10`;
   node allocation `0x220`, payload `0x210`.

## Next milestone

1. Keep `sync_passive_prototype()` unchanged and false in this evidence-only
   wave.
2. Finish the `ParseStaticBoosts` closure-storage/predicate ABI or locate a
   narrower callable wrapper. If neither can be proved, stop: full passive
   sync remains a no-go even though scalar/functor refresh is understood.
3. In a separate implementation milestone, build an exact-version-gated
   existing-entry refresher first. It must call `Clean`, mirror FixedString
   ownership, call `StatsDescription::SetFromStatsObject`, clone the three
   functor lists through their native virtual methods, and refresh `Boosts`.
4. In a separate live-validation milestone, snapshot vptrs at `+0xb8`,
   `+0x118`, and `+0x178`; mutate one scalar, `Boosts`, and each functor-list
   property independently; prove all three vptrs are unchanged and ownership
   survives reload/unload.
5. Only after existing-entry refresh is complete should new insertion be
   attempted. Require `Initialized`, nonzero capacity/buckets, duplicate-key
   rejection, failure rollback, node destruction, and load/unload validation
   before returning success.
