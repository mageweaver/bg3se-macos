# Wave-2 `-continueGame` menu-stall analysis

**Date:** 2026-07-29

**Baseline:** `origin/main` / `91313f0`

**Investigated build:** `ebb61e8` (including Wave-2 functor hooks and the `result_out` ABI fix)

**Verdict confidence:** high for the two-stage failure and rejection of the profile-dirty hypothesis; medium for whether Wave-2 changed the probability of the pre-existing input race.

## Symptom

`PYTHONPATH=tools python3 -m bg3se_harness launch --continue` no longer reliably advances from the main menu to the newest save. The last known-good run entered `LoadSession` about 20 seconds after launch, while the two observed stalls never called `COsiris::Load` and never left `Init`.

The later “Mod Verification” window is a second failure. It appears only after Continue is actually activated. It is not what prevented the stalled runs from activating Continue in the first place.

## Root-cause verdict

There is **no demonstrated repository regression in the launcher or save-load plumbing since `91313f0`**. The post-release changes install new dylib hooks, but do not change `tools/bg3se_harness`, `src/input/focusless_input.m`, the launcher scripts, `video_skip`, or `focus_hack`. The automated transition still depends on a documented, focus-sensitive Escape/Space injection: [`noesis-input-bypass-re.md:11`](noesis-input-bypass-re.md#L11) records that `-continueGame` can merely highlight Continue, and [`noesis-input-bypass-re.md:58`](noesis-input-bypass-re.md#L58) explains that Space activates only a focused button.

What regressed observably is therefore:

1. A **pre-existing input/focus race became repeatable in the two sampled Wave-2 stalls**. In both stalls, the injected attempt occurred but no `COsiris::Load` followed.
2. When Continue is activated manually, BG3 performs a **fresh save-versus-live-ModManager validation** and reports six required mods as missing or disabled. This modal and this exact six-mod set are not new: the 2026-05-16 investigation records reaching Mod Verification at [`headless-cli-goal-progress.md:374`](headless-cli-goal-progress.md#L374) and names the same six mods at [`headless-cli-goal-progress.md:435`](headless-cli-goal-progress.md#L435).
3. The existing harness has a Mod Verification handler, but it is a blind coordinate sequence and did not clear this dialog.

The post-release audit was:

```bash
git log --format='%h|%ad|%s' --date=iso-strict 91313f0..HEAD
git diff --name-status 91313f0..HEAD
```

It finds, in order, `a0bc648` (component writes), `4bec7ae` (Stats/prototype/treasure implementations), `a0f3ee6` (functor hooks), `2fcd6a2` (tests), and `ebb61e8` (functor ABI repair). The diff contains dylib source, tests, documentation/RE artifacts, and the new reverse-engineering helper `scripts/re/sig_scan_functors.py`; it contains no file under `tools/bg3se_harness` or `src/input` and no launch/video/focus plumbing.

The hypothesis that the 18:23 crash dirtied `PlayerProfiles/Public/profile8.lsf` and thereby created a persistent verification gate is rejected:

- The first stall began at 18:14, before the 18:23 crash.
- `/Users/tomdimino/Documents/Larian Studios/Baldur's Gate 3/PlayerProfiles/Public/profile8.lsf` has mtime `2025-12-26 12:33:03` (6,452 bytes; SHA-256 `b357a12d02a5331529d6cb7923d193461161696bd2da2e51aa5219f974b746b8`), so it was not written by any 2026-07-29 run.
- `/Users/tomdimino/Documents/Larian Studios/Baldur's Gate 3/PlayerProfiles/playerprofiles8.lsf` was rewritten at boot (`2026-07-29 19:32:27`) but its readable payload contains only active-profile bookkeeping (`ActiveProfile`, `UserProfiles`, and the profile UUID), not a verification decision.
- `/Users/tomdimino/Documents/Larian Studios/Baldur's Gate 3/PlayerProfiles/Public/modsettings.lsx` remained at mtime `2026-07-29 13:22:30` (SHA-256 `ccab051d3db48ed0b4fa7893a765e980c493a8c0966dac8934ac4e88d2a0c9c8`).
- Earlier sessions also ended without a logged orderly shutdown and were followed by successful loads. The 12:22 log ends mid-event at `/Users/tomdimino/Library/Application Support/BG3SE/logs/bg3se_2026-07-29_12-22-12.log:31828`, yet the following 12:26 session loads at `/Users/tomdimino/Library/Application Support/BG3SE/logs/bg3se_2026-07-29_12-26-25.log:5632`. The 12:26 log ends mid-event at line 50848, yet the 13:21 run loads at `/Users/tomdimino/Library/Application Support/BG3SE/logs/bg3se_2026-07-29_13-21-25.log:7452`. The 13:21 log similarly ends at line 37004, yet the last-good 13:39 run loads at line 6025.
- The last-good 13:39 session itself ends mid-game, not with a shutdown sequence (`/Users/tomdimino/Library/Application Support/BG3SE/logs/bg3se_2026-07-29_13-39-12.log:146553-146632`). Thus an unclean exit is neither new nor sufficient to make later auto-continue fail.

The absence of readable verification strings in `profile8.lsf` is supporting evidence only; the stale mtime is the decisive evidence that the July crash did not mutate it.

## Evidence

### 1. Good and stalled runs diverge at input consumption, before save validation

In the last-good run:

- FocuslessInput emits dismiss attempt 9 at `/Users/tomdimino/Library/Application Support/BG3SE/logs/bg3se_2026-07-29_13-39-12.log:6005`.
- It posts Space at line 6015.
- `COsiris::Load` begins 339 ms later at line 6025.
- The game transitions `Init -> LoadSession` at line 6031 and `LoadSession -> Running` at line 18619.

In the first stall:

- Attempt 9 occurs at `/Users/tomdimino/Library/Application Support/BG3SE/logs/bg3se_2026-07-29_18-14-20.log:6013` and Space is posted at line 6023.
- Input attempts continue through attempt 11; the native timer is then stopped because the harness socket becomes ready at lines 6134-6138.
- The log ends at line 8666 without `COsiris::Load` or any game-state transition.

In the current stall:

- Attempt 9 occurs at `/Users/tomdimino/Library/Application Support/BG3SE/logs/bg3se_2026-07-29_19-32-26.log:6012` and Space is posted at line 6022.
- Socket readiness stops the native timer at line 6117; the final native click is logged at line 6135.
- The complete log contains no `COsiris::Load` call and no `Init -> LoadSession`.

This is exactly the control flow implemented in [`src/input/focusless_input.m:222`](../../src/input/focusless_input.m#L222): the native two-second timer posts Escape, Space, and a center click, then stops when either game state advances or the Python harness socket is ready ([`src/input/focusless_input.m:245`](../../src/input/focusless_input.m#L245)). The source explicitly delegates subsequent menu/modal handling to the Python watchdog ([`src/input/focusless_input.m:278`](../../src/input/focusless_input.m#L278)).

### 2. The Python watchdog tried to handle the verification dialog, but did not prove UI state

The handler is not absent. [`tools/bg3se_harness/launch.py:1029`](../../tools/bg3se_harness/launch.py#L1029) defines the timing, and [`tools/bg3se_harness/launch.py:1044`](../../tools/bg3se_harness/launch.py#L1044) hard-codes a Continue click, six checkbox clicks, and a Start Game click. The menu watchdog repeats it up to six times at [`tools/bg3se_harness/launch.py:1274`](../../tools/bg3se_harness/launch.py#L1274). OCR is used to find Continue, but not to locate or verify the modal controls ([`tools/bg3se_harness/launch.py:1654`](../../tools/bg3se_harness/launch.py#L1654)).

The 19:32 run records:

- splash/menu click at `/Users/tomdimino/Library/Application Support/BG3SE/logs/bg3se_2026-07-29_19-32-26.log:6173-6174`;
- Continue click at lines 6461-6462;
- six checkbox clicks at lines 6733-7009;
- Start Game at lines 7197-7198;
- a second checkbox pass at lines 7495-7771 and second Start Game at lines 7961-7962.

No load followed. The 18:14 stall shows the same blind sequence at lines 6190-7967. These log messages establish that coordinates were clicked, not that the intended Noesis controls received or accepted them.

### 3. The dialog is produced by live engine validation, not a stored profile verdict

Read-only disassembly of the installed game binary

`/Users/tomdimino/Library/Application Support/Steam/steamapps/common/Baldurs Gate 3/Baldur's Gate 3.app/Contents/MacOS/Baldur's Gate 3`

shows this current-build call path:

```text
gui::DCMainMenu::OnContinueGameCommand                 0x1023e10f8
  -> gui::DCSavegames::CheckLoadSelectedSavegame      0x102477c54
     -> ecl::SavegameManager::GetMissingAddons        0x10312885c
        -> ecl::ModManagerClient::GetModSettingsMissmatch
                                                        0x103055728
     -> if mismatch count != 0:
          gui::TryOpenModVerificationWindow           0x1026ad600
        else:
          gui::DCSavegames::LoadSelectedSavegame      0x1024782cc
```

`OnContinueGameCommand` obtains the last save and branches to `CheckLoadSelectedSavegame` at `0x1023e125c`. The check calls `GetMissingAddons` at `0x102477eb8`, tests the returned collection count at `0x102477ebc-0x102477ec4`, opens the verification window at `0x102477fc8` when nonzero, and otherwise calls the load leaf at `0x102477fec`.

`GetMissingAddons` calls `GetModSettingsMissmatch` at `0x103128a28`, passing the selected save's module settings and addon identifiers to the live client ModManager. `GetModSettingsMissmatch` refreshes available mods before comparing them (`ls::ModManager::RefreshAvailableMods`, call near `0x103055ecc`). Therefore the dialog is recomputed for the selected save; no profile verification-cache read exists in this decision path.

### 4. Disk inventory is internally consistent, but is not authoritative for the live ModManager

The current `PlayerProfiles/Public/modsettings.lsx` contains Gustav and Mod Configuration Menu at lines 9-24, followed by all six save-required mods at lines 25-72:

| Lines | Mod |
|---:|---|
| 25-32 | `IN_Core_1_03` |
| 33-40 | `HT_Camp Event Overhaul` |
| 41-48 | `Better Inventory UI` |
| 49-56 | `ACT1 Capes and Cloaks` |
| 57-64 | `LIX_OriginDialogTags` |
| 65-72 | `Facial Animations` |

The read-only command

```bash
PYTHONPATH=tools python3 -m bg3se_harness mod verify --modsettings --continue
```

reports 8 active mods, 14 installed/registered mods, 6 save-required mods, zero issues, and no required UUID missing from either disk-active or installed sets. The comparison code is in [`tools/bg3se_harness/savegames.py:387`](../../tools/bg3se_harness/savegames.py#L387), and explicitly notes that its disk-based result does not prove engine load order ([`tools/bg3se_harness/savegames.py:421`](../../tools/bg3se_harness/savegames.py#L421)).

Likewise, the byte-identical `=== Enabled Mods ===` blocks do not show BG3's live set. BG3SE builds that block by opening and parsing `PlayerProfiles/Public/modsettings.lsx` directly ([`src/mod/mod_loader.c:333`](../../src/mod/mod_loader.c#L333), [`src/mod/mod_loader.c:389`](../../src/mod/mod_loader.c#L389)). It proves that the LSX was unchanged and readable to BG3SE, not that BG3's refreshed `ecl::ModManagerClient` accepted and enabled each package.

## Ranked mechanism for the Mod Verification dialog

1. **Fresh mismatch between the save's required addons and BG3's live refreshed ModManager state — high confidence.** This is the exact branch observed in the current game binary. The six displayed rows are the output of `GetMissingAddons`; the engine believes those save-required module descriptors are unavailable, disabled, or incompatible at that moment.
2. **Live-engine rejection or deactivation despite correct on-disk LSX/PAK inventory — medium-high confidence.** Disk verification and the BG3SE log both pass, but neither reads the live ModManager result. The remaining discriminators are UUID, version, folder/MD5 identity, and active/available state after `RefreshAvailableMods`. Instrumenting the mismatch outputs will identify which bucket each of the six occupies.
3. **A persistent player-profile or per-save “dirty/unverified” cache created by the 18:23 crash — very low confidence / contradicted.** The first stall predates the crash, `profile8.lsf` was not written, previous unclean exits were followed by successful loads, the identical six-mod verification problem was recorded in May, and the engine decision path refreshes and compares mods live.

The verification mismatch may have existed before Wave-2 without being visible: the known input flake sometimes activates Continue and sometimes does not, and the dialog is only reachable after that activation. Existing research already documented the validation branch and modal problem before this regression investigation ([`continuegame-bypass-research-2026-07-28.md:98`](continuegame-bypass-research-2026-07-28.md#L98), [`headless-cli-goal-progress.md:374`](headless-cli-goal-progress.md#L374)).

## Functor-hook correlation

The correlation is real but does not establish a hard causal block:

- Last good: functor hooks are version-gated off at `/Users/tomdimino/Library/Application Support/BG3SE/logs/bg3se_2026-07-29_13-39-12.log:5521`; load begins at line 6025.
- First stall: 10/10 hooks install at `/Users/tomdimino/Library/Application Support/BG3SE/logs/bg3se_2026-07-29_18-14-20.log:5533-5534`; no load occurs.
- 18:22 run: the same 10/10 hooks install at `/Users/tomdimino/Library/Application Support/BG3SE/logs/bg3se_2026-07-29_18-22-30.log:5533-5534`; nevertheless `COsiris::Load` begins at line 6036, the state becomes `LoadSession` at line 6044, and later becomes `Running` at line 18758.
- Current stall: 10/10 hooks install at `/Users/tomdimino/Library/Application Support/BG3SE/logs/bg3se_2026-07-29_19-32-26.log:5533-5534`; no load occurs.

Thus the hooks do not hard-block loading or validation. At most they may perturb startup timing enough to change which control owns focus when the injected Space arrives. That remains plausible but unproven.

The installed wrappers have a no-subscriber fast path that immediately calls the original function ([`src/stats/functor_hooks.c:70`](../../src/stats/functor_hooks.c#L70), [`src/stats/functor_hooks.c:143`](../../src/stats/functor_hooks.c#L143)). There is no log evidence that any functor wrapper executes between main-menu arrival and Continue activation. Installation is the only known pre-load delta, not evidence of execution.

### Discriminating test

Use a surgical functor-only A/B, not `BG3SE_NO_HOOKS`. The latter disables StaticData and VideoSkip as well ([`src/injector/main.c:4120`](../../src/injector/main.c#L4120), [`src/injector/main.c:4159`](../../src/injector/main.c#L4159)), changing the very splash/input timing under test.

1. Add a temporary, build-gated `BG3SE_DISABLE_FUNCTOR_HOOKS=1` around only `functor_hooks_init()` at [`src/injector/main.c:4173`](../../src/injector/main.c#L4173), or compare otherwise-identical builds of `4bec7ae` and `a0f3ee6`.
2. Alternate at least 10 cold `launch --continue` trials per arm with the same BG3 build, profile, `modsettings.lsx` hash, window geometry, and no manual input.
3. Count success only when the per-run BG3SE log records `COsiris::Load`, `Init -> LoadSession`, and `LoadSession -> Running`; socket readiness or a click log is not success.
4. Add temporary counters to all ten functor wrappers and emit them immediately before the first load transition. Any nonzero pre-load counter supplies a possible direct path; all-zero counters constrain the effect to install-time timing.
5. Separately activate Continue once in each arm. If both display the same six-row verification result, the functor hooks do not create the live mod mismatch.

A repeated hook-on-only failure rate would establish a timing regression caused by hook installation. Similar flake rates and identical dialogs would make the correlation coincidental.

## Recommended fix

### First, diagnose the live mismatch without modifying game data

1. Keep using `mod verify --modsettings --continue` as the disk/save preflight. It currently passes, so rewriting `modsettings.lsx`, `profile8.lsf`, or the save is not supported by the evidence.
2. Confirm `/Users/tomdimino/Documents/Larian Studios/Baldur's Gate 3/PlayerProfiles/Public/modsettings.lsx` still contains the exact UUID and `Version64` values at lines 25-72.
3. Confirm `/Users/tomdimino/Documents/Larian Studios/Baldur's Gate 3/ModCrashSanityCheck` is absent. This exact path is defined at [`tools/bg3se_harness/config.py:82`](../../tools/bg3se_harness/config.py#L82), and the harness doctor treats its presence as a mod-disable/safe-mode marker ([`tools/bg3se_harness/doctor.py:219`](../../tools/bg3se_harness/doctor.py#L219)). It is currently absent, so there is nothing to flip here.
4. Instrument `ecl::SavegameManager::GetMissingAddons` or `ecl::ModManagerClient::GetModSettingsMissmatch` and log every returned `ModuleShortDesc`: UUID, name, version, folder/MD5 where available, and which mismatch output array received it. This is the shortest path to explaining why BG3's live view rejects the exact six modules while the disk preflight accepts them.

### Then, make automated continuation semantic rather than coordinate-driven

Preferred implementation:

- Wait until the save list and main-menu controller are ready.
- Resolve the last save exactly as `gui::DCMainMenu::OnContinueGameCommand` does.
- Run the independent disk/save preflight and the engine's own missing-addon check.
- If the live check is clean, invoke `gui::DCSavegames::LoadSelectedSavegame` (`0x1024782cc` for build `4.1.1.7209685`) on the main thread. Version-gate the address/signature.
- If the live check returns mismatches, fail closed with the exact UUID/version differences instead of clicking through an unknown dialog.

If explicit acceptance of the known six-mod mismatch is desired, detect the actual Mod Verification rows and invoke `gui::DCModVerification::Continue` (`0x102413968`) only when the returned UUID set exactly matches the pre-approved set. Do not treat the current six fractional checkbox clicks plus Start Game as confirmation.

`--accept-mod-verification` is only a harness preflight policy switch; it does not repair BG3's live ModManager state and does not make the Noesis modal handler reliable.

## Prevention

- Treat the BG3SE-parsed `=== Enabled Mods ===` block as an on-disk configuration observation, not proof of engine activation.
- Log the live `GetMissingAddons` result before any automated bypass.
- Replace focus-dependent Space/coordinate automation with a main-thread native command or controller invocation, guarded by state and build.
- Keep a cold-launch A/B regression test that requires the full `Init -> LoadSession -> Running` sequence.
- Record a terminal shutdown marker so abrupt log endings can be distinguished from normal exits without inference.
