# Wave 3 Builder E — physics VMT dispatch repair

## Outcome

The macOS ARM64 `phx::PhysXScene` dispatch block now matches the audited
Itanium vtable:

```text
RaycastClosest/All/Any             8 / 9 / 10
SweepSphere/Capsule/Box Closest   11 / 12 / 13
SweepCylinderClosest              14
SweepSphere/Capsule/Box All       15 / 16 / 17
SweepCylinderAll / SweepShapeAll  18 / 19
TestBox / TestSphere              20 / 24
```

The nine wrong-by-one constants were corrected. Cylinder 14/18, shape-all 19,
TestBox 20, and TestSphere 24 were preserved.

The six sphere/capsule/box sweep calls and the two overlap calls now match the
proven AAPCS64 register shape: every `Vector3f const&` is passed as a pointer,
scalar radii remain floats, hit results are caller-provided pointers, and the
audited flag/context/object-index sequence is preserved. The three raycasts
are quarantined because their final C++ value parameters cannot be constructed
or destroyed safely from C.

No game process was launched, `/tmp/bg3se.sock` was not touched, and no live
harness command was run.

## Per-function verdicts

The C prototypes below use `uint32_t` for the audited enum/unsigned integer
machine arguments. `UINT32_MAX` is passed for both sentinel object indices.

| Function | VMT | Verdict | macOS C dispatch shape / rationale |
|---|---:|---|---|
| `RaycastClosest` | 8 | **QUARANTINED** | The engine takes `this, src*, dst*, PhysicsHit*, EPhysicsType, uint, uint, EPhysicsContext, uint, uint, ls::Function<bool(PhysicsShape const*)>` with the function wrapper **by value**. Its C representation, construction, destruction, and ownership are unverified. Lua warns once and returns `nil`. The internal C shim performs no VMT read. |
| `RaycastAll` | 9 | **QUARANTINED** | The engine takes vector pointers and `PhysicsHitAll*`, but ends in `ls::Optional<PhysicsSceneScopedReadLock&>` by value. That value ABI is unverified. Lua warns once and returns `nil`; the C shim performs no VMT read. |
| `RaycastAny` | 10 | **QUARANTINED** | Same unverified by-value optional lock as `RaycastAll`. Lua warns once and returns `false`; the C shim performs no VMT read. |
| `SweepSphereClosest` | 11 | **REPAIRED** | `bool(this, float radius, src*, dst*, PhysicsHit*, uint32_t type, uint32_t include, uint32_t exclude, uint32_t context, uint32_t objectIndex, uint32_t excludeObjectIndex)` |
| `SweepCapsuleClosest` | 12 | **REPAIRED** | `bool(this, float radius, float halfHeight, src*, dst*, PhysicsHit*, type, include, exclude, context, objectIndex, excludeObjectIndex)` |
| `SweepBoxClosest` | 13 | **REPAIRED** | `bool(this, extents*, src*, dst*, PhysicsHit*, type, include, exclude, context, objectIndex, excludeObjectIndex)` |
| `SweepCylinderClosest` | 14 | **PRESERVED** | Already used the proven `extents*, src*, dst*, PhysicsHit*` reference pattern and correct index. |
| `SweepSphereAll` | 15 | **REPAIRED** | `bool(this, float radius, src*, dst*, PhysicsHitAll*, type, include, exclude, context, objectIndex, excludeObjectIndex)` |
| `SweepCapsuleAll` | 16 | **REPAIRED** | `bool(this, float radius, float halfHeight, src*, dst*, PhysicsHitAll*, type, include, exclude, context, objectIndex, excludeObjectIndex)` |
| `SweepBoxAll` | 17 | **REPAIRED** | `bool(this, extents*, src*, dst*, PhysicsHitAll*, type, include, exclude, context, objectIndex, excludeObjectIndex)` |
| `SweepCylinderAll` | 18 | **PRESERVED** | Already used the proven `extents*, src*, dst*, PhysicsHitAll*` reference pattern and correct index. |
| `SweepShapeAll` | 19 | **PRESERVED / UNEXPOSED** | Audited index remains defined; no Lua binding or engine call was added. |
| `TestBox` | 20 | **REPAIRED** | `bool(this, position*, extents*, PhysicsHitAll*, uint32_t type, uint32_t include, uint32_t exclude)` |
| `TestSphere` | 24 | **REPAIRED** | `bool(this, position*, float radius, PhysicsHitAll*, uint32_t type, uint32_t include, uint32_t exclude)` |

`TestBox` and `TestSphere` now return a Lua hit array or `nil`, rather than a
fabricated boolean that omitted the engine's required `PhysicsHitAll&`.
The caller supplies a zeroed `LevelPhysicsHitAll`; the engine owns its inner
array storage, and Lua copies the result fields immediately without freeing
those engine-owned pointers.

The repaired `Sweep*All` bindings also honor the engine boolean: no hit returns
`nil`; a hit returns the copied hit array. Closest calls continue to return a
hit table or `nil`.

## Test changes

`tests/harness/test_physics_vmt_audit.py` adds four offline guards:

1. Parse every `PHYSICS_VMT_*` value from `level_manager.c` and compare the
   complete 14-entry audited block.
2. Prove the three raycast Lua bindings each use a private static warn-once
   flag, call `warn_deferred_once`, return their documented `nil`/`false`
   value, and do not invoke a level-manager engine call. It also checks the
   tier-2 deferral assertions.
3. Prove the repaired source call sites pass vector and result pointers rather
   than decomposed scalar floats, and prove the internal raycast shims cannot
   read a VMT entry.
4. When the game is installed, parse the universal Mach-O fat header to find
   the ARM64 slice (no hard-coded `0xf534000` or `0xf558000`), parse the thin
   slice's `LC_SEGMENT_64` mappings, locate
   `__ZTVN3phx10PhysXSceneE` with `nm`, add the Itanium `+0x10` address-point
   bias, read each selected 64-bit vtable word, and join its exact target
   address through `nm | c++filt` to the intended named method. The installed
   binary check calls `pytest.skip` when BG3 or the required tools are absent.

Tier-2 changes:

- `Parity.Level.RaycastClosestDeferred` calls the binding twice, asserts both
  results are `nil`, and gives the live log a warn-once count check.
- `Parity.Level.RaycastAllDeferred` asserts `nil`.
- `Parity.Level.RaycastAnyDeferred` asserts `false`.
- All six sphere/capsule/box sweep tests now execute their repaired call shape
  under `pcall` and require `nil` or the appropriate result table.
- `Parity.Level.TestBox` and `Parity.Level.TestSphere` execute at the host
  position and require `nil` or a `PhysicsHitAll` array.
- Cylinder execution tests were left intact.

## Offline gates

```text
cmake --build . --target clean             PASS
cmake --build .                            PASS
./build/bin/bg3se_test_tier0               PASS — 55/55
PYTHONPATH=tools python3 -m pytest
  tests/harness/ -q                        PASS — 199 passed
git diff --check                           PASS
```

The clean build produced the universal ARM64/x86_64 dylib. Its optional
post-build copy into the installed app bundle was denied by the sandbox, as
expected; linking and all build targets completed. Pre-existing warnings were
emitted outside the changed physics files.

## Exact live filters for the orchestrator

Run only with a loaded save and a stopped/controlled game lifecycle. Builder E
did not run these commands.

### 1. Quarantined raycasts

```bash
PYTHONPATH=tools python3 -m bg3se_harness test Parity.Level.Raycast --headless --continue --tier 2
```

Expected:

- `Parity.Level.RaycastClosestDeferred`, `RaycastAllDeferred`, and
  `RaycastAnyDeferred` pass.
- Closest and All return `nil`; Any returns `false`.
- Exactly one warning per process for each API:
  `Ext.Level.<name> is deferred on macOS`.
- `RaycastClosest` is called twice by its test but logs only once.
- No raycast VMT entry is invoked and there is no native crash.

### 2. Repaired closest sweeps

```bash
PYTHONPATH=tools python3 -m bg3se_harness test Parity.Level.SweepSphereClosest --headless --continue --tier 2
PYTHONPATH=tools python3 -m bg3se_harness test Parity.Level.SweepCapsuleClosest --headless --continue --tier 2
PYTHONPATH=tools python3 -m bg3se_harness test Parity.Level.SweepBoxClosest --headless --continue --tier 2
```

Expected for each: the filter passes without a native crash; the result is
`nil` or a hit table. Any hit table has `Position`, `Normal`, numeric
`Distance`, and numeric `PhysicsGroup`.

### 3. Repaired all-hit sweeps

```bash
PYTHONPATH=tools python3 -m bg3se_harness test Parity.Level.SweepSphereAll --headless --continue --tier 2
PYTHONPATH=tools python3 -m bg3se_harness test Parity.Level.SweepCapsuleAll --headless --continue --tier 2
PYTHONPATH=tools python3 -m bg3se_harness test Parity.Level.SweepBoxAll --headless --continue --tier 2
```

Expected for each: the filter passes without a native crash; the result is
`nil` when the engine reports no hit or a Lua hit array otherwise. Every array
element has the same hit fields as the closest result.

### 4. Repaired overlap output calls

```bash
PYTHONPATH=tools python3 -m bg3se_harness test Parity.Level.TestBox --headless --continue --tier 2
PYTHONPATH=tools python3 -m bg3se_harness test Parity.Level.TestSphere --headless --continue --tier 2
```

Expected for each: the filter passes without a native crash and returns `nil`
or a Lua `PhysicsHitAll` array, never a boolean.

### 5. Preserved cylinder references and complete Level group

```bash
PYTHONPATH=tools python3 -m bg3se_harness test Parity.Level.SweepCylinderClosest --headless --continue --tier 2
PYTHONPATH=tools python3 -m bg3se_harness test Parity.Level.SweepCylinderAll --headless --continue --tier 2
PYTHONPATH=tools python3 -m bg3se_harness test Parity.Level --headless --continue --tier 2
```

Expected: both cylinder filters retain their prior behavior (Closest is `nil`
or a hit table; All is a table), then the complete Level group passes. The
three quarantined raycasts emit at most their one warning each, while repaired
sweeps and overlap calls emit no deferral warning.

Do not treat a registration-only pass as physics validation. Each repaired
filter above invokes its VMT entry and must complete without a native crash.
