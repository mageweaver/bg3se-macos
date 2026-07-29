# BG3SE-macOS Lateral Strategy — Post-Root-Cause Edition

**Date:** 2026-07-28  
**Scope:** Novel, testable attack vectors that do not duplicate the wave plan,
the `NO_HOOKS` debugger report, the methodology audit, or the headless design  
**Safety:** Static/offline work only. BG3 was not launched.

## Current Architecture Assessment

The other reports landed while this strategy was being prepared, and they
materially changed the problem.

`docs/bugs/codex-debugger-nohooks-2026-07-28.md` establishes a direct cause:

- `BG3SE_NO_HOOKS=1` does not disable every Dobby patch.
- `staticdata_manager_init()` still installs five `Get<T>` hooks and one Feat
  hook; VideoSkip can install another.
- stale `OFFSET_GET_CLASS = 0x0262f184` is not the entry of
  `ImmutableDataHeadmaster::Get<ClassDescriptions>()` in the installed build;
  it lands at `gui::HotbarSystem::Update + 0xacc`.
- Dobby therefore treats a mid-function instruction sequence as a function
  entry, builds an invalid “original” trampoline, resumes Hotbar on the wrong
  stack, and later reads a null `WorldView*` at `HotbarSystem::Update + 3076`.

That finding makes several otherwise reasonable ideas poor uses of time:

- a watchpoint on the null slot would observe the wrong stack frame, not an
  earlier writer corrupting the real `WorldView`;
- Guard Malloc is unlikely to clarify a deterministic mid-function trampoline;
- Metal validation and ImGui swizzle probes have near-zero gain because the
  crash logs show the backend never initialized;
- input replay is already covered by the methodology/headless reports and
  cannot explain the exact code-patch collision;
- generic subsystem toggles, exact-version gating, function-entry/prologue
  validation, a startup hook manifest, and correction of `NO_HOOKS` are already
  proposed by the debugger report.

The lateral opportunity is now broader: make hidden runtime patching
independently observable, eliminate hand-maintained offsets where the shipped
binary already contains the symbols, and create offline machinery that finds
the next collision before a live session.

## Ranked Proposals by Information per Hour

| Rank | Proposal | Effort | Expected information gain | Novel discriminator |
|---:|---|:---:|:---:|---|
| 1 | LLDB interception census at the Dobby API boundary | S | Very high | actual runtime patches and callers, independent of SE logs |
| 2 | Runtime executable-page attestation | M | Very high | every modified instruction range, including unknown patchers |
| 3 | UUID-keyed symbol-derived hook manifest | S–M | Very high | removes manual offsets for symbolized functions |
| 4 | Cross-build semantic hook migration | M–L | High | carries hooks across builds by function identity and CFG |
| 5 | Crash-PC ↔ hook-target proximity miner | S–M | High | automatically detects “crash shortly after bad patch” patterns |
| 6 | ARM64 trampoline ABI canary and relocation fuzz harness | M | High | proves Dobby/custom trampolines preserve ABI offline |
| 7 | Batch hook-admission transaction | M–L | High | validates the whole patch set before installing any patch |
| 8 | Residual Mach-O/dyld null-image matrix | M | Medium-high | patch/load/image/constructor separation if a post-fix crash remains |
| 9 | Export-namespace quarantine build | M | Medium | tests weak binding or `RTLD_DEFAULT` symbol pollution |

## Proposed Changes

### 1. LLDB interception census at the Dobby API boundary

**What**

Launch a diagnostic run under LLDB from process start and break on the patch
APIs themselves:

- `DobbyHook`;
- `DobbyCodePatch`;
- `arm64_safe_hook`;
- `arm64_hook_at_offset`, if used.

At each entry, record the target, replacement, caller, nearest loaded image,
target bytes, and target symbol. Auto-continue after recording. This is an
external census: it does not trust `NO_HOOKS`, source call-site enumeration, or
BG3SE's own “installed” log lines.

**Why it could be decisive or a force-multiplier**

The root bug survived because the diagnostic mode and logs described a narrower
surface than the process actually had. A breakpoint at the common patch
boundary observes the ground truth. It catches:

- hooks installed through forgotten initializers;
- duplicate installs;
- third-party Dobby users;
- dynamically computed targets absent from offset tables;
- a target resolving to the middle of an unrelated symbol.

It would have exposed the StaticData patch at normalized
`0x10262f184` immediately.

**Exact tooling / commands**

Future runbook; do not use the harness path that rebuilds or redeploys while
the target is being audited:

```bash
bg3_exec="/Users/tomdimino/Library/Application Support/Steam/steamapps/common/Baldurs Gate 3/Baldur's Gate 3.app/Contents/MacOS/Baldur's Gate 3"
lldb "$bg3_exec"
```

Inside LLDB:

```text
(lldb) settings set target.run-args -continueGame
(lldb) settings set target.env-vars BG3SE_NO_HOOKS=1 BG3SE_SKIP_VIDEOS=1
(lldb) breakpoint set -n DobbyHook
(lldb) breakpoint set -n DobbyCodePatch
(lldb) breakpoint set -n arm64_safe_hook
(lldb) breakpoint set -n arm64_hook_at_offset
(lldb) breakpoint command add 1
> register read x0 x1 x2
> image lookup -a $x0
> memory read --format x --size 4 --count 8 $x0
> bt 8
> continue
> DONE
(lldb) run
```

For durable evidence, replace the interactive commands with an LLDB Python
callback writing one JSONL record per hit:

```json
{"api":"DobbyHook","target":"0x104e1f184","image":"Baldur's Gate 3",
 "normalized":"0x10262f184","replacement":"0x10...","caller":"install_get_manager_hooks"}
```

Acceptance criterion: the observed patch set must exactly equal the declared
manifest. Missing or extra sites make the run invalid before the save is
loaded.

**Effort:** S, roughly 30–60 minutes to collect one trace; M to automate JSON.  
**Expected information gain:** Very high. It supplies source-independent truth
about the live patch surface.

**Risk:** LLDB changes timing and pauses BG3 at constructor time. It does not
modify the Steam installation. Run only against the exact owned PID and stop
after the patch census if gameplay is unnecessary.

### 2. Runtime executable-page attestation

**What**

After constructor initialization, compare the mapped arm64 `__TEXT,__text`
bytes of BG3 and loaded dylibs with the on-disk arm64 slices. Produce a map of
every changed instruction range and reconcile it with the approved hook
manifest.

This operates below Dobby. It detects patches made by Dobby, the custom ARM64
hooker, raw `mach_vm_write`, or any future patch library.

**Why it could be decisive or a force-multiplier**

API interception can miss inlined/private patch paths. Code-page attestation
asks the invariant that matters: “Which executable bytes differ from the
signed/on-disk image?” Any unexplained mutation is a release blocker.

For this crash it should show a branch/trampoline patch covering
`0x10262f184`, inside the symbol range for `HotbarSystem::Update`, regardless
of what BG3SE calls that hook.

**Exact tooling / commands**

Inspect the slice and text layout offline:

```bash
xcrun dwarfdump --uuid "$bg3_exec"
xcrun llvm-objdump --macho --arch=arm64 --section-headers "$bg3_exec"
xcrun llvm-nm --arch=arm64 --numeric-sort --demangle "$bg3_exec" \
  | rg 'HotbarSystem::Update|ImmutableDataHeadmaster::Get'
```

Manual proof for one suspect range:

```text
(lldb) image list -o -f
(lldb) memory read --binary --outfile /tmp/getclass.runtime.bin \
  --count 64 <BG3-load-base+0x0262f164>
```

Then compare against the 64 bytes at the matching arm64 file offset, derived
from `LC_SEGMENT_64`/section metadata:

```bash
cmp -l /tmp/getclass.disk.bin /tmp/getclass.runtime.bin
```

Disassemble the live range directly in LLDB with
`disassemble --start-address <address> --count 16`; do not ask
`llvm-objdump` to infer an object format from a raw byte dump.

The production form should be a small read-only tool:

```bash
python3 tools/hook_attest.py snapshot \
  --pid "$bg3_pid" \
  --image "$bg3_exec" \
  --output "/tmp/bg3se-code-$bg3_pid.json"

python3 tools/hook_attest.py verify \
  --snapshot "/tmp/bg3se-code-$bg3_pid.json" \
  --allowlist build/hook-manifest.json
```

The tool should parse the Mach-O fat header and arm64 `LC_SEGMENT_64` itself,
subtract the ASLR slide, group adjacent changed instructions, symbolize each
range, and reject changes outside approved ranges. Ignore data fixups by
limiting the first implementation to executable `__text`.

**Effort:** M, 1–2 days for a reusable tool; under 1 hour for the known-site
manual proof.  
**Expected information gain:** Very high. It reveals the actual mutation set,
including patch mechanisms not yet known to the source auditor.

**Risk:** Read-only process inspection. Large full-text reads may pause the
process; hash page-by-page and stop after initialization. No Steam-install
risk.

### 3. UUID-keyed symbol-derived hook manifest

**What**

Stop hand-maintaining offsets for functions that are present in the installed
binary's local symbol table. Generate a build artifact mapping the arm64 Mach-O
UUID and exact demangled function names to normalized VAs/image offsets.

At runtime, accept only a manifest whose UUID matches the loaded main image.

**Why it could be decisive or a force-multiplier**

The current binary already answers the StaticData question exactly:

```text
0x102614874  Get<ClassDescriptions>()
```

The stale source used `0x10262f184`. Symbol derivation converts a manual RE
maintenance problem into a deterministic build step. It also exposes overload
ambiguity and missing symbols before live testing.

This is different from merely checking a prologue at a hardcoded address: the
address itself comes from the current binary identity.

**Exact tooling / commands**

```bash
xcrun dwarfdump --uuid "$bg3_exec"

xcrun llvm-nm \
  --arch=arm64 \
  --numeric-sort \
  --demangle \
  "$bg3_exec" \
  | rg 'ls::ImmutableDataHeadmaster::Get<.*>\\(\\) const'
```

For the installed build, that command currently returns symbolized entries for
ActionResourceTypes, ClassDescriptions, BackgroundManager, OriginManager, and
ProgressionManager.

Generate and validate a manifest:

```bash
python3 ghidra/scripts/generate_hook_manifest.py \
  --binary "$bg3_exec" \
  --arch arm64 \
  --spec ghidra/offsets/hook-symbols.yaml \
  --output build/hook-manifest.json

python3 ghidra/scripts/validate_hook_manifest.py \
  --binary "$bg3_exec" \
  --manifest build/hook-manifest.json \
  --require-unique \
  --require-function-start
```

Suggested manifest row:

```json
{
  "uuid": "9A647311-E263-3FF2-AF98-111CEDCB3034",
  "name": "ls::ImmutableDataHeadmaster::Get<eoc::ClassDescriptions>() const",
  "va": "0x102614874",
  "image_offset": "0x02614874",
  "size": 80,
  "first_16_sha256": "..."
}
```

Fail closed when a required name is absent, occurs more than once, or its UUID
does not match. Keep pattern scanning only for stripped future builds, as a
separate lower-confidence tier.

**Effort:** S–M, about 4–8 hours for the five StaticData functions; M to cover
all hook families.  
**Expected information gain:** Very high and immediately actionable.

**Risk:** Offline/read-only. The main failure mode is trusting a symbol with the
right name but wrong ABI; retain ABI/signature review before enabling the hook.

### 4. Cross-build semantic hook migration

**What**

Use the last supported BG3 arm64 binary and the installed binary as a
before/after pair. Match every old hook target to the new build using a
combination of:

- exact demangled symbol identity;
- normalized instruction fingerprints;
- callers/callees;
- control-flow graph shape;
- referenced type/string globals.

Produce a migration report with confidence and explicit ABI changes.

**Why it could be decisive or a force-multiplier**

Updating offsets one crash at a time creates a whack-a-mole cycle. A
cross-version semantic diff answers two questions for the entire hook set:

1. where did the intended function move?
2. did it change enough that the existing replacement/trampoline ABI is no
   longer safe?

This catches the more dangerous case where a symbol still exists but its
calling convention or prologue changes.

**Exact tooling / commands**

```bash
old_bg3="/path/to/verified-4.1.1.6995620-arm64"
new_bg3="$bg3_exec"

xcrun llvm-nm --arch=arm64 --numeric-sort --demangle "$old_bg3" \
  > /tmp/bg3-old.symbols
xcrun llvm-nm --arch=arm64 --numeric-sort --demangle "$new_bg3" \
  > /tmp/bg3-new.symbols

xcrun llvm-objdump --macho --arch=arm64 --disassemble --demangle "$old_bg3" \
  > /tmp/bg3-old.asm
xcrun llvm-objdump --macho --arch=arm64 --disassemble --demangle "$new_bg3" \
  > /tmp/bg3-new.asm

python3 ghidra/scripts/migrate_hook_targets.py \
  --old-binary "$old_bg3" \
  --new-binary "$new_bg3" \
  --hooks build/hook-manifest-old.json \
  --output build/hook-migration.json
```

Use Ghidra Version Tracking for every result below the automatic confidence
threshold. Require two independent features—never raw byte similarity alone—
for an unsymbolized match.

Acceptance criteria per hook:

- unique target;
- function entry;
- compatible parameter/return ABI;
- no new PAC/BTI or PC-relative relocation hazard in the overwritten window;
- replacement callback signature reviewed;
- first basic block and direct callees attached to the report.

**Effort:** M–L, 1–3 days depending on availability of the old binary.  
**Expected information gain:** High across all current and future hook sites.

**Risk:** Offline. The principal risk is false-confidence matching; ambiguous
results must remain disabled.

### 5. Crash-PC ↔ hook-target proximity miner

**What**

Create an offline tool that ingests:

- every `.ips` crash body;
- every current and historical hook target;
- the exact matching Mach-O UUID;
- the binary symbol table.

It should rank hook targets near each crash PC and highlight:

- the crash inside the same symbol as a patch;
- duplicate frames of the patched symbol;
- crash distance in instructions from the patch;
- patched target not equal to a function start;
- target symbol inconsistent with the intended name.

**Why it could be decisive or a force-multiplier**

The decisive clue here is geometric:

```text
bad patch: 0x10262f184
crash:     0x10262f2bc
distance:  0x138 bytes
same symbol: HotbarSystem::Update
```

That relationship can be found automatically before deep manual disassembly.
The miner turns future “vanilla frames” into a ranked list of SE patch
collisions instead of assuming the SE is absent from the causal path.

**Exact tooling / commands**

```bash
python3 tools/crash_hook_correlate.py \
  --ips docs/bugs/evidence-2026-07-28 \
  --binary "$bg3_exec" \
  --hook-source src \
  --hook-manifest build/hook-manifest.json \
  --radius 0x1000 \
  --output docs/bugs/evidence-2026-07-28/hook-proximity.json
```

Quick manual seed:

```bash
rg -n '#define (OFFSET_|VA_)|DobbyHook\\(|arm64_safe_hook\\(' src \
  > /tmp/bg3se-hook-sites.txt
xcrun llvm-nm --arch=arm64 --numeric-sort --demangle "$bg3_exec" \
  > /tmp/bg3-symbols.txt
```

The tool must parse the `.ips` body's `slice_uuid`, `pid`, `procLaunch`,
`captureTime`, frame image index, and image offset. It must reject an IPS whose
arm64 UUID differs from the supplied binary.

**Effort:** S–M, roughly 4–8 hours.  
**Expected information gain:** High as a recurring crash-triage accelerator.

**Risk:** Offline/read-only. Avoid treating proximity alone as proof; use it to
prioritize disassembly and runtime confirmation.

### 6. ARM64 trampoline ABI canary and relocation fuzz harness

**What**

Add an offline arm64 test executable that hooks synthetic functions with
deliberately difficult prologues and verifies:

- arguments and return values;
- SP alignment;
- LR and callee-saved `x19–x28`;
- large stack frames;
- `ADRP+ADD`, literal loads, conditional branches, and forward/backward
  branches in relocated instructions;
- PAC/BTI prologues where the toolchain emits them;
- calling the “original” trampoline from the replacement.

Run each case against both Dobby and `arm64_safe_hook`.

**Why it could be decisive or a force-multiplier**

The present bug is a bad target, but its visible mechanism is a broken
trampoline/stack context. After target admission is fixed, relocator errors
remain a separate hazard. The repo has ARM64 decoder/hooker code but no
end-to-end ABI canary suite for the actual patch engines.

This gives a no-game way to distinguish:

- target-selection failures;
- Dobby relocation failures;
- custom safe-hook relocation failures;
- replacement callback ABI mistakes.

**Exact tooling / commands**

Add a CMake target linked to the same Dobby and hook sources as the dylib:

```bash
cmake -S . -B build-hook-tests \
  -DCMAKE_BUILD_TYPE=Debug \
  -DBG3SE_BUILD_HOOK_CANARY=ON
cmake --build build-hook-tests --target bg3se_hook_abi_canary -j"$(sysctl -n hw.logicalcpu)"
arch -arm64 build-hook-tests/bin/bg3se_hook_abi_canary --engine all --repeat 10000
ctest --test-dir build-hook-tests -R 'hook_(abi|relocation)' --output-on-failure
```

Seed prologues with small `.S` fixtures so the compiler cannot optimize away
the exact instruction forms. Record pre/post registers in assembly rather than
relying only on C assertions.

For fuzzing, generate valid basic-block prefixes from the decoder's supported
instruction classes, relocate them into a scratch executable page, and compare
the original and trampoline results from randomized register inputs.

**Effort:** M, 1–2 days for deterministic fixtures; L for constrained fuzzing.  
**Expected information gain:** High for hook-engine confidence and future ARM64
regressions.

**Risk:** Offline test-process crashes are expected and contained. Generated
instructions must run only in a disposable helper process, never inside BG3.

### 7. Batch hook-admission transaction

**What**

Replace scattered “validate then immediately patch” calls with a two-phase
hook plan:

1. **Plan:** resolve and validate every requested target, callback ABI,
   version/UUID, symbol identity, instruction fingerprint, and collision.
2. **Commit:** install hooks only if the entire plan is valid.

If one target fails admission, install **zero** hooks in that plan. Emit the
rejected plan as JSON.

**Why it could be decisive or a force-multiplier**

Per-hook fail-closed checks can still leave a partially patched process whose
behavior is hard to reason about. StaticData installed a family of related
capture hooks; they should be treated as one compatibility unit.

A batch transaction prevents:

- five “good” old offsets plus one catastrophic stale offset;
- two hook families claiming the same target;
- a callback address outside the BG3SE image;
- one plan changing code before a later target reveals the build is
  incompatible.

This extends the debugger report's target validation into an architectural
all-or-nothing boundary.

**Exact tooling / commands**

Proposed interface:

```c
HookPlan *plan = hook_plan_begin("staticdata", loaded_uuid);
hook_plan_add_symbol(plan, "Background", kBackgroundSymbol, hook_GetBackground, &g_orig_GetBackground);
hook_plan_add_symbol(plan, "Origin", kOriginSymbol, hook_GetOrigin, &g_orig_GetOrigin);
hook_plan_add_symbol(plan, "Class", kClassSymbol, hook_GetClass, &g_orig_GetClass);
hook_plan_add_symbol(plan, "Progression", kProgressionSymbol, hook_GetProgression, &g_orig_GetProgression);
hook_plan_add_symbol(plan, "ActionResource", kActionResourceSymbol, hook_GetActionResource, &g_orig_GetActionResource);
if (!hook_plan_validate(plan) || !hook_plan_commit(plan)) {
    hook_plan_emit_rejection(plan);
}
```

Offline audit before live use:

```bash
build/bin/bg3se_hook_plan_audit \
  --binary "$bg3_exec" \
  --manifest build/hook-manifest.json \
  --plan staticdata \
  --json build/staticdata-hook-plan.json
```

Required invariants:

- unique target per plan row;
- no overlap between patch windows;
- every target within executable `__text`;
- every replacement within the expected BG3SE image;
- full plan UUID equality;
- post-commit re-read matches the expected branch encoding;
- original/trampoline outputs are non-null and executable;
- no partial commit on validation failure.

**Effort:** M–L, 2–4 days because existing hook call sites need centralization.  
**Expected information gain:** High as a permanent compatibility boundary.

**Risk:** Architectural refactor; per repository instructions it requires
explicit approval before implementation. A commit failure after patching starts
cannot always be rolled back safely while threads run, so all fallible checks
must occur in phase 1 and commit must be minimal.

### 8. Residual Mach-O/dyld null-image matrix

**What**

Use this only if the StaticData collision is removed and an identical or new
crash remains. Split “SE present” into:

| Arm | Patched executable | Deployed image | Constructor |
|---|---|---|---|
| A | no | none | none |
| B | yes | weak target missing | none |
| C | yes | tiny libSystem-only stub | empty |
| D | yes | real dylib | existing `BG3SE_DISABLE=1` early return |
| E | yes | real dylib | normal |

The debugger report already proposes Arm D. The novel value is B/C/D together:
they separate the load command/signature, dyld image presence, full image
layout/dependencies, and constructor execution.

**Why it could be decisive**

If the direct bad hook is fixed but the game still differs, this matrix stops
the investigation from prematurely returning to constructor subsystems:

- B fails: patch/signature/load-command effect;
- C fails: mere image mapping;
- D fails: full image layout/dependency/initializer effect outside
  `bg3se_init`;
- only E fails: normal constructor/runtime activity required.

**Exact tooling / commands**

Build a minimal arm64 stub:

```bash
matrix_dir="$(mktemp -d /tmp/bg3se-null-matrix.XXXXXX)"
printf '%s\n' 'void bg3se_null_image(void) {}' > "$matrix_dir/null.c"
xcrun clang -arch arm64 -dynamiclib \
  -Wl,-install_name,@loader_path/libbg3se.dylib \
  -Wl,-no_uuid \
  -o "$matrix_dir/libbg3se-null.dylib" "$matrix_dir/null.c"
otool -L "$matrix_dir/libbg3se-null.dylib"
codesign -f -s - "$matrix_dir/libbg3se-null.dylib"
```

Hash everything before substitution:

```bash
bg3_dylib="/Users/tomdimino/Library/Application Support/Steam/steamapps/common/Baldurs Gate 3/Baldur's Gate 3.app/Contents/MacOS/libbg3se.dylib"
shasum -a 256 "$bg3_exec" "$bg3_exec.bg3se-original" "$bg3_dylib"
otool -l "$bg3_exec" | sed -n '/LC_LOAD_WEAK_DYLIB/,+5p'
```

Use one guarded launcher that never builds, deploys, repatches, retries, or
replaces the owned PID during the matrix.

**Effort:** M, 1–2 hours once the launch guard is reliable.  
**Expected information gain:** Medium-high only as a residual investigation;
near-zero before removing the established bad hook.

**Risk**

⚠️ **Steam-install risk.** This temporarily replaces the deployed dylib and
changes patched/unpatched state. Preserve exact hashes, never overwrite
`.bg3se-original`, require the game to be stopped, and restore in a shell trap.
Do not run it merely to reconfirm the already established StaticData cause.

### 9. Export-namespace quarantine build

**What**

Build BG3SE with hidden default visibility and an explicit minimal exported
symbol list while keeping constructor/runtime behavior unchanged.

**Why it could be decisive or a force-multiplier**

The current dylib exposes roughly 399 global symbols per architecture,
including embedded Lua and Dobby entry points. BG3 uses two-level namespace
binding, so ordinary collisions are less likely, but the executable advertises
weak binding and a plugin can use `dlsym(RTLD_DEFAULT, ...)`.

A hidden build tests an injection-specific hazard without removing threads,
allocations, or subsystems. It also narrows the callable surface exposed to
other in-process mods.

**Exact tooling / commands**

```bash
nm -arch arm64 -gjU build/lib/libbg3se.dylib \
  | sort -u > /tmp/bg3se.exports
nm -arch arm64 -um "$bg3_exec" \
  | sed -E 's/^.* //g' | sort -u > /tmp/bg3.undefined
comm -12 /tmp/bg3se.exports /tmp/bg3.undefined
```

Diagnostic build:

```cmake
target_compile_options(bg3se PRIVATE
    -fvisibility=hidden
    -fvisibility-inlines-hidden
)
```

```bash
cmake -S . -B build-hidden -DCMAKE_BUILD_TYPE=RelWithDebInfo
cmake --build build-hidden --target bg3se -j"$(sysctl -n hw.logicalcpu)"
nm -arch arm64 -gjU build-hidden/lib/libbg3se.dylib | sort
otool -L build-hidden/lib/libbg3se.dylib
```

If public entry points are required, use a linker exported-symbol list
containing only those names.

**Effort:** M, 1–2 days including visibility breakage.  
**Expected information gain:** Medium; high only if a post-fix residual depends
on image presence but not constructor execution.

**Risk:** Separate-build risk only until deployment. ⚠️ Deploying the variant
temporarily changes the Steam app's dylib; use the same hash/restore discipline
as Proposal 8.

## Data Flow

```text
Established stale StaticData patch
        |
        +--> external Dobby census --------+
        |                                  |
        +--> executable-page attestation --+--> actual runtime mutation truth
                                                   |
                                                   v
symbol-derived UUID manifest --> cross-build migration --> batch admission
                                                   |
                                                   v
                                      only validated plans can patch

historic IPS + hook manifests --> proximity miner --> next collision ranked

Dobby/custom hook code --> ABI canary/fuzz harness --> relocator confidence

post-fix residual only --> null-image matrix --> export quarantine if indicated
```

## Trade-offs

| Decision | Pros | Cons |
|---|---|---|
| Observe patch APIs externally | Does not trust SE's toggle/log semantics | Requires debugger-from-start |
| Attest executable bytes | Catches every patch mechanism | Mach-O/ASLR mapping work |
| Derive from current symbols | Eliminates many stale offsets | Future stripped builds need fallback |
| Migrate semantically across versions | Reviews the full hook set together | Needs old binary and confidence policy |
| Mine crash/patch geometry | Cheaply ranks hidden SE causes | Proximity is evidence, not proof |
| Test trampolines in a helper | No BG3 required; repeatable | Synthetic functions cannot cover every game prologue |
| Admit hooks as a batch | Prevents partially compatible patch sets | Centralization is a real refactor |
| Keep loader matrix residual-only | Preserves a clean escape hatch | Temporarily touches Steam deployment |

## Recommended Execution Order

1. Accept the debugger report's direct cause and fix/disable the stale
   StaticData hook family; do not spend live runs on disproven Metal, allocator,
   or input hypotheses.
2. Before the confirmation launch, use Proposal 3 to generate the current
   build's five StaticData symbol addresses.
3. During the confirmation launch, use Proposal 1 to record every actual patch.
4. Use Proposal 2 to reconcile mapped `__text` changes with that census.
5. Add Proposal 5 immediately; it is small and turns the present discovery into
   a reusable crash-triage rule.
6. Build Proposal 6 before expanding ARM64 hooks for parity work.
7. Adopt Proposal 4 and Proposal 7 as the durable game-update workflow.
8. Run Proposals 8/9 only if a post-fix residual survives.

## Exit Criteria

The lateral program is successful when:

- every runtime code patch is observed by both the API census and page
  attestation;
- every patch belongs to a UUID-matching symbol-derived manifest;
- no hook target is inside a different symbol or overlaps another patch;
- the StaticData plan is admitted or rejected as one compatibility unit;
- the ABI canary passes Dobby and the custom ARM64 hooker;
- every future `.ips` is automatically correlated with nearby runtime hook
  targets before manual triage.

## Risks Requiring Explicit Attention

- ⚠️ Proposals 8 and 9 temporarily change files inside the Steam app.
- ⚠️ Proposal 7 is a material hook-architecture refactor and requires approval
  before implementation.
- LLDB and runtime memory reads can perturb scheduling but do not change the
  system.
- The trampoline fuzzer must never execute generated instructions inside BG3.
- None of the proposed offline tools should write to the game binary.

## Sources and Evidence

- `docs/bugs/wave-campaign-progress.md`
- `docs/plans/2026-07-28-001-feat-parity-closure-codex-wave-campaign-plan.md`
- `docs/bugs/codex-debugger-nohooks-2026-07-28.md`
- `docs/bugs/codex-methodology-audit-2026-07-28.md`
- `docs/bugs/codex-headless-design-2026-07-28.md`
- `src/staticdata/staticdata_manager.c`
- `src/hooks/arm64_hook.c`
- `src/hooks/arm64_decode.c`
- `src/game/video_skip.c`
- `src/injector/main.c`
- `tools/bg3se_harness/patch.py`
