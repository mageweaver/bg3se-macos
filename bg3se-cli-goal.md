# /goal: Make the BG3SE macOS CLI Fully Functional for Autonomous Mod Vetting

You are working in:

```bash
/Users/tomdimino/Desktop/Programming/game-modding/bg3/bg3se-macos
```

Your objective is to make the `tools/bg3se_harness` CLI genuinely functional for its scoped purpose: build, patch, launch BG3 with the Script Extender, autonomously reach a loaded-save socket session, run test/probe/mod-vetting commands, and produce machine-readable evidence that the CLI works. Do not broaden this into full Windows BG3SE API parity unless a CLI feature directly depends on it.

Treat this as an end-to-end engineering task. Read the repo, make needed code changes, run offline tests, run live BG3 validation when required, and leave clear docs/results. The game may currently be closed; do not assume a live BG3 process exists.

## Current Known State

Read these files first:

```bash
docs/bugs/headless-boot-debug-log.md
docs/bugs/headless-boot-bugs.md
docs/harness.md
CLAUDE.md
tools/bg3se_harness/cli.py
tools/bg3se_harness/launch.py
tools/bg3se_harness/menu.py
tools/bg3se_harness/screenshot.py
src/input/focusless_input.m
src/game/global_switches.c
src/game/video_skip.c
src/injector/main.c
```

Relevant recent work:

- `main` is ahead of `origin/main` with commits for mod vetting, offline tests, headless windowed mode, focusless input, observability APIs, and harness fixes.
- The worktree is dirty. Do not reset or revert unrelated user work.
- Offline tests have recently passed:
  - `PYTHONPATH=tools python3 -m pytest tests/harness -q` => 30 passed
  - `build/bin/bg3se_test_tier0` => 41/41 passed
  - `PYTHONPATH=tools python3 -m bg3se_harness.tests_nexus` => 23 passed
  - `PYTHONPATH=tools python3 -m bg3se_harness.tests_wiki` => 23 passed
- `doctor` under real macOS access recently showed app/binary/dylib/patch/config/Accessibility OK, with `se_socket` failing when BG3 was not in a responding session.
- Latest live logs show the injected direct `LSMTLView keyDown:` / `keyUp:` path firing with `direct_view=yes`, but that does not prove BG3's Noesis/menu layer consumed the input. The game reached menu state and did not load a save or produce a responding socket.
- CGEvent mouse clicks also may not be landing. Check for Retina/pixel-vs-point coordinate problems and mixed Quartz/System Events coordinate spaces.
- The user closed the game after the last attempt, so begin live validation from a clean stopped state.

## Non-Negotiable Outcome

The goal is complete only when this command works without manual clicks or keystrokes:

```bash
PYTHONPATH=tools python3 -m bg3se_harness test --headless --continue --tier 1
```

It must:

1. Build and deploy the dylib.
2. Patch or verify the BG3 binary.
3. Launch BG3 with NoLauncher and video/splash automation enabled.
4. Reach menu or bypass menu deterministically.
5. Trigger Continue or load a known save.
6. Wait for `/tmp/bg3se.sock` to both accept and respond to `Ext.GetVersion()`.
7. Hide or move the window only after doing so without stalling Metal.
8. Restore the user's `graphicSettings.lsx`.
9. Mute audio in headless mode or report why mute could not be applied.
10. Run Tier 1 tests and exit according to test success, not boot timeout.
11. Emit structured JSON that proves the phase sequence.

If true headless rendering proves infeasible, the accepted practical mode is "temporary windowed launch plus offscreen/hide after socket readiness." But you must explicitly investigate and document the feasibility of a truly headless/no-window mode.

## Working Definitions

- "Socket exists" is not success. Success means the socket responds to a command such as `Ext.GetVersion()`.
- "Input method called" is not success. Success means BG3 state changes: splash dismissed, menu selection accepted, save loading begins, or session loads.
- "Headless" currently means temporary windowed launch plus hide/offscreen behavior. Do not claim true headless unless no macOS window/Space is created and BG3 still boots to a usable socket.
- "Menu click works" means a button press changes BG3 state, not merely that a CGEvent was posted.

## First Pass: Inventory and Baseline

Run:

```bash
git status --short
git log --oneline --decorate -n 20
PYTHONPATH=tools python3 -m bg3se_harness --help
PYTHONPATH=tools python3 -m bg3se_harness launch --help
PYTHONPATH=tools python3 -m bg3se_harness test --help
PYTHONPATH=tools python3 -m bg3se_harness status
```

Record findings in a new progress file:

```bash
docs/bugs/headless-cli-goal-progress.md
```

Keep it updated with dated attempts, commands, observed phases, screenshots/log paths, changes made, and remaining blockers.

## Offline Validation Gate

Before every live BG3 attempt after code changes, run the relevant offline checks:

```bash
PYTHONPATH=tools python3 -m pytest tests/harness -q
build/bin/bg3se_test_tier0
PYTHONPATH=tools python3 -m bg3se_harness.tests_nexus
PYTHONPATH=tools python3 -m bg3se_harness.tests_wiki
```

If build-affecting C/ObjC changes were made, also run:

```bash
cmake --build build
PYTHONPATH=tools python3 -m bg3se_harness build
```

Do not skip tests because previous attempts passed. If a test is unrelated and already failing, document it with the exact failure and continue only if the risk is understood.

## Live Attempt Protocol

Use a repeatable live sequence:

```bash
PYTHONPATH=tools python3 -m bg3se_harness quit --force
PYTHONPATH=tools python3 -m bg3se_harness doctor
PYTHONPATH=tools python3 -m bg3se_harness launch --headless --continue --timeout 180
PYTHONPATH=tools python3 -m bg3se_harness status
PYTHONPATH=tools python3 -m bg3se_harness boot-log
tail -n 160 "$HOME/Library/Application Support/BG3SE/logs/latest.log"
```

For each live attempt, capture:

- PID
- command line
- monitor phases
- socket phases
- latest BG3SE log path
- latest gold/network log paths if present
- screenshot path if one can be captured
- foreground app before and after launch
- whether the terminal received any leaked keys
- whether `graphicSettings.lsx` was restored

Recovery command:

```bash
PYTHONPATH=tools python3 -m bg3se_harness quit --force
```

If BG3 is hidden but running, use:

```bash
PYTHONPATH=tools python3 -c 'from bg3se_harness.launch import show_window; import json; print(json.dumps(show_window(), indent=2))'
```

## Track A: Fix Boot Automation to Loaded Save

The boot pipeline must move through these observable phases:

```text
process_launched
dylib_loaded
socket_listening
menu_or_splash_detected
continue_or_load_requested
save_load_started
session_loaded
socket_responded
headless_hide_or_offscreen
graphics_restored
tests_started
tests_finished
```

Add or fix instrumentation so the harness can distinguish:

- socket file missing
- socket accepts but Lua is not ready
- BG3 on splash screen
- BG3 on main menu
- Continue click/key sent
- Continue accepted by game state
- save load started
- `SessionLoaded` or equivalent fired
- process exited
- timeout

The current `focusless_input.m` direct `LSMTLView` call is not enough. Prove whether input reaches BG3's actual input stack. Acceptable proof includes one or more of:

- Noesis/menu state changes in game logs.
- Game state logs transition from `Menu` to save loading.
- A screenshot before/after input changes visible state.
- A temporary debug hook around `ls::InputManager::InjectInput` shows queue insertion and downstream consumption.
- A Lua/console event fires after the input action.

If direct `LSMTLView keyDown:` still does not affect menu state:

1. Re-check whether the constructed `NSEvent` needs location, window number, modifier flags, repeat state, `characters`, or `charactersIgnoringModifiers` adjusted.
2. Verify the key mapping for Return, Space, Escape in `CocoaInputTranslator::s_KeyboardKeys[]`.
3. Investigate direct `InputManager::InjectInput` calls rather than calling ObjC event handlers.
4. Investigate direct Noesis keyboard APIs if the menu consumes Noesis-level input rather than raw input.
5. Prefer state-changing game APIs or flags over synthetic UI input if they are safer.

## Track B: Mouse Click and Retina Coordinate Validation

CGEvent mouse clicks are suspected not to land. Validate this explicitly before changing the click code.

The current menu coordinate path is:

```text
screencapture -l <Quartz window id>
Vision OCR bbox in normalized image coordinates
image pixels from sips
window position/size from System Events
screen_x = bounds.x + px_x / (img_w / bounds.width)
screen_y = bounds.y + px_y / (img_h / bounds.height)
CGEventPost(kCGHIDEventTap)
```

This may fail if:

- Screenshot pixels are Retina backing pixels while CGEvent wants display points.
- System Events window bounds and Quartz window bounds disagree.
- The BG3 window has title-bar/chrome offsets not represented in captured content.
- The window was moved offscreen to negative coordinates before click.
- `screencapture -l` captures a scaled/cropped surface.
- The chosen window ID is stale or is a non-interactive Metal child/surface.
- BG3 must be frontmost for HID mouse events even if coordinates are correct.

Add a diagnostic command or JSON mode, for example:

```bash
PYTHONPATH=tools python3 -m bg3se_harness menu geometry
```

It should report:

- BG3 PID.
- Quartz window ID.
- Quartz owner name and window name.
- Quartz bounds in global coordinates.
- System Events window position and size.
- Captured screenshot pixel width/height.
- Computed `scale_x` and `scale_y`.
- Main display scale factor if accessible.
- OCR bbox and computed pixel center.
- Computed point-space screen coordinate.
- Alternative coordinate candidates:
  - point-space coordinate from System Events bounds
  - pixel-space coordinate from Quartz bounds
  - titlebar-adjusted coordinate
  - center-of-window sanity coordinate
- Whether the coordinate is inside the Quartz bounds and System Events bounds.

Add an optional non-destructive visual overlay artifact for debugging:

```bash
PYTHONPATH=tools python3 -m bg3se_harness menu detect --debug-image .screenshots/menu-debug.png
```

The debug image should mark OCR boxes and intended click centers. Use standard macOS tools or Python stdlib if possible; do not add heavy dependencies unless justified.

Acceptance for this track:

- `menu detect` returns raw OCR plus geometry metadata.
- `menu click "Continue"` reports the exact coordinate basis used.
- At least one live click attempt proves state change or documents why click delivery is impossible.
- The code no longer mixes System Events and Quartz bounds silently. If both are used, the JSON shows both and the selected basis.
- Retina scaling is explicitly tested. A 2x scaling assumption must be measured, not guessed.

## Track C: True Headless Feasibility

Explore whether BG3 can boot to a useful Script Extender socket without creating a normal visible/windowed Metal window. Do this as a feasibility track, not as a blocker for the practical headless CLI.

Investigate:

1. CLI flags and binary strings:

```bash
PYTHONPATH=tools python3 -m bg3se_harness flags --verify
strings -a "$HOME/Library/Application Support/Steam/steamapps/common/Baldurs Gate 3/Baldur's Gate 3.app/Contents/MacOS/Baldur's Gate 3" | rg -i "headless|null|server|dedicated|window|fullscreen|renderer|metal|display|screen|offscreen|nosound|silent|graphics"
```

2. Game state bypass:
   - Can `-continueGame`, `-loadSaveGame`, `-load`, or `-testLoadLevel` bypass menu after splash?
   - Can the launcher/game state code be called directly from the injected dylib?
   - Can a save load be triggered by setting state machine components instead of UI input?

3. Rendering/window options:
   - Does `graphicSettings.lsx` support a borderless/minimized/offscreen mode that does not stall Metal?
   - Can the harness move the window offscreen immediately after creation while preserving drawables?
   - Can a dummy/null Metal layer or offscreen `CAMetalLayer` be substituted safely?
   - Does hiding/minimizing still stall Metal? If yes, document exact evidence.

4. Process modes:
   - Is there a server/dedicated mode string in the binary?
   - Does `--nodb`, `--noxml`, `--cpuLimit`, or other flags alter boot enough to help?
   - Is a no-render load path present in developer/test functions?

5. Audio:
   - Keep `BG3SE_MUTE_AUDIO` deferred to a point where Wwise is ready.
   - Report success/failure in JSON.

Deliverable for this track:

```bash
docs/bugs/true-headless-feasibility.md
```

It must classify true headless as one of:

- `viable-now`: implement it and make `--headless-mode true` or equivalent.
- `possible-with-RE`: describe exact functions/offsets still needed.
- `not-viable-currently`: explain why windowed/offscreen mode is the correct CLI default.

Do not spend unlimited time here if practical headless is close. The CLI's accepted default can remain windowed/offscreen if true headless requires invasive renderer replacement.

## Track D: CLI Surface Completeness

Validate every scoped CLI group at the appropriate level.

Offline-only or no-game commands:

```bash
PYTHONPATH=tools python3 -m bg3se_harness --help
PYTHONPATH=tools python3 -m bg3se_harness flags
PYTHONPATH=tools python3 -m bg3se_harness flags --verify
PYTHONPATH=tools python3 -m bg3se_harness save list
PYTHONPATH=tools python3 -m bg3se_harness save list --fixtures
PYTHONPATH=tools python3 -m bg3se_harness mod list
PYTHONPATH=tools python3 -m bg3se_harness compat list
PYTHONPATH=tools python3 -m bg3se_harness parity missing
PYTHONPATH=tools python3 -m bg3se_harness doctor
```

Live socket commands after boot succeeds:

```bash
PYTHONPATH=tools python3 -m bg3se_harness run "return Ext.GetVersion()"
PYTHONPATH=tools python3 -m bg3se_harness test --tier 1
PYTHONPATH=tools python3 -m bg3se_harness test --tier 2
PYTHONPATH=tools python3 -m bg3se_harness parity scan
PYTHONPATH=tools python3 -m bg3se_harness parity verify Stats
PYTHONPATH=tools python3 -m bg3se_harness components --count
PYTHONPATH=tools python3 -m bg3se_harness stats WPN_Longsword
PYTHONPATH=tools python3 -m bg3se_harness screenshot
PYTHONPATH=tools python3 -m bg3se_harness events --list
```

Mod-vetting commands:

```bash
PYTHONPATH=tools python3 -m bg3se_harness compat run mcm
PYTHONPATH=tools python3 -m bg3se_harness compat vet mcm
```

If a command is intentionally unavailable without credentials, external apps, or a specific fixture, it should fail with a structured JSON error and actionable message, not a traceback.

## JSON Contract

For `launch`, `test`, `compat run`, and `compat vet`, include enough data to audit the automation:

```json
{
  "success": true,
  "command": "test",
  "launch": {
    "pid": 123,
    "continue_game": true,
    "load_save": null,
    "headless": {
      "requested": true,
      "mode": "windowed_offscreen",
      "hidden": true,
      "graphics_restore": {"success": true}
    },
    "automation": {
      "video_skip": {"method": "bink_hook", "success": true},
      "splash": {"method": "direct_lsmtlview", "success": true},
      "menu": {"method": "retina_corrected_cgevent|system_events_key|direct_game_state", "success": true},
      "audio_mute": {"method": "deferred_wwise_pause", "success": true}
    },
    "phases": []
  },
  "tests": [],
  "summary": {}
}
```

Use whatever exact schema fits the existing code, but keep it stable and documented.

## Documentation Deliverables

Update or create:

```bash
docs/bugs/headless-cli-goal-progress.md
docs/bugs/true-headless-feasibility.md
docs/harness.md
```

Update `docs/bugs/headless-boot-debug-log.md` only if continuing that chronological debug log is useful. Do not overwrite its history.

The progress doc should end with:

- final command run
- final JSON summary path/output
- tests passed
- remaining limitations
- whether true headless is viable
- whether Retina coordinate bug was confirmed or rejected

## Acceptance Criteria

The goal is complete when all are true:

1. `PYTHONPATH=tools python3 -m bg3se_harness test --headless --continue --tier 1` succeeds without manual input.
2. The command reaches a loaded-save socket session and runs tests.
3. If the window appears, it does not remain visible after socket readiness.
4. The user's graphics settings are restored after success and failure.
5. Audio mute is attempted at the correct lifecycle point and reported.
6. Input/click automation is proven by game state changes, not just event-post return values.
7. Retina coordinate handling is measured and either fixed or documented as not the cause.
8. True headless feasibility is documented with evidence.
9. Offline tests pass.
10. Live CLI smoke tests for core launch/test/status/run/screenshot/menu or their structured failure modes are recorded.
11. No unrelated user changes are reverted.

## Stop Conditions

Stop and report if:

- BG3 repeatedly exits before menu with the same native crash and no useful log trail.
- A live probe would require destructive mutation of the BG3 app bundle beyond the existing reversible patching model.
- True headless requires replacing or deeply patching Metal rendering and practical windowed/offscreen mode already satisfies the CLI objective.
- Accessibility or Screen Recording permission is missing and cannot be granted by the agent.

In a stop report, include exact commands, logs, screenshots if available, and the smallest next experiment.
