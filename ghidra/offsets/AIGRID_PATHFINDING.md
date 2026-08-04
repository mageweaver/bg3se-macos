# AiGrid pathfinding and tile layouts — macOS ARM64

Build: **4.1.1.7209685**

Binary: `Baldur's Gate 3.app/Contents/MacOS/Baldur's Gate 3`

Architecture: macOS ARM64

## Scope and confidence

This report recovers the layouts and call ABIs needed by the seven deferred
`Ext.Level` functions:

- `GetEntitiesOnTile`
- `GetTileDebugInfo`
- `BeginPathfinding`
- `FindPath`
- `ReleasePath`
- `GetPathById`
- `GetActivePathfindingRequests`

Offsets marked **VERIFIED** are supported by at least two independent code
sites. **PROVISIONAL** means that the value is strongly indicated but presently
has only one independent macOS code-site confirmation. **OPEN** means that the
Windows layout provides a hypothesis, but the macOS build has not been
adequately confirmed.

No game process was launched. Analysis used the symbol table and static ARM64
disassembly only. The Ghidra HTTP bridge did not respond, so all instruction
evidence below comes from the Mach-O symbol table and `otool` disassembly.

### Important fat-binary offset correction

The installed binary's fat header reports the ARM64 slice at **`0xf558000`**.
Static reads at that offset agree with `otool -arch arm64`. The initially
supplied `0xf534000` value is `0x24000` bytes earlier and does not produce the
instructions shown by `otool`.

For this installed binary, use:

```text
file_offset = 0xf558000 + (vaddr - 0x100000000)
```

Do not silently apply this correction to a different copy of the executable;
read that file's fat header first.

## Anchor symbols

| Symbol | ARM64 virtual address |
|---|---:|
| `eoc::AiGrid::UpdatePathfinding()` | `0x101136088` |
| `eoc::AiGrid::ToWorldPos(eoc::AiTilePos const&, Vector3f&)` | `0x10113d844` |
| `eoc::AiGrid::GetMetaData(eoc::AiTilePos const&) const` | `0x10114b2e4` |
| `eoc::AiGrid::ToTilePos(Vector3f const&, eoc::AiTilePos&, bool) const` | `0x1011619c0` |
| `eoc::AiGrid::RemovePath(int)` | `0x101161ea4` |
| `eoc::AiGrid::CreatePath(float, float, float, eoc::CoverManager*)` | `0x101162020` |
| `eoc::AiGrid::FillPathFromSync(...)` | `0x101162688` |
| `eoc::AiGrid::FindPath(int)` | `0x101162a4c` |
| `eoc::AiGrid::FindPathImmediate(int, bool)` | `0x101165cec` |
| `eoc::AiPath::AiPath()` | `0x101170968` |
| `eoc::AiPath::Reset()` | `0x1011716c4` |

## `eoc::AiGrid` pathfinding region

### Layout

| Offset | macOS ARM64 field | Status | Evidence |
|---:|---|---|---|
| `0xc8` | `EntityWorld*` | VERIFIED | Constructor `0x101154604`; retained as the first pointer in the paired store based at `this+0x78+0x50`. |
| `0xd0` | `ThothMachine*` | VERIFIED | Constructor `0x101154604`; second pointer in the same paired store. |
| `0xd8` | `int32_t NextPathHandle` | VERIFIED | Initialized at `0x101154608`; loaded, incremented, and stored by `CreatePath` at `0x101162058`–`0x101162060`. |
| `0xe0` | `AiPath** PathPool.data` | VERIFIED | Constructor `0x101154710`, `0x101155abc`; scanned by `CreatePath` at `0x10116206c` onward. |
| `0xe8` | `int32_t PathPool.capacity` | VERIFIED | Constructor `0x101154718`; used by the `CreatePath` growth path at `0x1011620f4` onward. |
| `0xec` | `int32_t PathPool.size` | VERIFIED | Constructor append at `0x101155ac4`; `CreatePath` scan at `0x101162064` and expansion at `0x1011620f4` onward. |
| `0xf0` | `int32_t PathMap.nodeCount` | VERIFIED | Initialized at `0x10115470c`; incremented at `0x10116254c`–`0x101162554`; decremented at `0x101162000`–`0x101162008`. |
| `0xf4` | `int32_t PathMap.bucketCount` | VERIFIED | Constructor `0x1011547d8`; lookups in `CreatePath`, `FindPath`, and `RemovePath`, including `0x101162a6c` and `0x101161eb4`. |
| `0xf8` | `PathMapNode** PathMap.buckets` | VERIFIED | Constructor `0x1011547b0`; lookups at `0x101162a7c` and `0x101161ecc`. |
| `0x100` | `AiPath** Paths.data` | VERIFIED | Initialized at `0x1011547f0`/`0x101154858`; appended by `FindPath` at `0x101162d68` onward; consumed by `UpdatePathfinding` at `0x101136154`. |
| `0x108` | `int32_t Paths.capacity` | VERIFIED | Initialized with the active-list pair; used by the `FindPath` growth logic at `0x101162d68` onward. |
| `0x10c` | `int32_t Paths.size` | VERIFIED | Used by `FindPath`, `RemovePath` (`0x101161ef4` onward), and `UpdatePathfinding` (`0x101136148`). |

The three containers use the macOS `DynamicArray` shape:

```cpp
template <class T>
struct DynamicArray
{
    T* Data;             // +0x00
    int32_t Capacity;    // +0x08
    int32_t Size;        // +0x0c
};                       // sizeof 0x10
```

### Path-map node

| Offset | Field | Status | Evidence |
|---:|---|---|---|
| `0x00` | `PathMapNode* Next` | VERIFIED | Traversed by `FindPath` and `RemovePath`; written during insertion at `0x101162530` onward. |
| `0x08` | `int32_t PathId` | VERIFIED | Compared by `FindPath` and `RemovePath`; written during insertion at `0x101162530`–`0x101162558`. |
| `0x0c` | padding | VERIFIED | Value pointer is eight-byte aligned at `+0x10`. |
| `0x10` | `AiPath* Path` | VERIFIED | Loaded by `FindPath` and `RemovePath`; written during insertion. |

`sizeof(PathMapNode) == 0x18`, independently supported by the allocation size
in `CreatePath`.

### Free-path bookkeeping

There is no separate free list in this build.

`CreatePath` scans `PathPool[0..size)` and tests `AiPath::InUse` at `+0x274`
(`0x101162074`–`0x101162088`). If no free object exists, it allocates a new
`0x2a8`-byte `AiPath`, calls its constructor, and appends it to `PathPool`
(`0x101162160`–`0x101162194`). `RemovePath` calls `AiPath::Reset`
(`0x101161ef0`), which clears `InUse`, and then removes the path from the active
list and ID map.

The invalid/sentinel path ID is **`-1337`** (`cmn wN, #0x539` in path-creation
consumers and `FindPathImmediate`).

## `eoc::AiPath`

`sizeof(AiPath) == 0x2a8`, verified by the allocation in `CreatePath` and the
constructor's complete initialization range.

### Fields required by the deferred APIs

| Offset | Field | Status | Evidence |
|---:|---|---|---|
| `0x00` | `DynamicArray<SurfacePathInfluence> SurfacePathInfluences` | VERIFIED | Constructor initialization and reset/destruction behavior; array header matches the common `+0/+8/+c` shape. |
| `0x10` | `EntityHandle Source` | VERIFIED | Invalidated by constructor/`Reset`; written by `ecl::aigrid::CreatePathForCharacter` at `0x102de45dc`. |
| `0x18` | `EntityHandle Target` | VERIFIED | Invalidated by constructor/`Reset`; written by `CreatePathForCharacter` at `0x102de45e4`. |
| `0x20` | `float MovingBound` | VERIFIED | `CreatePath` stores `s0` here at `0x1011622e8` onward; character path creation supplies the corresponding bound. |
| `0x24` | `float StandingBound` | VERIFIED | `CreatePath` stores `s1` here; independently populated through the character path-creation caller. |
| `0x28` | `uint64_t CollisionMask` | PROVISIONAL | Consistent with macOS accesses and the Windows field order; a second isolated semantic use was not recorded. |
| `0x30` | `uint64_t CollisionMaskMove` | PROVISIONAL | Same qualification as `CollisionMask`. |
| `0x38` | `uint64_t CollisionMaskStand` | PROVISIONAL | Same qualification as `CollisionMask`. |
| `0x40` | `float MovingBound2` | VERIFIED | `CreatePath` stores `s2` here at `0x101162328`; character path creation supplies the same moving bound for `s0` and `s2`. |
| `0x64` | `Vector3f SourceAdjusted` | PROVISIONAL | Position-vector accesses agree with Windows ordering; semantic name needs another macOS caller confirmation. |
| `0x70` | `Vector3f SourceOriginal` | PROVISIONAL | Position-vector accesses agree with Windows ordering; semantic name needs another macOS caller confirmation. |
| `0x7c` | `Vector3f TargetAdjusted` | PROVISIONAL | `FindPath` copies this vector to the cached target region; semantic name follows the Windows layout. |
| `0x88` | `Vector3f ProjectileTarget` | PROVISIONAL | Layout-compatible vector field; not required for the seven APIs. |
| `0x110` | `CoverManager* CoverManager` | VERIFIED | `CreatePath` stores integer-register argument `x1` at `0x101162320`–`0x101162324`; `Reset` clears it at `0x101171a00`. |
| `0x138` | `DynamicArray<EntityHandle> IgnoreEntities` | PROVISIONAL | Array operations match the header shape; semantic identification follows the Windows order. |
| `0x148` | cached target `Vector3f` | PROVISIONAL | Written from `+0x7c` by `FindPath`; exact public field name is unresolved. |
| `0x270` | `bool SearchStarted` | VERIFIED | Constructor/`Reset` clear it; `FindPath` and `UpdatePathfinding` write/read it at `0x101162a98`, `0x101162d64`, `0x101137514`, and `0x101137528`. |
| `0x271` | `bool SearchComplete` | VERIFIED | Constructor/`Reset` clear it; `FillPathFromSync` sets it at `0x1011626c4`; teleport caller tests it at `0x104898b5c`; update completion stores occur at `0x101136e30` and `0x101137f08`. |
| `0x272` | `bool GoalFound` | VERIFIED | Cleared/set by pathfinding; `FillPathFromSync` sets it at `0x1011626c8`–`0x1011626cc`; teleport caller tests it at `0x104898b70`. |
| `0x273` | `bool DestinationReached` | VERIFIED | Set in independent update paths at `0x101138570` and `0x1011388bc`; adjacent-state initialization confirms placement. |
| `0x274` | `bool InUse` | VERIFIED | Tested by `CreatePath` at `0x101162078`; set at `0x1011622e4` and `0x1011626c0`; cleared by `Reset` at `0x1011716e8`. |
| `0x278` | `AiPathNode* Nodes.data` | VERIFIED | Constructor `0x101170cbc`; `FillPathFromSync`; update logic; teleport caller at `0x104898b84`. |
| `0x280` | `int32_t Nodes.capacity` | VERIFIED | Constructor and array growth logic. |
| `0x284` | `int32_t Nodes.size` | VERIFIED | Cleared at `0x10116255c` and `0x101171818`; teleport caller reads it at `0x104898b78`. |
| `0x288` | `AiPathCheckpoint* Checkpoints.data` | VERIFIED | Constructor allocation/initialization and checkpoint writers. |
| `0x290` | `int32_t Checkpoints.capacity` | VERIFIED | Constructor and array growth logic. |
| `0x294` | `int32_t Checkpoints.size` | VERIFIED | Cleared at `0x101162560` and `0x10117181c`; checkpoint generation uses it. |
| `0x298` | `uint64_t LimitNodeIndex` | VERIFIED | Constructor/`Reset` and path-limit logic. |
| `0x2a0` | `bool HasLimitNode` | VERIFIED | Cleared by `Reset` at `0x101171a04` and used with `LimitNodeIndex`. |
| `0x2a4` | `uint32_t ErrorCause` | VERIFIED | Cleared by `Reset` at `0x101171a0c`; terminal field before the verified `0x2a8` object size. |

### `AiPathNode`

| Offset | Field | Status | Evidence |
|---:|---|---|---|
| `0x00` | `Vector3f Position` | VERIFIED | Written by `FillPathFromSync` at `0x10116277c` onward; read by update/path consumers. |
| `0x0c` | padding | VERIFIED | Next field is eight-byte aligned. |
| `0x10` | `EntityHandle Portal` | VERIFIED | Written by `FillPathFromSync`; independently consumed by update logic. |
| `0x18` | `float Distance` | VERIFIED | Paired floating-point write in `FillPathFromSync`; read by path consumers. |
| `0x1c` | `float DistanceModifier` | VERIFIED | Second half of the paired floating-point value. |
| `0x20` | `uint8_t Flags` | VERIFIED | Byte write around `0x101162790`; independently tested by consumers. |

`sizeof(AiPathNode) == 0x28`, verified by the `FillPathFromSync` stride and
independent node iteration in `UpdatePathfinding`.

The checkpoint array has a **likely `0x30`-byte element stride**, inferred from
the constructor allocation (`0x10` allocation header plus `50 * 0x30`), but its
full element layout is **OPEN** and is not needed to return the path polyline.

## Pathfinding behavior

### `CreatePath`

`CreatePath`:

1. obtains a new ID from `NextPathHandle`;
2. reuses the first pool entry with `InUse == false`, or allocates a
   `0x2a8`-byte `AiPath`;
3. initializes bounds and the cover manager;
4. marks the object in use and clears result state;
5. inserts `{id, path}` into `PathMap`; and
6. returns the integer ID.

The path is not added to the active `Paths` list until `FindPath(id)` succeeds
in locating and queueing it.

### `FindPath`

At entry, `FindPath` pessimistically writes:

```text
SearchStarted  = true
SearchComplete = true
GoalFound      = false
```

For a valid request that can be queued, it clears `SearchStarted` and
`SearchComplete`, then appends the `AiPath*` to `AiGrid::Paths`.

### `UpdatePathfinding`

`UpdatePathfinding` operates on `Paths[0]`. It starts an unstarted request,
advances the search, records completion and goal state, and removes completed
work from the active list. This makes `Paths` the set of queued/active requests,
not the complete set of allocated path objects. `PathMap` remains the
authoritative path-ID lookup until `RemovePath`.

### `FindPathImmediate`

`FindPathImmediate` resolves the ID through `PathMap`, then repeatedly calls
`UpdatePathfinding` until `path->SearchComplete` at `+0x271` becomes nonzero.
Its second `bool` argument is not read in this build. The function does not
place a meaningful value in `w0`; the observed caller ignores a return value.

The sole direct caller found, in `esv::CharacterMover::Teleport`, performs:

```text
FindPath(id)
if (!path->SearchComplete)
    FindPathImmediate(id, <ignored>)
if (path->GoalFound)
    consume path->Nodes
```

Evidence: calls/tests at `0x104898b58`–`0x104898b84`.

Wrappers must reject `-1337` and unknown IDs before calling
`FindPathImmediate`: the native routine can reach a null-path dereference for
an invalid ID.

## Tile and metadata layouts

### `AiTilePos`

The macOS output object matches the Windows eight-byte layout:

| Offset | Field | Status | Evidence |
|---:|---|---|---|
| `0x00` | `int16_t X` | VERIFIED | `GetMetaData` reads it with a signed halfword load at `0x10114b328`; `ToTilePos` writes the output object. |
| `0x02` | `int16_t Y` | VERIFIED | `GetMetaData` reads it with a signed halfword load at `0x10114b32c`; `ToTilePos` writes the output object. |
| `0x04` | `int32_t SubgridId` | VERIFIED | `GetMetaData` reads it at `0x10114b2e4`; subgrid lookup and `ToTilePos` output use the same member. |

```cpp
struct AiTilePos
{
    int16_t X;
    int16_t Y;
    int32_t SubgridId;
}; // sizeof 0x08
```

### Tile lookup recovered from `GetMetaData`

`GetMetaData(tilePos)` performs:

1. look up `tilePos.SubgridId` in the AiGrid subgrid map;
2. load the subgrid's tile grid from `subgrid + 0x30`;
3. compute `tilePos.X + tileGrid->Width * tilePos.Y`;
4. address a **`0x10`-byte tile**;
5. read the metadata index at tile `+0x0c`;
6. treat `0xffff` as no metadata; and
7. return `AiGrid::MetaData.data[index]`.

Relevant instructions are `0x10114b2e4`–`0x10114b36c`. The subgrid tile-grid
pointer is loaded at `0x10114b324`, X/Y at `0x10114b328`/`0x10114b32c`, and
the metadata index at `0x10114b340`.

Additional container fields observed in this function:

| Owner/offset | Field | Status |
|---|---|---|
| `AiGrid + 0x18` | `AiMetaData** MetaData.data` | VERIFIED |
| `AiGrid + 0x20` | `int32_t MetaData.capacity` | VERIFIED |
| `AiGrid + 0x24` | `int32_t MetaData.size` | VERIFIED |
| `AiGrid + 0x6c` | subgrid-map bucket count | VERIFIED |
| `AiGrid + 0x70` | subgrid-map buckets | VERIFIED |
| `AiSubgrid + 0x18` | X tile count | VERIFIED |
| `AiSubgrid + 0x1c` | Y tile count | VERIFIED |
| `AiSubgrid + 0x30` | tile-grid pointer | VERIFIED |
| `TileGrid + 0x08` | width | VERIFIED |
| `TileGrid + 0x10` | tile-data pointer | VERIFIED |

`CreateMetaData` (`0x10114fae4`–`0x10114fd10`) independently repeats the
subgrid lookup, dimension checks, tile-grid dereference, `0x10` stride,
metadata-array access/growth, and tile `+0x0c` write. `GetHeightAtTile`,
`GetSurface`, and `GetStateAtTile` provide further independent tile lookup
sites.

### `AiGridTile`

The recovered stride and metadata-index access agree with this Windows-derived
layout:

| Offset | Field | Status | Evidence |
|---:|---|---|---|
| `0x00` | `uint64_t AiFlags` | VERIFIED | `GetStateAtTile` returns the 64-bit value at `0x10334dfe0`; `GetSurface` independently tests flag bytes at `0x10115fd30` and the full word at `0x10115fd88`. |
| `0x08` | `uint16_t MaxHeight` | PROVISIONAL | `GetHeightAtTile` loads this value at `0x101140b74`, converts it to float, and applies the verified `1/50` scale. A second isolated height accessor was not recorded. |
| `0x0a` | `uint16_t MinHeight` | OPEN | Windows layout hypothesis; macOS height consumer not cross-checked in this report. |
| `0x0c` | `uint16_t MetaDataIndex` | VERIFIED | Direct load at `0x10114b340`; `0xffff` null test; `0x10` tile stride. |
| `0x0e` | `uint16_t SurfaceMetaDataIndex` | VERIFIED | Loaded and decoded by `GetSurface` at `0x10115fd40` onward; independent of the metadata-index access at `+0x0c`. |

The ground and cloud surface masks are confirmed on macOS. The
`ESurfaceLayer` overload of `GetSurface` maps its enum through a two-entry
constant table loaded at `0x10115fc74`–`0x10115fc7c`; the ARM64 literal at
`0x107842bd0` contains:

```text
ground mask = 0x00000000ff000000  // bits 24..31
cloud mask  = 0x000000ff00000000  // bits 32..39
```

The second overload uses those masks against the raw flag word and decodes the
tagged `SurfaceMetaDataIndex`. Thus the macOS evidence supports:

```text
GroundSurface = (AiFlags >> 24) & 0xff
CloudSurfaceRaw = (AiFlags >> 32) & 0xff
```

The Windows API adds 38 to a nonzero cloud value when converting it to the
public `SurfaceType`; that enum conversion remains **PROVISIONAL** on macOS.
The Windows base-flags mask (`0x00ffffff`), material bits (`40..45`), and
extra-flags shift (`46`) are layout-consistent but still **PROVISIONAL**, since
the limited macOS pass did not isolate semantic accessors for those fields.

`GetHeightAtTile` confirms the max-height conversion:

```text
worldMaxHeight = *(float*)(subgrid + 0x0c)
               + 2.0f * tile->MaxHeight / 100.0f
               = subgridTranslateY + tile->MaxHeight / 50.0f
```

Evidence: `0x101140b6c`–`0x101140b8c`. The subgrid translation-Y offset
`+0x0c` is **PROVISIONAL** because only this semantic site was retained. The
corresponding `MinHeight` load at tile `+0x0a` remains **OPEN**.

### `AiMetaData`

The common macOS `LEGACY_Set<EntityHandle>` helper immediately adjacent to
`GetMetaData` confirms the array-header layout used here:

```text
+0x00 data pointer
+0x08 int32 capacity
+0x0c int32 size
```

`CreateMetaData` allocates exactly `0x40` bytes at `0x10114fbc4`, zeros three
consecutive array/set headers, initializes the layer at `+0x30`, and copies the
eight-byte `AiTilePos` to `+0x34`. `UpdateMetaData` independently checks the
three sizes at `+0x0c/+0x1c/+0x2c`. When the first set contains one item, it
loads the first handle from metadata `+0x00` and resolves its
`BoundComponent` (`0x1011500b8`–`0x1011500c4`). Its replacement path passes the
metadata base directly to `LEGACY_Set<EntityHandle>::Add` at
`0x101150108`–`0x10115011c`.

| Offset | Field | Status |
|---:|---|---|
| `0x00` | `LEGACY_Set<EntityHandle> Entities` | VERIFIED |
| `0x10` | portal set/array | VERIFIED |
| `0x20` | end-portal set/array | VERIFIED |
| `0x30` | `uint16_t LayerId` | VERIFIED |
| `0x32` | padding | VERIFIED |
| `0x34` | `AiTilePos Position` | VERIFIED |

`sizeof(AiMetaData) == 0x40`. The entity set uses an eight-byte
`EntityHandle`, and `GetEntitiesOnTile` may safely snapshot
`Entities.data[0..size)` after validating the header.

## Call ABIs

All ABIs below use the standard Darwin ARM64 convention. `Vector3f const&` and
other C++ references are pointers in integer registers. None of these anchors
uses the `x8` indirect-result register.

| Function | Arguments | Return | Evidence/confidence |
|---|---|---|---|
| `AiGrid::ToTilePos(Vector3f const&, AiTilePos&, bool) const` | `x0=this`, `x1=&worldPos`, `x2=&outTilePos`, `w3=bool` | `w0=bool` | Mangled signature, callee use, and direct callers. Output is caller-owned eight-byte storage; no `x8`. |
| `AiGrid::GetMetaData(AiTilePos const&) const` | `x0=this`, `x1=&tilePos` | `x0=AiMetaData*` or null | Direct field loads at `0x10114b2e4` onward and the `CheckTile` caller at `0x101141d78`. |
| `AiGrid::CreatePath(float, float, float, CoverManager*)` | `x0=this`, `x1=coverManager`, `s0=bound0`, `s1=bound1`, `s2=bound2` | `w0=int32 pathId` | Stores at `0x1011622e8`–`0x101162328`; caller `ecl::aigrid::CreatePathForCharacter` at `0x102de439c`, which saves the returned ID and resolves it through `PathMap`. |
| `AiGrid::FindPath(int)` | `x0=this`, `w1=pathId` | `void` | Callee map lookup/queueing and multiple direct callers. It returns without establishing a result value. |
| `AiGrid::FindPathImmediate(int, bool)` | `x0=this`, `w1=pathId`, `w2=bool` | `void` | Callee never reads `w2` and returns no meaningful `w0`; teleport caller at `0x104898b6c` ignores the return. |
| `AiGrid::RemovePath(int)` | `x0=this`, `w1=pathId` | `void` | Callee lookup/reset/removal and direct callers. |
| `AiPath::Reset()` | `x0=this` | `void` | `RemovePath` call at `0x101161ef0` and other reset call sites. |

Mixed integer/floating-point arguments to `CreatePath` are in their respective
register banks: `CoverManager*` is `x1`, while the three floats are still
`s0`–`s2`.

## Deferred API feasibility

### Summary

| Lua API | Verdict | Reason |
|---|---|---|
| `GetEntitiesOnTile` | **IMPLEMENTABLE NOW** | Tile conversion and metadata lookup ABIs are known; `AiMetaData + 0x00` is independently verified as the entity-handle set by construction and update/component-resolution code. Snapshot its validated entries. |
| `GetTileDebugInfo` | **NEEDS MORE RE** | Raw flags, ground/cloud masks, max height, metadata ID, and surface-metadata ID are recovered. Exact parity still needs the min-height load and macOS confirmation of the public cloud conversion plus material/extra flag decoding. A reduced raw-flags diagnostic could be implemented now. |
| `BeginPathfinding` | **NEEDS MORE RE** | `CreatePath` is callable and its pool/map behavior is known, but high-level character setup populates many `AiPath` fields beyond source, target, and bounds. Calling only `CreatePath` and writing a target vector would create an incompletely configured request. |
| `FindPath` | **IMPLEMENTABLE NOW, with validation** | ID lookup, queueing, immediate completion, result flags, and the node polyline layout are verified. Validate the ID through `PathMap`, reject `-1337`, call `FindPath`, optionally call `FindPathImmediate`, then read nodes only when complete and `GoalFound`. |
| `ReleasePath` | **IMPLEMENTABLE NOW** | `RemovePath(int)` ABI and complete cleanup behavior are verified. Reject invalid/unknown IDs before calling it. |
| `GetPathById` | **IMPLEMENTABLE NOW** | `PathMap` container and node layouts are verified. It can safely return a snapshot of status fields and nodes without exposing a raw native pointer. |
| `GetActivePathfindingRequests` | **IMPLEMENTABLE NOW** | `Paths` data/size and active-queue semantics are verified in `FindPath`, `UpdatePathfinding`, and `RemovePath`. Iterate a snapshot defensively because update/removal mutates the ordered array. |

### Safety notes

- Never pass `-1337` or an unresolved ID to `FindPathImmediate`.
- Resolve IDs through `PathMap`; do not confuse a pool index or active-list
  index with a path ID.
- Do not retain `AiPath*` or node-array pointers across calls that can update,
  release, or grow containers. Copy values into Lua-owned storage.
- Avoid exposing raw path pointers to Lua. An ID plus copied status/polyline is
  safer and preserves `RemovePath` ownership.
- `Paths` contains queued/active work only. Enumerating all allocated/in-use
  requests requires `PathMap` or `PathPool`, depending on the intended API
  semantics.
- `FindPathImmediate` is synchronous and repeatedly drives the global grid
  update routine. It should only be called on the game thread and only for a
  validated request.

## Recommended implementation order

1. **`GetPathById`** — implement the verified map lookup as a private helper and
   return copied state only.
2. **`ReleasePath`** — use the same validated lookup, then call
   `RemovePath(int)`.
3. **`GetActivePathfindingRequests`** — snapshot `Paths[0..size)` and convert
   each pointer to a safe Lua value.
4. **`FindPath`** — queue, optionally finish synchronously, then copy the
   verified `AiPathNode` polyline.
5. **`GetEntitiesOnTile`** — use `ToTilePos`, `GetMetaData`, and a defensive
   snapshot of the verified entity set at metadata `+0x00`.
6. **`GetTileDebugInfo`** — enable full Windows-compatible output after
   confirming min height and the remaining public flag conversions.
7. **`BeginPathfinding`** — recover or reuse the engine's complete
   character-path setup routine rather than manually initializing a partial
   `AiPath`.

## Minimal remaining RE

Only narrow targeted checks remain:

1. Confirm `AiGridTile +0x0a` (`MinHeight`), the public cloud-surface enum
   conversion, and the material/extra flag decoders.
2. For `BeginPathfinding`, identify a stable high-level engine entry point or
   enumerate every field written by both client and server
   `CreatePathForCharacter` callers.

Until those checks are complete, the OPEN fields above should remain absent
from runtime bindings rather than being guessed from the Windows layout.

## 2026-08-03 — MinHeight +0x0a recon — Wave 7 B3

### Method

The Ghidra HTTP bridge at `127.0.0.1:8080` did not respond, so this pass used
the installed universal Mach-O and static `llvm-objdump` disassembly only. The
fat header reports the ARM64 slice offset as decimal `257261568`, which is
`0xf558000`, confirming the file-offset correction already recorded above:

```text
file_offset = 0xf558000 + (vaddr - 0x100000000)
```

The Windows reference pairs `AiGridTile::MaxHeight` at `+0x08` and
`MinHeight` at `+0x0a`, with both using a `1/50` scale. The macOS search first
rechecked the known max-height accessor, then inspected tile-selection
routines for a sibling `+0x0a` load with the same conversion.

### Disassembly evidence

`eoc::AiGrid::GetHeightAtTile` independently retains the known max-height
load and conversion:

```asm
101140b6c  ldr    s0, [x8, #0xc]       ; subgrid Translate.y
101140b70  add    x8, x9, w10, sxtw #4 ; 0x10-byte tile stride
101140b74  ldr    h1, [x8, #0x8]       ; tile MaxHeight
101140b78  ucvtf  s1, s1
101140b7c  fadd   s1, s1, s1
101140b80  mov    w8, #0x42c80000      ; 100.0f
101140b88  fdiv   s1, s1, s2           ; 2 * raw / 100 = raw / 50
101140b8c  fadd   s0, s0, s1
```

`eoc::AiGrid::ToClosestTilePos` reads both members from the same tile and
applies the identical scale:

```asm
101139978  add    x16, x17, x1, lsl #4 ; 0x10-byte tile stride
10113997c  ldr    h20, [x16, #0x8]     ; MaxHeight
101139980  ucvtf  s20, s20
101139984  fadd   s20, s20, s20
10113998c  fdiv   s20, s20, s21        ; divide by 100.0f
101139990  fadd   s19, s19, s20        ; Translate.y + MaxHeight/50
1011399a0  fsub   s20, s19, s20        ; recover Translate.y
1011399a4  ldr    h21, [x16, #0xa]     ; MinHeight
1011399a8  ucvtf  s21, s21
1011399ac  fadd   s21, s21, s21
1011399b4  fdiv   s21, s21, s22        ; divide by 100.0f
1011399b8  fadd   s20, s20, s21        ; Translate.y + MinHeight/50
```

`eoc::AiGrid::ToTilePos` supplies a second independent paired site:

```asm
101161e0c  ldr    h7, [x15, #0x8]      ; MaxHeight
101161e10  ucvtf  s7, s7
101161e14  fadd   s7, s7, s7
101161e1c  fdiv   s7, s7, s17
101161e20  fadd   s16, s16, s7
101161e2c  fsub   s7, s16, s7          ; recover Translate.y
101161e30  ldr    h16, [x15, #0xa]     ; MinHeight
101161e34  ucvtf  s16, s16
101161e38  fadd   s16, s16, s16
101161e40  fdiv   s16, s16, s17
101161e44  fadd   s7, s7, s16          ; Translate.y + MinHeight/50
```

### Verdict and API consequence

**CONFIRMED / VERIFIED:** `AiGridTile +0x0a` is `uint16_t MinHeight`, paired
with `uint16_t MaxHeight` at `+0x08`; both convert to world height as
`subgrid->Translate.y + raw / 50.0f`.

The diagnostic `Ext.Level.GetTileRawDebugInfo(x, z)` may therefore expose
both `MinHeight` and `MaxHeight`. Full Windows-compatible
`Ext.Level.GetTileDebugInfo` remains deferred: public base/material/extra flag
conversion and the cloud-surface enum adjustment are still not fully confirmed
on macOS. This finding does not grant parity credit to the raw diagnostic.

## 2026-08-03, Subgrid world bounds — GetHeightsAt

### Windows contract

The Lua binding constructs an `AiWorldPos` from `(x, 0, z)` and delegates
directly to `AiGrid::GetHeightsAt` (`BG3Extender/Lua/Libs/Level.inl:162-166`).
The implementation obtains the patch's candidate subgrid-ID span, iterates it
in stored span order, resolves each ID through `Subgrids`, rejects candidates
whose `WorldToTilePos` bounds test fails, and skips blocker tiles
(`BG3Extender/GameDefinitions/Ai.inl:406-422`). For every remaining subgrid it
appends exactly one value:

```cpp
subgrid->Translate.y + tile->GetLocalMaxHeight()
```

It does not append `MinHeight`, sort, or deduplicate. The observable result
order is therefore the `SubgridsAtPatch` candidate-array order. `WorldToTilePos`
uses a half-open tile rectangle (`0 <= x < SizeX`, `0 <= z < SizeY`) at
`BG3Extender/GameDefinitions/Ai.inl:262-280`. The tile is selected with
`floor((world - origin) / CellSize)`, and the tile address is
`Tiles[x + y * Width]` (`BG3Extender/GameDefinitions/Ai.inl:311-315`). The
Windows declarations place `Translate`, `CellSize`, and sizes consecutively
and define `HeightScale = 1/50` (`BG3Extender/GameDefinitions/Ai.h:150-177`).

### Static method

The Ghidra HTTP bridge did not respond. `otool -f` re-confirmed the installed
universal binary's ARM64 slice offset as decimal `257261568` (`0xf558000`). A
thin ARM64 slice was produced in a temporary directory and disassembled with
Xcode `llvm-objdump`; the equivalent direct-read relationship remains:

```text
file_offset = 0xf558000 + (vaddr - 0x100000000)
```

No game process was launched.

### Creation layout: Translate, CellSize, and encoded origin

`eoc::AiGrid::CreateSubgrid` receives the origin `Vector3f const&` in `x3`.
Here `x24` is `subgrid + 0x08`; the paired and scalar copies establish the
three consecutive `Translate` members:

```asm
10115d7c8  ldr    x9, [x27]          ; input origin x/y
10115d7cc  str    x9, [x24]          ; subgrid +0x08/+0x0c
10115d7d0  ldr    w9, [x27, #0x8]   ; input origin z
10115d7d4  str    w9, [x24, #0x8]   ; subgrid +0x10
```

The same constructor fixes the cell size at `0.5f` and stores the supplied
tile dimensions. `x25` is `subgrid + 0x70`, making the two negative offsets
`+0x14` and `+0x18`:

```asm
10115d71c  stp    w23, w22, [x25, #-0x58] ; SizeX/SizeY +0x18/+0x1c
10115d72c  mov    w9, #0x3f000000          ; 0.5f
10115d73c  stur   w9, [x25, #-0x5c]        ; CellSize +0x14
```

`CreateSubgrid` also decomposes origin x/z into global half-cell coordinates
and local remainders, then stores both pairs:

```asm
10115d7a4  fadd.2s  v0, v2, v2       ; origin x/z * 2
10115d7a8  frintm.2s v0, v0           ; floor
10115d7ac  fcvtzs.2s v8, v0           ; WorldPos global x/z
10115d7b4  fmul.2s  v0, v0, v1       ; * -0.5
10115d7b8  fadd.2s  v0, v2, v0       ; WorldPos local remainders
10115d7c0  stp      d8, d0, [x19, #0x40]
```

Thus `+0x40/+0x44` are the signed global x/z pair and `+0x48/+0x4c` are the
float local x/z pair.

### Native world-to-tile bounds test

`eoc::AiGrid::ToTilePos(Vector3f const&, AiTilePos&, bool) const` at
`0x1011619c0` uses the encoded origin—not direct `Translate.x/z` loads—for its
actual containment test. Its zero-local fast path subtracts the global origin
and applies the already verified sizes:

```asm
101161ae8  ldr    d3, [x10, #0x48]    ; origin local x/z
101161aec  fabs.2s v4, v3
101161b0c  ldr    d3, [x10, #0x40]    ; origin global x/z
101161b10  sub.2s v3, v2, v3          ; tile x/z
101161b2c  ldr    w13, [x10, #0x18]   ; SizeX
101161b38  ldr    w12, [x10, #0x1c]   ; SizeY
```

The general path reads `CellSize`, reconstructs world coordinates, subtracts
the local origin, divides and floors, then subtracts the global origin:

```asm
101161b48  ldr      s4, [x10, #0x14]  ; CellSize
101161b58  fmla.2s  v7, v5, v4[0]     ; global * CellSize + local
101161b5c  fsub.2s  v3, v7, v3        ; subtract origin local x/z
101161b60  fdiv.2s  v3, v3, v6
101161b64  frintm.2s v3, v3
101161b68  fcvtzs.2s v3, v3
101161b6c  ldr      d4, [x10, #0x40]  ; origin global x/z
101161b70  sub.2s   v3, v3, v4
```

`AiGrid::ToClosestTilePos` independently repeats the same field accesses at
`0x101139668-0x101139740` and `0x1011398a0-0x101139920`. Algebraically, the
encoded calculation is `floor((world - Translate.xz) / CellSize)` because the
constructor derives both representations from the same origin, but matching
the native `AiWorldPos` path avoids relying on that algebraic invariant after
creation. At the confirmed `0.5f` cell size, each tile spans 0.5 world units;
subgrid bounds are origin-inclusive and `(origin + size * 0.5)`-exclusive.

Finally, the multi-candidate loop rejects the tile when flags bit zero is set:

```asm
101161df8  ldrb   w1, [x16, x1]       ; low byte of AiGridTile::Flags
101161dfc  tbnz   w1, #0x0, 0x101161cc8 ; blocker: skip candidate
```

The independent `ToClosestTilePos` loop has the same test at
`0x101139968-0x101139970`.

### Offset verdicts and implementation consequence

| Offset/value | Verdict | Evidence |
|---:|---|---|
| `AiSubgrid +0x08` `Translate.x` | **CONFIRMED** | `CreateSubgrid` copies input origin x to the first word at `+0x08`. |
| `AiSubgrid +0x0c` `Translate.y` | **CONFIRMED** | Creation copy above plus the previously verified height consumers. |
| `AiSubgrid +0x10` `Translate.z` | **CONFIRMED** | `CreateSubgrid` copies input origin z to `+0x10`. |
| `AiSubgrid +0x14` `CellSize` | **CONFIRMED** | Initialized to `0.5f`; independently read by both tile-position routines. |
| `AiSubgrid +0x40/+0x44` world globals | **CONFIRMED** | Constructed from `floor(origin * 2)` and consumed independently by both position routines. |
| `AiSubgrid +0x48/+0x4c` world locals | **CONFIRMED** | Constructed as the half-cell remainder and consumed independently by both position routines. |
| tile world width/depth | **CONFIRMED: `0.5f`** | Constructor's `CellSize`; native conversion divides by the stored value. |
| `AiGridTile::Flags` bit 0 blocker | **CONFIRMED** | Independent `tbnz #0` rejection sites in both position routines. |

All offsets needed for a fail-closed multi-subgrid walk are confirmed. The
macOS implementation may snapshot the bounded subgrid map, apply the exact
native bounds mapping to every entry, skip blocker tiles, and append only
`Translate.y + MaxHeight/50`. Bucket traversal supplies deterministic map
order; Windows exposes its patch candidate-array order and performs no
height-based sorting, so callers must not infer vertical sorting from either
result.
