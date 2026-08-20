# Reach honest Windows parity closure on `feat/qedeshot-parity`

This ExecPlan is a living document. Sections Progress, Surprises &
Discoveries, Decision Log, and Outcomes & Retrospective must be
kept up to date as work proceeds.

## Purpose / Big Picture

The branch `feat/qedeshot-parity` already closes most of the obvious parity
gaps, but the remaining work is uneven: one gap is blocked by reverse
engineering tooling, one is blocked by a runtime offset that is still an
estimate, one is blocked by missing replication internals, one was just
re-enabled and needs proof, and one is partly solved but still intentionally
gated for safety. The practical goal of this plan is to move the project from
"roughly 94% parity with known caveats" to an honest end state that can be
demonstrated.

There are only two acceptable end states.

The first acceptable end state is literal feature closure for every remaining
agreed-in-scope Windows API outside the already excluded Noesis UI layer.
Someone can see that working by running the parity scan, the Lua test suite,
combat-event tests, component mutation tests, audio smoke tests, and MCM
compatibility checks, and by finding no remaining stubs for those APIs.

The second acceptable end state is an explicit project-scope correction: the
code and docs state that bg3se-macos has reached 100% parity for the supported
macOS surface area, while `Ext.UI` remains a permanent non-goal and
entity-replication APIs remain deferred until the replication subsystem exists.
Someone can see that working by running the same validation commands and then
checking that `ROADMAP.md`, `CLAUDE.md`, `agent_docs/api-status.md`, and the
parity baseline all agree on the denominator and the exclusions.

This plan assumes the project will not claim "100% Windows BG3SE parity" until
the team chooses one of those two end states and makes the docs truthful.

## Progress

- [x] (2026-04-02 07:22Z) Confirmed current working branch is `feat/qedeshot-parity` and captured the current head commit `4a29138` ("re-enable PlayExternalSound with verified STDString construction").
- [x] (2026-04-02 07:23Z) Mapped each remaining parity gap to its current code owner and reverse-engineering evidence.
- [x] (2026-04-02 07:24Z) Wrote this ExecPlan and saved it as a handoff artifact in `plans/`.
- [ ] Rebuild the installed the Ghidra HTTP bridge extension from `/Users/tomdimino/ghidra/the Ghidra HTTP bridge/` so the HTTP bridge exposes `POST /create_function`.
- [ ] Recover and verify ARM64 `BeforeDealDamage` and `DealDamage` hook targets, then wire live hooks and tests.
- [ ] Runtime-probe `ComponentOpsRegistry` inside `EntityWorld`, replace the current estimate with a verified offset, and ungate `CreateComponent` and `RemoveComponent`.
- [ ] Decide whether entity replication is truly implementable on this branch or must be formally deferred from the parity claim.
- [ ] Prove `PlayExternalSound` with a live smoke test and decide whether any remaining ABI caveats require gating.
- [ ] Update docs, parity baseline, and acceptance text so the reported parity number matches reality.

## Surprises & Discoveries

- Observation: `Ext.UI` is not absent; it already exists as a graceful stub layer for MCM compatibility.
  Evidence: `/Users/tomdimino/Desktop/Programming/game-modding/bg3/bg3se-macos/src/lua/lua_ui.c` explicitly registers `Ext.UI` and states that Noesis UI is not available on macOS.

- Observation: the local source checkout of the xebyte the Ghidra HTTP bridge fork already contains the missing `create_function` endpoint, so the blocker is the installed extension JAR, not the source tree.
  Evidence: `/Users/tomdimino/ghidra/the Ghidra HTTP bridge/src/main/java/com/xebyte/core/EndpointRegistry.java` and `/Users/tomdimino/ghidra/the Ghidra HTTP bridge/src/main/java/com/xebyte/core/FunctionService.java` both contain `/create_function`, while `/Users/tomdimino/Desktop/Programming/game-modding/bg3/bg3se-macos/ghidra/offsets/FUNCTORS.md` records that the live bridge currently returns HTTP 404.

- Observation: `ComponentOpsRegistry` is still based on an estimate, and the estimate moved from `0x368` to roughly `0x390`.
  Evidence: `/Users/tomdimino/Desktop/Programming/game-modding/bg3/bg3se-macos/ghidra/offsets/ENTITY_SYSTEM.md` marks the offset as unconfirmed and prescribes a runtime probe over `EntityWorld + 0x350..0x3B0`.

- Observation: version mismatch tolerance is already partially solved for data reads, but not for code patching.
  Evidence: `/Users/tomdimino/Desktop/Programming/game-modding/bg3/bg3se-macos/src/core/version_detect.c` uses sentinel probes to tolerate minor layout-preserving hotfixes, and `/Users/tomdimino/Desktop/Programming/game-modding/bg3/bg3se-macos/src/injector/main.c` still skips Dobby functor hooks unless `version_detect_matches()` is true.

- Observation: entity replication is much less complete than the transport layer suggests.
  Evidence: `/Users/tomdimino/Desktop/Programming/game-modding/bg3/bg3se-macos/src/entity/entity_system.c` returns `nil` for `GetNetId()` and logs that `entity:Replicate()` is a stub no-op, while `/Users/tomdimino/Desktop/Programming/game-modding/bg3/bg3se-macos/src/network/net_hooks.c` and `/Users/tomdimino/Desktop/Programming/game-modding/bg3/bg3se-macos/src/lua/lua_net.c` only cover the extender message transport, not `ReplicationManager` or `EntityToNetId`.

- Observation: `PlayExternalSound` has crossed from "disabled" to "implemented but not yet proven stable in live use".
  Evidence: head commit `4a29138` re-enabled it, and `/Users/tomdimino/Desktop/Programming/game-modding/bg3/bg3se-macos/src/audio/audio_manager.c` now builds `ls::STDString` with a verified constructor path.

## Decision Log

- Decision: treat `Ext.UI` as an agreed project exclusion unless the team explicitly starts a separate Noesis integration program.
  Rationale: the current codebase already ships Noesis stubs for compatibility, the roadmap still lists real `Ext.UI` work as deep and costly, and the user explicitly called it intentionally excluded.
  Date/Author: 2026-04-02 / Codex

- Decision: do not use the phrase "100% Windows BG3SE parity" in docs or release notes unless replication and remaining hook work are either finished or explicitly removed from the claim.
  Rationale: the current code still contains real stubs and safety gates in those areas.
  Date/Author: 2026-04-02 / Codex

- Decision: make the the Ghidra HTTP bridge rebuild the first execution milestone.
  Rationale: `BeforeDealDamage` and `DealDamage` are on the critical path, and the local fork already appears to have the missing endpoint. This is the fastest way to unblock static analysis without guessing hook addresses.
  Date/Author: 2026-04-02 / Codex

- Decision: verify `ComponentOpsRegistry` with a runtime probe instead of promoting `~0x390` from estimate to fact.
  Rationale: a wrong write offset here risks silent memory corruption during component creation and removal.
  Date/Author: 2026-04-02 / Codex

- Decision: treat entity replication as a go or no-go milestone, not a cosmetic parity checkbox.
  Rationale: the project already has rate-limited extender networking, but it does not yet have proven `ReplicationManager`, `EntityToNetId`, component whitelist, or replication-flag plumbing. Shipping fake parity here would mislead users and mod authors.
  Date/Author: 2026-04-02 / Codex

## Outcomes & Retrospective

Pending execution.

If this plan succeeds without scope changes, the branch will close the remaining
in-scope parity gaps with live evidence for combat hooks, component mutation,
audio, and compatibility. If it succeeds with an explicit scope correction, the
project will still be in a much better state: the unsupported areas will be
named honestly, the parity percentage will stop drifting away from reality, and
future work will have a clean boundary instead of carrying ambiguous claims.

## Context and Orientation

This repository is a macOS port of Norbyte's Script Extender for Baldur's Gate
3. The immediate work happens on branch `feat/qedeshot-parity`, which already
contains about 23 recent parity-focused commits. The branch is not green by
definition just because functions are registered. The project only counts as
working when the underlying engine calls, hooks, or data paths are proven safe.

A few terms of art appear repeatedly in this plan.

A "sentinel probe" means a safe `vm_read` against a known global address to
check whether a BG3 hotfix preserved the binary layout closely enough for data
reads to remain valid.

A "VMT" means a virtual method table, which is the table of function pointers
used by a C++ class for virtual calls.

A "runtime probe" means a temporary diagnostic read against the live game
process to verify an offset or pointer chain, rather than trusting static
reverse engineering alone.

"Dobby" is the inline hook library used by the project to patch machine code at
runtime. Code patching is stricter than data reads and remains gated behind an
exact version match today.

"Noesis" is the Windows BG3 UI framework behind `Ext.UI`. The project already
provides stubs for mod compatibility, but it does not implement the real UI
hooks.

The key files and directories for this plan are:

`/Users/tomdimino/Desktop/Programming/game-modding/bg3/bg3se-macos/CLAUDE.md` explains the current branch goals, harness commands, and version-mismatch behavior.

`/Users/tomdimino/Desktop/Programming/game-modding/bg3/bg3se-macos/ROADMAP.md` is the public parity matrix and still includes replication and `Ext.UI` discussion that must match reality at the end.

`/Users/tomdimino/Desktop/Programming/game-modding/bg3/bg3se-macos/agent_docs/api-status.md` is the short-form public API status summary.

`/Users/tomdimino/Desktop/Programming/game-modding/bg3/bg3se-macos/ghidra/offsets/FUNCTORS.md` holds the current damage-hook research, signatures, and the `create_function` blocker.

`/Users/tomdimino/Desktop/Programming/game-modding/bg3/bg3se-macos/ghidra/offsets/ENTITY_SYSTEM.md` holds the `ComponentOpsRegistry` estimate and the runtime probe strategy.

`/Users/tomdimino/Desktop/Programming/game-modding/bg3/bg3se-macos/src/stats/functor_hooks.c` is the natural home for `BeforeDealDamage` and `DealDamage` once addresses are verified.

`/Users/tomdimino/Desktop/Programming/game-modding/bg3/bg3se-macos/src/lua/lua_events.c` owns event registration and event object exposure to Lua.

`/Users/tomdimino/Desktop/Programming/game-modding/bg3/bg3se-macos/src/entity/entity_system.c` owns `GetNetId`, `Replicate`, `CreateComponent`, and `RemoveComponent` behavior.

`/Users/tomdimino/Desktop/Programming/game-modding/bg3/bg3se-macos/src/network/net_hooks.c`, `/Users/tomdimino/Desktop/Programming/game-modding/bg3/bg3se-macos/src/network/message_bus.h`, and `/Users/tomdimino/Desktop/Programming/game-modding/bg3/bg3se-macos/src/lua/lua_net.c` show what network infrastructure exists today.

`/Users/tomdimino/Desktop/Programming/game-modding/bg3/bg3se-macos/src/audio/audio_manager.c` and `/Users/tomdimino/Desktop/Programming/game-modding/bg3/bg3se-macos/src/lua/lua_audio.c` own `PlayExternalSound`.

`/Users/tomdimino/Desktop/Programming/game-modding/bg3/bg3se-macos/src/core/version_detect.c` and `/Users/tomdimino/Desktop/Programming/game-modding/bg3/bg3se-macos/src/injector/main.c` own the safety policy for version mismatches.

`/Users/tomdimino/Desktop/Programming/game-modding/bg3/bg3se-macos/tools/bg3se_harness/catalog/windows_parity_baseline.json` is the parity denominator that must agree with the docs.

There is one external but local dependency that matters for this plan.

`/Users/tomdimino/ghidra/the Ghidra HTTP bridge/` is the xebyte fork source tree whose Java
code already exposes `POST /create_function`.

`/Users/tomdimino/ghidra/Ghidra/Extensions/the Ghidra HTTP bridge/lib/the Ghidra HTTP bridge.jar` is the
installed Ghidra extension JAR that must be rebuilt and replaced.

## Plan of Work

Start by freezing the meaning of the parity claim. Before writing any more
implementation code, decide whether the project is still pursuing literal
Windows parity for every non-Noesis feature, or whether it is pursuing full
parity for the supported macOS surface area. This is not paperwork. It changes
the denominator for the parity scan, the acceptance criteria for the branch,
and whether replication is treated as blocking.

Next, unblock reverse engineering. Rebuild the local xebyte the Ghidra HTTP bridge fork,
replace the installed extension JAR, restart Ghidra, and prove that
`POST /create_function` returns success instead of HTTP 404. Only after that
should the team spend more time on `BeforeDealDamage` and `DealDamage`, because
the current branch already knows the signatures and candidate addresses but
lacks the function objects needed for decompilation.

Once Ghidra is unblocked, finish the damage-hook work in
`src/stats/functor_hooks.c`, `src/lua/lua_events.c`, and any required
registration in `src/injector/main.c`. The goal is not just to expose event
objects. The goal is to prove that live combat emits `BeforeDealDamage` and
`DealDamage` with stable payloads, without violating the current version-safety
rules for code patching.

After that, finish component mutation. Add a temporary, well-scoped runtime
probe to `src/entity/entity_system.c` or a nearby debug entry point so the
branch can scan `EntityWorld + 0x350..0x3B0`, identify the real
`ComponentOpsRegistry`, and replace the current estimate with a verified offset.
Then remove the gate on `CreateComponent` and `RemoveComponent`, but only for
safe component classes. Preserve the current plan's safety posture: validate
type index bounds, blacklist one-frame components, and avoid mutation during an
engine iteration if removal still requires deferral.

Then resolve the hardest product question: replication. Read the current code
as if it were a new subsystem. If the project can map `EntityToNetId`, locate
the actual replication manager, define server-only guards, and implement
flag-safe replication without inventing semantics, do that work on this branch.
If it cannot, stop pretending this is a small remaining gap. Move it into an
explicit deferred milestone, remove it from any claim of completed parity, and
document the dependency on replication internals.

Finally, prove the less risky items. `PlayExternalSound` has already been
re-enabled, so this milestone is a smoke test and a guard-rail decision, not a
large implementation effort. Version-mismatch tolerance already uses sentinel
probes for data reads, so the remaining work is to document and validate the
matrix clearly: what works on a minor hotfix, what remains gated, and what log
messages the user should expect. End by updating the parity baseline, public
docs, and changelog so the branch state, docs, and parity scan all tell the
same story.

## Concrete Steps

1. Capture the branch state and current parity denominator.

   Working directory: `/Users/tomdimino/Desktop/Programming/game-modding/bg3/bg3se-macos`

   Run:

   ```bash
   git branch --show-current
   git log --oneline -23
   PYTHONPATH=tools python3 -m bg3se_harness parity missing
   ```

   Expected output:

   The branch name is `feat/qedeshot-parity`. The head commit starts with
   `4a29138`. The missing-parity list still mentions the remaining event,
   entity, replication, or denominator issues that this plan is meant to close
   or formally defer.

2. Verify that the local the Ghidra HTTP bridge source tree already contains the missing endpoint, then rebuild the extension.

   Working directory: `/Users/tomdimino/ghidra/the Ghidra HTTP bridge`

   Run:

   ```bash
   rg -n "create_function" src/main/java/com/xebyte/core/EndpointRegistry.java src/main/java/com/xebyte/core/FunctionService.java
   mvn clean package assembly:single
   cp target/the Ghidra HTTP bridge.jar /Users/tomdimino/ghidra/Ghidra/Extensions/the Ghidra HTTP bridge/lib/the Ghidra HTTP bridge.jar
   ```

   Expected output:

   `rg` prints both endpoint definitions. Maven ends with `BUILD SUCCESS`. The
   installed JAR at `/Users/tomdimino/ghidra/Ghidra/Extensions/the Ghidra HTTP bridge/lib/the Ghidra HTTP bridge.jar`
   receives a fresh timestamp.

3. Restart Ghidra with the BG3 macOS binary loaded and prove the endpoint is live.

   Working directory: any shell directory

   Run:

   ```bash
   curl -s -o /tmp/create_function_probe.json -w "%{http_code}\n" \
     -X POST http://127.0.0.1:8080/create_function \
     -H 'Content-Type: application/json' \
     -d '{"address":"0x10538f374"}'
   cat /tmp/create_function_probe.json
   ```

   Expected output:

   The HTTP status is `200`, not `404`. The JSON body confirms success or says
   that the function already exists. Either result is acceptable. A `404` means
   the branch is still blocked and no further damage-hook RE should be trusted.

4. Recover the damage-hook functions and update the reverse-engineering notes before editing runtime code.

   Working directory: `/Users/tomdimino/Desktop/Programming/game-modding/bg3/bg3se-macos`

   Run:

   ```bash
   curl -s -X POST http://127.0.0.1:8080/create_function \
     -H 'Content-Type: application/json' \
     -d '{"address":"0x10538f374"}'
   curl -s "http://127.0.0.1:8080/decompile_function?address=0x10538f374"
   curl -s -X POST http://127.0.0.1:8080/create_function \
     -H 'Content-Type: application/json' \
     -d '{"address":"0x105783a38"}'
   curl -s "http://127.0.0.1:8080/decompile_function?address=0x105783a38"
   ```

   Expected output:

   The decompiler returns actual function bodies or at least structured control
   flow, not "No function found". Update `ghidra/offsets/FUNCTORS.md` with the
   final verified addresses and any calling-convention notes before wiring the
   hooks.

5. Implement and validate `BeforeDealDamage` and `DealDamage`.

   Working directory: `/Users/tomdimino/Desktop/Programming/game-modding/bg3/bg3se-macos`

   Edit these files:

   `src/stats/functor_hooks.c`

   `src/stats/functor_hooks.h`

   `src/lua/lua_events.c`

   `src/injector/main.c` if hook registration or safety gating changes are required

   Build and test with:

   ```bash
   cmake --build build
   PYTHONPATH=tools python3 -m bg3se_harness run "local id=Ext.Events.BeforeDealDamage:Subscribe(function(e) _P('BeforeDealDamage fired') end)"
   PYTHONPATH=tools python3 -m bg3se_harness run "local id=Ext.Events.DealDamage:Subscribe(function(e) _P('DealDamage fired') end)"
   ```

   Expected output:

   The build succeeds. In a save where combat damage can occur, the console log
   shows both events firing at least once without crashing. If a live combat
   setup is required, use a disposable combat save and record the exact save in
   the Progress section when it is chosen.

6. Verify `ComponentOpsRegistry` at runtime before enabling `CreateComponent` and `RemoveComponent`.

   Working directory: `/Users/tomdimino/Desktop/Programming/game-modding/bg3/bg3se-macos`

   Edit these files:

   `src/entity/entity_system.c`

   `src/entity/entity_system.h` if a temporary probe entry point is exported

   `ghidra/offsets/ENTITY_SYSTEM.md`

   After adding the probe helper, run:

   ```bash
   cmake --build build
   PYTHONPATH=tools python3 -m bg3se_harness run "Ext.Debug.ProbeComponentOps()"
   ```

   Expected output:

   Exactly one candidate offset in the `0x350..0x3B0` range looks like a
   registry whose entry VMT contains a valid code pointer at index `4`. Record
   the verified offset in `ghidra/offsets/ENTITY_SYSTEM.md`, replace the
   estimate in code, and only then expose live `CreateComponent` and
   `RemoveComponent` behavior.

7. Prove component mutation on safe components and reject unsafe ones loudly.

   Working directory: `/Users/tomdimino/Desktop/Programming/game-modding/bg3/bg3se-macos`

   Run after the implementation lands:

   ```bash
   PYTHONPATH=tools python3 -m bg3se_harness run "local e=Ext.Entity.Get(Osi.GetHostCharacter()); local ok=e:CreateComponent('esv::inventory::IsReplicatedComponent'); _P(tostring(ok))"
   PYTHONPATH=tools python3 -m bg3se_harness run "local e=Ext.Entity.Get(Osi.GetHostCharacter()); local ok=e:RemoveComponent('esv::inventory::IsReplicatedComponent'); _P(tostring(ok))"
   ```

   Expected output:

   Safe test components can be added and removed without a crash. Unsafe or
   blacklisted component classes produce a clear Lua error or a `false` return
   instead of corrupting the session.

8. Decide replication scope based on code, not optimism.

   Working directory: `/Users/tomdimino/Desktop/Programming/game-modding/bg3/bg3se-macos`

   Run:

   ```bash
   rg -n "ReplicationManager|EntityToNetId|GetNetId|Replicate\\(|ReplicationFlags" src/entity src/network src/lua
   ```

   Expected output:

   This command should make the current gap obvious. If a real implementation
   is added, the search must show a concrete `EntityToNetId` path,
   server-context guards, and flag accessors. If that work does not land, this
   exact search should still show why replication was deferred, and the docs and
   baseline must remove it from the completed parity claim.

9. Smoke-test `PlayExternalSound` and record whether long paths are now safe.

   Working directory: `/Users/tomdimino/Desktop/Programming/game-modding/bg3/bg3se-macos`

   Run:

   ```bash
   PYTHONPATH=tools python3 -m bg3se_harness run "return Ext.Audio.PlayExternalSound('Global', 'Play_UI_Click', 'Data/Public/Sounds/Test/test.ogg', 1, 0.0)"
   ```

   Expected output:

   The call returns `true` and does not crash the game. If a path-length limit
   still exists, document the exact failing threshold and re-gate the function
   rather than leaving a silent crash risk.

10. Run the final proof set and update the public documents.

    Working directory: `/Users/tomdimino/Desktop/Programming/game-modding/bg3/bg3se-macos`

    Run:

    ```bash
    cmake --build build
    PYTHONPATH=tools python3 -m bg3se_harness test
    PYTHONPATH=tools python3 -m bg3se_harness parity scan
    PYTHONPATH=tools python3 -m bg3se_harness compat run mcm
    ```

    Expected output:

    The build succeeds, the test suite passes, the parity scan reports the
    agreed denominator, and MCM compatibility still passes. Then update
    `ROADMAP.md`, `CLAUDE.md`, `agent_docs/api-status.md`, and
    `tools/bg3se_harness/catalog/windows_parity_baseline.json` so they all tell
    the same story.

## Validation and Acceptance

Success is not one number. Every gate below must be satisfied.

The reverse-engineering gate passes when `POST /create_function` works against
the running Ghidra bridge and the verified addresses for `ThrowDamageEvent` and
`ApplyDamage` are recorded in `ghidra/offsets/FUNCTORS.md`.

The damage-hook gate passes when `Ext.Events.BeforeDealDamage` and
`Ext.Events.DealDamage` both fire in live combat on a disposable test save and
the game stays stable.

The component-mutation gate passes when `CreateComponent` and
`RemoveComponent` work on at least one safe test component, reject blacklisted
or unsafe types without crashing, and stop relying on an estimated offset.

The replication gate passes in one of two ways. It passes either because
`GetNetId`, `Replicate`, `GetReplicationFlags`, and `SetReplicationFlags` are
backed by real engine plumbing and documented guards, or because the project
explicitly defers them and removes them from any claim of completed parity.

The audio gate passes when `PlayExternalSound` succeeds in a live smoke test
and its remaining caveats, if any, are documented.

The safety-policy gate passes when the docs clearly state that sentinel probes
allow data-read features to remain active across safe hotfixes, while code
patching stays gated behind exact version matches until proven otherwise.

The documentation gate passes when `ROADMAP.md`, `CLAUDE.md`,
`agent_docs/api-status.md`, and the parity baseline all agree on the same
numerator, denominator, exclusions, and wording.

## Idempotence and Recovery

Most steps in this plan are repeatable.

Rebuilding `/Users/tomdimino/ghidra/the Ghidra HTTP bridge/` with Maven is safe to repeat.
Replacing the installed JAR is also safe to repeat. If `POST /create_function`
already succeeds, the rebuild milestone is already complete and can be marked
done.

Creating functions in Ghidra is usually idempotent. If the endpoint reports
that a function already exists, treat that as success and continue.

The build, parity scan, test suite, and compatibility commands are all safe to
rerun. Always prefer rerunning them over trusting stale output.

Component mutation and combat-hook testing are not safe against important save
files. Use a disposable save dedicated to parity testing. If a test corrupts
state, quit the game without saving, relaunch, and rerun against a fresh test
save.

If a milestone produces bad code, recover by reverting only the specific files
or commits associated with that milestone. The branch head before this plan is
`4a29138`, which is the stable reference point for "before parity closure work".
Do not roll back unrelated user changes in the worktree.

## Interfaces and Dependencies

At completion, the following interfaces must exist and be truthful.

`Ext.Events.BeforeDealDamage` must be a real event object backed by a live hook,
not just a registered placeholder.

`Ext.Events.DealDamage` must be a real event object backed by a live hook, not
just a registered placeholder.

`entity:CreateComponent(name)` and `entity:RemoveComponent(name)` must call the
real engine paths through a verified `ComponentOpsRegistry` offset and must
reject unsafe component classes deterministically.

`entity:GetNetId()`, `entity:Replicate(name)`, `entity:GetReplicationFlags()`,
and `entity:SetReplicationFlags(flags)` must either be implemented against the
real replication subsystem or be documented as deferred and excluded from the
completed parity claim.

`Ext.Audio.PlayExternalSound(soundObject, eventName, filePath[, codec[, positionSec]]) -> boolean`
must remain callable from Lua and must have a documented safety envelope.

`version_detect_addresses_safe() -> bool` must continue to control data-read
features through sentinel probes, and Dobby-installed code hooks must remain
behind an exact-match safety policy unless the policy itself is deliberately
extended and validated.

The external dependency `/Users/tomdimino/ghidra/the Ghidra HTTP bridge/` must remain
buildable with Maven, and the installed extension JAR at
`/Users/tomdimino/ghidra/Ghidra/Extensions/the Ghidra HTTP bridge/lib/the Ghidra HTTP bridge.jar` must
expose `POST /create_function` during this work.

## Documentation Parity (Nomos Audit — 2026-04-02)

The Qedeshot swarm added ~35 new API functions, 10 events, 29 tests, and
multiple subsystems (sentinel probes, STDString ABI, functor hooks) that are
not reflected in any documentation file. A Nomos subdaimon audit identified
the full scope at `.subdaimon-output/nomos-1775116318.md`.

### Ground Truth Delta

| Metric | Documented | Actual | Delta |
|--------|-----------|--------|-------|
| Version | v0.36.50 | **v0.37.1** | Stale in ROADMAP, api-status |
| Ext.Level | 9 functions | **16** | +7 (RaycastAll, 6 Sweeps) |
| Ext.Audio | 13 functions | **18** | +5 (PlayExternalSound, 4 Banks) |
| Ext.Types | 9 functions | **15** | +6 |
| Ext.Math | 57 functions | **66** | +9 |
| Ext.Loca | 1 function | **6** | +5 (CreateHandle, etc.) |
| Events | 33 | **~43** | +10 one-frame polling |
| Tests T1/T2 | 85/40 | **101/53** | +16/+13 |
| Parity | ~94% | **~96%** | Only Ext.UI + Debugger at 0% |

### File Update Priority

| Priority | File | State |
|----------|------|-------|
| **HIGH** | `agent_docs/api-status.md` | VERY STALE — the assistant reads every session |
| **HIGH** | `README.md` | Public-facing, wrong numbers |
| **MEDIUM** | `ROADMAP.md` | Feature Parity Matrix behind |
| **MEDIUM** | `agent_docs/development.md` | Test counts wrong |
| **LOW** | `docs/CHANGELOG.md` | Needs new version entry |
| **LOW** | `agent_docs/architecture.md` | Missing new modules |
| **NONE** | `ghidra/offsets/INDEX.md` | Already accurate |
| **NONE** | `agent_docs/reference.md` | Already accurate |

### Cross-File Inconsistencies

- Version: v0.36.50 in ROADMAP/api-status vs v0.37.1 in CHANGELOG/version.h
- Parity: ~94% everywhere, should be ~96%
- Test count: 85/40/125 everywhere, should be 101/53/154
- Events: "33" everywhere, should be ~43

### Parity Recalculation

Only two namespaces remain at 0%: Ext.UI (Noesis — excluded) and Debugger.
All other namespaces are at 76-100%+. Weighted overall: ~96%.

### Code Review Findings (Codex Reviewer — 2026-04-02)

In addition to parity work, the Codex Reviewer identified architectural
issues that should be addressed before or during the final push:

**Critical:** `osiris_call_by_id()` at main.c:2633 has an unsafe fallback
to `pfn_InternalCall` which expects a different struct. Fix: fail closed.

**Moderate (5):** main.c is 3,607 lines mixing 6 domains — extract
`osiris_bridge`, `mod_bootstrap`, `lua_runtime`, `injector_hooks` modules.
Osiris listener split between lua_osiris.c and main.c. Mod path duplication.
Leaky headers (input.h, math_ext.h). Enum cross-file externs without header.

**Minor (3):** luaL_dostring blobs, naming inconsistency, unused includes.
