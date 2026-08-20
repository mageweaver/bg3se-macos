# Wave 3 Builder G — ComponentOps / passive-singleton report

## Scope and result

Builder G consumed the instruction-level build-`4.1.1.7209685` findings in
`ghidra/offsets/COMPONENT_OPS_AND_PROTO_INIT.md`.

- `Entity:CreateComponent(name)` is unlocked through the verified native
  `IComponentOps::AddImmediateDefaultComponent` dispatch.
- `Entity:RemoveComponent(name)` remains an honest `false` deferral because
  macOS has no verified generic runtime-TypeId removal entry point.
- The refuted passive singleton was replaced by the nm-backed
  `eoc::Passives::m_ptr` address.
- Passive and interrupt prototype sync remain fail-closed. No VMT-copy or
  partial construction path was enabled.
- No `VersionOffsets` field or offset-manifest recipe was added. The
  ComponentOps registry location is a build-specific structure offset, not a
  callable function address.

No Baldur's Gate 3 process was launched, `/tmp/bg3se.sock` was not touched,
and no `bg3se_harness launch` or `bg3se_harness test` command was run.

## Exact CreateComponent recipe consumed

The implementation in `src/entity/entity_system.c` is independently gated on
the exact string `4.1.1.7209685`; a compatible-sentinel result or
`BG3SE_FORCE_ADDRESSES` cannot enable the path on a different offset-table
row.

The consumed recipe is:

1. Resolve `componentName` through the runtime component registry.
2. Require a discovered TypeId other than `0xffff`.
3. Compute `idx = componentTypeId & 0x7fff`.
4. Treat `EntityWorld+0x390` as an **embedded**
   `DynamicArray<UniquePtr<IComponentOps>>`:
   - Buffer: `EntityWorld+0x390`
   - Capacity: `EntityWorld+0x398` (`int32_t`)
   - Size: `EntityWorld+0x39c` (`int32_t`)
5. Use `safe_memory_read_i32` and `safe_memory_read_pointer` for every
   registry/vtable walk and require:
   - `idx < Size`
   - `Buffer != NULL`
   - `Buffer[idx] != NULL`
   - the object vptr is non-null
   - the callable at `vptr+0x28` is non-null
6. Invoke address-point slot 5:

   ```c
   typedef void (*AddImmediateDefaultComponentFn)(
       void *ops, uint64_t entityHandle, int retryCount);

   addImmediateDefaultComponent(ops, entityHandle, 0);
   ```

Slot 5 is the macOS Itanium location corresponding to Windows slot 4 after
the two destructor entries. A completed dispatch returns Lua `true`.
Unknown names, unresolved TypeIds, non-7209685 builds, missing worlds,
out-of-bounds indices, unreadable/null registry state, and null vtable slots
warn once and return Lua `false` without calling native code.

The refuted pointer-field interpretation and old `EntityWorld+0x368`
estimate are not used.

## RemoveComponent deferral

The stub comment and warning now cite the report's **NOT GENERICALLY
UNLOCKED** result:

- build 7209685 emits 734
  `ImmediateWorldCache::RemoveComponent<T>(EntityHandle)` instantiations;
- each specialization loads its own hard-coded `TypeId<T>::m_TypeIndex`;
- there is no verified non-template entry point accepting a runtime TypeId.

`Entity:RemoveComponent(name)` therefore still returns `false` and never
calls a representative specialization with an arbitrary component type.

## Passive/interrupt corrections

Updated files:

- `ghidra/offsets/STATS.md`
- `src/stats/prototype_managers.c`
- `src/stats/prototype_managers.h`

Corrections:

- The runtime singleton is now `eoc::Passives::m_ptr` at `0x1089bc228`,
  backed by its local BSS symbol and 74 ADRP+LDR references.
- The legacy PassivePrototypeManager address claim is removed from the
  requested current-build source and documentation.
- Status/debug logging identifies `eoc::Passives` and reports the verified
  InterruptPrototypeManager singleton at `0x1089ba8f0`.
- The passive singleton define is now included in the nm-based harness offset
  audit.
- Passive sync remains `false`: `PassivePrototype` is exactly `0x210`, has no
  top-level vptr, and its post-`Clean` field population is inlined into the
  stats loader.
- Interrupt sync remains `false`: `InterruptPrototype` is exactly `0x1f0`,
  has a `FixedString` rather than a vptr at object offset zero, and its manager
  uses a hash table plus contiguous storage rather than the generic RefMap
  insertion helper.

Copying a template VMT to offset zero would corrupt either prototype, so no
passive/interrupt sync implementation was attempted.

## Test changes

Tier-2 Lua contracts in `src/lua/lua_ext.c` now verify:

- `Parity.Entity.CreateComponent`: an intentionally invalid component name
  returns `false` before any native dispatch.
- `Parity.Entity.RemoveComponent`: the deferred operation returns `false`.

`tests/harness/test_component_ops_audit.py` adds five offline contracts for:

- the embedded `+0x390/+0x398/+0x39c` registry layout;
- the `& 0x7fff` mask, safe-memory walk, and `vptr+0x28` dispatch;
- guard ordering before the native call;
- the 734-specialization RemoveComponent deferral;
- the passive singleton/no-top-level-vptr corrections and Tier-2 contracts.

`tests/harness/test_offset_audit.py` now checks
`OFFSET_PASSIVE_PROTOTYPE_MANAGER_PTR` against the
`eoc::Passives::m_ptr` nm symbol.

## Offline gates

Commands and results:

```text
cd build
cmake --build . --target clean
cmake --build .
```

Result: exit status 0; universal x86_64/ARM64 dylib built. The optional
post-build copy into the installed app bundle was denied by the workspace
sandbox, while all compilation/link targets completed. Pre-existing warnings
were outside Builder G's changed C files.

```text
./build/bin/bg3se_test_tier0
```

Result: **55/55 passed**.

```text
PYTHONPATH=tools python3 -m pytest tests/harness/ -q
```

Result: **210 passed** (baseline 204).

## Exact live filters for the orchestrator

Builder G did not run these commands. Run them only in the orchestrator's
controlled loaded-save lifecycle.

### CreateComponent guard contract

```bash
PYTHONPATH=tools python3 -m bg3se_harness test Parity.Entity.CreateComponent --headless --continue --tier 2
```

Expected:

- the filter passes;
- `entity:CreateComponent` exists;
- `CreateComponent("__BG3SE_InvalidComponent__")` returns `false`;
- the component-name guard runs before the EntityWorld registry walk and no
  native ComponentOps function is called;
- at most one refusal warning is logged for the process; and
- there is no native crash or entity mutation.

This filter deliberately verifies the safe guard path. It does not claim to
exercise a successful component attachment.

### RemoveComponent deferral contract

```bash
PYTHONPATH=tools python3 -m bg3se_harness test Parity.Entity.RemoveComponent --headless --continue --tier 2
```

Expected:

- the filter passes;
- `entity:RemoveComponent` exists;
- the call returns `false`;
- the warning cites 734 type-specialized removers and the missing generic
  runtime-TypeId entry point;
- no native removal specialization is invoked; and
- there is no entity mutation or native crash.

### Passive sync honesty

```bash
PYTHONPATH=tools python3 -m bg3se_harness test Stats.Goal23.PrototypeSyncHonesty --headless --continue --tier 2
```

Expected:

- the existing status-prototype checks pass;
- the gated PassiveData sync returns `false`;
- the passive warning cites the absent callable loader population path and
  absence of a top-level vptr; and
- no partial passive prototype is inserted.

### Successful CreateComponent dispatch

There is intentionally no general Tier-2 success filter that mutates the host
character. To validate the unlocked native branch, the orchestrator should
add or supply a disposable entity plus a component type known to be absent
and safe to default-construct, then run that owned fixture under a dedicated
filter. Expected for such a fixture:

- `CreateComponent(fullyQualifiedName)` returns `true`;
- exactly the slot-5 call receives `x0=ops`, `x1=entityHandle`, and
  `w2=0`;
- the component becomes observable through `GetComponent`; and
- no unrelated engine-owned entity is mutated.
