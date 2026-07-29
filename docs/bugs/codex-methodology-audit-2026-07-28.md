# Adversarial Methodology Audit — 2026-07-28 BG3SE Crash Bisection

Date: 2026-07-28
Scope: `wave-campaign-progress.md`, `evidence-2026-07-28/`, `se_bisect2.sh`,
`se_bisect3.sh`, `tools/bg3se_harness/launch.py`, `cli.py`, the probe task
outputs, and the still-present 2026-07-28 DiagnosticReports/game logs.
Safety: no game process was launched for this audit.

## Executive verdict

The campaign established a strong and repeatable association between the
**SE-present launch condition** and an identical hotbar crash. It did **not**
establish that the SE dylib itself, all Dobby hooks, or the focus hack are the
cause.

| Claim | Verdict | Why |
|---|---|---|
| “SE causes the hotbar crash” | **Supported only at the condition level; the claimed clean A/B is refuted** | The vanilla observation and SE crash are real, but the arms differed in launch path, automation, focus/window handling, process history, graphics mutation, and active SE-only code. In particular, the SE arm loaded and ran the loose `EntityTest` SE mod. |
| “Dobby hooks exonerated” | **Refuted** | PID 36512 did run with `BG3SE_NO_HOOKS=1` and did crash after reaching `PrepareRunning`, but that same log explicitly records a StaticData **Dobby hook successfully installed** and a VideoSkip hook installed. The arm did not remove all Dobby/code hooks. |
| “focus_hack exonerated as sole cause” | **Refuted** | IPS `190312` is PID 36512; PID 36512 logged `Forced focus: 0 -> 1`. The 20×-NULL/no-write log is replacement PID 37698, which started after PID 36512 crashed. |
| “Probe B (`BG3SE_MINIMAL`) inconclusive” | **Confirmed, but for more serious reasons than logged** | The printed result is internally contradictory (`ALIVE`, `CRASH`, and `SESSION LOADED: no`). The target PID actually reached `PrepareRunning` later and crashed with the same signature. `MINIMAL` was echoed by the driver but never confirmed in the target log, and the implementation gates only deferred session init, not the extensive constructor-time init shown in the log. |

The current evidence is sufficient to prioritize a new controlled experiment.
It is not sufficient to eliminate Dobby or focus writes from the suspect set.

## Timestamp conventions

SE logs and IPS metadata use local EDT (`-0400`). BG3 `network.*.log` filenames
also use local time, but the timestamps inside those logs are four hours ahead
(UTC). Thus `network...T19-02-36...` contains `23:02:36`–`23:03:05` entries.
All correlations below normalize that difference.

IPS filenames/header timestamps lag the actual crash capture. The authoritative
fields are the JSON body’s `pid`, `procLaunch`, and `captureTime`, all on line 2
of each `.ips`.

## Claim 1: “SE causes the hotbar crash”

### What is solid

The SE crash is well identified:

- [`bg3se_2026-07-28_18-56-21.log:12`](evidence-2026-07-28/bg3se_2026-07-28_18-56-21.log#L12)
  identifies target PID **19766**.
- [`Baldur's Gate 3-2026-07-28-185709.ips:2`](evidence-2026-07-28/Baldur's%20Gate%203-2026-07-28-185709.ips#L2)
  identifies PID **19766**, launch `18:56:18.4273`, capture
  `18:57:04.6442`, `EXC_BAD_ACCESS`, address `0x10`, and
  `gui::HotbarSystem::Update` at image offset `40039100`.
- [`network.2026-07-28T18-56-22-257991.log:34`](evidence-2026-07-28/network.2026-07-28T18-56-22-257991.log#L34)
  names the save:
  `Tamarru-62412511136__Ebonlake Grotto - 27h 19m`.
- The same network log records the actual client transition
  `StopLoading -> PrepareRunning` at
  [`line 148`](evidence-2026-07-28/network.2026-07-28T18-56-22-257991.log#L148).
- The SE log records the configured eight modules at
  [`lines 15–24`](evidence-2026-07-28/bg3se_2026-07-28_18-56-21.log#L15).

The vanilla survival observation also exists in the preserved probe output:

- `.../tasks/bx5qprfu5.output:1` says `start-game click: True`.
- `.../tasks/bx5qprfu5.output:2` says `STILL ALIVE 240s post-start`.
- Its filesystem interval is `18:28:30`–`18:32:31`.

This is meaningful evidence that the save can render and remain playable in a
vanilla-present condition.

### Why this was not a clean A/B

The claims log’s assertion “only variable = the SE dylib” is not supported by
the preserved artifacts.

1. **Different launch and input paths.** The vanilla process was manually
   activated and driven through the splash, menu, verification checkboxes, and
   Start Game. The patched process used harness `launch --continue` and
   SE-provided automation. The vanilla output itself records a manual Start
   Game click. There is no identical input trace applied to both arms.

2. **Different window/focus conditions.** The claims log explicitly says the
   vanilla clicks worked only with a visible, activated window
   (`wave-campaign-progress.md:38–41`). The harness injects
   `BG3SE_AUTO_DISMISS_SPLASH=1`
   ([`launch.py:417–418`](../../tools/bg3se_harness/launch.py#L417)) and that
   path starts `focus_hack`; PID 19766 wrote its focus byte at
   [`bg3se...18-56-21.log:5452`](evidence-2026-07-28/bg3se_2026-07-28_18-56-21.log#L5452).
   No equivalent focus write exists in vanilla.

3. **Graphics were not held byte-identical.** Every default harness launch
   calls `ensure_skip_videos()`
   ([`launch.py:401–402`](../../tools/bg3se_harness/launch.py#L401)), which
   writes `SkipVideo` and `SkipSplashScreen` into `graphicSettings.lsx`
   ([`launch.py:102–117`](../../tools/bg3se_harness/launch.py#L102)).
   No pre/post hashes or snapshots for either A/B arm were preserved. The claim
   “same graphicSettings” therefore cannot be verified.

4. **The modsettings equality is asserted, not demonstrated per arm.** The SE
   log proves that PID 19766 read eight configured modules. Vanilla PID 15736
   launched from the earlier 11-entry state; its verification workflow later
   rewrote the file to eight entries before the save loaded. Thus the processes
   did not even start from the same configuration history. There is also no
   timestamped vanilla-side copy/hash taken immediately before launch and again
   immediately before save load. This omission is material.

5. **“Eight configured mods” was not the complete active-code condition.**
   PID 19766 also detected a loose SE mod:
   [`bg3se...18-56-21.log:29–30`](evidence-2026-07-28/bg3se_2026-07-28_18-56-21.log#L29)
   says `[SE] EntityTest (from Mods folder)`. It then loaded
   `EntityTest/BootstrapServer.lua`
   ([`lines 5470–5490`](evidence-2026-07-28/bg3se_2026-07-28_18-56-21.log#L5470)),
   and that mod’s `SessionLoaded` callback ran immediately before the crash
   ([`lines 14859–14862`](evidence-2026-07-28/bg3se_2026-07-28_18-56-21.log#L14859)).
   Vanilla could not execute that SE bootstrap. The contrast was therefore
   “vanilla” versus “SE core + loose EntityTest + SE automation,” not simply
   “dylib absent” versus “dylib present.”

6. **Different process histories.** The preserved Claude session transcript
   shows a direct vanilla launch at 18:06:52 and all subsequent watchers target
   the same PID, **15736**. That one process remained alive through manual
   splash/menu/dialog experiments, loaded only after the Start Game click at
   18:27:51, survived the 240-second observation, and was not force-quit until
   18:45:16. The SE arm was a fresh launch at 18:56:18. No cold/warm-start
   control, cache reset, or randomized order was used.

7. **The save identity is not independently proved in vanilla.** The SE game
   log names the save. The vanilla task output contains only the click and
   survival verdict, not the save UUID/name, PID, or loaded-session evidence.

8. **The reported “~35s” is observer-relative, not launch-relative.** The
   preserved task output (`bs12rdljp.output:1`) says `EXITED after ~35s`, but
   IPS metadata gives launch `18:56:18.4273` and capture `18:57:04.6442`:
   **46.217 seconds**. The 35-second clock began after the watcher did, not at
   process launch or session entry.

### Claim-1 disposition

Use the bounded statement:

> With the preserved save/configuration, the vanilla/manual condition survived
> 240 seconds, while the SE-present automated condition (including the loose
> EntityTest SE bootstrap) crashed in HotbarSystem shortly after session entry.

Do not yet use:

> The SE dylib itself causes the crash, with no other variable changed.

The decisive re-test is a randomized ABBA series using a cloned read-only save,
byte-hashed settings, the same state-aware input trace, identical visibility
and focus, and an empty `ScriptExtender/Lua`/loose-SE-mod surface. Run at least
three replicates per condition.

## Claim 2: “Dobby hooks exonerated”

### IPS ↔ SE log pairing

This pairing is exact:

- [`Baldur's Gate 3-2026-07-28-190312.ips:2`](evidence-2026-07-28/Baldur's%20Gate%203-2026-07-28-190312.ips#L2)
  says PID **36512**, `procLaunch=19:02:36.2770`,
  `captureTime=19:03:08.7666`, `EXC_BAD_ACCESS` at `0x10`, top frame
  `HotbarSystem::Update`, offset `40039100`.
- [`bg3se_2026-07-28_19-02-36.log:12`](evidence-2026-07-28/bg3se_2026-07-28_19-02-36.log#L12)
  says PID **36512**.
- [`line 170`](evidence-2026-07-28/bg3se_2026-07-28_19-02-36.log#L170)
  confirms `BG3SE_NO_HOOKS=1` was observed by that process.

The pairing is with `19-02-36.log`, **not** `19-03-08.log`.

### Network-log pairing

The network evidence is high-confidence but not cryptographically PID-bound:

- The filename `network...T19-02-36-951387.log` begins in the same local second
  as PID 36512’s launch.
- Its content is a single continuous state-machine history from
  initialization through save load.
- It names the same save at
  [`line 34`](evidence-2026-07-28/network.2026-07-28T19-02-36-951387.log#L34).
- It reaches the actual client transition
  `StopLoading -> PrepareRunning` at
  [`line 148`](evidence-2026-07-28/network.2026-07-28T19-02-36-951387.log#L148),
  3–4 seconds before the IPS capture.
- Replacement PID 37698 did not start until `19:03:08.952`, after PID 36512’s
  `captureTime=19:03:08.7666`.

No line in `network.*.log` records a PID or run UUID. Thus the filename/start
continuity and non-overlap establish a strong pairing, but the format itself
cannot prove PID identity. Future logs must include the target PID/run ID.

### Why “all Dobby hooks” is false

The target log contradicts its own `ALL Dobby hooks SKIPPED` wording:

- [`bg3se...19-02-36.log:14`](evidence-2026-07-28/bg3se_2026-07-28_19-02-36.log#L14):
  `Dobby inline hooking: enabled`.
- [`line 170`](evidence-2026-07-28/bg3se_2026-07-28_19-02-36.log#L170):
  the `NO_HOOKS` diagnostic message.
- [`lines 5367–5368`](evidence-2026-07-28/bg3se_2026-07-28_19-02-36.log#L5367):
  `installing standard Dobby hook` followed by
  `Dobby hook installed successfully!` for StaticData.
- [`line 5407`](evidence-2026-07-28/bg3se_2026-07-28_19-02-36.log#L5407):
  `VideoSkip Hook installed`.

Therefore the arm exonerates, at most, the specific hook group guarded by
`BG3SE_NO_HOOKS` (principally the four Osiris interception hooks). It does not
exonerate StaticData Dobby patching, VideoSkip patching, or any other hook
installed outside that guard.

### Claim-2 disposition

Refute “Dobby hooks exonerated.” Replace it with:

> The four main Osiris hooks are not necessary for reproduction. The
> `NO_HOOKS` arm still installed at least one explicitly logged Dobby hook, so
> Dobby/code-patching as a class remains untested.

Before repeating this arm, make `NO_HOOKS` fail closed around **every** call to
`DobbyHook`/code-patching helper and print a startup manifest with each hook
name and installed/skipped status.

## Claim 3: “focus_hack exonerated as sole cause”

### Wrong-PID attribution

The claim used the wrong SE log:

- Crash IPS `190312` is PID **36512**.
- PID 36512 is `bg3se_...19-02-36.log`, and that log says:
  - initial NULL at
    [`line 120`](evidence-2026-07-28/bg3se_2026-07-28_19-02-36.log#L120);
  - `Forced focus: 0 -> 1` at
    [`line 5437`](evidence-2026-07-28/bg3se_2026-07-28_19-02-36.log#L5437);
  - deferred success at
    [`line 5438`](evidence-2026-07-28/bg3se_2026-07-28_19-02-36.log#L5438).
- `bg3se_...19-03-08.log` is PID **37698**
  ([`line 12`](evidence-2026-07-28/bg3se_2026-07-28_19-03-08.log#L12)).
  Its first SE timestamp, `19:03:08.952`, is 186 ms **after** the crashed
  PID’s IPS capture time, `19:03:08.7666`.

PID 37698 is a replacement process. Its 20 NULL observations and lack of
`Forced focus` cannot describe PID 36512’s pre-crash memory writes.

### Did PID 37698 ever write without logging?

During its observed lifetime, no. The log contains one initial NULL plus 19
deferred NULLs (20 total), no `Forced focus`, and an orderly shutdown beginning
at [`line 5465`](evidence-2026-07-28/bg3se_2026-07-28_19-03-08.log#L5465).

The implementation writes the byte and immediately emits `Forced focus`
(`src/game/focus_hack.c:63–69`). The session logger is explicitly line-buffered
(`src/core/logging.c:308–312`), so a completed call should normally appear.
There is only a tiny theoretical store-before-log crash window.

However, the replacement was force-terminated after roughly 10.8 seconds,
before the configured 30 attempts at 500 ms intervals were exhausted
(`src/game/focus_hack.c:80–115`). It could have written later had it remained
alive and had `BaseApp::s_AppInstance` appeared. More importantly, it is not
the crashing process.

### Claim-3 disposition

Refute the exoneration. The only correctly paired `NO_HOOKS` crash arm includes
a confirmed focus write. A decisive focus arm needs a dedicated
`BG3SE_NO_FOCUS_HACK=1` checked and logged before any scheduling or memory
access, while all other variables remain identical.

## Claim 4: Probe B (`BG3SE_MINIMAL`) is inconclusive

### The printed result is self-contradictory

Preserved output
`.../tasks/bwttxutav.output:1–7` says:

```text
=== B: MINIMAL (no subsystem init) === env: BG3SE_MINIMAL=1
"socket_connected": true
SESSION LOADED: no
RESULT: ALIVE at 150s
CRASH: EXC_BAD_ACCESS ... 0x10
TOP: gui::HotbarSystem::Update(...)
INCONCLUSIVE: never reached a running session
```

The script permits all four verdicts simultaneously because it never
reconciles its independent `SESSION`, `pgrep`, and `find` checks.

### The target actually reached a session and crashed

The evidence bundle was copied while this arm was still running. Its copy of
`network...T19-15-52...log` is only 19 lines/3562 bytes and stops at Menu.
The original repository-root log later reached 154 lines/22573 bytes:

- `network.2026-07-28T19-15-52-672276.log:34`:
  the same Tamarru save load request at 19:17:27 local-equivalent;
- `:132`: `GameStateQueued: PrepareRunning`;
- `:154`: actual `CLIENT STATE SWAP - from: StopLoading, to: PrepareRunning`
  at 19:17:44 local-equivalent.

The fourth crash report, omitted from the bundle because it was created later,
is:

`~/Library/Logs/DiagnosticReports/Baldur's Gate 3-2026-07-28-191750.ips:2`

Its authoritative fields are:

- PID **57155**;
- `procLaunch=2026-07-28 19:15:52.1575 -0400`;
- `captureTime=2026-07-28 19:17:46.4088 -0400`;
- `EXC_BAD_ACCESS`, `KERN_INVALID_ADDRESS at 0x10`;
- top frame `gui::HotbarSystem::Update`, image offset `40039100`.

[`bg3se_2026-07-28_19-15-52.log:12`](evidence-2026-07-28/bg3se_2026-07-28_19-15-52.log#L12)
also identifies PID 57155. This is an exact run/PID/signature match.

The reason the script printed `SESSION LOADED: no` is temporal: its 16-iteration
session-driving loop ended first. The save did not begin loading until later,
during the nominal survival-watch loop. `SESSION` was never updated after line
39 of `se_bisect3.sh`.

### `ALIVE at 150s` is invalid, but current harness code is not the relauncher

`se_bisect3.sh:43–47` asks only whether **any** command line matching
`Baldur's Gate 3` exists. It does not retain PID 57155. Thus a replacement,
CrashReporter-related process, or other matching neighbor can keep the result
“alive.”

There was in fact a replacement BG3 process:

- `~/Library/Application Support/BG3SE/logs/bg3se_2026-07-28_19-17-46.log:12`
  identifies PID **67632**;
- its first SE timestamp is `19:17:46.565`, 156 ms after PID 57155’s IPS
  capture time;
- it shut down at `19:17:52.007`.

But the current foreground harness path does **not** implement a post-crash
watchdog relaunch:

- `launch.py:386–444` performs one `Popen` and returns that process.
- `cli.py:128–175` retries only while booting.
- `launch.py:866–870` limits retryable stages to `timeout` and
  `menu_stalled`, not `process_exited`.
- `/tmp/se_bisect_launch.txt` records `boot_retries: 0` for PID 57155.
- `_monitor.py` is used only by `launch --background`, and it monitors one PID;
  it contains no relaunch call.

Therefore “the harness watchdog can relaunch after a crash” is not supported by
the inspected source for this foreground probe. The origin of the observed
replacement processes is not established by this bundle. The methodological
point still stands: global `pgrep` cannot distinguish the target from a
replacement or unrelated match.

### Was the Probe-B `CRASH` line stale?

The script’s mechanism is unsafe in general:

- `START_MARK` has only whole-second precision;
- it is set **before** the opening `quit --force`;
- `find -newermt` uses report-file mtime, not IPS `procLaunch`/`captureTime`;
- `head -1` is unsorted;
- no PID is checked.

So a previous-arm or neighbor IPS can be reported.

For this particular Probe B, however, the later-discovered `191750.ips` matches
target PID 57155 exactly and was created during the arm. The printed crash line
was substantively correct; the script simply failed to preserve or print the
path/PID that would have proved it.

### Was `BG3SE_MINIMAL` actually effective?

The driver output proves only that the shell exported the requested text.
`launch.py:414–429` copies the parent environment into the target, so
inheritance is expected, but there is no startup environment manifest in the
SE log.

No `BG3SE_MINIMAL=1` marker appears in PID 57155’s log. The implementation
checks and logs `BG3SE_MINIMAL` only inside
`deferred_session_init_tick()` (`src/injector/main.c:2313–2327`), rather than at
startup. PID 57155 crashed around session transition without ever emitting that
marker, so the target-side state is unverified.

More importantly, the flag does not skip “all subsystem init” at construction.
PID 57155’s log shows Lua, entity layouts, Stats, StaticData, input/CGEventTap,
focus hack, ImGui, and hooks initialized before any session. The `MINIMAL`
branch gates only the later deferred-session work. Thus even successful env
inheritance would not isolate constructor-time subsystem initialization—the
surface the claims log says Probe B was intended to split.

### Claim-4 disposition

Probe B is inconclusive as a `MINIMAL` experiment, but it is a genuine fourth
reproduction in PID 57155 after `PrepareRunning`. Preserve `191750.ips` and the
completed root network log in the evidence bundle, then rerun with a
startup-logged flag and a flag implementation that actually gates the intended
initialization surface.

## Reconstructed launch timeline

“Every launch” below means every launch for which the supplied/preserved logs,
probe outputs, or campaign session transcript contain affirmative evidence.
Vanilla did not produce an SE log, but the transcript preserves its direct
launch command and the PID used by every subsequent watcher.

| Local launch time | PID | Patched / requested toggles | Configured mods and extra SE code | Session reached? | Outcome |
|---|---:|---|---|---|---|
| 17:42:07.4438 | 78545 | Patched; normal hooks; campaign command `test --headless --continue --tier 1`; harness auto-dismiss/video-skip implied | 11 total / 10 user mods | Yes: root `network...T17-42-11...:148` reaches `PrepareRunning` | Crash captured 17:42:44.8093; IPS `174248`; hotbar offset 40039100, `0x10` |
| 17:42:44.753 SE start | 81026 | Patched replacement; normal hooks | 11 / 10 | No loaded-session evidence; hooks report 0 calls | Orderly SE shutdown 17:42:53.801 after menu-side Tier-1 work |
| ~18:06:52 | 15736 | **Unpatched vanilla**, launched directly with `-continueGame`; manual activation/keystrokes/Return/clicks; visible window | Began from the 11-entry configuration; verification workflow/dialog rewrote it to 8 entries | Yes, after manual Start Game click at 18:27:51 | Same PID survived the 240-second observation and remained until `quit --force` at 18:45:16 |
| 18:56:18.4273 | 19766 | Patched; normal hooks; `--continue`; auto-dismiss/video-skip; focus write | 8 / 7 configured; loose `EntityTest` SE bootstrap loaded and ran | Yes: evidence network log line 148 | Crash captured 18:57:04.6442; IPS `185709`; identical signature |
| 18:57:04.870 SE start | 26125 | Patched replacement; normal hooks | 8 / 7; loose `EntityTest` detected | No | Orderly shutdown 18:57:10.039 |
| 18:59:35.747 SE start | 33529 | Patched; `BG3SE_NO_HOOKS=1`; focus write; StaticData Dobby + VideoSkip hooks still installed | 8 / 7; loose `EntityTest` detected | No: network log stops at Menu | Probe printed `ALIVE at 120s`; process was terminated before next arm; no matching IPS |
| 19:02:36.2770 | 36512 | Patched; `BG3SE_NO_HOOKS=1`; **focus write occurred**; StaticData Dobby + VideoSkip hooks still installed | 8 / 7; loose `EntityTest` detected | Yes: evidence network line 148 | Crash captured 19:03:08.7666; IPS `190312`; identical signature |
| 19:03:08.952 SE start | 37698 | Patched replacement; inherited `NO_HOOKS`; no focus write during observed lifetime | 8 / 7; loose `EntityTest` detected | No | Orderly shutdown 19:03:19.747; this is the 20×-NULL log |
| 19:15:52.1575 | 57155 | Patched; driver requested `BG3SE_MINIMAL=1`; target-side marker absent; normal hooks/full constructor init; focus write | 8 / 7; loose `EntityTest` detected | **Yes**, but after script froze `SESSION=no`: completed root network line 154 | Crash captured 19:17:46.4088; IPS `191750` (not bundled); identical signature |
| 19:17:46.565 SE start | 67632 | Patched replacement; requested-toggle inheritance unknown; normal hooks/full init; no focus write before termination | 8 / 7; loose `EntityTest` detected | No | Orderly shutdown 19:17:52.007 |

Replacement starts occur after all four observed crashes (17:42, 18:57, 19:03,
19:17), but the inspected foreground harness code does not explain them. Their
origin must be instrumented rather than assumed.

## Methodological holes in `se_bisect3.sh` and concrete fixes

| Hole | Consequence | Concrete fix |
|---|---|---|
| No `set -euo pipefail` or argument validation | Failed launch/actions can degrade into a plausible verdict | Enable strict mode; require nonempty label and positive integer watch duration; trap and mark the arm invalid on any required-command failure |
| Environment is exported into the long-lived shell without clearing known toggles | `NO_HOOKS`, `MINIMAL`, `NO_NET`, or future toggles can leak between arms or from the caller | Start each target with an explicit allowlisted environment; unset every diagnostic toggle first; record the exact target env in a run manifest |
| Environment proof is only `echo "$*"` | Proves caller intent, not target behavior | Log all diagnostic env flags at the first instruction of SE initialization, before any gated work; require those target-side markers |
| `launch` rebuilds, deploys, and patches on every arm | Binary/deploy state can change between arms | Build/deploy once; hash the game executable and dylib; keep them immutable across the series; record hashes per arm |
| Harness launch mutates UserDefaults and `graphicSettings.lsx` | Graphics/video state differs or drifts across arms | Snapshot and hash the entire settings file and relevant defaults; apply one canonical profile to every arm; restore after each arm |
| No modsettings guard or hash | BG3 can rewrite/prune modules, as it already did in this campaign | Copy/hash `modsettings.lsx` before launch, verify after session and after exit, and restore from the same immutable snapshot before every arm |
| No complete mod/code inventory | Loose `EntityTest` ran only in SE-present arms while “same 8 mods” was claimed | Inventory PAKs, loose Mods directories, and every loaded SE bootstrap; run causal core tests with no third-party/loose SE Lua |
| No save fixture/hash or write protection | Autosave or metadata drift can make later arms non-equivalent | Clone one named fixture for each replicate, hash it, disable autosaves, and discard the per-run clone afterward |
| `START_MARK` is captured before the opening `quit --force` | A report caused by cleanup of the previous arm can be called the new arm’s crash | Fully terminate and settle the previous arm first; snapshot existing incident IDs; only then create a high-resolution run start |
| Whole-second `START_MARK` plus `find -newermt` | Boundary reports can be included/excluded incorrectly; report mtime is not crash time | Parse IPS JSON `procLaunch`/`captureTime`; use nanosecond monotonic/epoch metadata only for orchestration, not attribution |
| `find ... | head -1` is unsorted | An arbitrary matching report is selected | Collect all new incident IDs, parse them, sort by capture time, and require exactly one report matching target PID |
| Crash report is not PID-matched and its path is not printed | Stale/neighbor crash can masquerade as target; later audit cannot tell which file was used | Print and archive path, incident ID, PID, procLaunch, captureTime, exception, and top-frame offset; reject PID mismatch |
| No wait for CrashReporter after target exit | A real crash can be reported as “no new crash report” | After exact target exit, poll up to a bounded 30–60 seconds for a PID-matching IPS |
| Global `pgrep -f "Baldur's Gate 3"` | Replacement, neighbor, CrashReporter-related command, or broad match can make a dead target “alive” | Parse launch JSON and retain exact PID plus process start time; monitor only that identity with `ps -p`/`proc_pidinfo`; an IPS for that PID overrides liveness |
| Target PID is discarded even though launch JSON contains it | All later evidence loses process identity | Parse JSON with `jq`; store PID in a per-run manifest and use it for process, SE-log, and IPS checks |
| Fixed `/tmp/se_bisect_launch.txt` | Concurrent runs race; every arm overwrites its launch evidence | Use `mktemp -d` per run; archive stdout, stderr, JSON, script version/hash, and timestamps under a unique run ID |
| Launch JSON is grepped as text and launch exit status is ignored | `"socket_connected": false` or malformed output need not abort the arm | Check command status, parse one JSON document, require expected patch state/PID/socket fields, otherwise mark setup failure |
| `newest_netlog()` globally selects `ls -t ... | head -1` on every poll | It can switch files mid-run or select a neighbor/old replacement | Snapshot existing logs before launch, identify the one newly created for the target, pin its inode/path, and never reselect |
| Network log has no PID/run ID | Filename proximity is the only process binding | Add a unique `--logPath`/run ID if BG3 supports it, or write a sidecar mapping inode, PID, process start, and creation time immediately after launch |
| Session regex accepts queued `to: PrepareRunning` or server `LoadLevel` transition | A queued/not-yet-entered state can be called a loaded session | Require actual client `STATE SWAP - from: StopLoading, to: PrepareRunning`, then a subsequent stable/render-ready marker |
| Session detection stops after 16 iterations and freezes its result | Probe B loaded and crashed later while output remained `SESSION=no` | Use one continuous state machine until the global deadline; keep updating state, and start the crash-survival clock only after verified session entry |
| Survival clock starts after a variable-length driving loop | “150s” is not time since launch or session | Record monotonic `t_launch`, `t_session`, `t_exit`; report all three durations explicitly |
| `WATCH/5` integer division | Non-multiples are truncated; small durations behave oddly | Use a monotonic deadline and subsecond polling rather than `seq` arithmetic |
| Concurrent in-process auto-dismiss and external keystroke/click driving | Two automation systems race and inject different event counts by arm/timing | Disable all but one driver; use a deterministic, state-aware driver with an archived event trace |
| Repeated Space every five seconds and repeated Continue clicks | Inputs can land after state changes, toggle UI, or affect the loaded session/hotbar | Detect the exact UI state, send each required action once, verify its postcondition, and prohibit input after session entry |
| Hard-coded `cg_click(561, 383)` | Different window geometry/scale can click the wrong control | Derive coordinates from verified window bounds/OCR button bounds; store geometry and a screenshot for every action |
| Focus/frontmost manipulation is uncontrolled while focus is a suspect | The probe itself changes the suspected variable | Define visibility/focus as an experimental factor; keep it identical in causal arms or create explicit focus/no-focus arms |
| AppleScript/click errors are discarded | Failed input is indistinguishable from a real negative | Capture structured action results and stderr; fail the setup if activation/click postconditions are not met |
| `quit --force` between and after arms | SIGTERM timing/state flush differs; cleanup can create neighbor artifacts and replacement races | Prefer a controlled graceful exit for noncrash arms; wait for exact PID termination and artifact quiescence; reserve force termination for a separately labeled cleanup step |
| Only one replicate per condition in a fixed order | Order, cache, thermal, and stochastic effects are confounded | Use randomized/blocked ABBA order with at least three successful session-reaching replicates per arm |
| No cold/warm-start policy | A long-lived vanilla process is compared with a fresh SE process | Define and enforce one process-history policy; preferably fresh process, settled system, identical prelaunch delay |
| No invariant reconciles `ALIVE`, `CRASH`, and `SESSION` | Probe B can print mutually exclusive conclusions | Implement a single verdict state machine: setup-invalid, no-session, survived-target-PID, target-exited-cleanly, target-crashed-matching-signature, or conflicting-evidence-invalid |
| Toggle names overpromise their actual surface | `NO_HOOKS` still installs Dobby; `MINIMAL` leaves full constructor init | Add automated startup assertions enumerating every installed hook/init subsystem; refuse the arm if observed surface differs from its specification |
| Evidence was copied before the campaign settled | Probe B’s completed network transitions and `191750.ips` were omitted | Seal bundles only after all target/replacement PIDs exit and reports settle; include a manifest with SHA-256, size, original path, PID, and capture interval |
| No complete command/config provenance | Later audit must infer headless, visibility, and defaults from prose | Emit one JSON run manifest containing command, git commit/dirty diff hash, binary hashes, patch status, env, settings hashes, mod inventory, save hash, PID, window state, input trace, log paths, and verdict |

## Minimum acceptable next protocol

1. Remove/disable all loose SE mods, including `EntityTest`.
2. Build once and record SHA-256 for the executable, inserted load command,
   and dylib.
3. Create immutable canonical copies of the save, `modsettings.lsx`, and
   `graphicSettings.lsx`; restore and verify hashes before every arm.
4. Add startup flags that genuinely and exhaustively gate:
   - all Dobby/code patches;
   - focus scheduling/write;
   - input/CGEventTap;
   - ImGui/NSView/Metal;
   - timers/console;
   - all subsystem construction.
5. Make SE print the complete enabled/disabled surface and PID before any
   suspect initialization.
6. Use one state-aware input driver and identical visibility/focus policy.
7. Pin every SE/network log and IPS to the exact launch PID/run ID.
8. Require actual `StopLoading -> PrepareRunning`, then observe the exact PID
   for a fixed duration.
9. Run randomized ABBA blocks with at least three valid replicates per arm.
10. Seal the evidence only after reports settle and include checksums plus the
    verdict manifest.

Until that protocol is run, the surviving causal statement is:

> The crash is reproducibly associated with the current SE-present condition
> and is not prevented by disabling the main Osiris hook group. The existing
> experiments do not isolate SE core from loose SE Lua, do not disable all
> Dobby hooks, and do not test a no-focus-write crashing PID.
