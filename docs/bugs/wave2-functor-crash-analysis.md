# Wave 2 functor-hook SIGSEGV analysis

Date: 2026-07-29

Affected build: BG3 `4.1.1.7209685`, Wave 2 through `2fcd6a2`

Crash: `docs/bugs/wave2-crash-182309.ips`

## Symptom

The 18:22 process crashed on the ServerWorker thread while executing the
hooked Target overload:

```text
esv::functor::ExecuteStatsFunctors(
    eoc::StatsFunctorList const*,
    esv::functor::TargetContextData&
) + 0x8c
    -> ecs::EntityWorld::GetComponent<ls::TransformComponent const, true>() + 0x40
    -> EXC_BAD_ACCESS at 0x6d0133ed6db63ebf
```

The actual faulting instruction was at unslid `0x1010ba0a0`:

```asm
ldr x8, [x0, #0x2d0]
```

The crash report records:

```text
x0            = 0x6d0133ed6db63bef
fault address = 0x6d0133ed6db63ebf
difference    =                0x2d0
```

This is an exact effective-address match. The immediate cause was a bogus
`EntityWorld*` in `x0`, not a null component, Lua failure, or speculative PAC
failure.

## Root Cause

`a0f3ee6` changed the nine `ExecuteStatsFunctors` wrappers to match only the
explicit parameter list printed by `nm`. That dropped a hidden leading result
object argument which is present in the actual ARM64 machine ABI.

For the eight ordinary overloads, the real register contract is:

```text
x0 = esv::functor::Result output storage (hidden; not printed by nm)
x1 = StatsFunctorList const*
x2 = <ContextData>&
```

For Interrupt it is:

```text
x0 = esv::functor::Result output storage (hidden; not printed by nm)
x1 = ecs::EntityWorld&
x2 = StatsFunctorList const*
x3 = InterruptContextData&
```

The current wrappers instead declare two and three arguments respectively,
shifting every explicit parameter left and making the compiler unaware of the
last live input register. The Target wrapper consequently preserves only
incoming `x0` and `x1`, calls two helper functions, then uses `x2` as the
branch register holding `g_OrigTarget`. The original Target body therefore
receives the Dobby trampoline address as its context pointer.

This is definitive. It explains the exact instruction bytes found in the
crash register and fault address; no secondary corruption hypothesis is
needed.

## Evidence

### 1. `nm` matches source-level parameters but does not describe the full ABI

C++ mangled names do not encode ordinary return types. The signatures in
`docs/bugs/functor_nm_7209685.txt` therefore prove the explicit argument
types, but they do not prove how the returned `esv::functor::Result` is
materialized at this local function boundary.

| Hook | `nm` explicit parameters | Current wrapper | Binary machine inputs | Verdict |
|---|---|---|---|---|
| AttackTarget | `StatsFunctorList const*`, `AttackTargetContextData&` | same two source classes | hidden result, list, context in `x0..x2` | **Wrong** |
| AttackPosition | `StatsFunctorList const*`, `AttackPositionContextData&` | same two source classes | hidden result, list, context in `x0..x2` | **Wrong** |
| Move | `StatsFunctorList const*`, `MoveContextData&` | same two source classes | hidden result, list, context in `x0..x2` | **Wrong** |
| Target | `StatsFunctorList const*`, `TargetContextData&` | same two source classes | hidden result, list, context in `x0..x2` | **Wrong** |
| NearbyAttacked | `StatsFunctorList const*`, `NearbyAttackedContextData&` | same two source classes | hidden result, list, context in `x0..x2` | **Wrong** |
| NearbyAttacking | `StatsFunctorList const*`, `NearbyAttackingContextData&` | same two source classes | hidden result, list, context in `x0..x2` | **Wrong** |
| Equip | `StatsFunctorList const*`, `EquipContextData&` | same two source classes | hidden result, list, context in `x0..x2` | **Wrong** |
| Source | `StatsFunctorList const*`, `SourceContextData&` | same two source classes | hidden result, list, context in `x0..x2` | **Wrong** |
| Interrupt | `EntityWorld&`, `StatsFunctorList const*`, `InterruptContextData&` | same three source classes | hidden result plus the three explicit inputs in `x0..x3` | **Wrong** |
| ProcessDealDamageFunctors | 12 parameters: ten reference inputs, `int`, `DynamicArray&` | same order; references represented as pointers, `int` by value | `x0..x7` plus four stack arguments; no hidden leading result | Correct |

Thus the narrow answer to “does the wrapper match `nm`?” is “yes” for all ten
at the explicit source-parameter level. The ABI answer is “no” for all nine
`ExecuteStatsFunctors` overloads and “yes” for `ProcessDealDamageFunctors`.

`src/stats/functor_types.h:403-416` explicitly derives the current typedefs
from the `nm` argument lists. `src/stats/functor_hooks.c:138-230` implements
the corresponding shortened wrappers. `a0f3ee6^` had three arguments for the
ordinary wrappers and four for Interrupt. Although its first parameter was
named `self`/`hit`, that older shape forwarded the real register contract.
`a0f3ee6` removed precisely this leading machine argument.

The ten installed hooks are the eight ordinary overloads, Interrupt, and
`ProcessDealDamageFunctors` (`src/stats/functor_hooks.c:294-385`). The
`ADDR_EXECUTE_STATS_FUNCTOR` dispatcher is defined but is not one of the ten
installed hooks.

### 2. The Target call site proves the hidden result argument and register order

The crashing caller at unslid `0x104de6b60` prepares the call as follows:

```asm
0x104de6b60  add x0, sp, #0x108   ; result output object
0x104de6b64  add x2, sp, #0x420   ; TargetContextData
0x104de6b68  mov x1, x21          ; StatsFunctorList const*
0x104de6b6c  bl  0x10577a87c     ; ExecuteStatsFunctors(Target)
```

Another call site at `0x1057dcd50` uses the same order, then immediately calls
`esv::functor::Result::~Result()` on the object passed in `x0`. This confirms
that `x0` is result storage, not a `this` pointer or an optional extra context.

Every one of the eight ordinary entry points saves and uses `x2` as its
context. Examples:

```asm
AttackTarget +0x20:   mov x20, x2
AttackPosition +0x2c: mov x20, x2
Move +0x28:           mov x20, x2
Target +0x28:         mov x20, x2
NearbyAttacked +0x28: mov x27, x2
NearbyAttacking +0x28:mov x20, x2
Equip +0x20:          mov x20, x2
Source +0x28:         mov x20, x2
```

Interrupt saves `x3` as its context, `x2` as its list, `x1` as its
`EntityWorld&`, and uses `x0` as result storage:

```asm
0x105786578  mov x27, x3
0x10578657c  mov x28, x2
0x105786580  str x1, [sp, #0x1e8]
0x105786584  mov x25, x0
```

### 3. The wrapper-to-crash register chain is exact

The installed crash dylib has UUID
`EC234DAC-85B9-3881-8C35-03CFF1B6AFF1`, matching the image UUID in the crash
report. Its Target wrapper disassembles as:

```asm
_hook_ExecuteFunctors_Target:
  mov x19, x1                    ; preserves declared ctx
  mov x20, x0                    ; preserves declared list
  bl  _events_get_handler_count
  bl  _events_get_handler_count
  ...
  ldr x2, [g_OrigTarget]         ; x2 becomes Dobby original trampoline
  mov x0, x20
  mov x1, x19
  ...
  br  x2
```

Neither the 18:14 nor 18:22 log contains a subscription to
`ExecuteFunctor`, `AfterExecuteFunctor`, `BeforeDealDamage`, or `DealDamage`.
The crash therefore took this zero-subscriber fast path. Lua, `lua_gate`, and
event callbacks were not involved.

The Target body then does:

```asm
0x10577a8a4  mov x20, x2
...
0x10577a8f8  mov x25, x20
0x10577a8fc  ldp x1, x0, [x25, #0x98]!
0x10577a904  bl  EntityWorld::GetComponent<TransformComponent const, true>
```

`ldp x1, x0, [x25, #0x98]!` loads `x0` from original `x2 + 0xa0`.
Because `x2` is the Dobby trampoline address, the load reads executable code
from Dobby's adjacent relocated-code pool.

The loaded value was:

```text
0x6d0133ed6db63bef
```

Those are not merely “garbage-looking” bits. They are exactly the first two
instructions of the separately hooked Interrupt overload, concatenated as a
little-endian 64-bit load:

```asm
0x105786548  stp d15, d14, [sp, #-0xa0]!  ; word 0x6db63bef
0x10578654c  stp d13, d12, [sp, #0x10]    ; word 0x6d0133ed
```

The subsequent `GetComponent` load adds `0x2d0`, producing the crash address
exactly. This proves that Target used the trampoline pool as a
`TargetContextData`, extracted code bytes as an `EntityWorld*`, and faulted on
the first dereference.

### 4. Prologue relocation is ruled out for this crash

The raw Target bytes were read from:

```text
fat ARM64 slice offset: 0x0f558000
function relative VA:  0x0577a87c
file offset:            0x14cd287c
```

The entry decodes to:

```asm
+0x00  stp d11, d10, [sp, #-0x80]!
+0x04  stp d9,  d8,  [sp, #0x10]
+0x08  stp x28, x27, [sp, #0x20]
+0x0c  stp x26, x25, [sp, #0x30]
+0x10  stp x24, x23, [sp, #0x40]
+0x14  stp x22, x21, [sp, #0x50]
+0x18  stp x20, x19, [sp, #0x60]
+0x1c  stp x29, x30, [sp, #0x70]
+0x20  add x29, sp, #0x70
+0x24  sub sp, sp, #0x940
+0x28  mov x20, x2
+0x2c  mov x21, x1
+0x30  mov x19, x0
+0x34  adrp x8, 0x10840a000       ; first PC-relative instruction
```

The functor installer calls `DobbyHook` directly at all ten sites
(`src/stats/functor_hooks.c:296-378`); it never calls
`arm64_safe_hook`. Dobby's ARM64 entry patch is either a four-byte near
branch, a 12-byte `ADRP+ADD+BR`, or a 16-byte literal branch. All possible
overwritten ranges here consist solely of PC-independent `STP` instructions.
The first PC-relative instruction is at `+0x34`, well outside the relocated
entry range.

Dobby's ARM64 relocator also has explicit cases for `B/BL`, integer
`LDR`-literal, `ADR`, `ADRP`, `B.cond`, `CBZ/CBNZ`, and `TBZ/TBNZ`, with raw
copy as the fallback. Its limitations (for example, non-W/X literal loads)
are not exercised by this prologue.

The repository's custom `src/hooks/arm64_hook.c` is a separate latent risk,
but not a participant in this crash:

- `arm64_decode` recognizes ADR/ADRP, branches, and literal loads as
  PC-relative, but the trampoline builder at `arm64_hook.c:332-340` simply
  `memcpy`s both skipped and overwritten instructions. It performs no
  instruction relocation.
- Its “skip and redirect” strategy can execute stack-adjusting prologue
  instructions before branching to an ordinary C replacement, then copy and
  execute them again when the replacement calls the trampoline. That can
  double-apply a prologue and leave the replacement returning with an
  unbalanced stack.
- Its safe-offset analysis can select an early offset before discovering a
  later unsafe instruction because `safe_hook_offset` is set during the
  forward scan and never revised.

Those defects should be tracked separately for the users of
`arm64_safe_hook`, but changing that module cannot fix this functor crash.

### 5. Work performed before `orig`

Every `ExecuteStatsFunctors` wrapper unconditionally calls
`events_get_handler_count()` twice before calling the original
(`src/stats/functor_hooks.c:70-75,138-230`). Under AAPCS64 those calls may
clobber every caller-saved register, including `x0..x18`.

The compiler correctly:

- saves all *declared* wrapper arguments in callee-saved registers;
- preserves/restores the callee-saved registers it uses;
- reconstructs the declared arguments before the tail call.

It cannot preserve an argument omitted from the C prototype. On the fast
path, the missing ordinary `x2`/Interrupt `x3` is destroyed. On the subscriber
path, `lua_gate_lock`, event dispatch, and `lua_gate_unlock` add more
caller-saved clobber opportunities, but they are not necessary to trigger the
bug. There is no per-call logging in these wrappers.

The `ProcessDealDamageFunctors` wrapper is different: its installed assembly
saves all eight register arguments plus the four incoming stack arguments,
then reconstructs them before the original call. No corresponding ABI loss
was found there.

### 6. The 18:14 menu stall is independent

The 18:14 log shows:

- all ten hooks installed at 18:14:20.957;
- BaseApp focus forcing succeeded at 18:14:23.190;
- repeated Escape/Space/mouse injection attempts and later coordinate-grid
  clicks;
- no `COsiris::Load`, no `Init -> LoadSession`, and no functor-event
  subscription;
- no crash report or evidence of a failed ServerWorker.

The 18:22 log follows the same startup path, but an input attempt eventually
causes `COsiris::Load` at 18:22:49.849 and `Init -> LoadSession` at
18:22:50.089. It reaches `Running` at 18:22:55.487 before the first observed
Target execution crashes it.

This matches the already documented Noesis behavior in
`docs/bugs/noesis-input-bypass-re.md`: `-continueGame` highlights Continue but
does not activate it; posting input proves that it entered the native input
queue, not that a focused Noesis command consumed it. The 18:14 process never
entered the server status/functor path where the bad wrapper is exercised.

The only plausible relationship would be an unlogged functor invocation at
the menu corrupting state without crashing, but there is no positive evidence
for that and the affected overloads are server gameplay paths. A per-wrapper
entry counter, recorded before any helper call, is the discriminating test.
Absent such a hit, treat the menu stall as the known input-activation flake.

## Recommended Fix

### Minimal code fix

Restore the hidden leading result argument to all nine
`ExecuteStatsFunctors` typedefs and wrappers. Name it for what the binary
shows rather than the old ambiguous `self`:

```c
typedef void (*ExecuteFunctorsProc)(
    void*                   result_out,
    const StatsFunctorList* functors,
    void*                   context
);

typedef void (*ExecuteInterruptFunctorsProc)(
    void*                   result_out,
    EntityWorld*            entity_world,
    const StatsFunctorList* functors,
    InterruptContextData*   context
);
```

Each ordinary wrapper must accept and forward
`(result_out, functors, context)` while passing only `functors` and `context`
to the Lua event layer. Interrupt must similarly forward all four machine
arguments. Leave `ProcessDealDamageFunctorsProc` unchanged.

This is effectively a focused revert of the wrapper-ABI portion of
`a0f3ee6`, with corrected names and `const` qualifiers. Do not express the
game function as returning a guessed C struct: an explicit opaque
`result_out` parameter exactly matches the observed local machine ABI and
avoids introducing a second compiler-specific struct-return lowering.

### Immediate fail-closed option

If the corrected wrapper assembly cannot be inspected and live-tested before
shipping, disable the nine `ExecuteStatsFunctors` hooks again. The
`ProcessDealDamageFunctors` hook may be considered separately because its ABI
checks out, but disabling the whole functor group is the lowest-risk release
action.

Do not switch these hooks to `arm64_safe_hook` as a fix. The active Dobby
relocation is not the cause, and the custom hook currently has the independent
correctness problems described above.

## Verification sequence

1. **Build-time wrapper disassembly**
   - For every ordinary wrapper, verify the entry preserves incoming
     `x0`, `x1`, and `x2`.
   - Immediately before the original branch/call, verify
     `x0=result_out`, `x1=functors`, and `x2=context`.
   - For Interrupt, perform the same check for `x0..x3`.
   - Ensure the chosen indirect branch register is not still occupying an
     argument register at transfer time.

2. **Static game-binary ABI fixture**
   - Record the 7209685 call-site and entry bytes for all nine overloads.
   - Assert that call sites populate the hidden result slot and that entries
     consume the shifted registers.
   - Treat `nm` as address/name evidence only; require call-site plus callee
     disassembly for wrapper ABI approval.

3. **Zero-subscriber regression run**
   - Use the same save and mods as the 18:22 crash.
   - Load to `Running`, exercise the status tick which selects Target, and
     keep the session active through several turns/status updates.
   - Add temporary entry/original-return counters or breadcrumbs per overload
     so the test proves Target was actually hit.

4. **Subscriber-path run**
   - Subscribe to both `ExecuteFunctor` and `AfterExecuteFunctor`.
   - Verify before/after ordering, equal counts, valid context type, and an
     unchanged result object across the wrapper/original boundary.
   - Repeat with `BeforeDealDamage` and `DealDamage` for the tenth hook.

5. **Context coverage**
   - Trigger AttackTarget, AttackPosition, Move, Target, NearbyAttacked,
     NearbyAttacking, Equip, Source, and Interrupt individually.
   - Record a hit for all nine; installation count alone is not coverage.

6. **Backend control**
   - If any crash remains, run the same deterministic trigger with only the
     implicated wrapper disabled, then with all functor hooks disabled.
   - Dump Dobby's patched byte count and relocated-original bytes only if the
     corrected ABI still fails. The current Target prologue already rules out
     relocation as the Wave 2 failure.

7. **Menu automation as a separate test**
   - Run repeated cold starts and score success solely by observing
     `COsiris::Load`/`Init -> LoadSession`.
   - Compare hook-enabled and functor-hook-disabled arms while recording
     per-wrapper entry counts. A stalled run with zero functor hits confirms
     independence from this crash.

## Prevention

- Never infer a C/C++ hook ABI from a demangled symbol alone. Return types and
  hidden result storage are not represented in these names.
- Require two-sided ABI evidence: caller register setup and callee register
  consumption.
- Keep disassembly assertions for wrapper forwarding in the test harness;
  source-level typedef compatibility cannot catch omitted machine arguments.
- Gate address migration and ABI migration separately. A symbol-exact address
  does not make its wrapper ABI verified.
- Add per-hook hit counters and a deterministic execution test. “10/10
  installed” only proves that Dobby accepted ten addresses.
- Audit or retire the custom `arm64_safe_hook` implementation before using it
  for additional hooks; its current copy-only trampoline is not a general
  ARM64 relocator.
