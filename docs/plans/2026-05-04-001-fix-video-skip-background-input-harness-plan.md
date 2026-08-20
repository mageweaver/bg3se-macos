# Fix Video Skip and Background Input for BG3SE Harness

This ExecPlan is a living document. Sections Progress, Surprises &
Discoveries, Decision Log, and Outcomes & Retrospective must be
kept up to date as work proceeds.

## Purpose / Big Picture

The harness should launch Baldur's Gate 3, avoid or finish the intro movie phase, dismiss the "Press Any Key" splash, wait for the Script Extender socket, and run tests while the CLI or another terminal remains the frontmost application. The user-visible value is an autonomous command such as `PYTHONPATH=tools python3 -m bg3se_harness test --headless --continue` that reaches `"socket_connected": true` without a person clicking BG3 or manually pressing Space.

The plan deliberately separates two problems. Video skipping means avoiding the Bink `.bk2` movie files that run before the splash. Background input means delivering the splash-dismiss Space key to BG3 without making BG3 frontmost. A Bink file is a RAD Game Tools video file with extension `.bk2`. A frontmost app is the macOS application currently receiving normal keyboard input.

## Progress

- [x] (2026-05-04 14:45Z) Read `tools/bg3se_harness/launch.py`, `tools/bg3se_harness/menu.py`, `src/input/input_hooks.m`, `src/input/lua_input.c`, `src/injector/main.c`, `CMakeLists.txt`, and the relevant Ghidra notes.
- [x] (2026-05-04 14:45Z) Confirmed the current harness already writes `SkipVideo` and `SkipSplashScreen` but the task reports BG3 ignores those writes.
- [x] (2026-05-04 14:45Z) Confirmed the installed BG3 app has no loose `.bk2` files under the bundle; videos are packed inside `Contents/Data/Gustav_Video.pak`.
- [x] (2026-05-04 14:45Z) Confirmed `Gustav_Video.pak` contains only three movie paths: `Mods/Gustav/Localization/English/Video/GUS_CGI01_Part1.bk2`, `Mods/Gustav/Localization/English/Video/GUS_CGI01_Part2.bk2`, and `Video/Splash_Logo_Larian.bk2`.
- [x] (2026-05-04 14:45Z) Confirmed the BG3 binary contains `SkipVideo`, `SkipSplashScreen`, `-mediaPath`, `.bk2`, `MovieSystem`, `PlayMovieMessage`, and `NETMSG_SKIPMOVIE_RESULT` strings.
- [x] (2026-05-04 14:45Z) Confirmed `libBink2Mac.dylib` appears stripped and does not expose useful exported `Bink*` symbols through `nm`, making symbol-level Bink hooks a poor first choice.
- [x] (2026-05-04 14:45Z) Evaluated all video-skip and background-input options requested in the task.
- [ ] Implement Milestone 1, the low-cost probes that determine whether `CGEventPostToPid` and `-mediaPath` work in this BG3 build.
- [ ] Implement Milestone 2, the exact-config or media-path video skip path chosen by Milestone 1.
- [ ] Implement Milestone 3, the focusless input path chosen by Milestone 1.
- [ ] Implement Milestone 4, the unified launch pipeline and JSON diagnostics.
- [ ] Run live validation while the CLI is frontmost and BG3 is not frontmost.
- [ ] Record final outcomes and any discarded alternatives in this document.

## Surprises & Discoveries

- Observation: Option A, deleting or renaming loose `.bk2` files in the app bundle, does not match the installed macOS layout.
  Evidence: `find "$HOME/Library/Application Support/Steam/steamapps/common/Baldurs Gate 3/Baldur's Gate 3.app" -iname '*.bk2'` returned no files, while `Contents/Data/Gustav_Video.pak` exists and is 1.6 GB.

- Observation: The exact intro movie surface is small.
  Evidence: `PYTHONPATH=tools python3 -m bg3se_harness.mod_manager.pak_inspector list ".../Contents/Data/Gustav_Video.pak"` returned three files: two `GUS_CGI01` movies and `Video/Splash_Logo_Larian.bk2`.

- Observation: The binary knows about the settings names the harness already writes, so the failure is probably wrong storage, wrong type, wrong read timing, or a separate movie path.
  Evidence: `strings -a ".../Contents/MacOS/Baldur's Gate 3" | rg -i "SkipVideo|SkipSplashScreen"` prints both `SkipVideo` and `SkipSplashScreen`.

- Observation: Bink function hooking is not a good first implementation path.
  Evidence: `nm -m ".../Contents/Frameworks/libBink2Mac.dylib"` shows undefined system symbols but no useful external `BinkOpen`, `BinkDoFrame`, or `BinkClose` export names.

- Observation: The current Lua-facing input injection is not focusless.
  Evidence: `src/input/input_hooks.m` implements `input_inject_key_down()` and `input_inject_key_up()` with `CGEventPost(kCGHIDEventTap, event)`, which posts to the system input stream instead of targeting BG3.

- Observation: The socket is too late to be the only splash-dismiss mechanism unless live testing proves it is responsive before the splash blocks progress.
  Evidence: `tools/bg3se_harness/launch.py:167` waits for the socket while also pressing Space, which means existing behavior assumes Space may be needed before the socket responds usefully.

## Decision Log

- Decision: Start with probes, not a permanent hook.
  Rationale: Two low-cost probes answer the most important unknowns: whether `CGEventPostToPid()` can deliver Space to a background BG3 process, and whether `-mediaPath` can redirect the three known `.bk2` paths. Both probes are reversible and avoid touching the 1.6 GB video PAK.
  Date/Author: 2026-05-04 / Planner Agent

- Decision: Prefer exact config repair or `-mediaPath` for video skipping before binary hooks.
  Rationale: Config and path-level approaches are reversible, easy to validate from the harness, and do not patch ARM64 code. Main-binary hooks are higher risk in this repository because the ARM64 docs warn that Dobby inline hooks can corrupt PC-relative instructions.
  Date/Author: 2026-05-04 / Planner Agent

- Decision: Do not rename `Gustav_Video.pak` as the normal solution.
  Rationale: It mutates the installed game bundle, affects all BG3 launches outside the harness, can be undone by Steam verification, and may break later movies beyond the intro path.
  Date/Author: 2026-05-04 / Planner Agent

- Decision: Treat Noesis `MediaState` as a research-only fallback.
  Rationale: The current macOS code exposes `Ext.UI` as Noesis stubs, while the binary also contains Noesis strings. That mixed evidence makes Noesis a poor first target for reliable splash and movie automation.
  Date/Author: 2026-05-04 / Planner Agent

- Decision: If `CGEventPostToPid()` fails, implement in-process splash automation in the injected dylib rather than repeatedly focusing BG3.
  Rationale: The dylib is already loaded inside BG3 via `insert_dylib`, links AppKit, and can schedule work on BG3's main run loop. Sending the event from inside the target process is the most direct path to background operation.
  Date/Author: 2026-05-04 / Planner Agent

- Decision: Keep force-focus as the final fallback, not the target behavior.
  Rationale: `NSRunningApplication.activate()` plus `CGEventPost(kCGHIDEventTap, ...)` can keep current behavior working, but it does not satisfy the requirement that the CLI remains frontmost.
  Date/Author: 2026-05-04 / Planner Agent

## Outcomes & Retrospective

(Pending completion)

## Context and Orientation

The Python harness lives in `tools/bg3se_harness/`. `tools/bg3se_harness/cli.py` builds, deploys, patches, launches, waits for the socket, and runs tests. `tools/bg3se_harness/launch.py` owns process launch, UserDefaults writes, `graphicSettings.lsx` edits, and socket polling. `tools/bg3se_harness/menu.py` owns macOS screenshot, OCR, click, and key automation.

The injected Script Extender dylib is built from C, Objective-C, C++, and Objective-C++ files listed in `CMakeLists.txt`. It is copied into `Baldur's Gate 3.app/Contents/MacOS/libbg3se.dylib` and loaded by the patched BG3 executable. `src/injector/main.c` initializes Lua, the socket console, the path override map, and the input system. `src/input/input_hooks.m` currently captures input through a Core Graphics event tap and injects keys through system-level `CGEventPost(kCGHIDEventTap, ...)`. `src/input/lua_input.c` registers `Ext.Input.InjectKeyPress`, `Ext.Input.InjectKeyDown`, and `Ext.Input.InjectKeyUp`.

The current launch flow is `kill_existing()`, `clean_socket()`, `ensure_no_launcher()`, `ensure_skip_videos()`, start BG3 with `arch -arm64`, then `wait_for_socket(..., dismiss_splash=True)`. `wait_for_socket()` calls `_try_dismiss_splash()`, which imports `menu.dismiss_splash_aggressive()`. `dismiss_splash_aggressive()` first uses AppleScript System Events to make BG3 frontmost, then posts Space with `CGEventPost(kCGHIDEventTap, ...)`. This explains why the current path works only when BG3 can become frontmost.

The relevant reverse-engineering files are `ghidra/offsets/CLI_FLAGS.md`, `ghidra/offsets/GAMESTATE.md`, `ghidra/offsets/NOESIS_UI_FRAMEWORK.md`, and `ghidra/offsets/ARM64_SAFE_HOOKING.md`. The CLI flag document confirms `-continueGame`, `-loadSaveGame`, and `-mediaPath`, and confirms there is no known `-skipVideo` or `-skipIntro` flag. The ARM64 hooking document explains why main-binary hooks require caution.

## Plan of Work

Milestone 1 is a probing milestone. It adds two small, reversible test paths before committing to a deeper implementation. The first probe adds a Python `cg_key_to_pid(pid, keycode)` helper in `tools/bg3se_harness/menu.py` using `CGEventPostToPid`. The second probe adds a temporary harness mode that launches BG3 with `-mediaPath` pointing at an empty harness-managed directory under `~/.config/bg3se-harness/media_skip_probe`. A novice can run each probe and record whether BG3 stays in the background and whether intro videos are skipped.

Milestone 2 implements the video-skip path. If `-mediaPath` successfully skips missing `.bk2` files without blocking launch, implement `tools/bg3se_harness/video_skip.py` to prepare the media-skip directory and add `mediaPath` to `extra_flags` from `tools/bg3se_harness/launch.py`. If `-mediaPath` does not work, use Ghidra xrefs to `SkipVideo` and `SkipSplashScreen` to find the exact settings file, section, type, and read timing, then update `ensure_skip_videos()` in `tools/bg3se_harness/launch.py` to write that exact location. If both fail, add a documented fallback that suppresses the known movie requests by in-process path interception or a safe hook, but only after runtime prologue analysis shows the target is safe.

Milestone 3 implements background input. If the `CGEventPostToPid` probe works while BG3 is behind the CLI, change `_try_dismiss_splash()` in `tools/bg3se_harness/launch.py` to pass the launched process PID into a new `dismiss_splash_background(pid)` path and stop activating BG3 by default. If `CGEventPostToPid` does not work, add a new Objective-C module such as `src/input/focusless_input.m` and `src/input/focusless_input.h`. That module should schedule a short-lived splash auto-dismiss timer on BG3's main run loop when the harness sets `BG3SE_AUTO_DISMISS_SPLASH=1`. Each tick should synthesize Space key down and up inside the BG3 process, first by `CGEventPostToPid(getpid(), event)`, then by constructing `NSEvent` key events and calling `[NSApp sendEvent:event]` on the main thread. The timer stops when a maximum duration expires or when a new C function marks the socket as responsive.

Milestone 4 wires the unified pipeline. `tools/bg3se_harness/launch.py` should set harness-specific environment variables in `subprocess.Popen`, prepare video skip only when `skip_videos=True`, pass `-continueGame` or `-loadSaveGame` exactly as it does now, and wait for the socket without requiring BG3 focus. `tools/bg3se_harness/cli.py` should include JSON fields showing which video method and input method were used. `docs/harness.md` should state the new behavior plainly: BG3 may open a window, but the harness should not need to make it frontmost to dismiss the splash.

Milestone 5 validates the full flow live. The important acceptance test is not just that the socket responds; it is that the frontmost application remains the CLI or Terminal while BG3 is launching. The validation commands below include explicit foreground-app checks before and after launch.

## Concrete Steps

Run all commands from `/Users/tomdimino/Desktop/Programming/game-modding/bg3/bg3se-macos`.

Step 1: Reconfirm the current video layout and binary strings before editing.

```bash
find "$HOME/Library/Application Support/Steam/steamapps/common/Baldurs Gate 3/Baldur's Gate 3.app" -iname '*.bk2' 2>/dev/null | head
PYTHONPATH=tools python3 -m bg3se_harness.mod_manager.pak_inspector list "$HOME/Library/Application Support/Steam/steamapps/common/Baldurs Gate 3/Baldur's Gate 3.app/Contents/Data/Gustav_Video.pak"
strings -a "$HOME/Library/Application Support/Steam/steamapps/common/Baldurs Gate 3/Baldur's Gate 3.app/Contents/MacOS/Baldur's Gate 3" | rg -i "SkipVideo|SkipSplashScreen|-mediaPath|\\.bk2|MovieSystem"
```

Expected output: the first command prints no loose `.bk2` files. The second command prints JSON with three paths and `"count": 3`. The third command prints at least `SkipVideo`, `SkipSplashScreen`, `-mediaPath`, `.bk2`, and `MovieSystem`.

Step 2: Add the background-input probe in `tools/bg3se_harness/menu.py`.

Define a new function named `cg_key_to_pid(pid, keycode)` beside `cg_key(keycode)`. It should use the existing ApplicationServices `ctypes` loader, create key down and key up events with `CGEventCreateKeyboardEvent`, and call `CGEventPostToPid(pid, event)` for each event. It should return a structured dictionary with `success`, `method`, `pid`, and an error string if event creation fails.

Add `dismiss_splash_background(pid)` beside `dismiss_splash_aggressive()`. It should call `cg_key_to_pid(pid, _kVK_Space)` and should not run AppleScript or call `CGEventPost(kCGHIDEventTap, ...)`.

Step 3: Wire the probe into `tools/bg3se_harness/launch.py` without removing the old fallback.

Change `_try_dismiss_splash(attempt)` to accept `process=None`. If `process` is present, try `dismiss_splash_background(process.pid)` first. If it succeeds, print `Splash dismiss #N (cg_key_to_pid)` and return. If it fails, call the existing `dismiss_splash_aggressive()` fallback and print the method that succeeded. Change the call inside `wait_for_socket()` so it passes the same `process` object it already receives.

Step 4: Run the first live background-input probe.

```bash
PYTHONPATH=tools python3 -m bg3se_harness quit --force
osascript -e 'tell application "System Events" to get name of first application process whose frontmost is true'
PYTHONPATH=tools python3 -m bg3se_harness launch --continue --timeout 90
osascript -e 'tell application "System Events" to get name of first application process whose frontmost is true'
```

Expected output after implementation: the launch JSON contains `"socket_connected": true`. The foreground app before and after should be the terminal or the CLI host, not `Baldur's Gate 3`. If BG3 becomes frontmost, record that `CGEventPostToPid` failed and the aggressive fallback ran.

Step 5: Add the media-path probe in `tools/bg3se_harness/video_skip.py`.

Create a new file `tools/bg3se_harness/video_skip.py` with a function `prepare_media_skip_probe() -> dict`. It should create `~/.config/bg3se-harness/media_skip_probe`, create the subdirectories `Video` and `Mods/Gustav/Localization/English/Video`, and return the path plus the three expected movie paths. For the first probe, leave the `.bk2` files absent. Missing files are the safest first test because invalid zero-byte Bink files may block on decoder errors.

Temporarily add an opt-in CLI flag such as `--video-skip-method media-path-probe` to `tools/bg3se_harness/cli.py` or pass raw flags with the existing `--flags` mechanism. The launch command should add `-mediaPath <probe_dir>` only for this probe.

Step 6: Run the media-path probe.

```bash
PYTHONPATH=tools python3 -m bg3se_harness quit --force
PYTHONPATH=tools python3 -m bg3se_harness launch --continue --timeout 90 --flags "-mediaPath $HOME/.config/bg3se-harness/media_skip_probe"
```

Expected successful output: BG3 does not play `Video/Splash_Logo_Larian.bk2`, the splash-dismiss path still works, and JSON contains `"socket_connected": true`. If BG3 logs missing-file errors but continues, the method is acceptable. If BG3 blocks, crashes, or ignores the media path, discard this method.

Step 7: If `-mediaPath` fails, find the exact config read site.

```bash
PYTHONPATH=tools python3 -m bg3se_harness ghidra search-strings SkipVideo
PYTHONPATH=tools python3 -m bg3se_harness ghidra search-strings SkipSplashScreen
PYTHONPATH=tools python3 -m bg3se_harness ghidra xrefs <address_from_search>
PYTHONPATH=tools python3 -m bg3se_harness ghidra decompile <function_or_address_from_xrefs>
```

Expected output: the decompile shows whether BG3 reads UserDefaults, `graphicSettings.lsx`, a different profile file, a command-line option, or an internal settings singleton. Update `tools/bg3se_harness/launch.py:ensure_skip_videos()` only after this evidence is recorded in Surprises & Discoveries.

Step 8: If `CGEventPostToPid` fails, implement in-process input.

Add `src/input/focusless_input.h` with these interfaces:

```c
bool focusless_input_init(void);
void focusless_input_shutdown(void);
bool focusless_input_post_key_press(uint16_t keyCode, uint32_t modifiers);
void focusless_input_start_splash_autodismiss(double durationSeconds, double intervalSeconds);
void focusless_input_mark_socket_ready(void);
```

Add `src/input/focusless_input.m` to implement those functions with AppKit and Core Graphics. It should dispatch all AppKit event construction to the main queue. It should log one line per method at debug level and avoid logging every timer tick unless a send fails. Add this file to `CMakeLists.txt` next to `src/input/input_hooks.m`.

Register a new Lua function in `src/input/lua_input.c`, such as `Ext.Input.InjectKeyPressLocal(key, modifiers)`, that calls `focusless_input_post_key_press()` rather than `input_inject_key_press()`. In `src/injector/main.c`, after `input_init()` succeeds, call `focusless_input_init()` and start the auto-dismiss timer only when `getenv("BG3SE_AUTO_DISMISS_SPLASH")` is `"1"`.

Step 9: Wire launch environment and diagnostics.

In `tools/bg3se_harness/launch.py`, add a helper that builds the child environment for `subprocess.Popen`. When `dismiss_splash=True` will be used by the caller, set `BG3SE_AUTO_DISMISS_SPLASH=1`. When a video method is active, set `BG3SE_VIDEO_SKIP_METHOD=<method>`. Include the selected methods in the health JSON under `"automation": {"video_skip": "...", "input": "..."}`.

Step 10: Build and run offline tests.

```bash
cmake --build build
PYTHONPATH=tools python3 -m bg3se_harness build
PYTHONPATH=tools python3 -m bg3se_harness.tests
PYTHONPATH=tools python3 -m bg3se_harness launch --help
PYTHONPATH=tools python3 -m bg3se_harness test --help
```

Expected output: the CMake and harness builds exit 0, the offline tests exit 0, and help text documents the video/input automation options without removing `--no-skip-videos`.

Step 11: Run final live validation with the CLI or the terminal kept frontmost.

```bash
PYTHONPATH=tools python3 -m bg3se_harness quit --force
osascript -e 'tell application "System Events" to get name of first application process whose frontmost is true'
PYTHONPATH=tools python3 -m bg3se_harness test --headless --continue --tier 1
osascript -e 'tell application "System Events" to get name of first application process whose frontmost is true'
```

Expected output: BG3 may create a window, but the foreground app remains the CLI or the terminal. The test command returns structured JSON with launch metadata, `"socket_connected": true` or a successful test summary, and automation metadata showing a focusless input method. If the foreground app changes to `Baldur's Gate 3`, the background-input requirement is not met.

## Validation and Acceptance

Acceptance criterion 1: The harness can launch BG3 and reach the Script Extender socket while the terminal or the CLI remains frontmost. This must be verified with the `osascript` foreground-app command before and after a live launch.

Acceptance criterion 2: Intro videos are skipped by a reversible harness-controlled mechanism. Acceptable mechanisms are an exact verified config write, a working `-mediaPath` redirection, or a narrowly targeted path/movie hook with documented runtime proof. Renaming `Gustav_Video.pak` is not acceptable as the default solution.

Acceptance criterion 3: Background input does not depend on `System Events` setting BG3 frontmost. The old `dismiss_splash_aggressive()` path may remain as an explicit fallback, but the normal path used during validation must be `CGEventPostToPid` or in-process input.

Acceptance criterion 4: The launch JSON reports the selected video-skip method and input method. A novice should be able to tell whether a run used `config`, `mediaPath`, `cgEventPostToPid`, `inProcessNSEvent`, or `forceFocusFallback`.

Acceptance criterion 5: `PYTHONPATH=tools python3 -m bg3se_harness test --headless --continue --tier 1` runs without manual clicks or keystrokes and exits according to test results, not according to splash timeout.

Acceptance criterion 6: `--no-skip-videos` remains available and disables only video-skip preparation. It must not disable background splash dismissal because splash dismissal is input automation, not video skipping.

Acceptance criterion 7: If BG3 exits early, the command returns JSON with `"stage": "process_exited"` and the exit code. If the video method fails, the command reports the method and failure reason instead of silently falling back to a different method without recording it.

## Idempotence and Recovery

The probing and implementation steps are safe to repeat. `launch()` already kills existing BG3 processes and removes the stale socket before starting a new process. The media-path probe writes only under `~/.config/bg3se-harness/media_skip_probe`, so rerunning it can recreate the same directory without affecting the app bundle.

If the media-path probe causes BG3 to fail launch, remove the probe directory and run without `-mediaPath`:

```bash
rm -rf "$HOME/.config/bg3se-harness/media_skip_probe"
PYTHONPATH=tools python3 -m bg3se_harness quit --force
```

If BG3 is left hidden or stuck after a failed live run, recover with:

```bash
PYTHONPATH=tools python3 -m bg3se_harness quit --force
```

If code changes need to be reverted, revert only the files touched by this plan. Do not reset the whole repository because the worktree already contains unrelated uncommitted changes. Expected touched files are `tools/bg3se_harness/menu.py`, `tools/bg3se_harness/launch.py`, `tools/bg3se_harness/cli.py`, `tools/bg3se_harness/video_skip.py`, `src/input/focusless_input.h`, `src/input/focusless_input.m`, `src/input/lua_input.c`, `src/input/input.h`, `src/injector/main.c`, `CMakeLists.txt`, `docs/harness.md`, and this plan.

If `CGEventPostToPid` works on one macOS version but fails on another, keep it as method `cgEventPostToPid` and let the harness fall back to method `inProcessNSEvent` when available. Do not remove the old force-focus path until multiple live runs prove the replacement works.

## Interfaces and Dependencies

`tools/bg3se_harness/menu.py` must provide:

```python
def cg_key_to_pid(pid: int, keycode: int) -> dict:
    """Send a key press to a specific process without activating it."""

def dismiss_splash_background(pid: int) -> dict:
    """Dismiss BG3 splash without making BG3 frontmost."""
```

The returned dictionaries must include `success: bool` and `method: str`.

`tools/bg3se_harness/launch.py` must keep:

```python
def wait_for_socket(timeout=HEALTH_TIMEOUT, dismiss_splash=False, process=None):
    """Wait for the SE socket to respond to Lua commands."""
```

When `dismiss_splash` and `process` are provided, `wait_for_socket()` should use the background dismiss method before any force-focus fallback.

If the media-path method is accepted, `tools/bg3se_harness/video_skip.py` must provide:

```python
def prepare_video_skip() -> dict:
    """Prepare reversible harness-owned video-skip resources."""
```

The result must include the selected method, the directory if one is used, and the three known movie paths.

If in-process input is required, `src/input/focusless_input.h` must provide:

```c
bool focusless_input_init(void);
void focusless_input_shutdown(void);
bool focusless_input_post_key_press(uint16_t keyCode, uint32_t modifiers);
void focusless_input_start_splash_autodismiss(double durationSeconds, double intervalSeconds);
void focusless_input_mark_socket_ready(void);
```

`src/input/lua_input.c` should expose `Ext.Input.InjectKeyPressLocal(key, modifiers)` only after `focusless_input_post_key_press()` exists. The old `Ext.Input.InjectKeyPress()` may remain system-level for compatibility, but documentation must say it is not guaranteed to work on a background BG3 process.

No new third-party dependency is required. Python changes use the standard library and ApplicationServices through `ctypes`, matching existing `tools/bg3se_harness/menu.py`. Native changes use frameworks already linked by `CMakeLists.txt`: Foundation, CoreFoundation, AppKit, QuartzCore, Metal, MetalKit, and GameController.

## Option Evaluation

Video option A, delete or rename `.bk2` files, has low feasibility in the current macOS install because the files are inside `Gustav_Video.pak`, not loose in the bundle. The implementation complexity for loose files would be low, but the actual PAK mutation risk is high because it changes the installed game and may affect all movie playback. This option should be rejected as the default.

Video option B, hook the video player function to immediately return, has medium feasibility but high risk. It may be reliable after the exact `MovieSystem` or Bink call is found, but the main binary and Bink dylib are stripped, and this repo's ARM64 notes warn about unsafe inline hooks. Complexity is high because it needs Ghidra or Frida analysis, ABI validation, and crash testing. This is a last-resort fallback.

Video option C, use `-mediaPath` to redirect video paths, has medium feasibility, low-to-medium risk, and medium complexity. It is attractive because it is reversible and the binary explicitly contains `-mediaPath`, `mediaPath.txt`, `.bk2`, and the three movie paths are known. The unknown is whether missing redirected `.bk2` files are treated as "skip and continue" or as fatal errors. This should be probed first.

Video option D, hook Noesis `MediaState`, has low feasibility for this specific problem. Noesis strings exist, but the macOS Ext.UI code treats Noesis as unavailable stubs, and the movie strings point at `ecl::screen_fade::MovieSystem` and Bink instead. Risk and complexity are high because it may target the wrong subsystem. This should stay research-only.

Video option E, find the actual config key read by the binary, has high value and low runtime risk, but unknown implementation complexity. The binary contains `SkipVideo` and `SkipSplashScreen`, so the current failure likely means the harness writes the wrong store, wrong type, or too late. This is the preferred durable fix if Ghidra xrefs reveal the read site quickly.

Input option A, `CGEventPostToPid`, has medium feasibility, low risk, and low complexity. It is deprecated, but it directly matches the requirement to target a PID without focus. It should be the first probe because it can be implemented entirely in Python and can fall back cleanly.

Input option B, add a socket command or in-process function to simulate keypresses, has high feasibility for post-socket commands and medium feasibility for pre-socket splash dismissal. A socket-only Lua command may be too late for the splash, so the useful version is an in-process auto-dismiss timer plus a Lua API for later commands. Risk is medium and complexity is medium-to-high, but it best matches the fact that the dylib is already injected into BG3.

Input option C, force focus with `NSRunningApplication.activate()` and then post a system CGEvent, has high feasibility and low complexity but does not satisfy the background requirement. It should remain a fallback only.

Input option D, hook `NSApplication sendEvent:` inside the injected dylib, has medium feasibility and high complexity. Directly calling `[NSApp sendEvent:event]` from an in-process timer is simpler than swizzling `sendEvent:`. A hook should be used only for instrumentation if direct AppKit event delivery fails.

Input option E, AppleScript `tell application` with process-ID targeting, has low feasibility for keyboard input. AppleScript can find and activate a process by PID, but sending keystrokes through System Events still goes through the frontmost app. Risk is low, complexity is low, and value is low. This should not be the primary path.
