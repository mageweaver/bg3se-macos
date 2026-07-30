# Wave 3 Builder B Small-Gap Sweep Report

## Scope and outcome

Wave 3 Goal 3.2 was audited and implemented across `Ext.Math`,
`Ext.Audio`, `Ext.Types`, `Ext.Loca`, and `Ext.Entity`.

- Implemented or confirmed implemented: 13 requested functions.
- Deferred with honest warn-once compatibility stubs: 4 functions
  (`Ext.Types.Construct`, `Ext.Types.GetHashSetValueAt`,
  `Entity:CreateComponent`, and `Entity:RemoveComponent`).
- Corrected two pre-existing Tier 2 tests that called APIs which do not exist
  in the Windows reference (`Ext.Entity.GetAllComponentNames` and
  `Ext.Entity.GetAll`).
- No Tier 1 or Tier 2 test was run by Builder B, and BG3 was never launched.

## Per-function status

### Ext.Math

| Function | Status | Evidence and notes |
|---|---|---|
| `Smoothstep(edge0, edge1, x)` | Implemented (present at sweep baseline) | `src/math/lua_math.c` already contained the scalar binding and registration. The underlying implementation clamps to `[0, 1]` and applies cubic Hermite interpolation. Tier 1 coverage now checks midpoint and both clamp edges. |
| `IsNaN(x)` | Implemented (present at sweep baseline) | `src/math/lua_math.c` already called `isnan()` through `math_is_nan`. Tier 1 coverage checks NaN, a finite value, and infinity. |

No math production change was necessary; the functions matched the recently
added `Fract` binding pattern requested by the campaign.

### Ext.Audio

| Function | Status | Evidence and notes |
|---|---|---|
| `LoadBank(name)` | Implemented; live verification pending | Uses verified exported Wwise SoundEngine functions. The special `Init` path calls `AK::SoundEngine::LoadBank`; ordinary banks mirror `ww::WwiseManager::LoadBank_Blocking` by loading all bank content through `PrepareBank`. |
| `UnloadBank(name)` | Implemented; live verification pending | Resolves the Wwise ID and mirrors `ww::WwiseManager::UnloadBank_Blocking`: `Init` uses `AK::SoundEngine::UnloadBank`, while ordinary banks use the unload preparation type. |
| `PrepareBank(name)` | Implemented; live verification pending | Calls the name overload of `AK::SoundEngine::PrepareBank` with structure-only bank content, then resolves the bank ID. |
| `UnprepareBank(name)` | Implemented; live verification pending | Mirrors the Windows BG3SE Lua wrapper exactly: `UnprepareBank` resolves/routs through the same unload operation as `UnloadBank`, rather than calling the SoundManager virtual named `UnprepareBank`. |

The previous implementation used guessed SoundManager VMT slots 32/34/36.
Those slots were not defensible and were removed. The installed
4.1.1.7209685 binary exports the following verified entry points:

| Symbol | Address |
|---|---:|
| `AK::SoundEngine::GetIDFromString(char const*)` | `0x10019cdf8` |
| `AK::SoundEngine::LoadBank(char const*, uint32_t&, uint32_t)` | `0x1001a10ac` |
| `AK::SoundEngine::UnloadBank(uint32_t, void const*, uint32_t)` | `0x1001a14b0` |
| `AK::SoundEngine::PrepareBank(..., char const*, ...)` | `0x1001a1638` |
| `AK::SoundEngine::PrepareBank(..., uint32_t, ...)` | `0x1001a1868` |

The game wrappers at `0x1037f3010`, `0x1037f3098`, `0x1037f31e8`, and
`0x1037f3230` established the argument constants and special `Init` behavior.
The functions are resolved with `dlsym(RTLD_DEFAULT, ...)`; if any required
symbol is unavailable, all four calls fail closed and emit one warning rather
than entering an unverified VMT slot.

The Lua bindings and registrations in `src/lua/lua_audio.c` already existed
and required no change.

### Ext.Types

| Function | Status | Evidence and notes |
|---|---|---|
| `Serialize(object)` | Implemented for `ComponentProxy` and component `ArrayProxy` | Walks the same `ComponentLayoutDef` database used by `GetComponentLayout` and `GetAllLayouts`. Each readable field is copied to a plain Lua table. Dynamic arrays with known element metadata become plain Lua arrays; generated arrays with no element size are skipped. Reads retain the component layer's lifetime and safe-memory checks. |
| `Unserialize(object, table)` | Implemented for safely writable component fields | Applies fields in place through the existing bounds-checked `component_property_write` path. Read-only fields, transient `OneFrame`/`Request` components, `FixedString`, pointer-bearing dynamic arrays, and unsupported field types are skipped. This is deliberately narrower than unsafe raw memory restoration. |
| `Construct(typeName)` | Deferred; warn once, return `nil` | A layout describes offsets but does not provide the game allocator, constructor, destructor, or ownership model. The Windows reference's `Types.inl` implementation also ends in `// TODO` after constructibility validation. Fabricating an object from `componentSize` would create invalid C++ state. |
| `GetHashSetValueAt(set, index)` | Deferred; warn once, return `nil` | macOS currently has component and array proxies, but no set proxy carrying element size, occupancy/tombstone state, hash/key operations, or lifetime metadata. Treating arbitrary userdata as a hash set would be unsafe. |

The old JSON fallback in `src/injector/main.c` was removed. It accepted plain
Lua tables and returned a JSON string, which was incompatible with Windows
BG3SE's proxy-to-table contract. Unsupported values now raise a clear Lua
error.

Current `Unserialize` scope is intentionally explicit: it handles the safe,
layout-backed scalar/fixed-array subset but does not claim Windows parity for
map, set, ownership-bearing array, or arbitrary reflected C++ object mutation.

### Ext.Localization (`Ext.Loca`)

| Function | Status | Evidence and notes |
|---|---|---|
| `GetTranslatedString(handle, fallback?)` | Implemented; live verification pending | Interns the handle with the game's `FixedString::Create`, calls native `TranslatedStringRepository::TryGet`, copies the returned view through `safe_memory_read`, and returns the fallback on missing/not-ready/invalid data. Thread-local storage grows on demand, so text is not truncated at a fixed buffer size. |
| `UpdateTranslatedString(handle, value)` | Implemented; live verification pending | Uses native `AddTranslatedString`, with the default translated-string repository read from repository offset `+0x08`. It re-reads the handle through `TryGet` and requires an exact byte-for-byte match before reporting success. |

The prior `TryGet` helper used the wrong register shape: it put an indirect
result in `x8` and treated `x0` as `this`. Installed-binary disassembly proves:

- `TryGet` at `0x10652390c`: `x0` is the explicit result buffer, `x1` is the
  repository, `x2` is the handle, and `w3`/`w4` are language identities.
  A successful result stores the view at `+0x00/+0x08` and zero at `+0x10`.
- `AddTranslatedString` at `0x106521148`: `x0` is the output handle, `x1` is
  the repository, `x2` is the input handle, `x3/x4` are `StringView`, `x5` is
  the translated map repository, and `x6` is flags.
- A native caller at `0x1065206d0` passes repository `+0x08` in `x5`,
  confirming the pool used by the update implementation.

The existing Lua bindings in `src/lua/lua_localization.c` already exposed both
functions and required no change.

### Ext.Entity

| Function/gap | Status | Evidence and notes |
|---|---|---|
| `Ext.Entity.GetAllEntities()` | Implemented; live verification pending | Walks every version-gated `EntityStorageContainer::Entities` archetype and copies each `InstanceToPageMap` key, matching the Windows reference algorithm. Returns lifetime-scoped `BG3Entity` userdata. A 65,536-handle safety cap is logged if reached. |
| `Ext.Entity.GetAllEntitiesWithComponent(name)` | Implemented (pre-existing native path retained) | Uses the captured storage container and resolved component type index. The fallback Lua stub was removed from the namespace stub list. |
| `Ext.Entity.GetAllComponents(entity)` | Implemented | Exposes the existing entity method as the requested namespace convenience function. `Entity:GetAllComponents()` now returns `ComponentProxy` values wherever a layout is available and light userdata only for unmapped layouts. |
| `Entity:GetAllComponentNames()` Tier 2 failure | Test fixed; implementation pre-existed | The failing test incorrectly called `Ext.Entity.GetAllComponentNames()`. Windows exposes this on entity userdata. The test now obtains the host entity and calls `entity:GetAllComponentNames()`. |
| `Ext.Entity.GetAll(...)` Tier 2 failure | Test fixed; nonexistent API removed from expectation | Windows has `GetAllEntities()` and `GetAllEntitiesWithComponent()`, not `GetAll(component)`. The registry-count test now calls `GetAllEntitiesWithComponent("esv::Character")`. |
| `Entity:CreateComponent(name)` | Deferred; warn once, return `nil` | Windows obtains a type's `ComponentOps` and invokes VMT slot 4 (`AddImmediateDefaultComponent`). The macOS `EntityWorld` location of `ComponentOpsRegistry` remains unconfirmed (`0x350`–`0x3b0` range); raw archetype mutation would bypass constructors, signals, and command-buffer bookkeeping. |
| `Entity:RemoveComponent(name)` | Deferred; warn once, return `false` | Windows routes removal through `ImmediateWorldCache::RemoveComponent`. Although the cache pointer at `EntityWorld+0x3f0` is known, the non-virtual removal function is not mapped/version-gated, and removal must be deferred safely around ECS iteration. |

## Tests registered

Tier 1:

- `Parity.Audio.LoadBank`
- `Parity.Audio.UnloadBank`
- `Parity.Audio.PrepareBank`
- `Parity.Audio.UnprepareBank`
- `Parity.Types.Serialize`
- `Parity.Types.Unserialize`
- `Parity.Types.SerializeRejectsPlainTable`
- `Parity.Math.Smoothstep`
- `Parity.Math.IsNaN`
- `Parity.Localization.CreateHandle`
- `Parity.Localization.TranslationSurface`

Tier 2:

- `Wave3.Entity.GetAllEntities`
- `Wave3.Entity.GetAllComponents`
- `Wave3.Types.ComponentSerializeRoundtrip`
- `Wave3.Localization.UpdateRoundtrip`
- corrected `Parity.Entity.ComponentEnumeration`
- corrected `Parity.Entity.RegistryCounts`

All modified embedded Lua chunks were syntax-checked with the local Lua
interpreter. Tier 1 and Tier 2 were not executed by Builder B, as required.

## Offline verification

| Gate | Result |
|---|---|
| `cd build && cmake --build .` | Pass; universal x86_64/arm64 dylib linked, exit 0 |
| `./build/bin/bg3se_test_tier0` | **55/55 passed** |
| `PYTHONPATH=tools python3 -m pytest tests/harness/ -q` | **194 passed** |
| `git diff --check` | Pass |
| Modified embedded Lua syntax | Pass |

The campaign baseline named 191 harness tests. Three concurrent Wave 3 audit
tests were present in the shared worktree by final verification, producing
194/194 rather than 191/191; there were no failures.

The required CMake build's post-build deployment copy was denied by the
workspace sandbox (`Operation not permitted`), but CMake returned success and
the universal dylib was produced in `build/lib`. No game installation file was
modified by Builder B.

## Live verification for the orchestrator

After the orchestrator starts BG3 through its authorized workflow and loads a
save:

1. Run Tier 1 filters:
   - `!test Parity.Math`
   - `!test Parity.Audio`
   - `!test Parity.Types`
   - `!test Parity.Localization`
2. Run Tier 2 filters:
   - `!test_ingame Wave3.Entity`
   - `!test_ingame Wave3.Types`
   - `!test_ingame Wave3.Localization`
   - `!test_ingame Parity.Entity.ComponentEnumeration`
   - `!test_ingame Parity.Entity.RegistryCounts`
3. For audio behavior, use a disposable custom/mod Wwise bank fixture whose
   lifecycle is controlled by the test:
   - `PrepareBank` then `UnprepareBank`
   - `LoadBank` then `UnloadBank`
   - require `true` from every call and confirm no Wwise error in the log.
   Do **not** unload or unprepare the game's `Init` bank as a test.
4. Confirm the localization round-trip returns the exact newly supplied text,
   including a value longer than 4096 bytes to exercise dynamic result storage.
5. Confirm `GetAllEntities()` returns a non-empty entity array, the
   `esv::Character` query is non-empty, and the host's `Health` entry from
   `GetAllComponents` is a usable component proxy.
6. Confirm `Construct`, `GetHashSetValueAt`, `CreateComponent`, and
   `RemoveComponent` emit at most one deferral warning per function and return
   their documented safe sentinel. Do not treat those four functions as
   behaviorally implemented.

## Files changed by Builder B

- `progress.json`
- `src/audio/audio_manager.c`
- `src/entity/component_lookup.c`
- `src/entity/component_lookup.h`
- `src/entity/component_property.c`
- `src/entity/component_property.h`
- `src/entity/entity_system.c`
- `src/injector/main.c`
- `src/localization/localization.c`
- `src/lua/lua_ext.c`
- `docs/bugs/wave3-builder-b-sweep-report.md`

Builder B did not modify `src/level/`, `src/lua/lua_level.c`, `src/stats/`, or
`tools/offset_manifest.json`; changes visible there belong to concurrent Wave
3 work.
