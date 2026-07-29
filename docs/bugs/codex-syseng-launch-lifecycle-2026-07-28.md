# BG3SE macOS launch-lifecycle robustness design

Date: 2026-07-28  
Scope: offline design only; no game launch, input injection, or Steam-install
mutation was performed.

Line anchors below describe the 2026-07-28 working tree and will move as the
recommendations are implemented.

## Infrastructure Assessment

The harness currently treats a `Popen` PID as both the launch attempt and the
game session. That model is invalid when Steam bootstraps the game: the direct
process can exit cleanly after about 1.3 seconds and Steam can create a second
BG3 process with the same arguments roughly 10 seconds later. The first exit is
a handoff, not a terminal session event.

The immediate recommendations are:

1. Introduce a launch-attempt record and a bounded, one-time Steam PID-adoption
   state. Use it in foreground launch and the detached monitor.
2. Require a ready Steam IPC service before any launch mutation or `Popen`.
3. Stop claiming that `graphicSettings.lsx` controls macOS display mode. Until
   a real LSF reader/writer is built and proven, require an explicit,
   hash-backed “this profile was manually set to Windowed” invariant for
   `--headless`.
4. Add a read-only memory-pressure preflight, blocking only at a critical
   threshold and reporting the measurement in every launch result.
5. Make `session_driver.sh` consume the harness-owned launch identity rather
   than rediscovering an arbitrary BG3 process with `pgrep | head -1`.

### Current gaps

| Risk | Current behavior | Consequence |
|---|---|---|
| Steam bootstrap bounce | `wait_for_socket()` immediately returns `process_exited` when the original `Popen` exits at [`launch.py:547-558`](../../tools/bg3se_harness/launch.py#L547-L558). The background `_FakeProcess` also treats any disappearance as a final exit at [`_monitor.py:47-60`](../../tools/bg3se_harness/_monitor.py#L47-L60). | Foreground retry can race Steam's relaunch. The monitor restores graphics before the relaunched process reads them. |
| Static monitor PID | `_monitor.py` receives one PID, creates one `_FakeProcess`, and overwrites health with that original PID at [`_monitor.py:65-96`](../../tools/bg3se_harness/_monitor.py#L65-L96) and [`_monitor.py:133-137`](../../tools/bg3se_harness/_monitor.py#L133-L137). | It cannot represent a legitimate Steam handoff. |
| Premature cleanup | The monitor restores headless graphics for every socket failure/process exit at [`_monitor.py:148-178`](../../tools/bg3se_harness/_monitor.py#L148-L178); foreground cleanup does the same at [`cli.py:96-105`](../../tools/bg3se_harness/cli.py#L96-L105). | The Steam-created process starts with restored fullscreen-sized settings mid-attempt. |
| Windowed-mode false claim | `HEADLESS_GRAPHICS_ENTRIES` inserts `Fullscreen` and fake-fullscreen keys at [`launch.py:22-29`](../../tools/bg3se_harness/launch.py#L22-L29), and the XML writer creates missing keys at [`launch.py:225-250`](../../tools/bg3se_harness/launch.py#L225-L250). | The macOS build ignores these invented keys; real display mode remains in `PlayerProfiles/Public/config.lsf`. |
| Steam absence | `launch()` begins destructive attempt setup (`kill_existing`, socket removal, preference/settings writes) without a Steam readiness check at [`launch.py:386-404`](../../tools/bg3se_harness/launch.py#L386-L404). | A Steam-less game appears healthy, then cleanly exits around 90–151 seconds with no useful crash report. |
| Weak ownership | The PID file contains only PID and wall time at [`launch.py:32-45`](../../tools/bg3se_harness/launch.py#L32-L45); validation checks only whether `ps comm` contains `Baldur` at [`launch.py:55-64`](../../tools/bg3se_harness/launch.py#L55-L64). | PID reuse or another BG3 instance can inherit the attempt. |
| Global process discovery | `session_driver.sh` selects the first matching process at [`session_driver.sh:78`](../../scripts/session_driver.sh#L78) and later treats any first-result change as replacement at [`session_driver.sh:162-171`](../../scripts/session_driver.sh#L162-L171). | Multiple instances and the expected Steam handoff are ambiguous. |
| Memory pressure not gated | `doctor.py` has no memory-pressure or Steam readiness check in its current checks at [`doctor.py:39-223`](../../tools/bg3se_harness/doctor.py#L39-L223). | Routine jetsam pressure can kill Steam, ReportCrash, or supporting processes and confound a run before it starts. |

### LSF reuse audit

There is no LSF resource reader/writer in this repository:

- [`src/pak/pak_reader.h:1-6`](../../src/pak/pak_reader.h#L1-L6) and
  [`src/pak/pak_reader.c:19-136`](../../src/pak/pak_reader.c#L19-L136) parse
  LSPK v18 archive headers and file lists, not LSOF/LSF resources.
- [`tools/extract_pak.py:1-18`](../../tools/extract_pak.py#L1-L18) is another
  LSPK extractor.
- [`pak_inspector.py:1-14`](../../tools/bg3se_harness/mod_manager/pak_inspector.py#L1-L14)
  is a stdlib LSPK/LSV container reader.
- [`savegames.py:128-183`](../../tools/bg3se_harness/savegames.py#L128-L183)
  searches decompressed LSF bytes for known ASCII markers; it does not parse
  nodes, attributes, types, string tables, or compression.

Read-only inspection of the active profile found `config.lsf` to be a 1,709-byte
`LSOF` version-7 resource. The active `graphicSettings.lsx` has exactly these
16 `MapKey` values:

```text
CASEnabled
DeviceName
FSR1Mode
GraphicSettingsVersion
HDRMaxNits
HDRPaperWhite
RefreshRateDenominator
RefreshRateNumerator
ResolutionUpscaleType
ScreenHeight
ScreenWidth
ShowGammaCalibration
ShowHDRCalibration
SkipSplashScreen
SkipVideo
UpscaleSharpness
```

No display-mode key is present. Existing tests currently codify insertion of
the ineffective keys at
[`test_headless_graphics.py:57-82`](../../tests/harness/test_headless_graphics.py#L57-L82);
those expectations must be replaced.

[Norbyte/lslib](https://github.com/Norbyte/lslib) is the appropriate upstream
format reference: it explicitly supports reading and writing LSF, LSB, LSX,
and LSJ resources. It is not presently vendored or callable from this
stdlib-only harness.

## Proposed Changes

### Component: launch-attempt identity and Steam handoff

- **Purpose:** Keep one logical attempt alive across the expected direct-process
  exit and adopt exactly one Steam-created successor.
- **Technology:** A `LaunchSession`/`ProcessTracker` object in
  `tools/bg3se_harness/launch.py`, backed by an atomic JSON record.
- **Dependencies:** `uuid`, `time.monotonic`, `subprocess`, `os.replace`, and a
  macOS process-identity helper using exact executable path plus process start
  time. Prefer `libproc` through `ctypes`; a narrowly parsed `ps` fallback is
  acceptable.

Replace the PID-only record at
[`launch.py:32-64`](../../tools/bg3se_harness/launch.py#L32-L64) with an
attempt record:

```yaml
schema_version: 2
launch_id: "uuid"
phase: "waiting_for_socket"
created_wall_time: 1785280000.0
created_monotonic: 12345.5
requested_args: ["-continueGame"]
expected_executable: ".../Contents/MacOS/Baldur's Gate 3"
steam:
  preflight: "ready"                 # ready | absent | starting | unknown
  appid_file_present: true           # telemetry only; never a policy switch
  bounce_observed: true
pid: 24680                           # always the current adopted PID
identity:
  start_time: 1785280011.2
  executable: ".../Contents/MacOS/Baldur's Gate 3"
lineage:
  - pid: 12345
    role: "direct"
    observed_exitcode: 0             # null in detached monitor if unknowable
  - pid: 24680
    role: "steam_relaunch"
    adopted_at_s: 11.1
window_contained: false
cleanup_complete: false
```

Write the record through a same-directory temporary file, `flush`/`fsync`, and
`os.replace`. Clear or mutate it only when `launch_id` still matches. A stale
monitor must never clear a newer attempt.

Use this bounded state machine:

```text
preflight
  -> direct_starting
  -> direct_running
       -> socket_ready                         (no bounce world)
       -> bootstrap_exit_candidate
            -> waiting_for_steam_relaunch
                 -> adopted_running
                      -> window_contained
                      -> socket_ready           (bounce world)
                 -> steam_relaunch_timeout      (terminal failure)
            -> process_exited                   (not a qualifying bounce)

socket_ready -> session owner remains the adopted/current PID
any later PID change -> unexpected_replacement (never adopt again)
```

A qualifying handoff must satisfy all of the following:

1. Steam was `ready` at preflight.
2. The direct PID disappears before socket readiness and within
   `STEAM_BOUNCE_MAX_INITIAL_LIFETIME_S=5`.
3. In foreground mode, the observed exit code is `0`. In detached mode, where
   the monitor is not the process parent and may not recover an exit status,
   record `observed_exitcode=null` and require all remaining identity checks.
4. No handoff was adopted earlier in this `launch_id`.
5. Within `STEAM_RELAUNCH_GRACE_S=20`, exactly one new process appears whose
   canonical executable path equals `BG3_EXEC`, whose start time is after the
   attempt began, which was not in the pre-launch process snapshot, and whose
   normalized game arguments match the request.

If there are zero candidates at the grace deadline, fail
`steam_relaunch_timeout`. If there are multiple or mismatched candidates, fail
`ambiguous_relaunch`; do not pick the first process and do not kill ambiguous
processes. A nonzero foreground exit is `process_exited`, not a bounce.

`steam_appid.txt` presence must only be recorded. When it prevents the bounce,
the direct PID continues normally. When absent, ignored, or removed by Steam
verification, the same tracker handles the bounce. This is how the code
tolerates both worlds without making file presence a correctness assumption.

Allow only one bounded grace extension: after a confirmed bootstrap exit, add
at most 20 seconds to the socket deadline. This preserves the configured socket
health budget for the adopted process without permitting an unbounded relaunch
loop.

Change `wait_for_socket()` at
[`launch.py:500-515`](../../tools/bg3se_harness/launch.py#L500-L515) to accept a
tracker rather than a fixed `Popen`. All PID-targeted menu/window actions must
read `tracker.current_pid` at call time; the current static assignment at
[`launch.py:570`](../../tools/bg3se_harness/launch.py#L570) becomes stale after
adoption.

Make the return-contract change explicit: `launch()` should return the
`LaunchSession` facade, with `.pid` resolving to the current PID and
`.direct_process` retaining the original `Popen` for foreground exit status.
Update `_launch_until_socket()` at
[`cli.py:128-154`](../../tools/bg3se_harness/cli.py#L128-L154) to return that
session object and serialize `session.pid` only after socket readiness. Merely
mutating health JSON while returning the original `Popen` would leave
`cmd_test()` reporting and tracking the dead direct PID.

On macOS, also verify the Unix socket peer PID before accepting the response at
[`launch.py:637-679`](../../tools/bg3se_harness/launch.py#L637-L679), using
`LOCAL_PEERPID`/`getsockopt` or an equivalent local peer credential API. The
peer must equal `tracker.current_pid`. A response from another process is
`socket_peer_mismatch`, not success.

Finally, normal `launch()` should not call `kill_existing(force_all=True)` as it
does at [`launch.py:386-389`](../../tools/bg3se_harness/launch.py#L386-L389).
Fail if an unowned BG3 instance exists; kill only the exact recorded identity.
Keep blanket termination behind the already-explicit `quit --force` workflow.

### Component: detached monitor

- **Purpose:** Apply the same identity and cleanup rules to background launch.
- **Technology:** Replace `_FakeProcess` with the shared `ProcessTracker`.
- **Dependencies:** The atomic launch record and PID-targeted window helpers.

At [`_monitor.py:65-96`](../../tools/bg3se_harness/_monitor.py#L65-L96), pass a
`launch_id`/record path rather than treating the numeric PID argument as the
whole attempt. For compatibility during migration, accept the old PID argument
but immediately resolve it into a schema-v2 record.

The fixed five-second off-screen move at
[`_monitor.py:121-129`](../../tools/bg3se_harness/_monitor.py#L121-L129) is
unsafe: the direct PID has already exited by then, while the Steam PID may not
yet exist. Replace it with a retry tied to the current identity:

1. Try `move_window_offscreen(current_pid)` only while that identity is alive.
2. On PID adoption, reset `window_contained=false`.
3. Retry against the adopted PID until the window exists and containment
   succeeds.
4. Use `System Events` lookup by Unix PID, not application name, so a duplicate
   process cannot be hidden accidentally.

Do not restore any transient graphics settings when the direct PID disappears.
Restore exactly once, and only after one of these terminal conditions:

- the current/adopted PID's window is off-screen or hidden and the socket peer
  has been verified;
- the relaunch grace expires with no candidate;
- an ambiguous/mismatched candidate makes the attempt terminal;
- another terminal boot failure has been reached and no owned successor can
  still appear.

Write health JSON after every important transition, not only at monitor start
and completion. Add `launch_id`, `phase`, `pid`, `lineage`, `steam`,
`window_contained`, `graphics_transaction_open`, `memory_preflight`, and
`failure_code`.

### Component: Steam readiness preflight

- **Purpose:** Refuse a predictably short-lived Steam-less run before changing
  sockets/settings or starting BG3.
- **Technology:** `steam_readiness()` in `launch.py`, reused by `doctor.py`,
  CLI launch/test preflight, and `session_driver.sh` diagnostics.
- **Dependencies:** macOS `launchctl` plus an exact-process fallback.

Steam readiness should have four states:

- `ready`: `steam_osx` is running and the per-user launchd domain advertises
  `com.valvesoftware.steam.ipctool`.
- `starting`: the Steam application process exists, but the IPC service is not
  registered yet.
- `absent`: neither is present.
- `unknown`: inspection failed.

Require `ready`. `starting` should ask the caller to wait; `absent` should say
to open Steam; `unknown` should fail closed for an automated launch. Do not
automatically open Steam.

Run this check twice: once in `cli.py` before build/deploy/patch so failure is
fast, and again at the top of `launch()` before
[`launch.py:388-394`](../../tools/bg3se_harness/launch.py#L388-L394) to close
the time-of-check/time-of-use gap. Neither failure path may kill BG3, remove the
socket, write defaults, or prepare graphics.

Required user-facing failure:

```text
BG3 launch refused: Steam IPC is not ready. Direct Steam-less BG3 sessions
cleanly self-exit after roughly 90–151 seconds. Open Steam, wait until the
Library is ready, then rerun the harness.
```

Implementation caution: `launchctl print gui/$UID` can include a launched
application's inherited environment. Never log, persist, or return its raw
stdout/stderr. Reduce it immediately to booleans/status strings.

### Component: truthful macOS window-mode invariant

- **Purpose:** Prevent a headless request from taking over the user's screen
  while the harness lacks a proven LSF writer.
- **Technology:** Read-only profile validation plus explicit user attestation.
- **Dependencies:** `hashlib.sha256`, `config.lsf`, doctor metadata under
  `HARNESS_CONFIG_DIR`.

Adopt the invariant path now rather than rushing an LSF patcher.

1. Add `PLAYER_CONFIG_PATH` beside the profile paths at
   [`config.py:23-27`](../../tools/bg3se_harness/config.py#L23-L27):
   `LARIAN_LOCAL / "PlayerProfiles/Public/config.lsf"`.
2. Remove `Fullscreen`, `FakeFullscreenEnabled`, and `FakeFullscreen` from
   `HEADLESS_GRAPHICS_ENTRIES` at
   [`launch.py:22-29`](../../tools/bg3se_harness/launch.py#L22-L29). Rename the
   constant to make the remaining role honest, for example
   `HEADLESS_TRANSIENT_GRAPHICS_ENTRIES = {"ScreenWidth": 1280,
   "ScreenHeight": 720}`.
3. Add doctor checks for:
   - `config.lsf` exists;
   - header magic is `LSOF`;
   - version is the supported observed version (`7`);
   - a windowed-profile attestation exists;
   - the current file SHA-256 matches that attestation.
4. Add an explicit, non-mutating command such as:

   ```bash
   PYTHONPATH=tools python3 -m bg3se_harness doctor --record-windowed-profile
   ```

   It must refuse while BG3 is running, explain that it cannot semantically
   inspect display mode, and record path, SHA-256, file size, LSOF version, and
   timestamp only after the user confirms that they set **Video > Display Mode
   > Windowed** in-game and quit normally.
5. A headless launch with a missing or mismatched attestation is a fatal
   `window_mode_unverified` preflight failure. A non-headless launch reports a
   warning but is allowed.

Required failure:

```text
Headless launch refused: macOS display mode is stored in
PlayerProfiles/Public/config.lsf, and this profile is not verified as Windowed
or has changed since verification. Start BG3 through Steam, select
Video > Display Mode > Windowed, quit BG3, then run:
PYTHONPATH=tools python3 -m bg3se_harness doctor --record-windowed-profile
```

The documentation must call this an **attestation**, not display-mode
detection. A whole-file hash is intentionally conservative: unrelated profile
changes can cause a safe false negative and require re-attestation.

Update the incorrect operational claim at
[`docs/harness.md:228`](../harness.md#L228). The docs should say that
`graphicSettings.lsx` supplies only the temporary 1280×720 size on this macOS
build; `config.lsf` supplies the pre-existing Windowed mode invariant.

#### Deferred LSF utility

A future `tools/bg3se_harness/lsf.py` is reasonable, but it is a separate
feature:

1. Port or wrap the LSF v7 reader/writer semantics from Norbyte/lslib; do not
   extend the LSPK reader and assume the formats are related.
2. Build a read-only converter first and compare manually captured Windowed,
   Fullscreen, and Fake Fullscreen copies to identify the exact node, attribute
   type, and enum.
3. Add golden fixtures for string tables, nodes, attributes, compression, and
   parse/write/parse semantic equivalence.
4. Patch only a copy until round-trip tests pass. Production write must use a
   content-addressed backup, a same-directory temporary file, post-write parse
   verification, `fsync`, and atomic replace.
5. Retain the attestation fallback until the utility has live-validated all
   three display modes and can restore byte-semantic equivalence.

The current 1,709-byte LSOF file and raw ASCII scans are not sufficient evidence
to implement a safe in-place writer.

### Component: memory-pressure doctor and launch preflight

- **Purpose:** Avoid launching into a known jetsam storm and preserve the
  diagnostic ecosystem.
- **Technology:** Read-only `/usr/bin/memory_pressure -Q`, with a short timeout.
- **Dependencies:** No third-party packages.

Parse `System-wide memory free percentage: N%` and classify:

```yaml
memory_pressure:
  command_timeout_s: 3
  warn_below_percent: 20
  block_below_percent: 10
  unavailable_policy: "warn"          # do not pretend an unknown metric is OK
  override_flag: "--allow-memory-pressure"
```

- `>=20%`: pass.
- `10–19%`: warning; proceed, but include the metric in health/diagnosis.
- `<10%`: block automated launch with `memory_pressure_critical`, unless the
  caller explicitly supplies `--allow-memory-pressure`.
- Command missing, timeout, or unparseable output: warning/unknown, not a false
  pass.

These are initial operating thresholds, not universal macOS truths. Record the
value next to successful and failed runs, then calibrate after at least ten BG3
boots. `doctor --verbose` may also count recent jetsam events with bounded
`/usr/bin/log show`, but that count should be diagnostic only; parsing unified
logs is too version-sensitive to gate launch.

Extend `_check()` at
[`doctor.py:29-36`](../../tools/bg3se_harness/doctor.py#L29-L36) with
`severity` (`critical`, `warning`, `info`) and `code`. The current
`all_passed = passed == total` summary at
[`doctor.py:224-233`](../../tools/bg3se_harness/doctor.py#L224-L233) should
become `launch_blocked = any(failed critical check)` so optional checks do not
mask the distinction.

### Component: session driver

- **Purpose:** Drive and soak the exact harness-owned attempt after any Steam
  handoff.
- **Technology:** Parse launch JSON/attempt record; retain shell only for
  orchestration and diagnostics.
- **Dependencies:** Python stdlib JSON parsing already used by the script.

At [`session_driver.sh:129-151`](../../scripts/session_driver.sh#L129-L151):

1. Capture stdout JSON separately from stderr; do not merge both into
   `/tmp/session_driver_launch.json`.
2. Check the launch command's exit status. The current script ignores it.
3. Read `launch_id`, current `pid`, identity start time, and lineage from the
   successful result/attempt record.
4. Remove the generic 30-second `pgrep` loop. Foreground launch must return only
   after the direct process is stable or the Steam successor has been adopted.
5. In attach mode, resolve all exact-path BG3 candidates. Require exactly one;
   fail `AMBIGUOUS_ATTACH` rather than choosing the first.

During the progress loop at
[`session_driver.sh:153-199`](../../scripts/session_driver.sh#L153-L199), verify
PID plus start time/executable on each poll. Once launch phase has returned, a
different PID is an unexpected replacement and remains exit 3; the shell
driver must not implement a second adoption policy.

Recommended verdicts:

| Exit | Verdict | Meaning |
|---:|---|---|
| 0 | `SESSION_RUNNING` | Exact adopted/current identity reached `PrepareRunning`/`Running`. |
| 1 | `GAME_DIED` | Owned identity exited. |
| 2 | `STUCK` | Bounded state/recovery budget exhausted. |
| 3 | `REPLACED` | Post-adoption identity changed unexpectedly. |
| 4 | `PREFLIGHT_FAILED` | Steam, window attestation, or critical memory check failed. |
| 5 | `AMBIGUOUS_ATTACH` | Attach mode found zero/multiple eligible processes. |

Every diagnosis should include `launch_id`, identity/lineage, current Steam
readiness, memory percentage, and window-attestation status. If an owned process
later exits without a crash report, call `/usr/bin/log` explicitly and
distinguish `STEAM_LOST` when Steam IPC is no longer ready; never invoke the zsh
`log` builtin accidentally.

The soak loop at
[`session_driver.sh:205-218`](../../scripts/session_driver.sh#L205-L218) must
check the full identity, not only `kill -0 PID`.

### Configuration

Centralize policy constants in `launch.py` initially; move them to
`config.py` only if they become user-configurable:

```yaml
steam:
  require_ipc: true
  initial_process_max_lifetime_s: 5
  relaunch_grace_s: 20
  max_adoptions_per_launch: 1
process_identity:
  require_exact_executable: true
  require_start_time: true
  require_matching_args: true
socket:
  require_peer_pid: true
headless:
  require_windowed_profile_attestation: true
  transient_width: 1280
  transient_height: 720
memory:
  warn_below_percent: 20
  block_below_percent: 10
```

### Offline tests and acceptance criteria

Add `tests/harness/test_launch_lifecycle.py` and
`tests/harness/test_monitor.py`; update `test_cli.py`,
`test_headless_graphics.py`, and doctor tests.

Required deterministic cases:

1. Direct PID remains alive: no adoption, socket peer matches, success.
2. Direct PID exits 0 at 1.3 seconds; one same-args Steam PID appears at 10
   seconds: adopt it, update the record, and do not restore graphics between
   PIDs.
3. The same bounce occurs whether `steam_appid.txt` is reported present or
   absent.
4. Direct PID exits nonzero: no adoption.
5. Relaunch candidate has mismatched args or executable: reject it.
6. Two candidates appear: fail `ambiguous_relaunch`; never choose `head -1`.
7. No candidate appears in 20 seconds: fail `steam_relaunch_timeout` and
   restore exactly once.
8. Socket response comes from a non-current PID: fail `socket_peer_mismatch`.
9. Steam absent/starting: no `Popen`, socket removal, defaults write, graphics
   mutation, build/deploy, or patch.
10. Window attestation missing/hash mismatch: headless launch does not mutate
    settings or start BG3.
11. `graphicSettings.lsx` keeps only width/height transient mutations and never
    receives the three display-mode keys.
12. Memory `9%`, `10%`, `19%`, `20%`, unavailable, and explicit override
    follow the policy above.
13. A stale monitor cannot overwrite or clear a newer `launch_id`.
14. Driver launch failure becomes `PREFLIGHT_FAILED`; attach ambiguity becomes
    `AMBIGUOUS_ATTACH`.

All process, clock, Steam, socket-peer, filesystem, and memory-pressure inputs
must be injected/faked. These tests must not enumerate or signal live BG3/Steam
processes.

## Deployment Plan

1. **Build the shared lifecycle primitives offline.** Add process identity,
   atomic attempt records, Steam readiness, memory parsing, and unit tests
   without changing launch behavior.
2. **Make foreground launch adopt the Steam successor.** Update
   `wait_for_socket()` and CLI result identity. Disable automatic boot retry
   while a bounce grace is open.
3. **Move the detached monitor to the same tracker.** Make off-screen/hide
   operations PID-specific and add transition-by-transition health writes.
4. **Harden cleanup.** Prove with tests that restore occurs once after
   containment or a terminal grace outcome, never at the bootstrap exit.
5. **Correct the macOS window contract.** Remove fake display-mode XML keys,
   add config attestation/doctor checks, update documentation, and require the
   attestation for headless mode.
6. **Wire preflight before build and before `Popen`.** Steam absence must be
   side-effect free; critical memory pressure must be explicit and
   overridable.
7. **Simplify `session_driver.sh`.** Consume the launch record and remove
   independent `pgrep` ownership logic.
8. **Run offline test gates.**

   ```bash
   PYTHONPATH=tools python3 -m pytest tests/harness/test_launch_lifecycle.py -q
   PYTHONPATH=tools python3 -m pytest tests/harness/test_monitor.py -q
   PYTHONPATH=tools python3 -m pytest tests/harness/test_headless_graphics.py -q
   PYTHONPATH=tools python3 -m pytest tests/harness/test_cli.py -q
   PYTHONPATH=tools python3 -m pytest tests/harness -q
   bash -n scripts/session_driver.sh
   ```

9. **Record a Windowed attestation manually before live validation.** This is
   an explicit operator action; the harness must not edit `config.lsf`.
10. **Perform later live validation as a separate approved activity.** Test
    once with `steam_appid.txt` present and once in a controlled copy/world
    where the bounce occurs. Confirm adopted PID, socket peer, no fullscreen
    Space, settings restoration, and session survival beyond 180 seconds.

Rollback is code-only: revert the lifecycle commits and remove the harness
attestation metadata. The attestation records a hash and never mutates
`config.lsf`. Do not remove or rewrite `steam_appid.txt` as part of this
rollout or rollback.

## Monitoring & Alerting

Emit one JSON event per transition with:

- `launch_id`, monotonic offset, phase, current PID and process start time;
- direct/adopted lineage and known exit code;
- Steam readiness and `appid_file_present`;
- bounce wait duration and candidate rejection reasons;
- window containment PID and result;
- socket peer PID and match result;
- graphics transaction open/restored state;
- memory free percentage/classification;
- terminal `failure_code`.

Suggested counters and thresholds:

- `steam_bounce_adopted`: informational; expected only without effective appid
  bypass.
- `steam_relaunch_timeout`: alert on any occurrence.
- `ambiguous_relaunch` or `socket_peer_mismatch`: alert on any occurrence.
- `window_mode_unverified`: operator-action warning, not a code crash.
- `memory_pressure_critical`: alert when a launch is blocked; correlate with
  recent jetsam counts.
- `graphics_restore_failed`: high priority because the user's profile state may
  remain transient.
- `unexpected_replacement`: high priority after socket/session readiness.

Never include raw process environments, full `launchctl` output, or unrelated
command lines in health artifacts.

## Runbook

### Steam is absent or starting

1. Read the structured `steam.status`.
2. Open Steam manually if absent; wait for the Library and IPC readiness.
3. Rerun doctor/preflight.
4. Do not bypass the check merely because `steam_appid.txt` exists.

### Monitor reports `waiting_for_steam_relaunch`

1. Do not restore graphics, launch a retry, or run `quit --force`.
2. Wait for the bounded 20-second grace.
3. If one exact candidate appears, confirm the health record changes PID and
   appends lineage.
4. If it times out or becomes ambiguous, preserve health/monitor logs and end
   the attempt.

### Headless launch is refused

1. Launch BG3 normally through Steam.
2. Set **Video > Display Mode > Windowed**.
3. Quit BG3 normally.
4. Run `doctor --record-windowed-profile`.
5. Rerun doctor, then the headless launch.
6. If the hash later changes, repeat these steps; do not hand-edit
   `config.lsf`.

### Critical memory pressure

1. Stop nonessential high-memory workloads and allow macOS pressure to recover.
2. Rerun doctor until the value is at least 10%; preferably at least 20%.
3. Use `--allow-memory-pressure` only for an intentional diagnostic run, and
   retain the warning in its report.
4. Recheck Steam readiness because jetsam may have killed Steam while its UI
   still appeared recently active.

### Unexpected process exit

1. Check whether the attempt had already adopted a Steam successor.
2. Check current Steam readiness.
3. Search for a new `.ips` report created after the recorded process start.
4. Query unified logs with `/usr/bin/log`, looking for `proc_exit`, Steam IPC
   lookup failures, and jetsam events.
5. Classify Steam loss or memory pressure before calling the exit a BG3SE
   crash.

## Deployment Checklist

- [ ] Infrastructure code reviewed
- [ ] Offline lifecycle fixtures pass
- [ ] Windowed-profile attestation recorded
- [ ] Steam-present stable-direct path tested
- [ ] Steam-bounce adoption path tested
- [ ] Monitoring events and failure codes verified
- [ ] Graphics restore is exactly-once across every terminal path
- [ ] Rollback procedure documented

## Monitoring Checklist

- [ ] Launch transition rate/errors/duration recorded
- [ ] Steam readiness and bounce duration recorded
- [ ] Current/adopted PID identity recorded without environment leakage
- [ ] Socket peer PID verified
- [ ] Memory-pressure percentage and class recorded
- [ ] Graphics transaction and restore status recorded
- [ ] Alerts configured for ambiguous identity, peer mismatch, timeout, and restore failure

## Incident Response Checklist

- [ ] Impact assessed: fullscreen takeover, lost session, or diagnostic loss
- [ ] Exact `launch_id` and PID lineage identified
- [ ] Steam and memory state checked before crash attribution
- [ ] Profile restore/attestation state checked
- [ ] Mitigation identified without editing `config.lsf`
- [ ] Root cause and follow-up test recorded
