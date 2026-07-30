# Wave 3 Builder A — Ext.Level report

## Outcome

Wave 3 Goal 3.1 gains two engine-backed APIs:

- `Ext.Level.SweepCylinderClosest`
- `Ext.Level.SweepCylinderAll`

The two tile-query names and five requested pathfinding names are registered as
honest deferrals. Each warns once and returns `nil` or `false`; none reports
success or fabricates an empty engine result.

The pre-existing tier-2 calls to `RaycastClosest` and `RaycastAll` now pass
positional vec3 tables (`{x, y, z}`), matching `lua_level.c::read_vec3()`.

## Cylinder implementation and VMT evidence

The campaign plan's proposed VMT indices 13/17 are the Windows layout, not the
current macOS ARM64 layout. The installed binary is:

`~/Library/Application Support/Steam/steamapps/common/Baldurs Gate 3/Baldur's Gate 3.app/Contents/MacOS/Baldur's Gate 3`

Evidence gathered read-only from its ARM64 slice:

| Evidence | Closest | All |
|---|---:|---:|
| Named `nm` symbol | `0x105c4ea38` | `0x105c4eb7c` |
| `PhysXScene` vtable entry | index 14 | index 18 |
| Vtable word address | `0x108829270` | `0x108829290` |

The relevant vtable words are:

```text
0x108829260  0x105c4e948  0x105c4e9d8
0x108829270  0x105c4ea38  0x105c4ea40
0x108829280  0x105c4eaa8  0x105c4eb1c
0x108829290  0x105c4eb7c  0x105c4eb84
```

`nm | c++filt` identifies `0x105c4ea38` and `0x105c4eb7c` as
`phx::PhysXScene::SweepCylinderClosest` and
`phx::PhysXScene::SweepCylinderAll`. Both take
`(Vector3f extents, Vector3f source, Vector3f destination, hit output, flags...)`.

The extra macOS slot comes from the Itanium C++ ABI's two destructor entries.
This also explains why `ghidra/offsets/PHYSICS_LEVEL.md` described indices 13
and 17-19 as unknown even though the Windows declaration order suggested
13/17.

Norbyte's Windows history provides independent call-shape evidence: commit
`13de4b0a` added Lua cylinder bindings taking `source`, `destination`, and a
vec3 `extents`, and dispatched the same arguments to the virtual methods. The
bindings were removed in `8f11a7c0`; the virtual declarations remained, and
that removal commit gives no cylinder-specific safety rationale.

On ARM64, the three `Vector3f const&` arguments and the hit output are passed
as pointers. The functions return `bool`, so no x8 indirect-result buffer is
required. `PhysicsHitAll` remains an explicit output-reference argument rather
than a struct return.

### Adjacent pre-existing dispatch risk

The older ray/sphere/capsule/box constants in `level_manager.c` remain
Windows-derived. The current ARM64 vtable shows that the ray/sweep range is
shifted by the dual destructor entries. Builder A did not silently rewrite all
of those pre-existing call ABIs while adding the cylinder functions. The
orchestrator must run the focused raycast and sweep live tests below before
declaring the broader physics surface validated.

## Windows semantics studied

### Tile queries

Windows `BG3Extender/Lua/Libs/Level.inl`:

- `GetEntitiesOnTile(pos)` converts the world vec3 to an `AiWorldPos`, calls
  `AiGrid::ToTilePos`, looks up tile metadata, and returns that metadata's
  `EntityHandle` array.
- `GetTileDebugInfo(pos)` returns flags, ground/cloud surface, material,
  extra flags, subgrid/tile coordinates, translated min/max height, metadata
  indices, and metadata entities.

### Pathfinding

Windows behavior:

- `BeginPathfinding` allocates an `AiPath`, records it as pending and in
  `AiGrid::Paths`, sets the source entity and target, and later invokes a Lua
  callback after completion.
- `FindPath` validates `InUse`, resolves a path ID, invokes
  `AiGrid::FindPathImmediate`, and returns `GoalFound`.
- `ReleasePath` frees only an in-use owned request.
- `GetPathById` reads `AiGrid::PathMap`.
- `GetActivePathfindingRequests` enumerates in-use entries in `PathPool`.

## Honest deferrals

| Lua API | Stub result | Why it is deferred |
|---|---|---|
| `GetEntitiesOnTile` | `nil` | macOS tile/subgrid and metadata layouts are not verified |
| `GetTileDebugInfo` | `nil` | same layouts plus flag/surface field offsets are not verified |
| `BeginPathfinding` | `nil` | path allocation ABI, ownership, callback lifecycle, and `AiPath` layout are not verified |
| `FindPath` | `false` | macOS immediate-find ABI and `AiPath` state fields are not verified |
| `ReleasePath` | `false` | no verified release routine or ownership layout |
| `GetPathById` | `nil` | `PathMap` layout is not verified |
| `GetActivePathfindingRequests` | `nil` | `PathPool` and `AiPath::InUse` offsets are not verified |

The current ARM64 symbols demonstrate why the Windows code cannot be copied:

```text
0x1011619c0 eoc::AiGrid::ToTilePos(Vector3f const&, AiTilePos&, bool) const
0x101162020 eoc::AiGrid::CreatePath(float, float, float, eoc::CoverManager*)
0x101162a4c eoc::AiGrid::FindPath(int)
0x101165cec eoc::AiGrid::FindPathImmediate(int, bool)
0x10114b2e4 eoc::AiGrid::GetMetaData(eoc::AiTilePos const&)
```

Those signatures differ materially from the Windows Lua layer's structures
and calls. No path-pool/map or tile-metadata offsets are documented in
`ghidra/offsets/`, so implementing them would require guessed layouts.

## Tier-2 tests

Added:

- `Parity.Level.SweepCylinderClosest`
- `Parity.Level.SweepCylinderAll`
- `Parity.Level.GetEntitiesOnTileDeferred`
- `Parity.Level.GetTileDebugInfoDeferred`
- `Parity.Level.BeginPathfindingDeferred`
- `Parity.Level.FindPathDeferred`
- `Parity.Level.ReleasePathDeferred`
- `Parity.Level.GetPathByIdDeferred`
- `Parity.Level.GetActivePathfindingRequestsDeferred`

Corrected:

- `Parity.Level.RaycastShape`: six scalar coordinates replaced by two
  positional vec3 tables.
- `Parity.Level.RaycastAll`: keyed tables replaced by positional vec3 tables.

The new C-string test chunks were extracted and syntax-checked with local Lua
before handoff.

## Offline verification

- `cd build && cmake --build .`: succeeded; universal ARM64/x86_64 dylib linked.
- `./build/bin/bg3se_test_tier0`: **55/55 passed**.
- `PYTHONPATH=tools python3 -m pytest tests/harness/ -q`: Builder A's first
  post-change run was **191 passed**; a final run after concurrent Builder B
  additions was **194 passed**, with no failures.
- `git diff --check` on the Builder A files: passed.

The build still reports one C99 overlength warning in the concurrently edited
Entity tier-2 string at `src/lua/lua_ext.c:2186`. Builder A's Level tests were
split into their own chunk and do not produce that warning. Sandbox policy
also prevented the build's optional copy into the installed game bundle; the
build artifact itself completed successfully.

## Exact live verification for the orchestrator

Builder A did not launch BG3 or touch `/tmp/bg3se.sock`.

From a stopped game and the repository root:

1. Run the focused Level tier-2 group:

   ```bash
   PYTHONPATH=tools python3 -m bg3se_harness test Parity.Level --headless --continue --tier 2
   ```

2. If that run crashes or stalls, isolate the now-executing pre-existing
   raycast tests first:

   ```bash
   PYTHONPATH=tools python3 -m bg3se_harness test Parity.Level.Raycast --headless --continue --tier 2
   ```

   A native crash here is evidence for the adjacent Windows-derived VMT/ABI
   risk described above, not a Lua argument-shape failure.

3. Isolate the two new engine-backed cylinder calls:

   ```bash
   PYTHONPATH=tools python3 -m bg3se_harness test Parity.Level.SweepCylinderClosest --headless --continue --tier 2
   PYTHONPATH=tools python3 -m bg3se_harness test Parity.Level.SweepCylinderAll --headless --continue --tier 2
   ```

   Both must complete without a native crash. Closest may return `nil` or a
   hit table; All must return a table.

4. With a loaded save, run a geometry-producing smoke query at the host:

   ```bash
   PYTHONPATH=tools python3 -m bg3se_harness run "local h=Osi.GetHostCharacter(); local x,y,z=Osi.GetPosition(h); local s={x,y+2,z}; local d={x,y-2,z}; local e={0.25,0.5,0.25}; local c=Ext.Level.SweepCylinderClosest(s,d,e); local a=Ext.Level.SweepCylinderAll(s,d,e); print('CYLINDER_VERIFY closest='..type(c)..' all='..type(a)..' count='..tostring(type(a)=='table' and #a or -1))"
   ```

   Expected: `closest=nil` or `closest=table`, `all=table`, no crash, and hit
   tables (when present) contain `Position`, `Normal`, and numeric `Distance`.

5. In the same log, verify each deferred API emits its `Ext.Level.<name> is
   deferred on macOS` warning no more than once and that all seven deferral
   contract tests pass.

6. Finally run the complete live tier:

   ```bash
   PYTHONPATH=tools python3 -m bg3se_harness test --headless --continue --tier 2
   ```

Do not promote the seven deferrals to functional parity until Ghidra work
recovers versioned macOS layouts/call wrappers and live tests exercise real
tile metadata and real path ownership.
