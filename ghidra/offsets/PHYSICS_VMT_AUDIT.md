# PhysXScene VMT Audit — macOS ARM64

**Audit date:** 2026-07-30

**Scope:** Every `PHYSICS_VMT_*` constant in `src/level/level_manager.c`

**Investigation mode:** Offline/read-only; BG3 was not launched and `/tmp/bg3se.sock` was not touched

## Symptom

`src/level/level_manager.c:37-45` uses Windows-derived vtable indices for the
raycast and non-cylinder sweep methods. On the installed macOS ARM64 binary,
nine of those constants are one slot early. They dispatch to a different
virtual method, frequently with an incompatible signature.

The direct result is:

- `RaycastClosest` currently dispatches `RemovePhysicsShape`.
- `RaycastAll` currently dispatches `RaycastClosest`.
- `RaycastAny` currently dispatches `RaycastAll`.
- Each sphere/capsule/box closest or all sweep dispatches the preceding query
  method rather than the requested method.

The cylinder and shape-all indices added from direct macOS evidence are
correct. The `TestBox` and `TestSphere` indices also resolve to the intended
`PhysicsHitAll&` overloads, but only because a macOS overload-order difference
cancels the destructor shift numerically.

## Root Cause

The Itanium C++ ABI used by macOS emits two destructor entries for
`phx::PhysXScene`:

| VMT index | Target | Mangled symbol | Meaning |
|---:|---:|---|---|
| 0 | `0x105c4dd78` | `__ZN3phx10PhysXSceneD1Ev` | complete-object destructor |
| 1 | `0x105c4dd7c` | `__ZN3phx10PhysXSceneD0Ev` | deleting destructor |

The Windows declaration model accounts for one destructor slot. Consequently,
the ordinary query sequence from `RaycastClosest` through `SweepShapeAll` is
shifted by one on macOS:

```text
Windows declaration slot 7  RaycastClosest
macOS ARM64 slot          8  RaycastClosest
```

The overlap methods have a second difference. The Windows reference declares
the callback overload before the `PhysicsHitAll&` overload, while the macOS
vtable puts the `PhysicsHitAll&` overload first:

```text
macOS 20 TestBox(..., PhysicsHitAll&, ...)
macOS 21 TestBox(..., Function<bool(PhysicsHit)> const&, ...)
macOS 24 TestSphere(..., PhysicsHitAll&, ...)
macOS 25 TestSphere(..., Function<bool(PhysicsHit)> const&, ...)
```

That reversal offsets the extra destructor slot for the intended result
overloads, leaving `TestBox=20` and `TestSphere=24` numerically unchanged.

## Evidence

### Audited binary identity

```text
Bundle version: 4.1.1.7209685
File size:      501104784 bytes
ARM64 UUID:     9A647311-E263-3FF2-AF98-111CEDCB3034
SHA-256:        2cce194d02fa76cc07372c99ac60c27d10cb25ca45d2a9fde4cd3576660af70b
```

The installed binary is:

```text
/Users/tomdimino/Library/Application Support/Steam/steamapps/common/
Baldurs Gate 3/Baldur's Gate 3.app/Contents/MacOS/Baldur's Gate 3
```

### Important fat-slice offset correction

The supplied `0xf534000` ARM64 slice offset is stale for the installed binary.
`lipo -detailed_info` reports:

```text
architecture arm64
    offset 257261568
```

`257261568 == 0xf558000`, which is `0x24000` later than `0xf534000`.
Reading the current binary with the stale offset lands in unrelated data.
All raw-word evidence below therefore uses the fat header's current value:

```text
file_offset = 0xf558000 + (vaddr - 0x100000000)
```

The ARM64 virtual addresses themselves still match the Builder A report.

### Vtable location and address point

`nm -arch arm64 -n` identifies:

```text
00000001088291f0 s __ZTVN3phx10PhysXSceneE
```

The Itanium vtable prefix occupies two words, so the object vptr/address point
is `0x108829200`:

```text
0x1088291f0  offset-to-top  0x00000000000
0x1088291f8  RTTI           0x00000000000
0x108829200  VMT[0]         0x00105c4dd78
```

For any audited slot:

```text
vtable_word_vaddr = 0x108829200 + index * 8
```

The raw vtable symbol begins at current file offset:

```text
0xf558000 + (0x1088291f0 - 0x100000000) = 0x17d811f0
```

### Complete query/overlap vtable map

The table below comes from little-endian 64-bit reads at the computed file
offsets. Every target was joined by exact address to the ARM64 `nm` symbol and
demangled with `c++filt`.

| Index | Vtable word VA | Target VA | Demangled target |
|---:|---:|---:|---|
| 0 | `0x108829200` | `0x105c4dd78` | `phx::PhysXScene::~PhysXScene()` (`D1`) |
| 1 | `0x108829208` | `0x105c4dd7c` | `phx::PhysXScene::~PhysXScene()` (`D0`) |
| 2 | `0x108829210` | `0x105c4de08` | `phx::PhysXScene::InstantiateReadLock()` |
| 3 | `0x108829218` | `0x105c4dec4` | `phx::PhysXScene::Unload()` |
| 4 | `0x108829220` | `0x105c4e144` | `phx::PhysXScene::AddPhysicsObjects(Span<PhysicsObject*>)` |
| 5 | `0x108829228` | `0x105c4e158` | `phx::PhysXScene::RemovePhysicsObjects(Span<PhysicsObject*>, bool)` |
| 6 | `0x108829230` | `0x105c4e6e4` | `phx::PhysXScene::AddPhysicsShape(PhysicsShape*)` |
| 7 | `0x108829238` | `0x105c4e6f8` | `phx::PhysXScene::RemovePhysicsShape(PhysicsShape*)` |
| 8 | `0x108829240` | `0x105c4e784` | `phx::PhysXScene::RaycastClosest(Vector3f const&, Vector3f const&, PhysicsHit&, ...) const` |
| 9 | `0x108829248` | `0x105c4e8b0` | `phx::PhysXScene::RaycastAll(Vector3f const&, Vector3f const&, PhysicsHitAll&, ...) const` |
| 10 | `0x108829250` | `0x105c4e8c8` | `phx::PhysXScene::RaycastAny(Vector3f const&, Vector3f const&, ...) const` |
| 11 | `0x108829258` | `0x105c4e8e0` | `phx::PhysXScene::SweepSphereClosest(float, Vector3f const&, Vector3f const&, PhysicsHit&, ...) const` |
| 12 | `0x108829260` | `0x105c4e948` | `phx::PhysXScene::SweepCapsuleClosest(float, float, Vector3f const&, Vector3f const&, PhysicsHit&, ...) const` |
| 13 | `0x108829268` | `0x105c4e9d8` | `phx::PhysXScene::SweepBoxClosest(Vector3f const&, Vector3f const&, Vector3f const&, PhysicsHit&, ...) const` |
| 14 | `0x108829270` | `0x105c4ea38` | `phx::PhysXScene::SweepCylinderClosest(Vector3f const&, Vector3f const&, Vector3f const&, PhysicsHit&, ...) const` |
| 15 | `0x108829278` | `0x105c4ea40` | `phx::PhysXScene::SweepSphereAll(float, Vector3f const&, Vector3f const&, PhysicsHitAll&, ...) const` |
| 16 | `0x108829280` | `0x105c4eaa8` | `phx::PhysXScene::SweepCapsuleAll(float, float, Vector3f const&, Vector3f const&, PhysicsHitAll&, ...) const` |
| 17 | `0x108829288` | `0x105c4eb1c` | `phx::PhysXScene::SweepBoxAll(Vector3f const&, Vector3f const&, Vector3f const&, PhysicsHitAll&, ...) const` |
| 18 | `0x108829290` | `0x105c4eb7c` | `phx::PhysXScene::SweepCylinderAll(Vector3f const&, Vector3f const&, Vector3f const&, PhysicsHitAll&, ...) const` |
| 19 | `0x108829298` | `0x105c4eb84` | `phx::PhysXScene::SweepShapeAll(PhysicsShape const*, Transform const&, PhysicsHitAll&, ...) const` |
| 20 | `0x1088292a0` | `0x105c4eb8c` | `phx::PhysXScene::TestBox(Vector3f const&, Vector3f const&, PhysicsHitAll&, uint, uint, uint) const` |
| 21 | `0x1088292a8` | `0x105c4eb94` | `phx::PhysXScene::TestBox(Vector3f const&, Vector3f const&, Function<bool(PhysicsHit)> const&, uint, uint, uint) const` |
| 22 | `0x1088292b0` | `0x105c4eb9c` | `phx::PhysXScene::TestShape(Transform const&, PhysicsShape const*, PhysicsHitAll&, uint, uint, uint) const` |
| 23 | `0x1088292b8` | `0x105c4eba4` | `phx::PhysXScene::TestShape(Transform const&, PhysicsShape const*, Function<bool(PhysicsHit)> const&, uint, uint, uint) const` |
| 24 | `0x1088292c0` | `0x105c4ebac` | `phx::PhysXScene::TestSphere(Vector3f const&, float, PhysicsHitAll&, uint, uint, uint) const` |
| 25 | `0x1088292c8` | `0x105c4ecd0` | `phx::PhysXScene::TestSphere(Vector3f const&, float, Function<bool(PhysicsHit)> const&, uint, uint, uint) const` |

As an independent structural check, the `phx::SimplePhysXScene` vtable at
`0x10882a6e8` has the same method sequence at indices 0-25, with its own named
implementations.

## Corrected Constant Audit

`WRONG-BY-1` means the current index is one less than the required macOS
index. `COINCIDENTALLY-SAFE` means only that the numerical index selects the
intended symbol; it does **not** certify the C call ABI.

| Constant | Current | Correct | Correct symbol proof | What the current value dispatches | Verdict |
|---|---:|---:|---|---|---|
| `PHYSICS_VMT_RAYCAST_CLOSEST` | 7 | **8** | `0x105c4e784 RaycastClosest` | `0x105c4e6f8 RemovePhysicsShape` | **WRONG-BY-1 — dangerous** |
| `PHYSICS_VMT_RAYCAST_ALL` | 8 | **9** | `0x105c4e8b0 RaycastAll` | `0x105c4e784 RaycastClosest` | **WRONG-BY-1 — dangerous** |
| `PHYSICS_VMT_RAYCAST_ANY` | 9 | **10** | `0x105c4e8c8 RaycastAny` | `0x105c4e8b0 RaycastAll` | **WRONG-BY-1 — dangerous** |
| `PHYSICS_VMT_SWEEP_SPHERE_CLOSEST` | 10 | **11** | `0x105c4e8e0 SweepSphereClosest` | `0x105c4e8c8 RaycastAny` | **WRONG-BY-1 — dangerous** |
| `PHYSICS_VMT_SWEEP_CAPSULE_CLOSEST` | 11 | **12** | `0x105c4e948 SweepCapsuleClosest` | `0x105c4e8e0 SweepSphereClosest` | **WRONG-BY-1 — dangerous** |
| `PHYSICS_VMT_SWEEP_BOX_CLOSEST` | 12 | **13** | `0x105c4e9d8 SweepBoxClosest` | `0x105c4e948 SweepCapsuleClosest` | **WRONG-BY-1 — dangerous** |
| `PHYSICS_VMT_SWEEP_CYLINDER_CLOSEST` | 14 | **14** | `0x105c4ea38 SweepCylinderClosest` | Same symbol | **CORRECT** |
| `PHYSICS_VMT_SWEEP_SPHERE_ALL` | 14 | **15** | `0x105c4ea40 SweepSphereAll` | `0x105c4ea38 SweepCylinderClosest` | **WRONG-BY-1 — dangerous** |
| `PHYSICS_VMT_SWEEP_CAPSULE_ALL` | 15 | **16** | `0x105c4eaa8 SweepCapsuleAll` | `0x105c4ea40 SweepSphereAll` | **WRONG-BY-1 — dangerous** |
| `PHYSICS_VMT_SWEEP_BOX_ALL` | 16 | **17** | `0x105c4eb1c SweepBoxAll` | `0x105c4eaa8 SweepCapsuleAll` | **WRONG-BY-1 — dangerous** |
| `PHYSICS_VMT_SWEEP_CYLINDER_ALL` | 18 | **18** | `0x105c4eb7c SweepCylinderAll` | Same symbol | **CORRECT** |
| `PHYSICS_VMT_SWEEP_SHAPE_ALL` | 19 | **19** | `0x105c4eb84 SweepShapeAll` | Same symbol; constant is currently unused | **CORRECT** |
| `PHYSICS_VMT_TEST_BOX` | 20 | **20** | `0x105c4eb8c TestBox(..., PhysicsHitAll&, ...)` | Same intended result overload | **COINCIDENTALLY-SAFE (index only)** |
| `PHYSICS_VMT_TEST_SPHERE` | 24 | **24** | `0x105c4ebac TestSphere(..., PhysicsHitAll&, ...)` | Same intended result overload | **COINCIDENTALLY-SAFE (index only)** |

All 14 constants found by:

```text
rg -n '^#define PHYSICS_VMT_' src/level/level_manager.c
```

are accounted for in the table.

## Exact Index Edits Required in `level_manager.c`

Replace the current constant block at `src/level/level_manager.c:37-53` with:

```c
#define PHYSICS_VMT_RAYCAST_CLOSEST          8
#define PHYSICS_VMT_RAYCAST_ALL              9
#define PHYSICS_VMT_RAYCAST_ANY             10
#define PHYSICS_VMT_SWEEP_SPHERE_CLOSEST    11
#define PHYSICS_VMT_SWEEP_CAPSULE_CLOSEST   12
#define PHYSICS_VMT_SWEEP_BOX_CLOSEST       13
#define PHYSICS_VMT_SWEEP_CYLINDER_CLOSEST  14
#define PHYSICS_VMT_SWEEP_SPHERE_ALL        15
#define PHYSICS_VMT_SWEEP_CAPSULE_ALL       16
#define PHYSICS_VMT_SWEEP_BOX_ALL           17
#define PHYSICS_VMT_SWEEP_CYLINDER_ALL      18
#define PHYSICS_VMT_SWEEP_SHAPE_ALL         19
#define PHYSICS_VMT_TEST_BOX                20
#define PHYSICS_VMT_TEST_SPHERE             24
```

Also update the stale explanatory comments:

- Replace the “legacy indices ... still needs a dedicated ABI audit” comment
  at lines 33-36 with a note that the indices are verified from the macOS
  ARM64 vtable address point `0x108829200`.
- Remove the comment saying the `SphereAll(14)`/`CylinderClosest(14)` overlap
  is intentional. After correction, `SphereAll=15`.
- Change `RaycastAll ... VMT[8]` at line 214 to macOS ARM64 `VMT[9]`
  (Windows declaration slot 8).
- Change `SweepSphereClosest — VMT[10]` at line 263 to macOS ARM64 `VMT[11]`.
- Change `SweepSphereAll — VMT[14]` at line 326 to macOS ARM64 `VMT[15]`.

No source file was modified by this audit.

## Independent ARM64 Call-ABI Hazard

The corrected indices are necessary, but they are not sufficient to make the
non-cylinder calls safe. The same named-symbol evidence proves that most
function-pointer typedefs in `level_manager.c` do not match the macOS ABI:

- Every raycast takes source and destination as `Vector3f const&`; the current
  typedefs pass six scalar floats.
- `RaycastClosest` takes arguments in the order `source`, `destination`,
  `PhysicsHit&`, followed by flags, two object indices, and an
  `ls::Function<...>` value. The current typedef starts with `PhysicsHit*`,
  passes scalar vectors, and omits the final arguments.
- Sphere/capsule/box sweeps take source/destination (and box extents) as
  `Vector3f const&`; their current typedefs pass those vectors as scalar
  floats. The cylinder typedefs correctly use vector pointers.
- The intended `TestBox` and `TestSphere` slots require a `PhysicsHitAll&`
  output argument. The current typedefs omit it and pass vectors as scalar
  floats.

On AAPCS64, floating-point scalars use `v0-v7`, while references/pointers use
`x` registers. This is not a cosmetic prototype mismatch. For example, the
current `TestBox` caller places `physics_type` in `x1`, but the real callee
interprets `x1` as a `Vector3f const*` and may dereference a small integer.
Accordingly, the two `COINCIDENTALLY-SAFE` verdicts certify slot selection
only; their current invocations remain dangerous.

The builder applying the index correction should keep the non-cylinder
engine calls disabled or unexercised until their ARM64 typedefs, argument
order, optional/function wrapper representations, and output ownership have
been corrected and independently tested. The cylinder methods are the only
currently exposed query calls whose vector-reference shape matches their
demangled macOS symbols.

## Why Existing Tests Could Miss This

- The six pre-existing sphere/capsule/box sweep tier-2 tests only assert that
  each Lua binding is a function; they do not invoke the VMT entry.
- Builder A's report explicitly states that BG3 was not launched and hands
  the live physics calls to the orchestrator for later verification.
- `Parity.Level.RaycastShape` permits a `nil` result. The current slot 7 call
  invokes `RemovePhysicsShape` with the zeroed hit buffer occupying the first
  pointer argument. If that wrong call happens to return without a crash, its
  undefined/stale boolean result can produce `nil`, satisfying the weak test
  without having raycasted.
- The correctly indexed cylinder tests do not exercise the nine stale
  constants.
- No tier-2 invocation of `TestBox` or `TestSphere` was found.

Thus offline passes, registration-only passes, or a `nil` closest-ray result
do not validate the pre-existing dispatches.

## Recommended Fix

1. Apply exactly the nine `+1` index corrections shown above.
2. Preserve cylinder `14/18`, shape-all `19`, TestBox `20`, and TestSphere
   `24`.
3. Repair or quarantine the independently mismatched ARM64 call typedefs
   before any live physics test.
4. Add a non-live regression check that extracts the vtable address point and
   asserts every named target address for the supported game UUID.
5. After the ABI wrappers are corrected, make live tests invoke every method
   with geometry that distinguishes its result shape; function-presence and
   nullable-result tests are insufficient.

## Prevention

- Derive the fat-slice offset from the Mach-O fat header for each installed
  build; do not hard-code `0xf534000`.
- Version vtable maps by ARM64 Mach-O UUID.
- Treat Windows declaration indices as semantic ordering evidence only.
- For each virtual call, verify both the vtable target and the complete
  demangled ARM64 signature before exposing it to Lua.

## Live VMT dump — build 4.1.1.7398727 (2026-08-20)

Captured from a running vanilla session. Image base `0x100a20000`
(slide `0xa20000`); the `ghidra` column rebases to the standard
`0x100000000` load address used by the project's Ghidra database.

    PhysicsScene instance = 0xb7bf42300
    vtable                = 0x10927aec8

| slot | runtime VA | ghidra VA | current binding in `level_manager.c` |
|---:|---|---|---|
| 0 | `0x1066a73fc` | `0x105c873fc` | |
| 1 | `0x1066a7400` | `0x105c87400` | |
| 2 | `0x1066a748c` | `0x105c8748c` | |
| 3 | `0x1066a7494` | `0x105c87494` | |
| 4 | `0x1066a7580` | `0x105c87580` | |
| 5 | `0x1066a7590` | `0x105c87590` | |
| 6 | `0x1066a7598` | `0x105c87598` | |
| 7 | `0x1066a75a8` | `0x105c875a8` | |
| 8 | `0x1066a7634` | `0x105c87634` | RAYCAST_CLOSEST |
| 9 | `0x1066a7760` | `0x105c87760` | RAYCAST_ALL |
| 10 | `0x1066a7778` | `0x105c87778` | **RAYCAST_ANY** |
| 11 | `0x1066a7790` | `0x105c87790` | SWEEP_SPHERE_CLOSEST |
| 12 | `0x1066a77f8` | `0x105c877f8` | SWEEP_CAPSULE_CLOSEST |
| 13 | `0x1066a7888` | `0x105c87888` | SWEEP_BOX_CLOSEST |
| 14 | `0x1066a78e8` | `0x105c878e8` | SWEEP_CYLINDER_CLOSEST |
| 15 | `0x1066a78f0` | `0x105c878f0` | SWEEP_SPHERE_ALL |
| 16 | `0x1066a7958` | `0x105c87958` | |
| 17 | `0x1066a79cc` | `0x105c879cc` | |
| 18 | `0x1066a7a2c` | `0x105c87a2c` | |
| 19 | `0x1066a7a34` | `0x105c87a34` | |
| 20 | `0x1066a7a3c` | `0x105c87a3c` | |
| 21 | `0x1066a7a44` | `0x105c87a44` | |
| 22 | `0x1066a7a4c` | `0x105c87a4c` | |

Observation worth checking during the audit: slots 8-15 are irregularly spaced
(`+0x12c`, `+0x18`, `+0x18`, `+0x68`, `+0x90`, `+0x60`, `+0x08`), whereas slots
18-22 are a run of tiny 8-byte stubs. The dense-stub region is characteristic of
thunks, so index identity should be established by decompiling each target, not
by assuming the 7209685 ordering still holds — the previous audit already found
nine wrong-by-one indices on this VMT.

**Purpose:** decompile `0x105c87778` (slot 10) in Ghidra and confirm it is
RaycastAny before advancing `s_raycast_any_verified_uuid` to
`0C51CAED-6D60-3DCD-9299-8519C92631B0`. See `RAYCAST_ABI_B4A.md`.
