# Wave 3 Builder F — AiGrid pathfinding and tile APIs

## Result

Implemented the five APIs marked **IMPLEMENTABLE NOW** by
`ghidra/offsets/AIGRID_PATHFINDING.md`, in the report's recommended order:

1. `Ext.Level.GetPathById`
2. `Ext.Level.ReleasePath`
3. `Ext.Level.GetActivePathfindingRequests`
4. `Ext.Level.FindPath`
5. `Ext.Level.GetEntitiesOnTile`

No native pointer is returned to Lua. Path objects, path nodes, active-list
entries, and metadata entity handles are copied into extender-owned memory
before Lua tables are built.

`Ext.Level.GetTileDebugInfo` and `Ext.Level.BeginPathfinding` remain explicit
warn-once deferrals. No game process or live harness command was run.

## Lua contracts

### Copied path state

`GetPathById`, `GetActivePathfindingRequests`, and `FindPath` use this copied
state shape:

```lua
{
    PathId = integer,
    SearchStarted = boolean,
    SearchComplete = boolean,
    GoalFound = boolean,
    DestinationReached = boolean,
    MovingBound = number,
    StandingBound = number,
    MovingBound2 = number,
    NodeCount = integer
}
```

`GetPathById(pathId)` returns that table or `nil`. The sentinel call
`GetPathById(-1337)` returns `nil` before any native function call.

`GetActivePathfindingRequests()` always returns a Lua array. Every element is a
copied state table; the array is empty if the grid is unavailable, its header
is rejected, or no active path can be authoritatively mapped to a PathId.

`FindPath(pathId, immediate)` returns a copied state table or `false`.
`immediate` defaults to `true`, preserving the Windows binding's synchronous
behavior. Passing `false` queues with `AiGrid::FindPath` but does not call
`FindPathImmediate`.

Only a result with both `SearchComplete == true` and `GoalFound == true`
receives a `Nodes` field:

```lua
{
    Nodes = {
        {
            Position = {x, y, z},
            Portal = EntityHandle, -- raw integer, matching existing conventions
            Distance = number,
            DistanceModifier = number,
            Flags = integer
        },
        ...
    }
}
```

`ReleasePath(pathId)` returns `true` only when a validated map entry was found
and the native remove call was issued. Invalid, unknown, and sentinel IDs
return `false`.

`GetEntitiesOnTile({x, y, z})` always returns a Lua array. Entity handles are
copied as raw Lua integers, matching `Entity:GetHandle`, entity events, and the
other existing handle-to-Lua paths.

## Per-API implementation

### 1. `GetPathById`

- Rejects `-1337` before reading the grid.
- Resolves the ID only through the verified `AiGrid::PathMap`.
- Mirrors the native integer hash sequence (`sxtw`, then unsigned
  divide/remainder).
- Bounds linked-list traversal by the validated PathMap node count.
- Validates the mapped `AiPath` object, its `InUse` byte, status bytes, and
  Nodes header.
- Returns only the copied Lua state listed above.

### 2. `ReleasePath`

- Performs the same sentinel rejection and authoritative PathMap lookup.
- Requires a stable, in-use copied state before the call.
- Calls `AiGrid::RemovePath(int)` with the verified direct-void ABI.
- Does not retain or return the resolved `AiPath*`.

### 3. `GetActivePathfindingRequests`

- Snapshots the verified `AiGrid::Paths` DynamicArray header and payload.
- Re-reads the header after copying and retries at most once if it changed.
- Treats active-list entries as pointers only, never as IDs or pool indices.
- Reverse-resolves each pointer through PathMap, then performs a forward
  PathMap lookup and pointer equality check before copying state.
- Skips malformed, stale, or unmapped entries and returns a compact Lua array.

### 4. `FindPath`

- Rejects `-1337` and unknown IDs before either native function call.
- Validates stable in-use state, calls `AiGrid::FindPath(int)`, and discards
  the pre-call `AiPath*`.
- When synchronous completion is requested, resolves and validates the ID
  through PathMap again before `AiGrid::FindPathImmediate(int, bool)`.
- Resolves through PathMap a third time after all mutating calls.
- Copies Nodes only after stable `SearchComplete && GoalFound` state.
- Re-reads state and the Nodes header after copying; changed state/count fails
  closed rather than publishing a mixed snapshot.

### 5. `GetEntitiesOnTile`

- Calls `AiGrid::ToTilePos` with caller-owned `Vector3f` and `AiTilePos`
  storage and `false` for the final bool.
- Rejects failed/invalid tile output before metadata lookup.
- Calls `AiGrid::GetMetaData`; null metadata yields an empty array.
- Validates and snapshots the `LEGACY_Set<EntityHandle>` header at
  `AiMetaData+0x00`.
- Re-reads the header after the handle copy and exposes only Lua integers.

## Exact report offsets consumed

All values below came from `AIGRID_PATHFINDING.md` for build
`4.1.1.7209685`.

| Owner | Offset/layout consumed | Use |
|---|---:|---|
| `AiGrid` | `+0xf0 int32 PathMap.nodeCount` | traversal bound/header validation |
| `AiGrid` | `+0xf4 int32 PathMap.bucketCount` | hash modulus/header validation |
| `AiGrid` | `+0xf8 PathMapNode** buckets` | authoritative ID lookup |
| `AiGrid` | `+0x100 AiPath** Paths.data` | active-list snapshot |
| `AiGrid` | `+0x108 int32 Paths.capacity` | active header validation |
| `AiGrid` | `+0x10c int32 Paths.size` | active snapshot count |
| `PathMapNode` | `+0x00 Next`, `+0x08 PathId`, `+0x10 Path` | map chain lookup/inverse lookup |
| `PathMapNode` | size `0x18` | compile-time layout assertion |
| `AiPath` | size `0x2a8` | mapped-pointer readable-range validation |
| `AiPath` | `+0x20 MovingBound` | copied state |
| `AiPath` | `+0x24 StandingBound` | copied state |
| `AiPath` | `+0x40 MovingBound2` | copied state |
| `AiPath` | `+0x270..+0x273` | four copied status booleans |
| `AiPath` | `+0x274 InUse` | internal validation only |
| `AiPath` | `+0x278 Nodes.data` | completed polyline snapshot |
| `AiPath` | `+0x280 Nodes.capacity` | Nodes header validation |
| `AiPath` | `+0x284 Nodes.size` | copied `NodeCount` and copy bound |
| `AiPathNode` | stride `0x28` | bounded node iteration |
| `AiPathNode` | `+0x00 Position` | copied node |
| `AiPathNode` | `+0x10 Portal` | copied node/entity handle |
| `AiPathNode` | `+0x18 Distance` | copied node |
| `AiPathNode` | `+0x1c DistanceModifier` | copied node |
| `AiPathNode` | `+0x20 Flags` | copied node |
| `AiTilePos` | `+0x00 int16 X`, `+0x02 int16 Y`, `+0x04 int32 SubgridId`, size `0x08` | caller-owned tile output |
| `AiMetaData` | `+0x00 Entities.data`, `+0x08 capacity`, `+0x0c size` | entity-set snapshot |
| DynamicArray/LEGACY_Set | `{pointer +0x00, int32 capacity +0x08, int32 size +0x0c}`, size `0x10` | all container validation |

No OPEN or PROVISIONAL field is read by an implemented API.

## Exact native addresses and ABIs consumed

The 7209685 `VersionOffsets` row stores the base-relative values, and
`offset_table_fn()` supplies callable runtime addresses:

| Function field | Ghidra VA | Darwin ARM64 ABI used |
|---|---:|---|
| `fn_aigrid_to_tile_pos` | `0x1011619c0` | `x0=this`, `x1=&worldPos`, `x2=&outAiTilePos`, `w3=bool`; `w0=bool` |
| `fn_aigrid_get_metadata` | `0x10114b2e4` | `x0=this`, `x1=&tilePos`; `x0=AiMetaData*` or null |
| `fn_aigrid_remove_path` | `0x101161ea4` | `x0=this`, `w1=pathId`; void |
| `fn_aigrid_find_path` | `0x101162a4c` | `x0=this`, `w1=pathId`; void |
| `fn_aigrid_find_path_immediate` | `0x101165cec` | `x0=this`, `w1=pathId`, `w2=bool`; void |

None uses `x8` indirect return storage.

The five fields are zero in the older `4.1.1.6995620` row, so the new layout
walkers fail closed there. All five 7209685 fields have exact symbol recipes in
`tools/offset_manifest.json`; the installed binary's `nm | c++filt` symbols
resolved to the report addresses.

## Safety enforcement

- `-1337` is checked before grid resolution or any native path call.
- Unknown IDs must fail the private PathMap lookup before `FindPath`,
  `FindPathImmediate`, or `RemovePath`.
- Active-array indices and PathPool indices are never interpreted as PathIds.
- Every struct and container read uses `safe_memory` wrappers after checked
  address arithmetic and mapped/readable-range validation.
- Negative sizes/capacities, `size > capacity`, null non-empty data, overflow,
  and defensive count caps are rejected.
- PathMap traversal is bounded by validated `nodeCount`; self-linked nodes are
  rejected.
- DynamicArray/LEGACY_Set headers are compared before and after payload copies.
- `AiPath*` values are discarded and re-resolved after container-mutating
  calls.
- Node and entity-set payloads are copied before Lua construction.
- Lua receives no lightuserdata or raw native address from these five APIs.
- Synchronous pathfinding is reachable only from the Lua gate and only after
  the request is revalidated.

## Deferred APIs

`GetTileDebugInfo` still returns `nil` and cites the report's remaining
evidence gap: `AiGridTile::MinHeight` at `+0x0a` is OPEN, while the public
cloud-surface enum conversion is PROVISIONAL.

`BeginPathfinding` still returns `nil` and cites the report's OPEN setup gap:
character path creation writes many fields beyond the verified
`AiGrid::CreatePath` call. The implementation does not manufacture a partially
configured `AiPath`.

## Test changes

Converted these tier-2 deferral tests into real copied-value/guard contracts:

- `Parity.Level.GetEntitiesOnTile`
- `Parity.Level.FindPath`
- `Parity.Level.ReleasePath`
- `Parity.Level.GetPathById`
- `Parity.Level.GetActivePathfindingRequests`

The tests call each API under `pcall`. They assert:

- tile entities are returned as a table of numeric copied handles;
- `GetPathById(-1337) == nil`;
- sentinel `FindPath` returns `false`/`nil`;
- sentinel `ReleasePath` returns `false`; and
- active requests are a table whose entries have the complete copied-state
  shape.

The two honest deferral contracts remain:

- `Parity.Level.GetTileDebugInfoDeferred`
- `Parity.Level.BeginPathfindingDeferred`

Added five `TABLE_FIELD_SYMBOLS` cases to
`tests/harness/test_offset_audit.py`. The existing
`test_offset_manifest_covers_all_function_fields` exact-set test also covers
all new `fn_*` fields.

## Offline gates run

```text
clean universal ARM64/x86_64 build: PASS
./build/bin/bg3se_test_tier0:      PASS, 55/55
PYTHONPATH=tools python3 -m pytest tests/harness/ -q:
                                     PASS, 204/204
tools/port_offsets.py verify:       PASS, all 30 fields + 18 remaps match
git diff --check:                   PASS
```

The optional post-build copy into the installed app bundle was denied by the
sandbox; compilation, linking, and the universal-binary checks completed with
exit status 0. No BG3 process was launched, `/tmp/bg3se.sock` was not touched,
and no `bg3se_harness launch/test` command was run.

## Exact live filters for the orchestrator

Builder F did not run these commands. Run them only in the orchestrator's
controlled loaded-save lifecycle.

### Copied tile handles

```bash
PYTHONPATH=tools python3 -m bg3se_harness test Parity.Level.GetEntitiesOnTile --headless --continue --tier 2
```

Expected: pass with no native crash; result is a table. Every returned element
is a numeric EntityHandle copy. An unoccupied/unmapped origin tile may produce
an empty table.

### PathMap sentinel guard

```bash
PYTHONPATH=tools python3 -m bg3se_harness test Parity.Level.GetPathById --headless --continue --tier 2
```

Expected: pass; `GetPathById(-1337)` returns `nil` and no native game function
is called.

### Active-list snapshot

```bash
PYTHONPATH=tools python3 -m bg3se_harness test Parity.Level.GetActivePathfindingRequests --headless --continue --tier 2
```

Expected: pass with no native crash; result is a table. If non-empty, every
entry has numeric `PathId`, numeric bounds/`NodeCount`, and the four boolean
status fields.

### FindPath invalid-ID guard

```bash
PYTHONPATH=tools python3 -m bg3se_harness test Parity.Level.FindPath --headless --continue --tier 2
```

Expected: pass; `FindPath(-1337, true)` returns `false`/`nil`. Neither
`AiGrid::FindPath` nor `FindPathImmediate` is called.

### ReleasePath invalid-ID guard

```bash
PYTHONPATH=tools python3 -m bg3se_harness test Parity.Level.ReleasePath --headless --continue --tier 2
```

Expected: pass; `ReleasePath(-1337)` returns `false` and
`AiGrid::RemovePath` is not called.

### Remaining honest deferrals

```bash
PYTHONPATH=tools python3 -m bg3se_harness test Parity.Level.GetTileDebugInfoDeferred --headless --continue --tier 2
PYTHONPATH=tools python3 -m bg3se_harness test Parity.Level.BeginPathfindingDeferred --headless --continue --tier 2
```

Expected: both pass and return `nil`; each API logs its updated deferral reason
at most once per process.

### Full Level tier

```bash
PYTHONPATH=tools python3 -m bg3se_harness test Parity.Level --headless --continue --tier 2
```

Expected for the five Builder F contracts: all pass with no native crash.
Adjacent Level physics tests are outside Builder F's scope.

The guard filters intentionally do not mutate an arbitrary engine-owned active
request. To validate the successful native `FindPath`/`RemovePath` branches,
the orchestrator must supply a request it owns and may safely release; it
should not reuse a random entry from `GetActivePathfindingRequests`. For such a
fixture:

- `GetPathById(id)` must return a copied state table;
- `FindPath(id, true)` must return a copied state table, with `Nodes` present
  only for completed/found paths;
- every Node must have the copied fields documented above; and
- `ReleasePath(id)` must return `true`, after which
  `GetPathById(id)` must return `nil`.
