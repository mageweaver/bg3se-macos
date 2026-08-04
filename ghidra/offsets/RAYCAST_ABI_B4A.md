# B4a — RaycastAny Zeroed-Aggregate ABI Recon

**Date:** 2026-08-03 | **Build:** 4.1.1.7209685 | **Session:** Wave 7 B4a static recon (sol agent, read-only; findings transcribed from the session transcript — the agent's sandbox blocked the doc write)
**Prior evidence:** `PHYSICS_VMT_AUDIT.md` (verified physics VMT), Wave 7 plan B4 row (zeroed-aggregate hypothesis)

## Verdict

**B4a GO.** A zeroed 16-byte `ls::Optional<PhysicsSceneScopedReadLock&>` is
ABI-legal at the RaycastAny call boundary and selects the safe internal
`lockRead → raycast → unlockRead` path inside the worker. A real in-game
caller (`CoverManager::Check3DLineAny` sight check) already invokes the slot
this way.

## Evidence

- `RaycastAny` is VMT slot 10 at `0x105c4e8c8` (PHYSICS_VMT_AUDIT.md:145).
- Wrapper `0x105c4e8c8` loads the optional with `ldp x10, x9, [sp, #8]`.
- Worker `0x105c58adc` reads the engagement word at incoming `SP+0x10` and
  tests its low byte:

  ```asm
  tst x8, #0xff
  b.eq 0x105c58c40        ; disengaged → internal-lock path
  ```

- The zero (disengaged) branch calls NpScene VMT `+0x310` and `+0x318`:
  - `0x1008727cc` — `physx::NpScene::lockRead`
  - `0x100872840` — `physx::NpScene::unlockRead`
- Native sight caller `0x101bb85d0` sets `x5 = 0`;
  `CoverManager::Check3DLineAny` forwards `(x4, x5)` with
  `stp x23, x24, [sp, #8]` before dispatching slot 10 — a legal zeroed-optional
  call site in shipping game code.

## Aggregate value layouts

| Aggregate | Layout | Call boundary |
|---|---|---|
| `ls::Optional<PhysicsSceneScopedReadLock&>` | 16 bytes: pointer `+0x0`, engagement byte `+0x8`, padding through `+0xf` | At RaycastAny: `[SP+8]` payload, `[SP+16]` engagement word |
| `ls::Function<bool(PhysicsShape const*)>` | 64 bytes: `MethodTable*` at `+0x0`, inline storage `+0x8..+0x3f` | Passed indirectly; pointer carrier at `[SP+8]` |

The Function ordering agrees structurally with Windows CoreLib's leading
storage pointer plus 56-byte storage (`CoreLib/Base/BaseFunction.h:23`). The
Windows raycast bindings intentionally pass empty optionals with `{}`
(`BG3Extender/Lua/Libs/Level.inl:262`) — the zeroed call is upstream-canonical,
not a macOS invention.

## RaycastAny C shim mapping (implementation sketch)

```text
x0 scene                 x4 include_group
x1 source*               x5 exclude_group
x2 destination*          x6 context
w3 physics_type          w7 physics_object_index (-1)

[SP+0x00] exclude_physics_object_index (-1)
[SP+0x08] optional payload = 0
[SP+0x10] optional engagement word = 0
```

Apple Clang codegen for the zeroed tail:

```asm
stp xzr, xzr, [sp, #8]
str w9, [sp]
blr x8
```

Rules:

- Dispatch via **VMT slot 10**, never a hard-coded worker address.
- Reserve at least 32 outgoing stack bytes to preserve 16-byte SP alignment.
- Source, destination, and scene must remain valid through the synchronous
  call; the empty optional carries **no** lock-object lifetime or destructor
  obligation.

## Early B4c evidence (not a B4c verdict)

The RaycastClosest wrapper (`0x105c4e784`) checks the Function's leading
method-table pointer. Null skips the copy and reaches `s_IgnoreFilterClosest`,
which applies the built-in masks/object exclusions and otherwise returns
`eBLOCK`. This supports the "no additional predicate" reading for a null
Function but does not replace B4c runtime validation.

## Remaining runtime checks (before any binding ships)

- Open sky → `false`; known floor/wall crossing → `true`.
- Verify include/exclude masks and object exclusions behave.
- Compare against proven sweep results once All/Closest exist.
- Thousands of calls during active physics simulation.
- Level unload/reload and session reset survival.
- Watch for deadlocks, lock imbalance, crashes, and memory growth.
- Fail closed on any binary UUID other than
  `9A647311-E263-3FF2-AF98-111CEDCB3034`.
- Keep B4b output lifetime and B4c empty-function behavior as separate gates
  per the plan's split-gate ladder.
