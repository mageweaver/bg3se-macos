# Build-specific ABI review: 4.1.1.7398727

**Date:** 2026-08-04  
**Scope:** Wave 2C static review only; no production gate constants changed  
**Baseline:** `4.1.1.7209685`, arm64 UUID
`9A647311-E263-3FF2-AF98-111CEDCB3034`, fat slice `0xf558000`  
**Candidate:** `4.1.1.7398727`, arm64 UUID
`0C51CAED-6D60-3DCD-9299-8519C92631B0`, fat slice `0xf5c0000`

## Method and verdict rules

This review follows the last paragraph of Phase 3 and the Wave 2C ownership
row in `docs/plans/2026-08-04-001-feat-offset-remigration-7398727-plan.md`.
Each function was resolved independently in both frozen images by its exact
local `nm -arch arm64` symbol before inspection with `otool`/LLVM disassembly.
Preferred VAs use image base `0x100000000`. Raw bytes were mapped through each
Mach-O segment rather than through a presumed build delta.

`PASS` below means the requested static ABI and layout are unchanged. It is
permission for the lead to consume this static evidence, not permission to
bypass the plan's later live prerequisites. In particular, the RaycastAny UUID
promotion still awaits the Phase 6 stress ladder and the savegame release gate
still awaits the E1.1 two-direction breadcrumb experiment.

The companion `tests/harness/test_abi_review_7398727.py` resolves the same
7398727 local symbols and checks these instruction words directly against the
frozen image. It skips when that image is absent.

## 1. Functor execution — PASS

Evidence anchors: `src/stats/functor_types.h`,
`src/stats/functor_hooks.c`, `src/core/offset_table.c`,
`ghidra/offsets/FUNCTORS.md`, and the corrected hidden-result analysis in
`docs/bugs/wave2-functor-crash-analysis.md`.

The 7398727 offset row contains ten `GAME_FN_EXECUTE_*` entries (dispatcher
plus nine list overloads) and `GAME_FN_PROCESS_DEAL_DAMAGE_FUNCTORS`, eleven
reviewed targets in total. Every exact demangled symbol is unchanged. Every
first 16-byte entry window is also byte-identical between builds:

| Function | 7209685 VA | 7398727 VA | First 16 bytes in both builds |
|---|---:|---:|---|
| `ExecuteStatsFunctor` | `0x10577399c` | `0x10577e650` | `e9 23 b9 6d fc 6f 01 a9 fa 67 02 a9 f8 5f 03 a9` |
| `ExecuteStatsFunctors(AttackTarget)` | `0x10577787c` | `0x105782530` | `fc 6f ba a9 fa 67 01 a9 f8 5f 02 a9 f6 57 03 a9` |
| `ExecuteStatsFunctors(AttackPosition)` | `0x105777bd0` | `0x105782884` | `ed 33 b7 6d eb 2b 01 6d e9 23 02 6d fc 6f 03 a9` |
| `ExecuteStatsFunctors(Move)` | `0x1057796c0` | `0x105784374` | `eb 2b b8 6d e9 23 01 6d fc 6f 02 a9 fa 67 03 a9` |
| `ExecuteStatsFunctors(Target)` | `0x10577a87c` | `0x105785530` | `eb 2b b8 6d e9 23 01 6d fc 6f 02 a9 fa 67 03 a9` |
| `ExecuteStatsFunctors(NearbyAttacked)` | `0x10577e43c` | `0x1057890f0` | `eb 2b b8 6d e9 23 01 6d fc 6f 02 a9 fa 67 03 a9` |
| `ExecuteStatsFunctors(NearbyAttacking)` | `0x10577fb0c` | `0x10578a7c0` | `eb 2b b8 6d e9 23 01 6d fc 6f 02 a9 fa 67 03 a9` |
| `ExecuteStatsFunctors(Equip)` | `0x10578098c` | `0x10578b640` | `fc 6f ba a9 fa 67 01 a9 f8 5f 02 a9 f6 57 03 a9` |
| `ExecuteStatsFunctors(Source)` | `0x1057829f4` | `0x10578d6a8` | `eb 2b b8 6d e9 23 01 6d fc 6f 02 a9 fa 67 03 a9` |
| `ExecuteStatsFunctors(Interrupt)` | `0x105786548` | `0x1057911fc` | `ef 3b b6 6d ed 33 01 6d eb 2b 02 6d e9 23 03 6d` |
| `ProcessDealDamageFunctors` | `0x10537e8b4` | `0x105389568` | `fc 6f ba a9 fa 67 01 a9 f8 5f 02 a9 f6 57 03 a9` |

### Hidden result and wrapper register contract

There is **no hidden `x8` indirect-result pointer at these functor
boundaries**. The returned `esv::functor::Result` is a non-trivial C++ return
lowered as an invisible leading argument in `x0`. This is the special case
that the earlier generic “large struct uses x8” rule does not describe.

The ordinary Target call proves the same contract in both images:

```asm
; 7209685, esv::Status::OnStatusEvent, call at 0x104de6b6c
104de6b60  add x0, sp, #0x108       ; Result output
104de6b64  add x2, sp, #0x420       ; TargetContextData&
104de6b68  mov x1, x21              ; StatsFunctorList const*
104de6b6c  bl  0x10577a87c

; 7398727, same exact symbol, call at 0x104df1820
104df1814  add x0, sp, #0x108
104df1818  add x2, sp, #0x420
104df181c  mov x1, x21
104df1820  bl  0x105785530
```

The two Target entries consume those registers identically. The 7398727 entry
at `0x105785558..0x105785560` is `mov x20,x2; mov x21,x1; mov x19,x0`;
the 7209685 sequence is at `0x10577a8a4..0x10577a8ac`. The other seven
ordinary overloads likewise save `x2=context`, `x1=list`, and `x0=result` at
the same function-relative offsets. This matches every ordinary wrapper in
`functor_hooks.c`.

Interrupt remains the four-register variant:

```asm
; 7209685 call at 0x105375acc
105375abc  ldr x1, [x8]             ; EntityWorld&
105375ac0  add x0, sp, #0x240       ; Result output
105375ac4  add x3, sp, #0x7c0       ; InterruptContextData&
105375ac8  mov x2, x19              ; StatsFunctorList const*
105375acc  bl  0x105786548

; 7398727 call at 0x105380780
105380770  ldr x1, [x8]
105380774  add x0, sp, #0x240
105380778  add x3, sp, #0x7c0
10538077c  mov x2, x19
105380780  bl  0x1057911fc
```

At 7398727 `0x10579122c..0x105791238`, the entry saves `x3` to `x27`,
`x2` to `x28`, `x1` to the stack, and `x0` to `x25`; the 7209685 entry has
the same instructions at `0x105786578..0x105786584`.

The dispatcher itself also has a hidden `x0` result. Its old caller at
`0x1057779e4..0x1057779f4` and new caller at
`0x105782698..0x1057826a8` set `x0=result`, `x1=functor`, `x2=id`, and
`x3=context`. `ExecuteStatsFunctorProc` in `functor_types.h` currently omits
that hidden argument, but the dispatcher is not one of the ten installed
hooks. It must not be hooked through that typedef without a separate source
fix.

`ProcessDealDamageFunctors` remains void: `x0..x7` carry its first eight
arguments and arguments 8–11 are on the stack. The old entry at
`0x10537e8f0..0x10537e90c` and new entry at
`0x1053895a4..0x1053895c0` save `x7..x4`, `x0/x3`, and `x2`, then load the
stack arguments through `x29+0x10`, `+0x18`, and `+0x20`. No hidden result is
present.

**Verdict: PASS.** The installed wrappers' machine arguments and all entry
windows are unchanged. The lead may move `FUNCTOR_ADDRS_VERIFIED_BUILD` after
reviewing the unused dispatcher-typedef caveat.

## 2. ComponentOps registry — PASS

Evidence anchors: `ghidra/offsets/COMPONENT_OPS_AND_PROTO_INIT.md` and the
consumer in `src/entity/entity_system.c`.

The two independent registration functions retain `EntityWorld+0x390`:

```asm
; 7209685 ecl::RegisterComponents at 0x100cff910
100cff910  add x22, x19, #0x390
100cff91c  mov x0, x22
100cff920  mov x1, x23
100cff924  bl  DynamicArray<UniquePtr<IComponentOps>>::SetEnsure

; 7398727 exact symbol, corresponding site at 0x100cffd28
100cffd28  add x22, x19, #0x390
100cffd34  mov x0, x22
100cffd38  mov x1, x23
100cffd3c  bl  0x100c86004 ; same exact SetEnsure symbol
```

`ls::RegisterSharedComponents` repeats the same instructions at old
`0x105e74f50..0x105e74f64` and new `0x105e81184..0x105e81198`.

The native dispatch chain is instruction-identical:

```asm
; 7209685 AttachImmediateComponentDependencies, 0x10636b93c..0x10636b958
and x8, x23, #0x7fff
ldr x9, [x20, #0x390]
ldr x0, [x9, x8, lsl #3]
ldr x8, [x0]
ldr x8, [x8, #0x28]
mov x1, x22
mov x2, x21
blr x8

; 7398727 same exact symbol and words, 0x1063935ec..0x106393608
```

Thus `x0=ComponentOps*`, `x1=EntityHandle`, and `w2=retryCount`; `+0x28`
from the vptr remains address-point slot 5. Two concrete vtables independently
retain that slot:

| Type | Build | vtable symbol | Address-point slot 5 (`symbol+0x38`) |
|---|---|---:|---:|
| `eoc::HealthComponent` | 7209685 | `0x1086b1d68` | `0x101e88bd0` |
| `eoc::HealthComponent` | 7398727 | `0x1086e2558` | `0x101e862c4` |
| `ls::TransformComponent` | 7209685 | `0x108831df8` | `0x105e8e368` |
| `ls::TransformComponent` | 7398727 | `0x1088625c8` | `0x105e9a59c` |

The Health implementations confirm the same concrete ABI. Old
`0x101e88be0..0x101e88be8` and new `0x101e862d4..0x101e862dc` execute
`mov x19,x2; str x1,[sp,#8]; ldr x20,[x0,#0x20]`. Transform repeats the same
sequence.

**Verdict: PASS.** Registry base `+0x390`, pointer stride 8, vptr slot 5, and
the three-argument call ABI are unchanged. The lead may move
`COMPONENT_OPS_VERIFIED_BUILD` once the current-build TypeIds are integrated.

## 3. ECS system update — PASS

Evidence anchors: `ghidra/offsets/ECS_SYSTEM_UPDATE_RECON.md` and
`src/entity/ecs_system_update.c`.

The central executor resolves by the same exact symbol:

| Build | `ecs::core::SystemDependencyExecutor::ExecuteWTKernel()` |
|---|---:|
| 7209685 | `0x1063788cc` |
| 7398727 | `0x1063a057c` |

The dispatch instructions are identical:

```asm
; old 0x1063788f8..0x10637891c / new 0x1063a05a8..0x1063a05cc
ldr  x9, [x0, #0x20]       ; executor->SystemTypeEntry
cbz  x9, skip
ldr  x8, [x9, #0x18]       ; entry->UpdateProc
cbz  x8, skip
ldrb w10, [x9, #0x12]      ; Activated
cbz  w10, skip
ldr  x0, [x9]              ; System*
ldr  x1, [x19, #0x28]      ; EntityWorld*
add  x2, x1, #0x200        ; GameTime const*
blr  x8
```

Two concrete `SystemUpdate<T>` thunks preserve the same external register
contract. PickingHelperManager is old `0x101053a8c`, new `0x101050cf4`; the
new entry immediately saves `x2` and `x0` at `0x101050d0c..0x101050d10` and
uses `x1` as the world. AnimationBlueprintSystem is old `0x101ef85d4`, new
`0x101ef5cc8`; both save `x2` and `x1` at relative offsets `+0x28/+0x2c`.

The registry math is also unchanged. Exact
`DEPRECATED_Array<SystemTypeEntry>::SetAt` moves from `0x100c81208` to
`0x100c81890`; old `0x100c8126c..0x100c81278` and new
`0x100c818f4..0x100c81900` load the source and buffer, load constant `0xf8`,
and execute `madd x8,x1,x9,x8`.

`RegisterSystem<ls::LevelInstanceLoadRequestSystem>` moves from `0x100f77540`
to `0x100f76f38` and retains all prefix math:

```asm
; new 0x100f77064..0x100f77078 (old relative offsets identical)
str    x21, [sp, #0x30]     ; entry System +0x00
dup.2s v0, w19
str    d0, [sp, #0x38]      ; indices +0x08/+0x0c
str    x8, [sp, #0x48]      ; UpdateProc +0x18

; new 0x100f772c8..0x100f772f8
add x23, x20, #0x28         ; embedded system array
ldr w8, [x20, #0x38]        ; capacity
...
mov x0, x23
mov x1, x19
bl  SetAt                   ; array buffer is world+0x30; stride 0xf8
```

This re-proves `world+0x30` buffer, `+0x38` capacity, `+0x3c` used/high-water,
entry stride `0xf8`, entry system `+0x00`, indices `+0x08/+0x0c`, activated
`+0x12`, and `UpdateProc +0x18`.

**Verdict: PASS.** Layout and `UpdateProc(system, world, time)` ABI are
unchanged. The lead may move `ECS_SYSTEM_UPDATE_VERIFIED_BUILD` only together
with the 7398727 system-TypeId addresses; the current source table is still
7209685-specific even though the dispatch ABI passed.

## 4. RaycastAny worker and optional ABI — PASS

Evidence anchors: `ghidra/offsets/RAYCAST_ABI_B4A.md`,
`src/level/level_manager.c`, and the separately proved VMT slot-10 mapping in
`tests/harness/test_physics_vmt_audit.py`.

Exact local symbols:

| Function | 7209685 VA | 7398727 VA |
|---|---:|---:|
| `phx::PhysXScene::RaycastAny(...) const` | `0x105c4e8c8` | `0x105c598b4` |
| `phx::PhysXSceneHelpers::RaycastAny(...)` | `0x105c58adc` | `0x105c63ac8` |

The public wrapper is word-for-word identical apart from the relocated branch
destination:

```asm
; old 0x105c4e8c8 / new 0x105c598b4
ldr w8, [sp]
ldp x10, x9, [sp, #8]       ; 16-byte Optional
ldr x0, [x0, #0x98]         ; PxScene*
stp x10, x9, [sp, #8]
str w8, [sp]
b   PhysXSceneHelpers::RaycastAny
```

Both workers have the same first 16 bytes
`ff 83 03 d1 e9 23 0b 6d f4 4f 0c a9 fd 7b 0d a9`. They read the optional's
second qword and test its engagement byte at the same relative offsets:

```asm
; old 0x105c58b20 / new 0x105c63b0c
ldr x8, [x29, #0x20]
...
; old 0x105c58bfc / new 0x105c63be8
tst x8, #0xff
b.eq internal_lock_path
```

The zeroed 16-byte value therefore remains a disengaged optional. The
disengaged branch preserves the lock/raycast/unlock sequence:

```asm
; 7209685: 0x105c58c40..0x105c58c9c
; 7398727: 0x105c63c2c..0x105c63c88
ldr x8, [x19]
ldr x8, [x8, #0x310]        ; lockRead
mov x0, x19
mov x1, #0
mov w2, #0
blr x8
...
ldr x8, [x19]
ldr x8, [x8, #0x2b8]        ; raycast
...
blr x8
ldr x8, [x19]
ldr x8, [x8, #0x318]        ; unlockRead
mov x0, x19
blr x8
```

The wrapper mapping remains `x0=scene`, `x1=source`, `x2=destination`,
`w3=physics type`, `w4=include`, `w5=exclude`, `w6=context`, `w7=-1`, then
stack `w8=-1` and the optional at `SP+8`. The worker still returns boolean in
`w0`; no `x8` return storage exists.

**Verdict: PASS.** The worker accepts the same zeroed optional and retains the
balanced internal read lock. This does not waive the plan's Phase 6 live
stress ladder; keep the production UUID on 7209685 until that ladder passes.

## 5. Savegame hook surface — PASS (static ABI only)

Evidence anchors: `ghidra/offsets/SAVEGAME_HOOK_SURFACE.md` and
`src/game/savegame_hook.c`.

The exact local symbol
`esv::OsirisVariableHelper::SavegameVisit(eoc::SavegameVisitor*)` resolves to
`0x104b51a9c` on 7209685 and `0x104b5c750` on 7398727. The first 16 entry
bytes match exactly:

```text
ff 03 01 d1  f6 57 01 a9  f4 4f 02 a9  fd 7b 03 a9
```

The following ABI instructions are also unchanged:

```asm
; old 0x104b51ab0..0x104b51ac0 / new 0x104b5c764..0x104b5c774
mov x19, x1                  ; eoc::SavegameVisitor*
mov x20, x0                  ; OsirisVariableHelper*
ldr x0, [x1, #0xb0]         ; nested visitor
ldr x8, [x0]
ldr x8, [x8, #0x70]
```

The function returns `bool` in `w0` and has no hidden `x8` result. Its entry
still consists solely of relocatable stack/register operations, so the
explicit offset-zero hook requirement remains valid.

**Verdict: PASS for the static entry ABI.** This is not a release-gate
promotion recommendation by itself: Phase 6 explicitly requires the E1.1
breadcrumb to observe repeatable write and read calls on 7398727 first. The
lead must keep `SAVEGAME_HOOK_VERIFIED_BUILD` closed until that runtime proof.

## 6. Replication SyncBuffers chain — PASS

Evidence anchor: `ghidra/offsets/REPLICATION_SYNCBUFFERS.md`.

All accessors used to derive the chain retain their exact symbols and
function-relative structure math.

### EntityWorld root and pool

`EntityWorld::GetComponent<eoc::ArmorComponent>` moves from `0x102906344` to
`0x10290ceb4`. Old `0x102906380..0x102906390` and new
`0x10290cef0..0x10290cf00` execute:

```asm
ldr x2, [sp, #8]
ldr x3, [x19]               ; *(EntityWorld + 0x00) = SyncBuffers*
add x1, sp, #0x10
mov x0, x20
bl  SyncedData<Armor>::SyncedData(..., SyncBuffers*)
```

`EntitiesDirtyFieldsBuffer::MarkDirty` moves from `0x10633f5b8` to
`0x106367268`. The old site `0x10633f5e4..0x10633f5f8` and new site
`0x106367294..0x1063672a8` are identical:

```asm
mov   w8, #1
strb  w8, [x0, #0x10]       ; SyncBuffers.Dirty
ldr   x8, [x0]              ; pool buffer
sxtw  x9, w2
add   x20, x8, x9, lsl #6   ; pool stride 0x40
ldrsw x19, [x20, #0x2c]     ; HashMap key count
```

The authority consumer confirms the embedded view at authority `+0x10`: exact
`EntityReplicationAuthority::Sync` moves from `0x1063422e8` to `0x106369f98`.
Old `+0x1178` (`0x106343460`) and new `+0x1178` (`0x10636b110`) load the dirty
byte at authority `+0x20`. Old `+0x383c` (`0x106345b24`) and new `+0x383c`
(`0x10636d7d4`) clear that byte, load the pool buffer at authority `+0x10`,
and load pool size at authority `+0x1c`. Relative to SyncBuffers these are
`+0x10`, `+0x00`, and `+0x0c`; capacity remains the adjacent `+0x08` field.

### HashMap and DynamicBitSet

Exact `protocol::FlattenComponentsLayout` moves from `0x106375f6c` to
`0x10639dc1c`. The old proof at `0x10637631c..0x106376370` and new proof at
`0x10639dfcc..0x10639e020` retain:

```asm
ldrsw x9, [x13, #0x08]      ; bucket count
ldr   x10, [x13, #0x00]     ; bucket heads
ldr   x10, [x13, #0x20]     ; entity keys
ldr   x11, [x13, #0x10]     ; next indices
ldr   x8, [x13, #0x30]      ; DynamicBitSet values
add   x22, x8, x9, lsl #4   ; value stride 0x10
```

Exact `DynamicBitSet<TaggedAllocator<int>>::Ensure` moves from `0x106375c94`
to `0x10639d944`. Old `0x106375cb0..0x106375cd8` and new
`0x10639d960..0x10639d988` retain `ldr w8,[x0,#8]` for size, then
`ldr w8,[x20,#0xc]; cmp w8,#0x41; ldr x20,[x20]` for capacity and the heap
transition above 64 bits.

**Verdict: PASS.** `EntityWorld+0x00`, SyncBuffers buffer/capacity/size/dirty
`+0x00/+0x08/+0x0c/+0x10`, pool stride `0x40`, the HashMap fields, and
DynamicBitSet `+0x00/+0x08/+0x0c` are unchanged. This approves the carried
struct math only; the plan's live numeric-index, hash-chain, inline/heap, and
dirty-lifecycle checklist remains required for behavioral acceptance.

## Verdict summary

| Subsystem | Verdict | Gate/action for lead |
|---|---|---|
| Functor execution | **PASS** | ABI gate may move; do not activate the stale dispatcher typedef without adding its hidden `x0` result. |
| ComponentOps registry | **PASS** | Gate may move with 7398727 component TypeIds. |
| ECS system update | **PASS** | Gate may move only with 7398727 system-TypeId addresses. |
| RaycastAny worker/optional | **PASS** | Static worker ABI passed; UUID promotion still waits for the Phase 6 live ladder. |
| Savegame hook | **PASS (static ABI)** | Keep the release gate closed until the E1.1 write/read breadcrumb proof. |
| Replication SyncBuffers | **PASS** | Struct math may carry; behavioral gap stays open until the live checklist passes. |
