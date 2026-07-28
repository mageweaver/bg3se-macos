# Fix Headless Splash Automation for BG3SE Harness

This ExecPlan is a living document. Sections Progress, Surprises &
Discoveries, Decision Log, and Outcomes & Retrospective must be
kept up to date as work proceeds.

## Purpose / Big Picture

The harness has `--headless` flags on `launch` and `test`, but the current implementation hides the Baldur's Gate 3 window before the game has accepted the "Press Any Key" splash-screen input. On macOS, BG3 is a Cocoa application, and its keyboard handling only processes the synthetic Space key when the app is visible and frontmost. The user-visible value of this plan is that `PYTHONPATH=tools python3 -m bg3se_harness launch --headless` and `PYTHONPATH=tools python3 -m bg3se_harness test --headless` should reliably reach the Script Extender socket, then hide the game window before the useful automated work begins.

After this plan is implemented, a novice can see it working by running the harness with `--headless`: the BG3 window may be visible while the game boots and the splash screen is dismissed, the socket health check succeeds, and only then the window is hidden. The observable proof is JSON containing `"socket_connected": true` plus stderr showing splash-dismiss attempts before the final "BG3 window hidden" message.

## Progress

- [x] (2026-05-03 14:21Z) Read `tools/bg3se_harness/cli.py`, `tools/bg3se_harness/launch.py`, `tools/bg3se_harness/menu.py`, and `tools/bg3se_harness/config.py` to confirm the current flow.
- [x] (2026-05-03 14:21Z) Confirmed the repository currently has uncommitted changes in `tools/bg3se_harness/cli.py` and `tools/bg3se_harness/launch.py` that match the broken early-hide behavior described in the task.
- [x] (2026-05-03 14:21Z) Evaluated Options A through E from the task prompt against the actual harness flow and macOS Cocoa focus requirements.
- [x] (2026-05-03 14:21Z) Chose the reliable macOS strategy: keep BG3 visible until the socket responds, then hide it for the test run.
- [x] (2026-05-03) Implement code changes in `tools/bg3se_harness/cli.py` — removed early-hide blocks from both cmd_launch and cmd_test, moved hiding to after socket success, pass process=proc for crash detection.
- [x] (2026-05-03) Implement diagnostic and return-value improvements in `tools/bg3se_harness/launch.py` — added process= param with early-exit on crash, structured dict returns from hide_window/show_window.
- [x] (2026-05-03) Run offline validation commands — 75/75 passed, --headless appears in help for both launch and test.
- [ ] Update `docs/harness.md` so the documented `--headless` behavior matches the implementation.
- [ ] Run one live validation command against BG3 on macOS.
- [ ] Record final outcomes and any surprises in this document.

## Surprises & Discoveries

- Observation: The argparse help text already describes the desired behavior, but the command body does the opposite.
  Evidence: `tools/bg3se_harness/cli.py:424` and `tools/bg3se_harness/cli.py:433` say "Hide BG3 window after socket connects", while `tools/bg3se_harness/cli.py:191-196` and `tools/bg3se_harness/cli.py:254-259` sleep for five seconds and hide the window before socket polling starts.

- Observation: The existing socket wait loop is already the correct place to dismiss the splash screen.
  Evidence: `tools/bg3se_harness/launch.py:167-241` polls `/tmp/bg3se.sock`, waits five seconds before the first splash dismiss attempt, retries every three seconds, and stops dismissing after a socket connection has ever succeeded.

- Observation: The current splash-dismiss function cannot work against a hidden BG3 process.
  Evidence: `tools/bg3se_harness/menu.py:123-148` first runs AppleScript to set `frontmost` for process `Baldur's Gate 3`, waits briefly, then sends a CGEvent Space key. A hidden process cannot become frontmost, so the function returns `{"success": False, "error": "could not focus BG3 window"}` before sending the key.

## Decision Log

- Decision: Implement Option C, "Hide only after socket connects", as the default and only behavior for `--headless`.
  Rationale: This works with the known macOS constraints instead of fighting them. BG3 must be visible and frontmost for the Cocoa view to process CGEvent keyboard input. Hiding after the socket responds means the splash is gone, Script Extender is accepting commands, and the subsequent automated test phase no longer needs splash interaction.
  Date/Author: 2026-05-03 / Planner Agent

- Decision: Treat Option A, "Reveal-Dismiss-Hide cycle", as equivalent to Option C for this codebase and do not create a separate mode.
  Rationale: The existing `wait_for_socket(..., dismiss_splash=True)` loop already performs the dismiss cycle while the window is visible. In practice, the reliable sequence is launch visible, dismiss as needed, wait for a responding socket, then hide. Naming this as Option C keeps the implementation simpler because there is no extra headless-specific wrapper around the dismiss loop.
  Date/Author: 2026-05-03 / Planner Agent

- Decision: Do not implement Option B, "Briefly reveal for each dismiss attempt", in the first fix.
  Rationale: It adds timing complexity and user-visible flicker while still needing the same visible-and-frontmost focus step for every retry. It also risks hiding too quickly before BG3 consumes the Space key. It can be added later behind a separate mode if visible boot time becomes unacceptable.
  Date/Author: 2026-05-03 / Planner Agent

- Decision: Do not pursue process-targeted NSEvent or AXUIElement injection for this fix.
  Rationale: The task states that CGEvent and System Events keystrokes need BG3 frontmost, and Cocoa event delivery to a hidden game window is the core failure. Building a new event injection mechanism is higher risk than the one-line sequencing fix and would require deeper permissions testing.
  Date/Author: 2026-05-03 / Planner Agent

- Decision: Do not pursue Option E, "Skip the splash entirely", for this fix.
  Rationale: `tools/bg3se_harness/launch.py:34-55` already sets NoLauncher, SkipVideo, and SkipSplashScreen best-effort settings, but the task states the "Press Any Key" screen still appears and `-continueGame` does not bypass it. There is no known reliable flag in the current harness, so the plan should use the known working Space-key path.
  Date/Author: 2026-05-03 / Planner Agent

- Decision: Keep `wait_for_socket(..., dismiss_splash=True)` as the integration point for pressing Space.
  Rationale: The loop already knows the correct stopping condition: keep trying until the socket responds or the timeout expires. Replacing it would duplicate timeout and socket-readiness logic.
  Date/Author: 2026-05-03 / Planner Agent

## Outcomes & Retrospective

(Pending completion)

## Context and Orientation

This repository contains a Python command-line harness under `tools/bg3se_harness/`. The harness builds and deploys the macOS BG3 Script Extender dylib, patches the BG3 executable, launches the game, waits for the Script Extender Unix domain socket at `/tmp/bg3se.sock`, and runs tests through that socket.

A Unix domain socket is a local inter-process communication file. Here it is the file path `/tmp/bg3se.sock`. The important detail is that the socket file can exist before it is useful: the plan must treat success as "connects and responds to `Ext.GetVersion()`", not merely "the file exists".

A CGEvent is a macOS synthetic input event posted to the system input stream. In this harness, `tools/bg3se_harness/menu.py:94-120` creates a Space key press with ApplicationServices. Because BG3 uses native Cocoa/AppKit input handling on macOS, the event only dismisses the splash when the BG3 window is visible and frontmost.

The current relevant files are:

`tools/bg3se_harness/cli.py`: The command handlers live here. `cmd_launch()` starts at `tools/bg3se_harness/cli.py:151`; `cmd_test()` starts at `tools/bg3se_harness/cli.py:214`. Both currently launch BG3, then hide the window during boot if `--headless` is set. The broken early-hide blocks are at `tools/bg3se_harness/cli.py:191-196` and `tools/bg3se_harness/cli.py:254-259`.

`tools/bg3se_harness/launch.py`: The launch and socket waiting logic lives here. `launch()` starts BG3 with `subprocess.Popen` at `tools/bg3se_harness/launch.py:123-154`. `wait_for_socket()` starts at `tools/bg3se_harness/launch.py:167`; it polls the socket, sends `Ext.GetVersion()`, and optionally calls `_try_dismiss_splash()` while waiting. `hide_window()` is at `tools/bg3se_harness/launch.py:244-251`, and `show_window()` is at `tools/bg3se_harness/launch.py:254-260`.

`tools/bg3se_harness/menu.py`: The splash-dismiss logic lives here. `dismiss_splash_aggressive()` is at `tools/bg3se_harness/menu.py:123-148`. It makes BG3 frontmost, waits 0.3 seconds, then sends the Space key. This is the working path and should remain the path used by `wait_for_socket()`.

`tools/bg3se_harness/config.py`: `SOCKET_PATH` is `/tmp/bg3se.sock`, `HEALTH_TIMEOUT` is 30 seconds, and `HEALTH_TIMEOUT_CONTINUE` is 90 seconds. These values are at `tools/bg3se_harness/config.py:8-11`.

## Plan of Work

First, change `cmd_launch()` in `tools/bg3se_harness/cli.py` so `--headless` no longer sleeps and hides before the socket wait. The command should launch BG3, immediately start `wait_for_socket(timeout=..., dismiss_splash=True)`, then hide the window only if the health result has `"socket_connected": true`. This makes the existing Space-key dismiss loop operate while BG3 is visible and focusable.

Second, make the same sequencing change in `cmd_test()` in `tools/bg3se_harness/cli.py`. The command should launch BG3, wait for the socket with splash dismissal enabled, fail early if the socket never responds, and hide the window only after a successful health check and before `run_tests(...)`. This means the automated test run is headless even though the boot phase is not hidden.

Third, improve `hide_window()` and `show_window()` in `tools/bg3se_harness/launch.py` so they return structured results instead of always printing success. A structured result means a dictionary such as `{"success": True, "method": "system_events_visible_false"}` or `{"success": False, "error": "...", "returncode": 1}`. This is useful because macOS Accessibility permissions or process-name mismatches can make AppleScript fail. The command handlers can include the hide result in their JSON output under a key such as `"headless": {"requested": True, "hidden": True}`.

Fourth, add an optional `process=None` parameter to `wait_for_socket()` in `tools/bg3se_harness/launch.py`. A process object is the `subprocess.Popen` object returned by `launch()`. At the top of each polling loop, if `process` is not `None` and `process.poll()` is not `None`, return a failure result containing `"socket_connected": false`, `"stage": "process_exited"`, `"exitcode": process.returncode`, and `"elapsed_ms": ...`. This catches the "BG3 crashed during boot" edge case quickly instead of waiting for the full timeout.

Fifth, update the two callers in `tools/bg3se_harness/cli.py` to pass `process=proc` into `wait_for_socket()`. This preserves existing behavior for any other caller because the new parameter has a default value.

Sixth, update `docs/harness.md` so the command table and troubleshooting text explain the exact meaning of headless mode: the game window stays visible during boot and splash dismissal, then hides after the Script Extender socket responds. This avoids promising "never steals focus", which is not compatible with the current macOS event path.

## Concrete Steps

Run all commands from `/Users/tomdimino/Desktop/Programming/game-modding/bg3/bg3se-macos`.

Step 1: Inspect the current command bodies before editing.

```bash
nl -ba tools/bg3se_harness/cli.py | sed -n '151,284p'
nl -ba tools/bg3se_harness/launch.py | sed -n '167,274p'
nl -ba tools/bg3se_harness/menu.py | sed -n '123,148p'
```

Expected output: The first command shows `cmd_launch()` and `cmd_test()`. The second command shows `wait_for_socket()`, `hide_window()`, `show_window()`, and `_try_dismiss_splash()`. The third command shows `dismiss_splash_aggressive()` focusing BG3 before sending Space.

Step 2: Edit `tools/bg3se_harness/cli.py`.

Remove the early-hide block in `cmd_launch()` currently at `tools/bg3se_harness/cli.py:191-196`. Keep `headless = getattr(args, "headless", False)`, but place it before or near the health check without sleeping or hiding. Change the socket wait call currently at `tools/bg3se_harness/cli.py:203` to pass the process object:

```python
health = launch_mod.wait_for_socket(timeout=timeout, dismiss_splash=True, process=proc)
```

After the metadata assignments to `health`, add this behavior before printing JSON: if `headless` is true and `health.get("socket_connected")` is true, call `launch_mod.hide_window()` and store its structured result in `health["headless"]`. If `headless` is true and the socket did not connect, do not hide; store `{"requested": True, "hidden": False, "reason": "socket_not_connected"}` so the failure output is explicit.

Remove the early-hide block in `cmd_test()` currently at `tools/bg3se_harness/cli.py:254-259`. Change the socket wait call currently at `tools/bg3se_harness/cli.py:264` to pass `process=proc`. After the `if not health.get("socket_connected"):` failure block and before `print("Running tests...", file=sys.stderr)`, add the same hide-after-success behavior. Include the hide result inside `output["launch"]["headless"]` when constructing the final test JSON at `tools/bg3se_harness/cli.py:278-282`.

Step 3: Edit `tools/bg3se_harness/launch.py`.

Change the function signature at `tools/bg3se_harness/launch.py:167` from:

```python
def wait_for_socket(timeout=HEALTH_TIMEOUT, dismiss_splash=False):
```

to:

```python
def wait_for_socket(timeout=HEALTH_TIMEOUT, dismiss_splash=False, process=None):
```

Immediately inside the `while` loop, before attempting splash dismissal, add a process-exit check. It should return a dictionary with `socket_connected`, `stage`, `exitcode`, and `elapsed_ms`. This makes a crash or early quit observable.

Change `hide_window()` at `tools/bg3se_harness/launch.py:244-251` so it captures the AppleScript result with `text=True`, checks `returncode`, prints the success message only when the command succeeds, and returns a dictionary. If `subprocess.TimeoutExpired` or `OSError` occurs, catch it and return `{"success": False, "error": str(exc)}`.

Change `show_window()` at `tools/bg3se_harness/launch.py:254-260` the same way. It should return a structured result and should not print a success message unless a caller needs it. `show_window()` is useful for recovery and future manual commands even if this plan does not require calling it during normal headless flow.

Step 4: Update `docs/harness.md`.

At `docs/harness.md:37-38`, update the command table entries for `launch` and `test` to include `--headless`. Add one short paragraph near the troubleshooting section explaining that headless mode on macOS hides BG3 after the socket responds, not during the splash screen, because the splash dismiss input requires a visible frontmost Cocoa window.

Step 5: Run offline validation.

```bash
PYTHONPATH=tools python3 -m bg3se_harness --help
PYTHONPATH=tools python3 -m bg3se_harness launch --help
PYTHONPATH=tools python3 -m bg3se_harness test --help
PYTHONPATH=tools python3 -m bg3se_harness.tests
```

Expected output: All help commands exit 0. The launch and test help include `--headless`. The offline test suite exits 0, or if existing unrelated failures appear, record them in Surprises & Discoveries with the exact failing test names.

Step 6: Run live launch validation.

```bash
PYTHONPATH=tools python3 -m bg3se_harness quit --force
PYTHONPATH=tools python3 -m bg3se_harness launch --headless --timeout 90
```

Expected stderr sequence: "Launching BG3...", then "Waiting for SE socket...", then one or more "Splash dismiss #N (...)" lines, then "BG3 window hidden (headless mode)" after the socket succeeds. Expected JSON: `"socket_connected": true`, a non-empty `"se_version"` or `"connected"`, `"pid": <number>`, and `"headless"` showing the hide request succeeded.

Step 7: Run live test validation.

```bash
PYTHONPATH=tools python3 -m bg3se_harness quit --force
PYTHONPATH=tools python3 -m bg3se_harness test --headless --tier 1
```

Expected behavior: BG3 is visible while it boots and receives Space on the splash screen. Once the socket responds, the window hides before the test runner starts. Expected JSON: a `"launch"` object with `"socket_elapsed_ms"` and `"headless"`, plus the usual test summary. The command exits 0 if all selected tests pass, or exits 1 with test failure details if the harness tests themselves fail after the socket is working.

## Validation and Acceptance

Acceptance criterion 1: `launch --headless` no longer hides the BG3 process before `wait_for_socket()` starts. A code review can verify this by checking that there is no `time.sleep(5)` plus `hide_window()` block between `launch_mod.launch(...)` and `launch_mod.wait_for_socket(...)` in either command handler.

Acceptance criterion 2: `wait_for_socket(..., dismiss_splash=True, process=proc)` still calls `_try_dismiss_splash()` while waiting, and `_try_dismiss_splash()` still uses `menu.dismiss_splash_aggressive()`. This preserves the currently working visible-window Space-key path.

Acceptance criterion 3: When `--headless` is used and the socket connects, hiding occurs after the socket health check and before tests run. For `cmd_test()`, the hide call must happen after the successful `if not health.get("socket_connected"):` guard and before `run_tests(...)`.

Acceptance criterion 4: When BG3 exits before the socket responds, the command returns JSON with `"socket_connected": false`, `"stage": "process_exited"`, and an `"exitcode"` instead of waiting until the full timeout.

Acceptance criterion 5: When BG3 reaches a recovery state or a state without a splash screen, the command still succeeds if the socket responds. Repeated Space keys are acceptable before the socket connects because `dismiss_splash_aggressive()` sends only Space and avoids mouse clicks that could activate menu buttons.

Acceptance criterion 6: If AppleScript hiding fails, the harness still prints the socket success result and exits according to socket/test success, while recording the headless hide failure in JSON. A hide failure should not turn a successful socket connection into a launch failure because the primary purpose of `launch` is to start and health-check BG3.

## Idempotence and Recovery

The code changes are idempotent because they only change control flow and diagnostics in the Python harness. Running `launch --headless` multiple times is already designed to be repeatable because `tools/bg3se_harness/launch.py:123-125` calls `kill_existing()` and `clean_socket()` before launching a new BG3 process.

If implementation goes wrong before live testing, recover by reverting only the planned files: `tools/bg3se_harness/cli.py`, `tools/bg3se_harness/launch.py`, and `docs/harness.md`. Do not reset the whole repository because there are existing unrelated uncommitted changes in this worktree.

If BG3 is left running or hidden after a failed run, recover with:

```bash
PYTHONPATH=tools python3 -m bg3se_harness quit --force
```

If the game is still running but hidden and the developer wants to inspect it manually, run a one-off Python command after `show_window()` has been updated:

```bash
PYTHONPATH=tools python3 -c 'from bg3se_harness.launch import show_window; print(show_window())'
```

If macOS Automation or Accessibility permissions block System Events, open System Settings and allow the terminal application running the harness to control System Events and post input events. The harness should surface this as a structured hide or focus failure rather than silently claiming success.

## Interfaces and Dependencies

`tools/bg3se_harness/launch.py` must provide:

```python
def wait_for_socket(timeout=HEALTH_TIMEOUT, dismiss_splash=False, process=None):
    """Wait for the SE socket to respond to Lua commands."""
```

The return value must remain a dictionary. On success it must keep the existing keys `"socket_connected"`, `"se_version"`, and `"elapsed_ms"`. On timeout it must keep returning `"socket_connected": False` and `"elapsed_ms"`. On early process exit it must return `"socket_connected": False`, `"stage": "process_exited"`, `"exitcode"`, and `"elapsed_ms"`.

`tools/bg3se_harness/launch.py` must provide:

```python
def hide_window():
    """Hide BG3 window via System Events."""

def show_window():
    """Show BG3 window via System Events."""
```

Both functions must return dictionaries with at least `"success": bool`. They may include `"method"`, `"returncode"`, `"stdout"`, `"stderr"`, or `"error"` for diagnostics.

`tools/bg3se_harness/cli.py` must keep accepting `--headless` on both `launch` and `test`. The meaning of `--headless` after this plan is precise: keep BG3 visible during boot and splash dismissal, then hide the window after the Script Extender socket responds.

No new third-party Python dependency is required. The plan uses the existing standard-library `subprocess`, `socket`, and `time` modules plus the existing macOS tools `osascript` and ApplicationServices CGEvent calls already present in `tools/bg3se_harness/menu.py`.
