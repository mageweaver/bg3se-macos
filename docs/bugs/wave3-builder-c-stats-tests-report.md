# Wave 3 Builder C — Stats Carryover and Test Infrastructure

Date: 2026-07-29  
Scope: Wave 2 stats carryover and review task #19 test gaps

## `stats_get_type` root cause and fix

`stats_get_type()` and `get_object_modifier_list()` treated byte `+0x00` of a
game `StatsObject` as its `ModifierListIndex`. Offset `+0x00` is the object
vtable. On build 4.1.1.7209685 the vtable address ends in `0x08`, so the bogus
byte read selected modifier list 8, `Weapon`. `BURNING` did not match the
legacy status-name heuristics and therefore fell through to this vtable-byte
read, producing the observed `Weapon` result.

The installed 4.1.1.7209685 binary provides direct layout evidence in
`StatsObject::SetType(int)` at `0x101fe9b44`:

```text
0x101fe9b64  ldr w8,  [x0,  #0xe4]   ; read current type
0x101fe9b80  ldr w8,  [x26, #0xb8]   ; ModifierLists entry count
0x101fe9b8c  ldr x8,  [x26, #0x68]   ; ModifierLists value buffer
0x101fe9ba8  str w21, [x19, #0xe4]   ; store new type
```

The fix changes `OBJECT_OFFSET_MODIFIERLIST_IDX` to `0xE4`, reads a signed
32-bit index, rejects negative indices, and resolves the real modifier list
before using the legacy name-prefix fallback. The same correction is applied
to property-to-ModifierList lookup. Consequently `BURNING` resolves as
`StatusData`, and `stats_sync()` dispatches it through
`sync_stat_prototype()` to `sync_status_prototype()`.

## Offset migration coverage

Added this `offset_table_functions` recipe to
`tools/offset_manifest.json`:

```json
{
  "field": "fn_status_proto_init",
  "baseline": "0x102012bf0",
  "symbol": "eoc::StatusPrototype::Init(ls::FixedString const&, bool)"
}
```

The 6995620 baseline address is derived from the exact `+0x1BAA0` old/new text
delta independently observed for nearby stats functions
(`SpellPrototype::Init` and `StatsObject::GetFixedStringValue`). Resolution
does not carry that value forward: `port_offsets.py` resolves the named symbol
from each target binary. Against 7209685 it resolves
`fn_status_proto_init = 0x01ff7150`.

No other prototype `Init` function used by `prototype_managers.c` was missing.
The spell recipe already existed. Passive and interrupt sync intentionally
fail closed because this build has no usable/verified corresponding `Init`
path.

Added offline audit
`test_offset_manifest_covers_all_function_fields`, which requires an exact
set match between every `VersionOffsets.fn_*` field and the manifest recipes.
This prevents a future migration from silently omitting a newly added function
field.

## Registered tests

### Tier 1

- `Stats.SyncNonexistentReturnsFalse`
  - Requires `Ext.Stats.Sync()` on an absent stat to return exactly `false`.
- `Stats.TreasureTable.EmptyIsRealTable`
  - Uses `_TradeItems`, which is declared without subtables in the shipped
    `Public/Shared/Stats/Generated/TreasureTable.txt`.
  - Requires a non-nil table with a real address, the correct name, and an
    empty `SubTables` table.
- `Stats.Goal23.ModuleLoadOrder` (strengthened)
  - Requires the base UUID and every returned prefix entry to match the exact
    `8-4-4-4-12` hexadecimal UUID form.

### Tier 2

- `Entity.ComponentWrite.DynamicArrayRefused`
  - Attempts to replace live `SpellBook.Spells`.
  - Requires a Lua refusal and verifies that the array count is unchanged.
- `Entity.ComponentWrite.UnknownSizeRefused`
  - Requires the live `Net` layout to remain the `Size == 0` fixture.
  - Attempts a write, requires a Lua refusal, and verifies the component proxy
    remains intact.
- `Stats.DamageEvents.PairedFiring`
  - Arms real `BeforeDealDamage` and `DealDamage` subscriptions when the test
    definitions load.
  - Requires at least one observed damage-functor call and exactly paired
    before/after counts.
- `Stats.Goal23.PrototypeSyncHonesty` (strengthened)
  - Now requires `BURNING.Type == "StatusData"` before Sync, a true status
    Sync result, and a named, typed, nonzero cached status prototype.
  - Still requires the allocator-gated passive prototype Sync to return
    exactly `false`.

### Offline component-write contracts

Added `tests/harness/test_component_write_audit.py` to pin the two internal
fail-closed invariants that live fixtures alone cannot completely distinguish:

- `componentSize == 0` unconditionally returns `false`.
- `FIELD_TYPE_DYNAMIC_ARRAY` remains classified as pointer-typed.

## Verification completed

- `cd build && cmake --build .`
  - Compile and link succeeded; `libbg3se.dylib` was produced for arm64 and
    x86_64.
  - The sandbox blocked the post-build copy into the installed game app; this
    did not fail the build target and no game process was started.
- `./build/bin/bg3se_test_tier0`
  - **55/55 passed**.
- `PYTHONPATH=tools python3 -m pytest tests/harness/ -q`
  - **194 passed** (previously 191; three offline audit tests added).
- `python3 tools/port_offsets.py resolve --version 4.1.1.7209685 --emit`
  - **43 addresses resolved**, including
    `.fn_status_proto_init = 0x01ff7150`.
- No BG3 launch, harness launch/test command, or `/tmp/bg3se.sock` access was
  performed.

## Live verification for the orchestrator

Run the normal Tier 1 suite after stats initialization, with special attention
to:

```text
!test Stats.SyncNonexistentReturnsFalse
!test Stats.TreasureTable.EmptyIsRealTable
!test Stats.Goal23.ModuleLoadOrder
```

With a loaded save, run:

```text
!test_ingame Entity.ComponentWrite.DynamicArrayRefused
!test_ingame Entity.ComponentWrite.UnknownSizeRefused
!test_ingame Stats.Goal23.PrototypeSyncHonesty
```

For the end-to-end damage test, its listeners must be armed before the
trigger. After the Lua state/test definitions have loaded, apply `BURNING` to
the host and allow at least one status tick:

```lua
local host = Osi.GetHostCharacter()
Osi.ApplyStatus(host, "BURNING", 6.0, 100, host)
```

Then run:

```text
!test_ingame Stats.DamageEvents.PairedFiring
```

The test must report a positive `BeforeDealDamage` count and the identical
`DealDamage` count. `Osi.ApplyDamage` is not a valid trigger for this hook:
the verified route is a real damage functor such as a `BURNING` status tick.
