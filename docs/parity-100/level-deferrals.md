I’ll use the BG3SE macOS Ghidra skill because this is a targeted ABI and reverse-engineering audit. I’ll stay read-only, inspect both implementations and the named evidence files, then give a per-API risk/effort ranking with exact citations.
The skill’s ARM64 guidance makes one distinction especially important here: “known symbol” is not the same as “safe callable signature.” I’m checking the native layouts and call sites before treating any discovered address as an unlock.
The evidence already narrows the asymmetry: tile debug is mostly a read-only layout problem; path creation is an ownership/state-initialization problem; raycasts are a C++ value-wrapper ABI problem. I’m now tracing the Windows field semantics and the exact macOS call boundaries so the effort estimates reflect actual RE scope.
## Answer

Attempt `GetTileDebugInfo` first, followed by `RaycastAny`, `RaycastAll`, and `RaycastClosest`. Leave the current low-level `BeginPathfinding` approach deferred permanently unless a stable engine-owned `CreatePathForCharacter` entry point can replace manual `AiPath` construction.

Assuming one focused RE session is roughly 3–4 hours:

| Rank | API | Estimated RE effort | Corruption risk | Recommendation |
|---:|---|---:|---:|---|
| 1 | `GetTileDebugInfo` | 1–2 sessions | Low, 1/5 | Implement raw form quickly; finish decoder proof afterward |
| 2 | `RaycastAny` | 1 session | High, 4/5 | Best ABI-validation target because it has no hit output |
| 3 | `RaycastAll` | 2 sessions | High, 4/5 | Attempt after `RaycastAny`; audit output destruction first |
| 4 | `RaycastClosest` | 2–3 sessions | Critical, 5/5 | Attempt zero/empty `ls::Function` path; stop if semantics differ |
| 5 | `BeginPathfinding` | 5–8 sessions | Critical, 5/5 | Do not expose a manually populated `AiPath` |

Confidence: high on the ranking and tile/pathfinding conclusions; medium-high on the zeroed raycast aggregate approach until verified in a live build.

## Evidence

- The deferral ledger identifies the three by-value raycast ABI blockers, the open tile height/decoder work, and the incomplete path field mapping at the native call site: [docs/deferrals.md:20](/Users/tomdimino/Desktop/Programming/game-modding/bg3/bg3se-macos/docs/deferrals.md:20).
- The corrected physics VMT is:
  - `[8]` `RaycastClosest` → `0x105c4e784`
  - `[9]` `RaycastAll` → `0x105c4e8b0`
  - `[10]` `RaycastAny` → `0x105c4e8c8`  
  See [PHYSICS_VMT_AUDIT.md:135](/Users/tomdimino/Desktop/Programming/game-modding/bg3/bg3se-macos/ghidra/offsets/PHYSICS_VMT_AUDIT.md:135). The audit also explains the prior Itanium dual-destructor slot shift at [PHYSICS_VMT_AUDIT.md:31](/Users/tomdimino/Desktop/Programming/game-modding/bg3/bg3se-macos/ghidra/offsets/PHYSICS_VMT_AUDIT.md:31).
- The native ABI hazards—references versus scalars, argument order, `ls::Function`, and output ownership—are explicitly unresolved at [PHYSICS_VMT_AUDIT.md:234](/Users/tomdimino/Desktop/Programming/game-modding/bg3/bg3se-macos/ghidra/offsets/PHYSICS_VMT_AUDIT.md:234).
- The macOS implementation deliberately fails closed for raycasts at [level_manager.c:1298](/Users/tomdimino/Desktop/Programming/game-modding/bg3/bg3se-macos/src/level/level_manager.c:1298), with corresponding Lua warnings at [lua_level.c:186](/Users/tomdimino/Desktop/Programming/game-modding/bg3/bg3se-macos/src/lua/lua_level.c:186).
- Tile `MinHeight` at `+0xa` is still open, while metadata and some other fields are proven or provisional: [AIGRID_PATHFINDING.md:298](/Users/tomdimino/Desktop/Programming/game-modding/bg3/bg3se-macos/ghidra/offsets/AIGRID_PATHFINDING.md:298). Flag layouts are catalogued at [AIGRID_PATHFINDING.md:311](/Users/tomdimino/Desktop/Programming/game-modding/bg3/bg3se-macos/ghidra/offsets/AIGRID_PATHFINDING.md:311).
- `CreatePath` is known at `0x101162020`, but many `AiPath` fields remain provisional: [AIGRID_PATHFINDING.md:48](/Users/tomdimino/Desktop/Programming/game-modding/bg3/bg3se-macos/ghidra/offsets/AIGRID_PATHFINDING.md:48) and [AIGRID_PATHFINDING.md:128](/Users/tomdimino/Desktop/Programming/game-modding/bg3/bg3se-macos/ghidra/offsets/AIGRID_PATHFINDING.md:128).
- `CreatePath` allocates/reuses the `0x2a8`-byte object but does not fully initialize a character request or place it in `Paths`; that happens later through `FindPath`: [AIGRID_PATHFINDING.md:181](/Users/tomdimino/Desktop/Programming/game-modding/bg3/bg3se-macos/ghidra/offsets/AIGRID_PATHFINDING.md:181).
- The Windows implementation performs more than allocation: it calls `SetSourceEntity`, `SetTarget`, tracks a callback, pushes the request into `Paths`, and releases it after completion: [Level.inl:30](/Users/tomdimino/Desktop/Programming/game-modding/bg3/bg3se/BG3Extender/Lua/Libs/Level.inl:30), [Level.inl:62](/Users/tomdimino/Desktop/Programming/game-modding/bg3/bg3se/BG3Extender/Lua/Libs/Level.inl:62), and [Level.inl:169](/Users/tomdimino/Desktop/Programming/game-modding/bg3/bg3se/BG3Extender/Lua/Libs/Level.inl:169).

## Details

### 1. `GetTileDebugInfo`

Precise blocker:

- The tile lookup and `0x10`-byte tile stride are understood.
- `Flags +0x0`, maximum-height storage at `+0x8`, metadata `+0xc`, and surface metadata `+0xe` are substantially mapped.
- The meaning and conversion of `MinHeight +0xa` remain unproven.
- Ground decoding is credible, but cloud/material/extra flag decoders are still inherited largely from Windows assumptions rather than proven from macOS code. The Windows formulas are visible at [Ai.h:14](/Users/tomdimino/Desktop/Programming/game-modding/bg3/bg3se/BG3Extender/GameDefinitions/Ai.h:14).

Most promising unlock:

1. Decompile and cross-reference:
   - `GetHeightAtTile` — `0x101140b14`
   - `GetHeightInArea` — `0x1011393a0`
   - `GetSurface` — `0x10115fc04` and `0x10115fca0`
   - `SetSurface` — `0x10115f6d0`
   - `ReapplyOverlappingObjectMasks` — `0x101160164`
   - `GetStateAtTile` — `0x10334dfe0`
2. Search their callers for:
   - `ldrh` loads from tile `+0x8` and `+0xa`
   - division/multiplication by `50.0`
   - `ubfx`/shifts corresponding to bits `24`, `32`, `40`, and `46`
3. Runtime probe several controlled locations:
   - flat terrain
   - stairs or a steep ramp
   - water/cloud surface
   - overlapping ground and cloud effects
4. For each tile, record raw `+0x8/+0xa`, compare both `/50 + Translate` candidates with `GetHeightsAt`, and observe which value follows the top and bottom of sloped geometry.
5. Apply known surfaces in a disposable save, diff the full 64-bit flags before/after, and validate the decoded bits against visible/gameplay state.

Effort: 1–2 sessions.

Risk: low, 1/5, provided the first implementation only reads tile memory and bounds-checks coordinates. Incorrect decoding gives wrong Lua data, not normally memory corruption.

Cheaper alternative:

Expose a clearly named raw/debug result now:

```text
RawFlags
RawMinHeight
RawMaxHeight
MetadataIndex
SurfaceMetadataIndex
Entities
```

The current Lua stub is at [lua_level.c:612](/Users/tomdimino/Desktop/Programming/game-modding/bg3/bg3se-macos/src/lua/lua_level.c:612). A raw version avoids presenting provisional enum decoders as parity. It should not be called full `GetTileDebugInfo` parity until `MinHeight` and flag meanings are proven.

### 2. `RaycastAny`

Precise blocker:

`RaycastAny` takes `ls::Optional<PhysicsSceneScopedReadLock&>` by value after the register arguments. Calling it requires exact knowledge of the optional’s size, engagement byte, stack offset, and alignment. A falsely engaged optional could make the engine dereference a bogus lock or skip its own synchronization.

Direct disassembly sharpens this substantially:

- Wrapper: `0x105c4e8c8`
- Worker: `0x105c58adc`
- The worker tests the optional engagement byte; the all-zero case enters an internal scene-lock path.

This strongly suggests an empty optional is a 16-byte zero aggregate. What remains unproven is that a C/C++ caller emits it in precisely the stack layout expected by the shipped compiler ABI.

Most promising unlock:

1. Define a temporary ABI-only representation equivalent to two zeroed 64-bit words.
2. Compile a minimal C++ call shim with the same function signature.
3. Inspect its ARM64 assembly and verify:
   - register arguments remain in `x0–x7`
   - the trailing object index and optional words land at the exact offsets read by `0x105c4e8c8`
   - there is no unexpected indirect aggregate convention
4. Call VMT slot `[10]`, not a hard-coded earlier slot.
5. Runtime-probe:
   - open sky → false
   - known wall/floor crossing → true
   - excluded collision groups → false
   - repeated calls during scene activity and level transitions
6. Cross-check its boolean result against a subsequently proven closest/all call and the existing sweep approximation.

Effort: 1 focused session; 1–2 if the compiler does not reproduce the expected stack layout and an assembly shim is needed.

Risk: high, 4/5. The result has no output container, making this the safest raycast experiment, but bad optional layout can still dereference garbage or suppress locking.

Cheaper alternative:

Use an epsilon-radius `SweepSphereClosest` and convert hit/no-hit to a boolean. The sweep code already exists at [level_manager.c:1081](/Users/tomdimino/Desktop/Programming/game-modding/bg3/bg3se-macos/src/level/level_manager.c:1081). It is useful for gameplay queries but is not exact: it can hit nearby geometry that a zero-width ray would miss.

### 3. `RaycastAll`

Precise blocker:

It shares the by-value empty-optional ABI problem with `RaycastAny`, at VMT `[9]` / `0x105c4e8b0`. It also returns a nontrivial `ls::PhysicsHitAll` containing six array-like members. The current header assumes the inner arrays are game-owned and should not be freed, but native symbols exist for:

- `ls::PhysicsHitAll` constructor — `0x1010df4a8`
- `ls::PhysicsHitAll` destructor — `0x1010df888`

Therefore, output lifetime is a second blocker. Blindly zeroing 96 bytes may leak allocations, while running the destructor on an incorrectly constructed object could free invalid pointers.

Most promising unlock:

1. Reuse the optional ABI proof from `RaycastAny`.
2. Decompile the `PhysicsHitAll` constructor and destructor.
3. Inspect several native `RaycastAll` callers to establish:
   - whether they construct before the call
   - which of the six arrays the callee allocates
   - whether arrays are borrowed, transferred, or destroyed by the output destructor
4. Use the native constructor/destructor if they form a stable pair; otherwise reproduce only after every field is mapped.
5. Runtime-probe:
   - zero-hit, one-hit, and multiple-hit rays
   - array pointer/size/capacity invariants
   - finite positions/normals
   - repeat thousands of calls while watching resident memory
   - level unload/reload to expose retained scene allocations
6. Copy all hits into extender-owned storage before destroying the native output.

Effort: about 2 sessions after `RaycastAny`; 3 if ownership differs among the six hit categories.

Risk: high, 4/5. Bad array ownership can produce leaks, double-frees, or stale scene pointers.

Cheaper alternative:

Use a very small-radius `SweepSphereAll`, then sort and deduplicate hits. This will over-report near misses and may order hits differently from a real raycast, so it should be labeled emulation.

### 4. `RaycastClosest`

Precise blocker:

VMT `[8]` / `0x105c4e784` receives `ls::Function<bool(PhysicsShape const*)>` by value. This is more dangerous than the optional because the object may contain a method table, captured-object storage, copy/move/destruction hooks, and an indirect callback invocation. The Windows implementation supplies an always-true callback at [Level.inl:240](/Users/tomdimino/Desktop/Programming/game-modding/bg3/bg3se/BG3Extender/Lua/Libs/Level.inl:240).

Direct disassembly indicates:

- The wrapper copies a `0x40`-byte function object into local storage.
- It checks the source object’s leading method-table field before copying.
- The worker at `0x105c573cc` has a distinct branch when that field is null.
- The wrapper only destroys the local callable when it is nonempty.

A zeroed `0x40`-byte function object may therefore express “no filter,” with the worker’s empty branch accepting the default behavior. That is promising but not yet proven equivalent to the Windows always-true callback.

Most promising unlock:

1. Decompile:
   - wrapper `0x105c4e784`
   - worker `0x105c573cc`
   - all indirect functions called by the wrapper’s copy/destruction paths
2. Determine the exact semantics of the worker’s empty-function branch. Confirm that it accepts every candidate rather than rejecting all or using a different engine filter.
3. Compile and disassemble a C++ shim that passes a zeroed, correctly aligned `0x40`-byte trailing aggregate.
4. Confirm the wrapper reads the function-object pointer and the two object indices from the expected caller-stack offsets.
5. Runtime-probe against controlled geometry:
   - unobstructed ray
   - single wall
   - two aligned walls, verifying the nearer hit
   - include/exclude masks
   - shapes that would be affected by a filter callback
6. Stress the empty path. Do not attempt a fabricated nonempty `ls::Function` unless its copy, invoke, and destruction tables are all identified.

Effort: 2 sessions if empty means “accept all”; 3 or more if a genuine always-true callable must be reconstructed.

Risk: critical, 5/5. A malformed nonempty `ls::Function` can cause immediate indirect calls through attacker-like garbage pointers or invoke a destructor on invalid captured storage.

Cheaper alternative:

Use epsilon-radius `SweepSphereClosest`. If the empty-function path does not exactly mean “accept all,” retain the emulation and leave native `RaycastClosest` deferred rather than synthesizing a callable object.

### 5. `BeginPathfinding`

Precise blocker:

Calling `CreatePath` at `0x101162020` only produces/reuses an engine `AiPath` and performs limited baseline initialization. The character-specific data populated by `CreatePathForCharacter` remains unmapped:

- source entity and source position
- target data
- standing/moving bounds
- collision masks
- movement/template parameters
- processed pathfinding settings
- cover manager and ignored entities
- request state and ownership

Many relevant offsets are explicitly provisional at [AIGRID_PATHFINDING.md:128](/Users/tomdimino/Desktop/Programming/game-modding/bg3/bg3se-macos/ghidra/offsets/AIGRID_PATHFINDING.md:128).

Full Lua parity also requires the request lifecycle, not merely returning an ID. The Windows implementation retains the callback, observes completion, invokes Lua on the correct thread, and releases the path. The macOS manager currently only provides safe inspection and release operations around already existing paths at [level_manager.c:699](/Users/tomdimino/Desktop/Programming/game-modding/bg3/bg3se-macos/src/level/level_manager.c:699).

Most promising unlock:

Prefer discovering and calling a higher-level engine routine over reproducing its field writes.

Decompile these in order:

1. Client:
   - `ecl::aigrid::ProcessSettingsForCharacter` — `0x102de243c`
   - `ecl::aigrid::CreatePathForCharacter` — `0x102de3d78`
   - known `CreatePath` call — `0x102de439c`
2. Server:
   - `esv::aigrid::ProcessSettingsForCharacter` — `0x104814fb0`
   - `esv::aigrid::CreatePathForCharacter` — `0x104815680`
   - known `CreatePath` call — `0x104815d38`
3. Simpler corroborating callers:
   - client `ecl::MSMoveToSync::Continue` — `0x103043d7c`, call at `0x10304429c`
   - server `esv::MSMoveToSync::Continue` — `0x104b16eac`, call at `0x104b1741c`

Runtime probes:

1. Trigger a known character movement request through supported game scripting.
2. Snapshot the resulting native `0x2a8`-byte `AiPath`.
3. Separately call bare `CreatePath`, resolve it through `PathMap`, snapshot it, then immediately remove it.
4. Diff several pairs across:
   - different characters
   - different targets
   - standing versus moving
   - client versus server
5. Correlate stable differences with known fields such as Source `+0x10`, Target `+0x18`, bounds, masks, and state.
6. Trace when the native request enters/leaves `Paths`, when completion state changes, and which owner calls `RemovePath`.
7. Only expose Lua once callback storage, game-thread delivery, cancellation, level unload, and release behavior are proven.

Effort: 5–8 sessions for credible parity. A stable callable high-level entry could reduce that to roughly 3–4; manually reproducing field population could exceed 10 and remain version-fragile.

Risk: critical, 5/5. A half-populated path can leave stale entity references, corrupt engine containers, pass invalid bounds into pathfinding, or be freed twice by competing owners.

Cheaper alternative:

Keep `BeginPathfinding` unavailable and retain `FindPath`, `GetPathById`, and `ReleasePath` only for paths whose ownership is already known. For behavior rather than API parity, use an existing engine/Osi movement request and observe the engine-created path.

## Related

The practical route toward maximum parity is:

1. Ship a raw, read-only tile debugger.
2. Prove the empty `ls::Optional` ABI using `RaycastAny`.
3. Reuse that proof for `RaycastAll`, after resolving `PhysicsHitAll` lifetime.
4. Test whether an empty `ls::Function` exactly implements an unfiltered closest raycast.
5. Reject manual `BeginPathfinding` construction. Reconsider it only if a stable higher-level `CreatePathForCharacter` ABI and complete callback/ownership lifecycle can be called directly.

No files were modified during this analysis.

## `BeginPathfinding` — full recon, still blocked (2026-08-20, final)

The recommended unlock ("prefer discovering and calling a higher-level engine
routine") was found, and it is not sufficient.

### Recovered

    0x10481fc64  esv::aigrid::ProcessSettingsForCharacter(
                     esv::Character const&, eoc::aigrid::UnprocessedSettings const&)
    0x104820334  esv::aigrid::CreatePathForCharacter(
                     esv::Character const*, eoc::aigrid::ProcessedSettings const&)

Call shape, identical across all 19 call sites:

    add    x0, sp, #<processed>     ; sret out-buffer
    add    x2, sp, #<unprocessed>   ; in
    mov    x1, <Character*>
    bl     ProcessSettingsForCharacter
    add    x1, sp, #<processed>
    mov    x0, <Character*>
    bl     CreatePathForCharacter   ; -> int path id in w0
    cmn    w0, #0x539               ; failure sentinel is -1337

`ProcessedSettings` is 0x120 bytes; `UnprocessedSettings` spans at least 0xD0.
Two independent callers agree on field offsets (+0x38, +0xB0).

### Why it is still blocked

Constructing `UnprocessedSettings` remains the whole problem, and neither caller
provides a reusable template:

- **`esv::CharacterMover::Teleport`** initialises it field by field, but three of
  the values are its own parameters — `d1` at +0x00, `s0` at +0x08 (Teleport's
  float argument) and `d0` at +0x20 (from a constant table). Transplanting a
  teleport's distances into a generic pathfinding request would be guesswork.
  It also allocates a 16-byte `ls::Function` at +0x18 via
  `ls::MemoryManager::Allocate(16, 16)`.
- **`esv::AiHelpers::UpdateRequest`** copies a settings blob wholesale, which
  looked promising, but the source is `[sp, #0x4c0]` — a local built from live
  request data, not a static default.

So there is no default `UnprocessedSettings` in the binary to copy, and the
remaining fields would have to be understood semantically rather than
transcribed. That is the multi-session work the original estimate described, and
a malformed settings block feeds invalid bounds into the engine's pathfinder —
the risk that had this rated 5/5 in the first place.

Recorded so the next attempt starts from the ABI, the sentinel and the two
analysed callers rather than repeating the recon.
