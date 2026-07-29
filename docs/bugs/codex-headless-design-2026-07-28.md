# True-headless BG3SE harness design

Date: 2026-07-28
Scope: static analysis only; BG3 was not launched.

## Infrastructure Assessment

### Decision

The harness should stop treating macOS input delivery and the SE socket as
proof of game readiness. The reliable architecture is:

1. launch one identified BG3 process into a renderable but off-screen window;
2. let the injected dylib dismiss the splash and, only when necessary, inject
   input through BG3's in-process input path;
3. use BG3's own fresh `network.*.log` to prove the client traversed
   `LoadSession -> LoadLevel -> ... -> PrepareRunning`;
4. verify that the responding Unix-socket peer and the still-running process
   are the PID for this launch attempt;
5. only then fully hide the application and run tests;
6. preserve `modsettings.lsx` byte-for-byte with a crash-safe guard for the
   entire lifetime of the owned process.

This removes OS focus, window visibility, Accessibility, and CGEvent delivery
from the normal headless path. OCR remains a diagnostic and modal-detection
fallback, not the actuator.

### Current flow

The foreground `test` path builds, patches, launches, waits for an
`Ext.GetVersion()` response, hides at that point, and immediately sends
`!test` or `!test_ingame` (`tools/bg3se_harness/cli.py:413-512`). The response
only proves that the injected console can execute a command. It does not prove
that a save has loaded: `wait_for_socket()` returns success on any non-empty
response (`tools/bg3se_harness/launch.py:637-679`), and Tier 1 tests can execute
at the menu (`tools/bg3se_harness/test_runner.py:50-78`).

The captured engine logs show the missing distinction:

- menu-only logs stop at `CLIENT STATE SWAP ... to: Menu`;
- loaded-session logs continue through `LoadSession`, `LoadLevel`,
  `SwapLevel`, `StopLoading`, and `PrepareRunning`;
- `PrepareRunning` is therefore the strongest observed SE-independent success
  marker available in the repository.

Examples are in
`docs/bugs/evidence-2026-07-28/network.2026-07-28T18-59-35-946235.log`
(menu only) and
`docs/bugs/evidence-2026-07-28/network.2026-07-28T18-56-22-257991.log`
(loaded session).

### Root-cause table

| Break point | Present symptom | Responsible code path | Root cause |
|---|---|---|---|
| Splash | Vanilla `-continueGame` parks at “Press Any Key”; external keys are timing-dependent. | Python sends System Events keys in `menu.py:221-280`; injected SE runs `focusless_input_start_splash_autodismiss()` in `focusless_input.m:205-267`. | The splash consumes the raw/LSMTLView path, not Noesis menu input. SE's in-process path is the correct solution, but its public post functions return “queued” even if the main-queue delivery later fails (`focusless_input.m:177-202`), so the harness has no positive acknowledgement. |
| Main menu | Return/System Events do not activate Noesis Continue; hidden/off-screen CGEvents miss. | `wait_for_socket()` OCRs the menu and calls `_try_click_continue()` (`launch.py:615-635`); the stall watchdog falls back to `click_fraction(... activate=True)` (`launch.py:873-928`); `click_fraction()` activates BG3 and emits WindowServer events (`menu.py:881-927`). | The actuator is outside the process. Noesis accepts input only through BG3's view/InputManager path and focus gate; WindowServer drops or withholds events for hidden/off-screen, non-active windows. |
| Menus/dialogs | The blind watchdog can hit the wrong control, loop a modal, or click Start Game without knowing what is displayed. | `_WATCHDOG_CLICK_SEQUENCE` hard-codes Continue, six checkbox positions, and two Start Game positions (`launch.py:460-493`). OCR only recognizes a small label set (`menu.py:702-717`). | Coordinates are treated as state. There is no semantic modal state, action acknowledgement, row count, or validation that a Mod Verification list matches the expected installed/save-required mods. |
| Focus | Noesis may remain unfocused despite background keystrokes; focus behavior can silently break after a BG3 update. | `focus_hack.c:8-15` uses hard-coded singleton VA `0x108ac0278` and byte offset `+0x142`; it polls for 15 seconds and writes the byte once (`focus_hack.c:80-116`). | OS focus and BG3's internal focus flag are separate. The internal bypass is appropriate for automation, but address/version validation is weak, the one-shot write can be overwritten by a later focus change, and it cannot make WindowServer CGEvents reliable. |
| Hidden renderer | Foreground `test --headless` is visible during boot and hides as soon as the socket answers; minimizing/hiding too early can prevent a Metal drawable. | `launch()` explicitly says “visible during boot” (`launch.py:440-442`); `cmd_test()` calls `hide_window()` on socket readiness (`cli.py:482-493`). Only the detached monitor moves off-screen, and waits five seconds first (`_monitor.py:121-129`). | “Launch hidden” and “renderable during boot” are currently conflated. App hiding/order-out can stall Metal, while off-screen placement preserves rendering but the synchronous test path does not use it. |
| `modsettings.lsx` rewrite | Confirming Mod Verification can silently remove SE-dependent mods, changing future launches. | `launch.py` snapshots/restores selected `graphicSettings.lsx` entries (`launch.py:323-383`) but has no launch-scoped guard for `MODSETTINGS_PATH`. `mod_manager/modsettings.py` only provides user-invoked timestamped backups/restores. | Preflight detects some bad inputs but does not protect the file from BG3. The dialog rewrite is a whole-file destructive side effect and must be transactionally guarded even when preflight passes. |
| False “session loaded” | Tests can run and pass at the menu, and the window can be hidden before a save is running. | Socket response is the sole boot-success predicate (`cli.py:153-154`, `cli.py:470-512`). The SE-side `SessionLoaded` tracker is inferred from hooked Osiris lifecycle and is unavailable or misleading in reduced-hook probes. | Transport readiness is being used as application readiness. Only BG3's own state-machine log provides an independent session-load boundary in all test arms. |
| Crash/relaunch masking | A crash followed by a watchdog/Steam/harness replacement can look like a surviving game; a new socket can receive tests. | PID file validation checks only that a PID's command contains “Baldur” (`launch.py:32-64`); general liveness uses `pgrep` (`launch.py:981-985`); `run_tests()` connects to the global socket with no peer-PID check. Headless boot also defaults to one retry (`cli.py:85-89`). | Process existence, launch-attempt identity, and socket ownership are not joined. A replacement PID or later socket must never satisfy the original attempt. A post-session crash must be final for that attempt, not converted into a menu-only retry that can report test results. |
| Known Hotbar crash | The original session can die about 35 seconds after load even though early tests/socket checks succeeded. | The fault is outside this automation design (`gui::HotbarSystem::Update`); the harness currently has no process check joined to the test completion result. | The SE bug is being investigated separately, but automation must surface it honestly: pin the PID through tests and fail if it exits. Do not use an arbitrary “some BG3 exists” check. |

`tools/bg3se_harness/patch.py` is not the menu failure: it makes dylib loading
idempotent and verifies the Mach-O load command. Its relevance is that the
normal headless path may depend on SE-provided splash and Continue behavior;
the harness must emit a distinct `dylib_not_loaded` result when that dependency
is absent.

## Proposed Changes

### Component: launch-attempt state machine

- **Purpose:** make every success claim evidence-based and bound to one process.
- **Technology:** a Python `LaunchAttempt` record plus an incremental parser for
  BG3 network logs.
- **Dependencies:** `subprocess.Popen`, macOS process identity/peer PID APIs,
  and the existing BG3 log directory.

Use explicit phases:

```text
created
  -> process_launched
  -> renderer_ready
  -> splash_dismissed_or_bypassed
  -> socket_ready
  -> session_loading
  -> session_loaded
  -> tests_running
  -> tests_complete
  -> process_verified
  -> settings_restored
```

For a `--continue` or `--save` run, `session_loaded` must require all of the
following from a network log created or modified by this attempt:

1. client entered `LoadSession`;
2. client entered `LoadLevel`;
3. client reached `PrepareRunning`;
4. the owned PID is still the same process;
5. the SE socket peer belongs to that PID and answers after the transition.

The parser should snapshot existing `network.*.log` paths, inode/file IDs,
sizes, and mtimes before `Popen`; then tail only a new file or new bytes whose
mtime is after the attempt start. It should retain the matched lines and log
path in the JSON report. It must not read an old “latest successful” log.

`Menu` is a useful diagnostic state, never success for `--continue`.
`PrepareRunning` is the observed loaded-session boundary. If a future BG3 build
logs an explicit `Running` transition, accept that as a stronger terminal
marker but keep the tested `PrepareRunning` rule versioned.

### Component: native in-process automation control plane

- **Purpose:** deliver input without app focus, cursor movement, Accessibility,
  or WindowServer routing.
- **Technology:** extend `focusless_input` and expose native console commands.
- **Dependencies:** LSMTLView/InputManager availability and main-queue dispatch.

Recommended first interface:

```text
!input key 0x24 0
!input move 0.443 0.889
!input click 0.443 0.889
!input status <action-id>
```

Coordinates are normalized to the LSMTLView content bounds with a documented
top-left origin. Each command should return an action ID immediately; delivery
on the main queue records `queued`, `view_found`, `input_manager_found`,
`delivered`, and an error. The harness polls `!input status` and never treats
“queued” as “clicked.”

Make these native built-ins in `src/console/console.c`, before Lua
`DoConsoleCommand` interception, so a mod cannot suppress the automation
control path. A small `Ext.Automation.Input.Key/Move/Click` Lua API can wrap
the same C functions later, but Lua should not be the only path because the
automation is needed before a loaded session and while mod initialization may
be unhealthy.

#### Delivery implementation order

1. **Use the existing direct LSMTLView path first.** It already calls
   `[LSMTLView keyDown:]`, `mouseMoved:`, `mouseDown:`, and `mouseUp:` on the
   main queue (`focusless_input.m:73-202`). Fix recursive view discovery,
   create a real `NSEventTypeMouseMoved` event instead of passing the
   mouse-down event to `mouseMoved:`, convert view coordinates to window
   coordinates correctly, and preserve move/down/up ordering.
2. **If Noesis still rejects these events, call BG3's InputManager/BaseApp
   event injection leaf.** Reverse the calls made by LSMTLView's mouse/key
   methods and enqueue the same internal input records. This is more general
   and less UI-version-specific than invoking a particular Noesis controller.
3. **Direct Noesis-view or `DCMainMenu` command invocation is the last
   fallback.** It could efficiently invoke Continue, but it couples the
   harness to private view-model/controller ABIs and does not generalize to
   Mod Verification, safe-mode prompts, or future dialogs.

The focus byte remains an enabling compatibility shim, not the transport.
Resolve `BaseApp::s_AppInstance` by symbol/pattern or a build-ID-keyed offset
table, validate the containing image and writable address, report whether the
write occurred, and reassert only while automation is active if BG3 clears it.

### Component: window-management policy

- **Purpose:** remain invisible/backgrounded without starving Metal.
- **Technology:** two-stage off-screen-then-hidden policy.
- **Dependencies:** early NSWindow/LSMTLView discovery and first-drawable signal.

Do not choose either current extreme:

- `visible=false`/minimize at process start can prevent drawable creation;
- visible until socket-ready leaks a window and still hides before a session is
  proven.

Instead:

1. force normal 1280x720 windowed graphics before launch as today;
2. as soon as the BG3 content window is created, but before it is ordered
   front if possible, place it at `{-10000, -10000}` and order it behind other
   apps; never activate BG3 and never warp the cursor;
3. keep it off-screen but ordered/renderable through splash, menu/dialog
   handling, and the `PrepareRunning` transition;
4. after `session_loaded`, set process visibility false/order the window out;
5. restore the user's graphics settings only after the transition, and on
   every error/cancel path.

For a no-flash guarantee, implement step 2 inside the dylib (an early window
observer or narrowly scoped NSWindow hook enabled by `BG3SE_HEADLESS=1`).
Polling System Events from Python is an acceptable interim patch but cannot
guarantee that the first frame never appears.

### Component: `modsettings.lsx` transaction guard

- **Purpose:** make every launch non-destructive even when BG3 confirms Mod
  Verification.
- **Technology:** byte-for-byte snapshot, SHA-256 verification, atomic restore,
  and a small guardian tied to the exact process identity.
- **Dependencies:** `MODSETTINGS_PATH`, `HARNESS_CONFIG_DIR`, and the launch
  attempt record.

The guard must protect the whole file, not parse and reserialize it:

1. acquire a harness launch lock;
2. recover any stale transaction from an interrupted prior run;
3. copy the original bytes and metadata to a uniquely named snapshot via
   temporary file plus `os.replace`;
4. record original SHA-256, size, mode, mtime, active UUID order, launch ID,
   and PID once known;
5. poll/kqueue the file during boot and tests; any hash change emits
   `modsettings_mutated`;
6. restore atomically as soon as a mutation is observed, again before command
   exit, and again when the owned process exits;
7. verify the restored hash byte-for-byte before deleting the transaction
   manifest.

If the command intentionally leaves BG3 running, a detached guardian must keep
the transaction alive until that exact PID exits. Otherwise the safest test
policy is to quit the owned process after tests and restore before returning.
A retry always restores the baseline before starting the next attempt.
Restoration failure is a command failure even if all tests passed.

The existing timestamped mod-manager backup remains useful for user operations,
but it is not a crash-recovery transaction and has second-level filename
collisions.

### Component: Mod Verification detection and handling

- **Purpose:** progress safe, expected dialogs without blindly accepting data
  loss or loading a save with missing mods.
- **Technology:** layered detection with fail-closed policy.
- **Dependencies:** native input control plane, preflight inventory, protected
  modsettings snapshot, and optionally in-process frame capture/OCR.

Detection order:

1. **Static preflight:** compare active order, installed PAKs, registry, and
   save-required markers. Block known missing dependencies before launch.
2. **Engine state stall:** if the fresh network log reaches `Menu` but does not
   enter `LoadSession` after Continue, classify it as `ui_blocked`, not
   `socket_timeout`.
3. **Semantic UI signal:** preferred long-term implementation is an SE hook or
   Noesis visual-tree query that reports the active top-level dialog and its
   rows/buttons.
4. **OCR fallback:** capture the LSMTLView/Metal drawable in-process so OCR
   still works while the macOS window is off-screen or hidden. Recognize
   `Mod Verification`, `Start Game`, row labels, checkbox state, and safe-mode
   prompts. WindowServer `screencapture` is diagnostic-only because hidden
   window capture is not a reliable contract.
5. **File rewrite signal:** a changed `modsettings.lsx` is definitive evidence
   of mutation and triggers restoration, but it is not sufficient to detect an
   unconfirmed dialog because the rewrite may occur only after Start Game.

Auto-acknowledge Mod Verification only when:

- the snapshot/guardian is active;
- preflight reports no missing installed PAKs;
- every displayed missing/changed row maps to the expected save-required or
  active mod set;
- checkbox and Start Game actions each receive native input acknowledgements.

Otherwise fail with `stage: mod_verification`, preserve diagnostics, restore
settings, and do not click Start Game. Remove the current blind six-checkbox
sequence from headless mode; keep it only behind an explicitly named debug
option if it is still useful for manual investigation.

### Component: honest PID, socket, and result reporting

- **Purpose:** distinguish a surviving owned process from a replacement.
- **Technology:** process identity tuple and Unix peer credentials.
- **Dependencies:** macOS `LOCAL_PEERPID`/libproc or equivalent.

Represent identity as at least:

```json
{
  "launch_id": "uuid",
  "attempt": 1,
  "pid": 12345,
  "process_start": "...",
  "executable": ".../Baldur's Gate 3",
  "network_log": ".../network.<timestamp>.log"
}
```

For the foreground path, `Popen.poll()` is authoritative. For detached
monitoring, capture process start time and executable path and watch the PID
with kqueue/libproc; `kill(pid, 0)` alone is vulnerable to PID reuse. On every
socket connection, query the peer PID and reject a peer that does not match the
attempt. Do not use `pgrep` as a success predicate.

Boot retries remain separate launch attempts. A retry may only occur before
`session_loaded`, after the old exact PID is confirmed gone and settings are
restored. Once a session has loaded, any process exit is
`process_exited_after_session_loaded`; never relaunch it inside the same test
result. If another BG3 PID appears, report it as `replacement_detected` and
fail the owned attempt.

Exit zero for `test --headless --continue` requires:

- same launch identity throughout;
- fresh engine `session_loaded` evidence;
- socket peer PID match;
- test summary present and all tests passed;
- owned process still alive after the final socket response;
- graphics and modsettings restoration verified.

## Concrete Ranked Patch List

Effort: **S** is roughly one focused day, **M** is two to five days, **L** is
more than a week or requires material reverse engineering.

| Rank | File(s) | Change | Effort | Risk | Why this order |
|---:|---|---|:---:|:---:|---|
| 1 | `tools/bg3se_harness/config.py`, `launch.py`, new `settings_guard.py`, harness tests | Add byte-exact, crash-recoverable `modsettings.lsx` snapshot/hash/atomic restore on all launch, retry, signal, and exit paths. | S | Low | Prevents the known destructive side effect before adding any more dialog automation. |
| 2 | new `tools/bg3se_harness/game_log.py`, `launch.py`, `cli.py`, tests | Snapshot/tail fresh `network.*.log`; require `LoadSession -> LoadLevel -> PrepareRunning` for continue/save; report matched evidence. | M | Low | Eliminates false success and gives every later automation step an authoritative state boundary. |
| 3 | `tools/bg3se_harness/console.py`, `test_runner.py`, `launch.py`, `_monitor.py` | Verify Unix socket peer PID; persist launch UUID/start identity; check exact process before and after tests; remove `pgrep` from verdicts. | S/M | Low | Stops watchdog/replacement processes from inheriting an attempt's success. |
| 4 | `src/input/focusless_input.m/.h`, `src/console/console.c`, unit/native tests | Add acknowledged native `!input key/move/click/status`; recursive view lookup; correct mouse event type/coordinate conversion; structured telemetry. | M | Medium | Replaces the OS-level actuator while reusing an already proven in-process path. |
| 5 | `tools/bg3se_harness/launch.py`, `menu.py`, `cli.py` | Replace `_WATCHDOG_CLICK_SEQUENCE` in headless mode with an explicit engine/UI state machine using `!input`; never activate BG3 or warp the cursor. | M | Medium | Makes menu fallback compatible with off-screen rendering and prevents blind modal clicks. |
| 6 | new `src/input/headless_window.m/.h` (or narrow integration in `focusless_input.m`), `src/injector/main.c`, `launch.py` | Under `BG3SE_HEADLESS=1`, place the window off-screen before first order/front, publish first-drawable/window-ready status, fully hide only after Python proves `session_loaded`. | M | Medium | Achieves the “hidden/backgrounded the whole time” requirement without starving Metal. |
| 7 | new `src/input/automation_ui.*` or Metal capture endpoint, `menu.py`, `launch.py` | Detect top-level modal semantically or capture the drawable for OCR; enumerate Mod Verification rows and drive only validated controls. | M/L | Medium/High | Required for autonomous robustness when the happy-path `-continueGame` flow is intercepted by dialogs. |
| 8 | `src/game/focus_hack.c/.h`, version/address tables | Resolve/validate BaseApp singleton per BG3 build, report focus state, and scope/reassert the focus byte only during automation. | S/M | Medium | Hardens the internal focus gate after the main actuator and evidence model are correct. |
| 9 | `tools/bg3se_harness/_monitor.py`, `docs/harness.md`, `agent_docs/harness.md` | Make background launch use the same state machine, guards, identity checks, and terminology as foreground test; remove “socket ready = ready” wording. | S | Low | Prevents the detached path from retaining the old semantics. |
| 10 | `tools/bg3se_harness/tests/` or current harness test suite | Add deterministic fixtures for menu-only log, successful session log, stale log, modal stall, modsettings mutation, PID replacement, peer-PID mismatch, retry, and restore failure. | M | Low | Locks the architecture before live validation and future BG3 updates. |

Ranks 1-6 are the smallest happy-path true-headless implementation. Rank 7 is
part of the minimum release gate for the stated “robust to menus and dialogs”
goal. Direct Noesis controller calls are not required unless rank 4 fails to
move Noesis through the general InputManager path.

## Deployment Plan

1. Land ranks 1-3 with offline fixtures. Change result vocabulary from
   `socket_connected` success to separate `socket_ready` and
   `session_loaded` fields.
2. Land rank 4 behind `BG3SE_AUTOMATION_INPUT=1`; validate action
   acknowledgement without enabling automatic menu decisions.
3. Replace the headless watchdog with rank 5. Keep current CGEvent automation
   available only for explicit visible/manual diagnostics.
4. Add early off-screen window placement and first-drawable telemetry.
5. Add semantic/OCR modal handling with fail-closed validation.
6. Run controlled live acceptance tests only after review; the static design
   task itself did not launch BG3.

Acceptance scenarios:

| Scenario | Required verdict |
|---|---|
| Healthy SE `-continueGame` | No menu click; fresh log reaches `PrepareRunning`; window never foregrounds; tests use the owned socket/PID. |
| Splash needs dismissal | Native action is acknowledged; no System Events input or cursor movement. |
| Continue stalls at menu | Native Continue action advances fresh engine log to `LoadSession`. |
| Mod Verification, expected rows | Rows match preflight/save set; guarded native actions progress; original modsettings hash is restored. |
| Mod Verification, unknown/missing row | Fail closed at `mod_verification`; do not click Start Game. |
| BG3 rewrites modsettings | Mutation is reported and original bytes are atomically restored. |
| Original PID crashes and another appears | Fail `replacement_detected`/`process_exited_after_session_loaded`; never run or credit tests on replacement. |
| Socket exists from wrong PID | Reject peer and fail attempt. |
| Menu-only engine log | Never report `session_loaded`; never run tests. |

Rollback is simple: disable `BG3SE_HEADLESS` and
`BG3SE_AUTOMATION_INPUT`, retain the new settings guard and honest readiness
checks, and use visible manual automation for diagnosis. Do not roll back the
modsettings guard.

## Monitoring & Alerting

Emit one structured event per phase with `launch_id`, attempt, PID, monotonic
elapsed time, and evidence:

- boot duration to first drawable, socket, `LoadSession`, and `PrepareRunning`;
- native input actions queued/delivered/failed by type;
- UI-blocked/modal classifications;
- modsettings original/current/restored hashes;
- process exit stage and replacement PID, if any;
- socket peer PID match;
- test duration/result;
- restore success for both graphics and modsettings.

High-priority failures are `modsettings_restore_failed`,
`socket_peer_mismatch`, `replacement_detected`, and
`process_exited_after_session_loaded`. Repeated `ui_blocked` failures should
retain the engine-log tail and an in-process frame/UI snapshot, not
automatically broaden the click sequence.

## Runbook

### Boot stops before `LoadSession`

1. Confirm the exact PID is alive and socket peer PID matches.
2. Inspect the fresh attempt-owned network log.
3. If it is at `Menu`, query modal/UI state and take an in-process capture.
4. Drive only a recognized Continue or validated Mod Verification flow.
5. If state remains unknown, fail closed and restore settings.

### Process exits or a replacement appears

1. Mark the attempt failed immediately.
2. Do not connect tests to the replacement socket.
3. Restore graphics and modsettings from the attempt transaction.
4. Preserve the network log, native-action ledger, SE log, and crash report.
5. Start a new attempt only if the configured policy permits a pre-session
   boot retry.

### Modsettings changes

1. Record the changed hash and mutation time.
2. Atomically restore the snapshot.
3. Verify the original hash.
4. If verification fails, stop automation and retain the transaction manifest
   for manual recovery.

## Deployment Checklist

- [ ] Infrastructure/code reviewed
- [ ] Offline state-machine and settings-guard fixtures pass
- [ ] Visible staging run proves native input acknowledgements
- [ ] Off-screen staging run proves no activation/cursor movement
- [ ] Modal safe/fail-closed cases pass
- [ ] PID replacement and socket-peer mismatch tests pass
- [ ] Rollback flags and recovery runbook documented

## Monitoring Checklist

- [ ] Phase durations and failure counts emitted
- [ ] Exact process and socket peer identity recorded
- [ ] Fresh engine-log evidence attached to every session verdict
- [ ] Modsettings mutation and restore hashes recorded
- [ ] Crash/replacement never converted into success
