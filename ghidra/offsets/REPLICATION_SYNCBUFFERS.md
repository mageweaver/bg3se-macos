# W7-C Step 1 — Replication / SyncBuffers Chain Recon

**Date:** 2026-08-03 | **Build:** 4.1.1.7209685 | **Session:** Wave 7 C step-1 static recon (sol agent, read-only; findings transcribed from the session transcript — the agent honored its `never modify files` posture and did not write the doc)
**Prior evidence:** `ECS_SYSTEM_UPDATE_RECON.md` (address convention, EntityWorld methodology)

## Recovered chain

```text
EntityWorld + 0x00
  → SyncBuffers*
      +0x00 ComponentPools.buffer
      +0x08 ComponentPools.capacity  (int32)
      +0x0c ComponentPools.size      (int32)
      +0x10 Dirty                    (byte)
          → pool[index]              (stride 0x40)
              → HashMap<EntityHandle, DynamicBitSet>
                  → DynamicBitSet    (stride 0x10)
```

Every link above is **CONFIRMED** by ARM64 disassembly. The replicated-type
global addresses are confirmed, but their runtime `int32_t` values still
require a live probe.

## Windows ground truth

- Replication indices are distinct `uint16_t` identifiers; undefined is
  `0xffff` (`EntitySystem.h:47`).
- `SyncBuffers` = `Array<HashMap<EntityHandle, BitSet<>>> ComponentPools`
  followed by `bool Dirty` (`EntitySystem.h:388`).
- `EntityWorld::Replication` is the first declared member
  (`EntitySystem.h:783`).
- Read lookup validates the replication index, selects
  `ComponentPools[typeId]`, calls `try_get(entity)` (`EntitySystem.cpp:610`).
- Write lookup inserts a missing entity with `add_key`; dirty notification sets
  `Replication->Dirty = true` (`EntitySystem.cpp:634`).
- Lua reads return zero for absent flags or out-of-range qwords
  (`LuaEntityProxy.inl:295`); writes are server-only, grow the bitset, OR
  flags, and dirty only when a bit changed; `Replicate()` = qword-zero/all-bits
  (`LuaEntityProxy.inl:308`).
- The upstream API is registered on the **entity proxy**, not `Ext.Entity`
  (`LuaEntityProxy.inl:415`). Upstream test expects
  `0 → 7 → 0xffffffffffffffff` (`ECSTests.lua:24`).

## ARM64 excerpts

`EntityWorld + 0x00 → SyncBuffers*`:

```asm
; EntityWorld::GetComponent<Armor>, 0x102906344
102906384  ldr x3, [x19]       ; *(EntityWorld + 0x00)
102906390  bl  SyncedData<Armor>::SyncedData(..., SyncBuffers*)
; EntityWorld::EntityWorld, 0x106362640
10636266c  mov x20, x0
106362670  str xzr, [x0]       ; initialize Replication = nullptr
```

Pool selection + dirty write:

```asm
; EntitiesDirtyFieldsBuffer::MarkDirty, 0x10633f5b8
10633f5e8  strb w8, [x0, #0x10]    ; Dirty = true
10633f5ec  ldr  x8, [x0]           ; ComponentPools.buffer
10633f5f4  add  x20, x8, x9, lsl #6 ; index * 0x40
10633f5f8  ldrsw x19, [x20, #0x2c] ; map key count
```

Read-only hash lookup:

```asm
; protocol::FlattenComponentsLayout, 0x10637631c
106376320  ldrsw x9, [x13, #0x8]     ; bucket count
10637633c  ldr   x10, [x13]          ; bucket heads
10637634c  ldr   x10, [x13, #0x20]   ; entity keys
106376350  ldr   x11, [x13, #0x10]   ; next indices
10637636c  ldr   x8, [x13, #0x30]    ; values
106376370  add   x22, x8, x9, lsl #4 ; value[index], stride 0x10
```

Bitset layout:

```asm
; DynamicBitSet::Ensure, 0x106375c94
106375cb0  ldr w8, [x0, #0x8]    ; size in bits
106375ccc  ldr w8, [x20, #0xc]   ; capacity in bits
106375cd0  cmp w8, #0x41
106375cd8  ldr x20, [x20]        ; heap buffer when capacity >= 65
```

Dirty consumption + purge:

```asm
; EntityReplicationAuthority::Sync, 0x106343460
106343460  ldrb w9, [x27, #0x20]  ; authority+0x10 → SyncBuffers+0x10 Dirty
106343464  cbz  w9, ...
; 0x106345b24
106345b24  strb wzr, [x24, #0x20] ; clear Dirty
106345b28  ldr  x20, [x24, #0x10] ; ComponentPools.buffer
106345b2c  ldrsw x8, [x24, #0x1c] ; ComponentPools.size
106345ba4  bl HashTable::Purge
```

## Recovered layouts

| Structure | Offset | Meaning | Confidence |
|---|---:|---|---|
| `EntityWorld` | `+0x00` | `SyncBuffers*` | CONFIRMED |
| `SyncBuffers` | `+0x00` | pool buffer | CONFIRMED |
|  | `+0x08` | pool capacity (int32) | CONFIRMED |
|  | `+0x0c` | pool size (int32) | CONFIRMED |
|  | `+0x10` | dirty byte | CONFIRMED |
| `HashMap` | `+0x00` | bucket-head buffer | CONFIRMED |
|  | `+0x08` | bucket count | CONFIRMED |
|  | `+0x10` | next-index buffer | CONFIRMED |
|  | `+0x18/+0x1c` | next capacity/size | CONFIRMED |
|  | `+0x20` | entity-key buffer | CONFIRMED |
|  | `+0x28/+0x2c` | key capacity/size | CONFIRMED |
|  | `+0x30` | `DynamicBitSet` values | CONFIRMED |
|  | `+0x38` | value count | CONFIRMED |
| `DynamicBitSet` | `+0x00` | inline qword or heap pointer | CONFIRMED |
|  | `+0x08` | size in bits | CONFIRMED |
|  | `+0x0c` | capacity in bits | CONFIRMED |

**ABI difference from Windows:** the reference `BitSet` stores capacity at
`+0x08` and size at `+0x0c`; macOS `DynamicBitSet` reverses those fields
(`CoreLib/Base/BitSet.h:5`). macOS uses
`ls::DynamicBitSet<TaggedAllocator<int>>` and
`ls::HashMap<..., RPLHashTableOps>`.

**PROBABLE (not confirmed):** `EntityReplicationAuthority + 0x10` is the
embedded `SyncBuffers`. Every observed authority access is shifted by exactly
`0x10` and the destructor clears `*(world + 0x00)`, but the constructor
assignment itself was not recovered.

## Confirmed replicated-type globals (runtime int32 indices, not literals)

| Component | VA (7209685, ORIGINAL) | VA (7398727, CURRENT) |
|---|---:|---:|
| God | `0x108902a88` | `0x1089329b8` |
| GameObjectVisual | `0x108905c30` | `0x108935b60` |
| AvailableLevel | `0x108911680` | `0x1089415b0` |
| DisplayName | `0x108914de0` | `0x108944d10` |
| ActionResources | `0x10891a990` | `0x10894a8c0` |
| Stats | `0x10891ac90` | `0x10894abc0` |
| Classes | `0x10891aca0` | `0x10894abd0` |
| EocLevel | `0x10891acc0` | `0x10894abf0` |
| CombatParticipant | `0x10891c7d0` | `0x10894c700` |

**The left column is for build 7209685 and is stale.** Every row shifted by
exactly `+0x2FF30` for 7398727. `src/entity/generated_typeids.h` already
carries the correct 7398727 values (they were migrated with the rest of the
offset table); only this document lagged.

**These indices do not need Ghidra.** They are exported symbols and can be
regenerated for any build directly from the shipped binary:

    nm -gU "<BG3 binary>" \
      | grep '4sync21ReplicatedTypeContext' \
      | grep '11m_TypeIndexE$' | grep -v '__ZGV'

That yields exactly **582** symbols on 7398727 — matching the observed
SyncBuffers pool size of 582, i.e. one pool slot per replicated type. Beware
the `__ZGV` guard-variable symbols, which sit 8 bytes after the real
`m_TypeIndex` and are easy to grab by mistake.

These are separate from ordinary component TypeIds (Wave 7 plan line 70).

## Step-2 recommendation: read-only `GetReplicationFlags`

Implement `entity:GetReplicationFlags(component, qword?)` as a manual,
read-only traversal (NO `MarkDirty`/`Ensure`/`Resize`/insertion):

1. Select the captured server or client `EntityWorld`.
2. Load `sync = *(world + 0x00)`; return `0` if null.
3. Resolve the component's replicated-type `int32_t` from the build-gated
   global.
4. Validate `0 <= index < *(int32_t *)(sync + 0x0c)`.
5. `pool = *(sync + 0x00) + index * 0x40`.
6. Traverse bucket heads / keys / next indices with bounded node/count
   validation.
7. `bitset = values + node * 0x10`.
8. Return zero when `qword >= (size + 63) / 64`.
9. Read inline storage at `bitset + 0x00` when capacity <= 64; else read
   through its heap pointer.

**Placement correction:** the port currently installs `GetReplicationFlags` as
an `Ext.Entity` warn-and-nil stub (`src/injector/main.c:1113`) while Windows
registers it on the **entity proxy**; `Replicate` is an entity-proxy no-op
(`src/entity/entity_system.c:1862`). Step 2 should move the reader onto the
entity proxy to match the upstream surface.

## Live-probe checklist (next in-game session)

- Dump server + client `world + 0x00`; verify nullability and pointer validity.
- Dump sync `+0x00/+0x08/+0x0c/+0x10`; require `size <= capacity`.
- Test the PROBABLE ownership relation: `sync - 0x10` should hold the current
  world at `+0x08`.
- Read each replicated-type global after applying the ASLR slide; require a
  nonnegative value below pool size.
- Resolve a known entity (e.g. Lae'zel) in the DisplayName pool; verify the
  hash-chain key.
- Dump the selected bitset's size, capacity, inline value or heap words.
- Compare qword-zero against `ent:GetReplicationFlags("DisplayName")`.
- Exercise an engine-native component change; observe dirty `0 → 1`.
- Observe the subsequent authority sync clearing Dirty and purging the pool.
- Exercise a bitset larger than 64 bits to validate heap mode + qword bounds.
- Confirm client-side write attempts stay fail-closed and never touch server
  memory.

## W7-C Step 2 — LIVE PROBE RESULT (2026-08-20)

First execution of the live-probe checklist. Build 4.1.1.7398727, ASLR slide
`0xa20000` (base `0x100a20000`), vanilla Tav session, server world only
(`GetClientWorld()` returned nil at this point in the session).

### Confirmed live

| Check | Result |
|---|---|
| `world + 0x00` → `SyncBuffers*` | **PASS** — world `0xb7b01a300` → sync `0xb8e5ae3b8`, pointer valid |
| pool `+0x00/+0x08/+0x0c/+0x10` | **PASS** — buf `0xb4a140000`, capacity 582, size 582, dirty 0, `size <= capacity` |
| ownership relation | **PASS, upgrades PROBABLE → CONFIRMED** — `*(sync - 0x8)` equals the current world pointer exactly |
| `DisplayName` pool is real | **PASS** — index 39, buckets `0xb3d818700`/389, values `0xb69190000`/256 |

The `EntityReplicationAuthority + 0x10` embedding can therefore be treated as
CONFIRMED: reading back one qword from the sync pointer recovers the owning
world.

### RETRACTED: "replicated-type globals are partly wrong"

An earlier revision of this section claimed 4 of 9 globals were out of range and
that `God`/`AvailableLevel` collided on index 257. **That was an artifact of the
probe, not a defect.** The probe read the *stale 7209685 VAs from this document*
rather than the current values already present in
`src/entity/generated_typeids.h`. Re-running against the correct 7398727
addresses gives a clean result:

| Component | index | pool key capacity |
|---|---:|---:|
| ActionResources | 10 | 128 |
| Classes | 47 | 256 |
| DisplayName | 69 | 1024 |
| GameObjectVisual | 82 | 0 |
| EocLevel | 108 | 16 |
| Stats | 157 | 256 |
| CombatParticipant | 260 | 16 |
| AvailableLevel | 286 | 16 |
| God | 297 | 1 |

**9/9 in range (pool size 582), zero duplicates.** The shipped table is correct
for this build; no re-derivation is required.

### CORRECTION: the pools are empty, and `GetReplicationFlags` is right

The first pass of this probe misread the pool header, reporting `DisplayName`
as "256 values but 0 keys" and blaming the key-size offset. That was wrong. A
raw dump of the full `0x40` header shows four consecutive `Array<T>` records of
`{ptr, capacity@+0x08, size@+0x0c}`:

| Array | ptr | capacity | size |
|---|---|---:|---:|
| buckets `+0x00` | `0xb3d818700` | 389 | **0** |
| nextIndex `+0x10` | `0xb93035c00` | 256 | **0** |
| keys `+0x20` | `0xb94e79000` | 256 | **0** |
| values `+0x30` | `0xb69190000` | 256 | **0** |

The `256` first read as a value *count* is the **capacity** at `+0x38`; the
size at `+0x3c` is 0. Every array is allocated but empty. The `+0x28/+0x2c`
key capacity/size offsets recorded in the layout table above are correct as
written — there is no discrepancy to chase.

Sweeping all 582 pools: **87 allocated, 0 non-empty.** The whole replication
table is empty.

`entity:GetReplicationFlags(...)` returning `0` is therefore **correct
behaviour, not a fail-closed miss** — there are no flags to read. The reader
may be entirely correct; single-player never populates these buffers, exactly
as the deferral registry predicted ("masked in single-player, unproven in
multiplayer").

**Consequence for validation: replication cannot be exercised in single-player
at all.** Confirming the read path, the dirty `0 -> 1` transition, the
authority sync clearing it, and >64-bit heap bitsets each require a
**multiplayer session with a second connected client**. No amount of
single-player probing advances this deferral beyond the chain checks above.

### Verdict

**The deferral stays closed and earns no parity credit — but nothing in the
implementation is known to be wrong.** Corrected findings:

- The `world -> sync -> pool` chain is CONFIRMED live.
- `*(sync - 0x8)` equals the owning world, upgrading the
  `EntityReplicationAuthority + 0x10` embedding from PROBABLE to CONFIRMED.
- The replicated-type index table in `generated_typeids.h` is **correct** for
  7398727: 9/9 in range, no duplicates, sensible pool capacities.
- The recovered `Array<T>{ptr, capacity@+0x08, size@+0x0c}` layout is correct.
- `entity:GetReplicationFlags(...)` returning `0` is **correct behaviour**.

The single blocker is environmental: **all 582 pools are empty in
single-player** (capacities allocated, sizes 0). Nothing exercises the read
path, the dirty bit, or the bitset modes, so none of it can be validated here.

**Remaining work requires a multiplayer session with a second connected
client.** Then run the outstanding checklist items: resolve a known entity in a
populated pool, compare qword-zero against `ent:GetReplicationFlags`, exercise
a component change and watch dirty `0 -> 1`, observe the authority sync
clearing it, exercise a >64-bit bitset for heap mode, and confirm client-side
writes stay fail-closed.

There is no longer any known single-player work outstanding on this deferral.
