# v0.38.0 pre-commit review — 2026-07-28

Scope: the complete tracked `git diff` plus the requested untracked files
`src/lua/lua_gate.c`, `src/lua/lua_gate.h`,
`scripts/session_driver.sh`, `tests/harness/test_launch_lifecycle.py`,
`tests/harness/test_monitor.py`, and `tests/harness/test_offset_audit.py`.
The other untracked diagnosis/evidence documents were treated as review inputs,
not release implementation. I did not launch BG3, Steam, or any input
automation.

Release recommendation: **do not ship v0.38.0 yet.** The build and offline
tests are green, but seven production-path issues below contradict core
stability/lifecycle claims in the release notes.

## Ship blockers

- [`src/core/version_detect.h:25`] The release migrates hardcoded addresses to
  game build `4.1.1.7209685`, but runtime compatibility still declares
  `4.1.1.6995620` as known-good.

  The mismatch is not cosmetic. The three sentinel addresses remain the old
  values at `src/core/version_detect.c:30-34`, while, for example, the server
  and client singleton addresses moved at `src/entity/entity_system.c:93-101`
  and the SpellPrototype singleton moved at
  `src/stats/prototype_managers.c:46-47`.
  `version_detect_addresses_safe()` treats a successful `vm_read` as proof of
  layout compatibility (`src/core/version_detect.c:203-229`). Old addresses
  can remain readable because they are still inside mapped DATA/BSS pages, so
  this can enable migrated address-dependent subsystems on a false-positive
  probe. Conversely, the exact-version condition at
  `src/injector/main.c:3467-3481` disables all nine new functor hooks on the
  current supported build.

  The new offset audit does not close the gap: its purported exhaustive list
  ends at `tests/harness/test_offset_audit.py:28-77` and omits all nine Dobby
  targets at `src/stats/functor_types.h:377-385`. Thus merely bumping
  `BG3_KNOWN_VERSION` would turn on unverified code patches.

  **Suggestion:** make the supported-version constant, all sentinels, and all
  migrated offsets one coherent `4.1.1.7209685` contract. Do not use page
  readability as layout identity; verify the expected symbols/values or fail
  closed. Add every functor patch target to the binary audit (or leave that
  family independently disabled) before updating the exact-version gate.

- [`src/lua/lua_imgui.c:2325`] One render-thread Lua entry still resolves
  registry-backed state before taking the Lua gate.

  `lua_imgui_fire_event()` reads `s_imgui_lua_state` and calls
  `imgui_object_get_event()` at lines 2325-2334, then acquires the gate at line
  2341. While it waits, gated Lua code can replace/unregister that callback,
  destroy the object, `luaL_unref()` the registry slot, and reuse the numeric
  ref. The waiter can consequently invoke the wrong function (or a stale ref)
  even though it re-resolves the `lua_State` itself under the gate.

  Similar optimistic reads remain outside the synchronization domain in the
  hotkey path (`src/input/lua_input.c:35-44`), log callback
  (`src/lua/lua_events.c:1839-1850`), and functor path
  (`src/stats/functor_hooks.c:64-75`). Those latter paths re-check the state
  before Lua use, so the immediate use-after-close window is fixed, but the
  plain cross-thread pointer/handler-count reads are still C data races. I did
  not find a new logging-mutex/Lua-gate deadlock: logging now releases its
  mutex before callbacks, and the recursive Lua gate covers nested same-thread
  entries.

  **Suggestion:** acquire the gate before the first liveness, callback-ref,
  recursion-state, or handler-count read. In the ImGui path, resolve both the
  live state and event ref under the gate and keep it held through the
  `lua_pcall`.

- [`tools/bg3se_harness/launch.py:1204`] The new `!identity` handshake is never
  used by a harness client.

  `wait_for_socket()` still sends only `Ext.GetVersion()` and accepts any
  non-empty response at lines 1204-1252. It never parses the native identity
  JSON, never compares its `pid`, and never requires
  `session_init == "complete"` or the appropriate `stats_ready` state.
  `LOCAL_PEERPID` is also fail-open on any `getsockopt`/decode error at
  `tools/bg3se_harness/launch.py:186-196`; the new test explicitly blesses
  that behavior at `tests/harness/test_launch_lifecycle.py:167-172`.

  This leaves intact the exact failure mode the changelog says is closed:
  another BG3SE process can rebind `/tmp/bg3se.sock`, answer the version
  expression, and be trusted even when it is at the menu with no initialized
  session.

  **Suggestion:** send `!identity`, robustly extract/parse its JSON, fail closed
  unless its PID matches the tracked process, and require readiness appropriate
  to the requested tier before returning `socket_connected=True`. Treat
  unavailable peer credentials as unverified, not success; the in-band
  identity can provide the portable fallback.

- [`tools/bg3se_harness/_monitor.py:83`] Detached/background monitoring cannot
  recognize the documented Steam exit-0 bounce.

  `_TrackerProcess.poll()` has no `Popen` handle and maps every dead tracked PID
  to `returncode = -1` at lines 83-88. The monitor passes this adapter to
  `wait_for_socket()` at lines 182-185. That function calls
  `ProcessTracker.record_direct_exit(-1)` at
  `tools/bg3se_harness/launch.py:1048-1058`, but the state machine only treats
  exact exit code `0` as a bounce at
  `tools/bg3se_harness/launch.py:290-305`. Background mode therefore returns
  `process_exited` instead of waiting for/adopting Steam's replacement.

  `tests/harness/test_monitor.py:53-57` asserts the broken `-1` mapping, while
  the restore tests at lines 110-165 reimplement monitor conditions instead of
  calling the monitor/wait state machine.

  **Suggestion:** represent detached exit status as unknown and define a
  bounded, identity-checked bounce transition for that state, or preserve the
  direct child's real exit status for the monitor. Add an integration-level
  `wait_for_socket()` test using `_TrackerProcess` that kills the direct PID,
  presents one successor, and proves adoption.

- [`tools/bg3se_harness/launch.py:307`] `ProcessTracker` records identity
  metadata but does not enforce it when adopting, monitoring, or cleaning up.

  `try_adopt()` accepts a candidate when its executable is unavailable, and
  otherwise checks only the basename suffix; it does not compare the exact
  `expected_executable`, ensure the start time is after this launch attempt, or
  validate `requested_args` (lines 307-329). `is_process_alive()` is only
  `kill(pid, 0)` (`tools/bg3se_harness/launch.py:339-348`), so PID reuse is
  indistinguishable from the tracked process. `_read_pid_file()` similarly
  ignores the stored start time and exact executable
  (`tools/bg3se_harness/launch.py:436-450`), after which `launch()` immediately
  sends that PID `SIGTERM` at lines 831-839. A stale launch record can therefore
  classify and terminate a different BG3 process as harness-owned.

  The test section labeled “Mismatched args/executable rejection” checks only
  an obviously different executable
  (`tests/harness/test_launch_lifecycle.py:110-121`); no argument or creation
  time is tested. `scripts/session_driver.sh:131-139` likewise calls PID plus
  start time “full identity” without checking executable, arguments, launch
  lineage, or the console identity.

  **Suggestion:** validate the stored `(exact executable, start time)` tuple on
  every destructive action and liveness decision. Adoption should fail closed
  on missing identity, require a start time inside the bounded relaunch window,
  and validate the expected command-line policy. Finish adoption only after
  the `!identity` socket response agrees with the OS identity.

- [`tools/bg3se_harness/launch.py:880`] Headless launches do not enforce the new
  windowed-mode preflight.

  `check_windowed_mode()` correctly interprets
  `FakeFullscreenEnabled=0`, but it is called only by doctor as a warning
  (`tools/bg3se_harness/doctor.py:287-301`). `launch(headless=True)` proceeds
  directly to `prepare_headless_graphics()` at lines 880-886. This contradicts
  the documented hard gate at `docs/CHANGELOG.md:69-73` and
  `agent_docs/harness.md:20-24`, allowing a borderless/fake-fullscreen launch
  to seize a macOS Space.

  The per-key mutation/restore implementation itself is correct: the transient
  key set contains only `ScreenWidth` and `ScreenHeight`, and the tests prove a
  valid restore preserves `FakeFullscreenEnabled`
  (`tests/harness/test_headless_graphics.py:57-91` and
  `tests/harness/test_launch_lifecycle.py:585-606`).

  **Suggestion:** before any headless settings mutation or process launch,
  require `check_windowed_mode()["windowed"]` and return a distinct
  `window_mode_unverified` failed session. Add a production `launch(headless)`
  test, not only helper-level parser tests.

- [`src/injector/main.c:3605`] The emergency disable path still runs the full
  destructor.

  `BG3SE_DISABLE` returns before the duplicate election and before
  `s_duplicate_image` can be set. At unload, the destructor skips cleanup only
  for duplicate images (`src/injector/main.c:3727-3758`), so a disabled image
  logs, fires/tears down subsystems, removes hooks, shuts down ImGui, and calls
  `shutdown_lua()` despite never initializing them. This violates the stated
  “zero side effects” kill-switch contract and makes the emergency recovery
  path unsafe; two disabled physical images would both run cleanup.

  **Suggestion:** track a separate elected/initialized/disabled lifecycle
  state. The destructor should return unless this image successfully won
  election and began initialization; duplicate and disabled images must be
  inert in both constructor and destructor.

## Should fix before tagging

- [`src/core/logging.c:616`] Callback snapshotting fixes the lock inversion, but
  unregister is not quiescent and its lifetime semantics are undocumented.

  A writer can copy `(callback, userdata)` at lines 621-632, another thread can
  unregister the slot at lines 434-438 and free its userdata, and the writer
  can then invoke the stale snapshot at lines 641-643. The slot can even be
  reused by a new registration before the old snapshot runs. The API comment
  at `src/core/logging.h:203-207` gives callers no warning.

  The current Lua shutdown caller avoids a VM use-after-free because it holds
  the Lua gate, unregisters, and nulls `g_log_callback_L`; a snapshotted callback
  later blocks on that gate and observes NULL. That makes the current callsite
  safe, not the callback API generally safe.

  **Suggestion:** either implement synchronous unregister with per-entry
  in-flight accounting, or explicitly specify asynchronous unregister and
  require callback/userdata lifetime to extend until a drain barrier. Add a
  concurrent unregister-during-invoke test, including slot reuse.

- [`tools/bg3se_harness/launch.py:1259`] Steam relaunch timeout never updates
  the tracker to the advertised terminal phase.

  After a qualifying bounce with no successor, the loop reaches generic
  `stage="timeout"` and leaves the tracker in
  `waiting_for_steam_relaunch`. The test named
  `test_no_candidate_stays_waiting` does not run this production path; it
  manually calls `set_failed("steam_relaunch_timeout")` at
  `tests/harness/test_launch_lifecycle.py:139-143`.

  **Suggestion:** on timeout, set `steam_relaunch_timeout` when that is the
  active phase (and an appropriate adopted-process terminal code when needed),
  then assert it by driving `wait_for_socket()` to its deadline.

- [`tests/harness/test_launch_lifecycle.py:51`] The “50 new offline tests” are
  not an honest behavioral gate for several headline claims.

  `test_direct_pid_alive_no_adoption` only asserts fixture fields (lines
  51-55); the app-id test loops over unused strings (lines 86-90); the
  “ambiguous relaunch” section tests only the adoption cap (lines 124-133);
  and the doctor blocking tests reproduce a list comprehension rather than
  call doctor/launch (`tests/harness/test_launch_lifecycle.py:501-528`).
  The monitor tests similarly simulate local booleans rather than exercise
  `main()` or `wait_for_socket()` (`tests/harness/test_monitor.py:110-165`).
  Finally, the offset test's line-1 claim of every main-binary Dobby patch is
  false because its manually curated list omits the functor family.

  **Suggestion:** replace state-fixture assertions and copied production logic
  with boundary tests that invoke the real state machine, launch preflight,
  monitor adapter, and doctor result calculation. Make patch-site discovery
  automatic or add a meta-test that fails when a new hardcoded Dobby target is
  absent from the audit.

- [`src/overlay/overlay.m:656`] `clearOutput` is still not linearized with
  appends.

  Dropping `pendingLines` under `@synchronized` fixes pre-clear replay, but
  `appendOutput:` and `clearOutput` both release that lock before enqueuing
  their main-queue blocks (lines 661-673 and 710-719). A post-clear append can
  join the existing scheduled batch, have the older flush run before the
  queued clear, and then be erased. The reverse enqueue race can also let an
  older flush run after clear.

  **Suggestion:** use a generation/barrier token shared by queued flushes and
  clear, or enqueue a single main-queue state-machine operation while
  preserving the synchronized ordering.

- [`tools/bg3se_harness/launch.py:865`] The documented memory-pressure override
  cannot be requested through the CLI.

  The refusal message tells users to pass `--allow-memory-pressure`, and
  `_launch_until_socket()` accepts the parameter, but neither launch/test
  parser defines the flag (`tools/bg3se_harness/cli.py:686-710`) and neither
  caller passes it (`tools/bg3se_harness/cli.py:394-404` and 482-491).

  **Suggestion:** add the explicit flag to the intended commands and propagate
  it through foreground, background, and test launches, with a CLI-level test.

- [`scripts/session_driver.sh:222`] The added 15-second settling phase does not
  actually require stability.

  It breaks on the first successful `kill -0` at lines 225-230, normally the
  first iteration, so it cannot observe a transient direct PID disappearing
  later in the advertised window. Foreground harness launch should already
  return an adopted PID when its state machine works, but the shell logic and
  release claim are still misleading and provide no independent protection.

  **Suggestion:** either remove the redundant settling claim and rely on a
  verified harness launch record, or require the same PID/identity to survive
  the whole settle interval while still allowing a bounded, uniquely
  identified replacement.

## Nit

- [`src/injector/main.c:3583`] `bg3se_get_identity_json()` interpolates the
  dylib path without JSON escaping. A quote, backslash, or control character
  in the path makes the handshake malformed, and `snprintf` truncation can do
  the same despite the 1024-byte caller buffer.

  **Suggestion:** emit with a small JSON string escaper and return an explicit
  error if the destination is too short.

- [`src/injector/main.c:3612`] The duplicate-image comment says the PID marker
  self-invalidates “across exec,” but `exec` preserves both PID and environment.
  A same-PID exec/reload can therefore suppress the only image; `setenv`
  failures are also ignored.

  **Suggestion:** correct the contract and check election writes. If reload
  within one process is supported, use an election mechanism whose lifetime is
  the loaded image set rather than an inherited environment marker.

## Validation

- `cmake --build build`: build and universal dylib link succeeded. The
  post-build copy into the installed game bundle was blocked by the review
  sandbox (`Operation not permitted`), although the CMake target still exited
  zero.
- `./build/bin/bg3se_test_tier0`: **41/41 passed**.
- `PYTHONPATH=tools python3 -m pytest tests/harness -q`: **130 passed** in
  2.80s.
- `bash -n scripts/session_driver.sh`: passed.
- `git diff --check`: passed.

These results establish compilation, current unit assertions, shell syntax,
and patch whitespace only. They do not exercise a live game, Steam bounce,
detached monitor handoff, `!identity`, concurrent Lua teardown, or concurrent
log callback unregister.
