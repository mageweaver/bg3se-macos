# BG3 4.1.1.7209685 Mod Verification mechanism analysis

**Date:** 2026-07-29

**Target save:** `Tamarru-62412511136__Ebonlake Grotto - 27h 19m`

**Save build:** `4.1.1.6995620`

**Current game build:** `4.1.1.7209685`

**Investigation mode:** read-only for the game installation, saves, profile, and
mod files

## Symptom

Activating **Continue** can open BG3's **Mod Verification** window with:

> We have detected changes to the following mods since your last playthrough...

The **Missing or Disabled** section contains these six mods:

1. `IN_Core_1_03`
2. `HT_Camp Event Overhaul`
3. `Better Inventory UI`
4. `ACT1 Capes and Cloaks`
5. `LIX_OriginDialogTags`
6. `Facial Animations`

All six are present in
`PlayerProfiles/Public/modsettings.lsx`, all six PAKs exist, and the save's own
manifest contains the same six modules.

Some `-continueGame` launches reach `COsiris::Load` and `LoadSession`; others
remain at the menu with no `LoadSession`.

## Root cause

The dialog is not driven by a persistent “pending verification” bit in
`profile8.lsf`. It is generated when BG3 compares the selected save's embedded
`ModuleSettings` against the **live `ecl::ModManagerClient` module set in the
current process**.

For build `4.1.1.7209685`, the relevant comparator:

- skips standard modules such as `GustavX`;
- looks up each nonstandard save module by UUID in the live ModManager;
- classifies a save module as missing/disabled when it is not in the live
  current/active module set;
- compares `Version64` only after finding the UUID; and
- does **not** compare the `ModuleShortDesc.Hash`/`MD5` or `Folder` fields in
  this save-load verification function.

Therefore the exact six-row result means that the game did not have those six
UUIDs in the applicable live active/current map when the check ran. Their
presence in the LSX and in the Mods directory proves disk consistency, but not
live engine activation.

The on-disk save-versus-`modsettings.lsx` diff does not explain the dialog:
**UUID, name, folder, `Version64`, MD5, and publish handle match exactly for
all six rows.** There are no version bumps, hash-string changes, or folder
renames among the six.

## Mechanism verdict, ranked

| Rank | Mechanism | Confidence | Evidence |
|---:|---|---|---|
| 1 | The save's six UUIDs are absent/disabled in BG3's live ModManager set at check time | High | This is the native `GetModSettingsMissmatch` branch and agrees with the dialog's “Missing or Disabled” category. |
| 2 | The successful/stalled automation difference is an input/focus/UI-timing race | High | Good runs start `COsiris::Load` about 0.3 seconds after attempt 9's Space; stalled runs receive the same sequence through attempt 11 and never load. The 18:22 success alternates with 18:14 and 19:32 stalls without a disk-state change. |
| 3 | The live set is sometimes incomplete because BG3 rejects or has not yet applied the disk load order | Medium-high | Disk and PAK metadata are consistent, while the engine result is not. The live comparator reads in-memory maps that are not represented by the BG3SE disk preflight. |
| 4 | The extra MCM entry destabilizes application of the following load order | Medium-low | MCM is before the six in `modsettings.lsx`, is absent from the old save, and its active LSX `Folder` is `Mod Configuration Menu` while its PAK metadata folder is `BG3MCM`. This is a real inconsistency, but no trace yet proves it causes the six live UUIDs to disappear. |
| 5 | Changed PAK contents/MD5 caused the dialog | Low/contradicted for this incident | None of the six PAKs postdates the save, all six stored hash strings match, whole-file PAK MD5 is not the stored hash, and this native comparator does not read the hash field. |
| 6 | A persisted verification flag was armed by a later crash or stored in `profile8.lsf` | Very low/contradicted | The first stall predates the crash, `profile8.lsf` is from 2025, alternating later runs succeed, and the gate is recomputed from the save and live ModManager. |

## Evidence

### 1. The save embeds the expected module manifest

The save archive is:

```text
~/Documents/Larian Studios/Baldur's Gate 3/PlayerProfiles/Public/Savegames/Story/
  Tamarru-62412511136__Ebonlake Grotto - 27h 19m/
    Ebonlake Grotto - 27h 19m.lsv
```

Its directory mtime is `2025-12-23 20:07:51 -0500`. `SaveInfo.json` reports:

```text
Save Name:    Ebonlake Grotto - 27h 19m
Game Version: 4.1.1.6995620
Current Level: WLD_Main_A
```

The harness marker scan:

```bash
PYTHONPATH=tools python3 -m bg3se_harness \
  save mods 'Ebonlake Grotto - 27h 19m'
```

finds exactly the six high-confidence required mods in `meta.lsf`. `Waypoints`
is only a low-confidence name string in `Globals.lsf`; it is not a save
manifest member.

A direct read-only LSF v7 decode of `meta.lsf` found:

```text
LSOF version:      7
metadata format:   0
compression flags: 0x22 (LZ4)
nodes:             24
attributes:        80
```

The manifest is under:

```text
MetaData / ModuleSettings / Mods / ModuleShortDesc
```

The exact save-to-current comparison is:

| Mod | UUID | Save `Version64` | Publish handle | Save MD5 | Disk diff |
|---|---|---:|---:|---|---|
| `IN_Core_1_03` | `b7e08ea5-2ece-991a-883f-619bb8fc0457` | `36028797018963969` | `0` | `92ae5c2df98777c464300fb9c7a5e6d2` | None |
| `HT_Camp Event Overhaul` | `d756bfd1-e403-8ce1-9f81-c6c988a60837` | `36028799166447617` | `5304919` | `75d5d92dd594569f2bd4c72cd90db6fb` | None |
| `Better Inventory UI` | `6b585be8-ed73-7347-2c58-73146e22b7d4` | `36591755562319893` | `4228735` | `642f438bfa5d6c773f717a62e3af8438` | None |
| `ACT1 Capes and Cloaks` | `139ee212-9e2c-78ef-98b9-a5c99ddd6e0c` | `36028797018963980` | `4360405` | `ee0fbba9ca662946381f5602fec2f623` | None |
| `LIX_OriginDialogTags` | `4d98334b-85cc-6e74-9d83-12ce80e8793c` | `72057594037927939` | `4249034` | `ffafa5b90cf49d5ee999e6b844f5016a` | None |
| `Facial Animations` | `f8e39ac7-87d1-716d-4f36-342d69094e75` | `36028797018963987` | `4346541` | `7603944a19fdef1b303ecb17c9e49661` | None |

The folders also match exactly:

| Mod | Save and current folder |
|---|---|
| `IN_Core_1_03` | `IN_Core_1_b7e08ea5-2ece-991a-883f-619bb8fc0457` |
| `HT_Camp Event Overhaul` | `Camp-Scene-Overhaul_d756bfd1-e403-8ce1-9f81-c6c988a60837` |
| `Better Inventory UI` | `BetterInventoryUI_6b585be8-ed73-7347-2c58-73146e22b7d4` |
| `ACT1 Capes and Cloaks` | `ACT1_Capes_139ee212-9e2c-78ef-98b9-a5c99ddd6e0c` |
| `LIX_OriginDialogTags` | `LIX_OriginDialogTags_4d98334b-85cc-6e74-9d83-12ce80e8793c` |
| `Facial Animations` | `Facial_Animations_f8e39ac7-87d1-716d-4f36-342d69094e75` |

The PAKs' internal `meta.lsx` values also agree on UUID, name, folder, and
version. This includes the three PAK filenames whose download suffix does not
reproduce the full internal UUID:

```text
act1_capes_139ee212-9e2c-78ef-7pgb.pak
lix_origindialogtags_4d98334b-hxyn.pak
facial_animations_f8e39ac7-87d-dlgr.pak
```

Those filenames are not folder renames; the internal folders are the exact
folders stored by the save and LSX.

### 2. Byte-level save fields

The following are payload offsets in the decompressed LSF `Values` stream.
GUID bytes use Larian/.NET GUID byte order: the first 32-bit word and next two
16-bit words are little-endian. `Version64` is an eight-byte little-endian
integer.

| Mod | UUID offset / raw 16 bytes | Version offset / raw 8 bytes | MD5 offset |
|---|---|---|---:|
| `IN_Core_1_03` | `81`: `a58ee0b7ce2e1a99883f619bb8fc0457` | `97`: `0100000000008000` | `105` |
| `HT_Camp Event Overhaul` | `206`: `d1bf56d703e4e18c9f81c6c988a60837` | `222`: `0100008000008000` | `230` |
| `Better Inventory UI` | `351`: `e85b586b73ed47732c5873146e22b7d4` | `367`: `1500000002008200` | `375` |
| `ACT1 Capes and Cloaks` | `491`: `12e29e132c9eef7898b9a5c99ddd6e0c` | `507`: `0c00000000008000` | `515` |
| `LIX_OriginDialogTags` | `626`: `4b33984dcc85746e9d8312ce80e8793c` | `642`: `0300000000000001` | `650` |
| `Facial Animations` | `770`: `c79ae3f8d1876d714f36342d69094e75` | `786`: `1300000000008000` | `794` |

At those MD5 offsets the save contains the 32-character strings in the
comparison table. Current `modsettings.lsx` contains the same strings at lines
27, 35, 43, 51, 59, and 67.

The save's folder/name payload offsets are:

| Mod | Folder offset | Name offset |
|---|---:|---:|
| `IN_Core_1_03` | `138` | `185` |
| `HT_Camp Event Overhaul` | `263` | `320` |
| `Better Inventory UI` | `408` | `463` |
| `ACT1 Capes and Cloaks` | `548` | `596` |
| `LIX_OriginDialogTags` | `683` | `741` |
| `Facial Animations` | `827` | `882` |

There is no byte/field delta for any of the six.

### 3. The two real manifest differences are outside the six

`GustavX` is present in both manifests, but differs:

| Field | Save | Current `modsettings.lsx` |
|---|---|---|
| `Version64` | `145241946983074840` | `36028797018963968` |
| MD5 | `ef3fcba3f3684b3088ad1f9874d4957c` | empty |

This does not produce a dialog row because the native comparator calls
`ModuleShortDesc::IsStandardModule()` and skips `GustavX`.

Mod Configuration Menu is the inverse case:

- it is absent from the December 2025 save;
- it is an extra current module with UUID
  `755a8a72-407f-4f0d-9a33-274ac0f0b53d`;
- current `modsettings.lsx` says `Folder="Mod Configuration Menu"`;
- the installed PAK says `Folder="BG3MCM"`; and
- its MD5 is empty.

MCM therefore cannot be a save-required dialog row. Its folder discrepancy is
worth an isolated test because it precedes the six entries in the active LSX,
but the present evidence does not prove that it invalidates the rest of the
load order.

### 4. None of the six PAKs was modified after the save

`ls -laT` gives:

| Mod / PAK | PAK mtime | After 2025-12-23 save? |
|---|---|---|
| `IN_Core_1_...pak` | 2025-11-18 10:54:17 -0500 | No |
| `Camp-Scene-Overhaul_...pak` | 2025-11-18 10:54:17 -0500 | No |
| `BetterInventoryUI_...pak` | 2025-11-18 10:54:17 -0500 | No |
| `act1_capes_...-7pgb.pak` | 2025-11-18 14:31:13 -0500 | No |
| `lix_origindialogtags_...-hxyn.pak` | 2025-11-18 11:01:57 -0500 | No |
| `facial_animations_...-dlgr.pak` | 2025-11-18 11:04:44 -0500 | No |
| `Mod Configuration Menu.pak` | 2026-03-06 12:27:02 -0500 | **Yes** |

MCM is the only relevant PAK that postdates the target save. It is also not in
the save manifest.

The whole-file PAK MD5 values are:

| Mod | Whole-file PAK MD5 | Stored manifest MD5 |
|---|---|---|
| `IN_Core_1_03` | `5a29d7836965b84f048c63f99917c62e` | `92ae5c2df98777c464300fb9c7a5e6d2` |
| `HT_Camp Event Overhaul` | `874dc1c09bac6c9026bb7c1b392d6917` | `75d5d92dd594569f2bd4c72cd90db6fb` |
| `Better Inventory UI` | `dbaad288fff9faa676f7b0dec0670667` | `642f438bfa5d6c773f717a62e3af8438` |
| `ACT1 Capes and Cloaks` | `c46df2b1c2765b842c18361cd7b1b5ec` | `ee0fbba9ca662946381f5602fec2f623` |
| `LIX_OriginDialogTags` | `781e1d6d8a80120bf906a1b736c11df5` | `ffafa5b90cf49d5ee999e6b844f5016a` |
| `Facial Animations` | `4deca6131ac97b1344ab8a78677832fc` | `7603944a19fdef1b303ecb17c9e49661` |

The `ModuleShortDesc` MD5 is plainly not `md5(pak_file)`. It may be package or
publishing metadata used elsewhere, but it is not a raw-file checksum.

More decisively, the save-load comparator described below accesses UUID and
`ModVersion` but not the `Hash` field. This particular dialog result is not a
raw PAK MD5 verification failure.

### 5. Native dialog trigger and load gate

The Ghidra HTTP bridge at `127.0.0.1:8080` was not reachable, so the fallback
was read-only `strings`, `nm`, and ARM64 `objdump` on:

```text
~/Library/Application Support/Steam/steamapps/common/Baldurs Gate 3/
  Baldur's Gate 3.app/Contents/MacOS/Baldur's Gate 3
```

Relevant binary strings and symbols include:

```text
ContinueGameCommand
CloseModVerificationWindowCommand
gui::DCModVerification
VerificationMods
ModInfoRequests
OpenModVerificationWindow
RequestedModsSingletonComponent
UnsupportedModVerification
-continueGame
```

The current-build control flow is:

```text
gui::DCMainMenu::OnContinueGameCommand                     0x1023e10f8
  -> select explicit/current save or GetLastSaveGame()
  -> gui::DCSavegames::CheckLoadSelectedSavegame           0x102477c54
     -> ecl::SavegameManager::GetMissingAddons             0x10312885c
        -> ecl::ModManagerClient::GetModSettingsMissmatch  0x103055728
     -> mismatch count != 0:
          gui::TryOpenModVerificationWindow                0x1026ad600
     -> mismatch count == 0:
          gui::DCSavegames::LoadSelectedSavegame           0x1024782cc
            -> ecl::SavegameManager::TryLoad               0x1031278e4
```

Specific instructions establish the gate:

- `OnContinueGameCommand` tail-branches to the common check at
  `0x1023e125c`.
- `CheckLoadSelectedSavegame` calls `GetMissingAddons` at `0x102477eb8`.
- It checks the returned array count at `0x102477ebc-0x102477ec4`.
- It opens intent-1 Mod Verification at `0x102477f60-0x102477fc8` when the
  count is nonzero.
- It calls `LoadSelectedSavegame` only at `0x102477fec`.
- `LoadSelectedSavegame` tail-branches to `SavegameManager::TryLoad` at
  `0x10247835c`.

The UI layer deliberately aliases all five mismatch output arrays to the same
temporary collection (`x2` through `x6` all point to `sp` before the
`GetMissingAddons` call). Any missing/disabled or version category therefore
sets the same gate.

Inside `GetModSettingsMissmatch`:

- a `ModuleShortDesc` is `0x60` bytes;
- `IsStandardModule()` is called at `0x1030559a0`;
- UUID lookup and matching occur before `0x103055ba4`;
- the live and saved `ModVersion` values at descriptor offset `+0x28` are
  compared at `0x103055bf4-0x103055c04`;
- higher and lower versions are appended to separate outputs; and
- no access or string comparison uses the descriptor `Hash` at `+0x38` or
  `Folder` at `+0x48`.

The function can call `ls::ModManager::RefreshAvailableMods` at
`0x103055ecc` while resolving additional data modules. The ordinary save
module comparison then reads the live client maps and arrays. The refresh call
is conditional, not evidence that every Continue activation performs a full
disk rescan.

The selected-save view model also caches the current result as a byte at
`VMSavegame + 0x260`: `CheckLoadSelectedSavegame` writes it at
`0x102477ed4` and raises a property notification. That is in-process UI state,
not a profile-file flag.

`gui::DCModVerification::Continue` at `0x102413968` calls
`ClearVerificationMods` first, processes the close/continue event, and invokes
the continuation callback at `0x102413a7c-0x102413a88`. Explicit acceptance
is therefore the path that resumes the pending load.

### 6. Where “since your last playthrough” state actually lives

The phrase refers to the comparison sources, not to a standalone timestamped
verification record:

1. **Previous playthrough state:** the selected save's embedded
   `meta.lsf/MetaData/ModuleSettings`.
2. **Current state:** the current process's live `ecl::ModManagerClient`.
3. **Transient UI result:** the selected-save view-model byte at `+0x260` and
   the `VerificationMods` data used by `DCModVerification`.

There is no separate `SaveInfo` file next to this save directory. `SaveInfo.json`
and `meta.lsf` are members of the `.lsv` archive.

The requested:

```bash
find ~/Documents/Larian\ Studios \
  -newermt '2026-07-29 13:00' -maxdepth 3
```

produced:

| Mtime | Path | Interpretation |
|---|---|---|
| 2026-07-29 18:22:46 | `Baldur's Gate 3/LevelCache/{WLD_Main_A,SYS_CC_I}.lsf` | Written by the successful 18:22 load; consequence of session loading, not a pre-load verification flag. |
| 2026-07-29 19:32:27 | `Baldur's Gate 3/PlayerProfiles/playerprofiles8.lsf` | Active-profile bookkeeping only. |
| 2026-07-29 19:32:27 | `Baldur's Gate 3/graphicSettings.lsx` | Graphics state, unrelated. |
| 2026-07-29 19:32:42 | `Baldur's Gate 3/` directory | Child metadata change, not a state file. |
| 2026-07-29 21:02:58-21:02:59 | `analytics.lsx`, `Temp/` | Later than the investigated runs and unrelated to the load gate. |

At greater depth, `Public/modsettings.lsx` has mtime
`2026-07-29 13:22:30 -0400`. No direct child of
`PlayerProfiles/Public` changed after that; `Public/profile8.lsf` remains
`2025-12-26 12:33:03 -0500`.

Other candidates:

- no `ModCache` directory exists;
- no `ModCrashSanityCheck` marker exists;
- `LevelCache` was updated only by a successful load;
- `~/Library/Caches/com.larian.bg3/Cache.db` is an empty CFURL cache database
  with no response rows; and
- the root `playerprofiles8.lsf` readable payload contains only
  `ActiveProfile`, `UserProfiles`, and the profile UUID.

No candidate contains evidence of a durable pending-verification decision.

### 7. Why 13:39 and 18:22 loaded while 18:14 and 19:32 stalled

The disk files do not distinguish these runs.

The 13:21 process read seven SE mods from `modsettings.lsx`, the game rewrote
that LSX at `13:22:30`, and the save loaded in the same session. There is no
native decision-path evidence that the rewrite cleared a persisted pending
flag. It updated the current load-order description.

The input/load sequence is:

| Session | Attempt 8 | Attempt 9 Space | First `COsiris::Load` | Result |
|---|---|---|---|---|
| 13:39 | Space `13:39:29.874`, center click `13:39:30.481` | `13:39:31.879` | `13:39:32.218` (+339 ms) | Loaded |
| 18:14 | Space `18:14:38.157`, center click `18:14:38.783` | `18:14:40.157` | None | Stalled through attempt 11 |
| 18:22 | Space `18:22:47.565`, center click `18:22:48.195` | `18:22:49.555` | `18:22:49.849` (+294 ms) | Loaded |
| 19:32 | Space `19:32:44.007`, center click `19:32:44.607` | `19:32:46.015` | None | Stalled through attempt 11 |

The native timer in [`src/input/focusless_input.m`](../../src/input/focusless_input.m)
posts Escape, then Space 200 ms later, then a center click 800 ms later. Its
own comment warns that sending these inputs after Mod Verification appears can
open and close UI repeatedly.

The successful timing is compatible with two UI histories:

1. attempt 9 Space activated the focused main-menu Continue button and the
   live mismatch result happened to be empty; or
2. an earlier Space/click had already opened Mod Verification and attempt 9
   Space activated its focused/default acceptance control.

The log has no Noesis view/modal-state telemetry, so it cannot distinguish
those histories. It does show that attempt 9's input was consumed in successful
runs and that the identical blind sequence did not reach the load continuation
in stalled runs.

Consequently, the 13:39 success is not evidence that a disk flag was reset.
The alternating 18:14 failure and 18:22 success with the same files is stronger
evidence for live-state readiness and focus/control ownership.

## Does Mod Verification block `-continueGame`?

**Yes as a live control-flow gate; no as a demonstrated persistent pending
state.**

On this macOS harness path, `-continueGame` does not safely bypass save
verification. Existing runtime evidence shows it can visually prime/highlight
Continue while a later Space or menu action performs the actual activation.
Once `OnContinueGameCommand` runs, manual and automated Continue share
`CheckLoadSelectedSavegame`.

If the current mismatch collection is nonempty:

- `LoadSelectedSavegame` is not called;
- `SavegameManager::TryLoad` is not called;
- no `COsiris::Load` or `LoadSession` appears; and
- the modal's continuation callback must be accepted.

This can look like a silent `-continueGame` failure to a headless harness
because the existing log does not announce the Noesis modal. It is not a
silent disk flag: it is an unobserved UI gate backed by a freshly evaluated
in-process mismatch result.

A no-`LoadSession` stall is not, by itself, proof that the modal opened. The
18:14 and 19:32 logs also fit failure to activate the focused Continue control.
Only modal-state logging or a hook on `TryOpenModVerificationWindow` can
separate those cases.

## Recommended fix

### Safest immediate recovery

Do not rewrite the save, `profile8.lsf`, or the six `ModuleShortDesc` entries.
They are already synchronized. In particular, do not replace the stored MD5
strings with whole-file PAK MD5 values; those are different concepts and the
native verification comparator does not compare them.

For a human-controlled load:

1. activate Continue;
2. verify that the modal contains exactly the known six UUIDs/names;
3. use BG3's explicit Continue/Start Game acceptance once for that load; and
4. confirm that the log reaches `TryLoad`, `COsiris::Load`, and
   `Init -> LoadSession`.

This clears the current modal invocation and runs its continuation. There is
no evidence that one confirmation permanently suppresses future checks; the
engine can recompute the result on the next load attempt.

MCM's `Folder="Mod Configuration Menu"` versus internal `Folder="BG3MCM"`
should be investigated separately with a backed-up, isolated A/B. It is not
safe to silently rewrite it as the primary fix because the game itself wrote
the current LSX and no live trace yet ties that discrepancy to the six missing
UUIDs.

### Safe automation

Replace blind coordinate acceptance with a semantic, build-gated path:

1. Wait until the save list and live ModManager are ready.
2. Resolve the selected/last save as `OnContinueGameCommand` does.
3. Hook or wrap `GetMissingAddons` and log the UUID, version, and output bucket
   for every returned `ModuleShortDesc`.
4. If the result is empty, invoke the normal
   `DCSavegames::LoadSelectedSavegame` path on the main thread.
5. If the result is nonempty, fail closed unless the UUID/version set exactly
   matches an explicit automation allowlist.
6. For an exact allowlisted result, invoke
   `DCModVerification::Continue` on the actual data context instead of clicking
   guessed checkbox/button coordinates.

The required build gates for `4.1.1.7209685` are:

```text
GetModSettingsMissmatch       0x103055728
TryOpenModVerificationWindow  0x1026ad600
DCModVerification::Continue   0x102413968
LoadSelectedSavegame          0x1024782cc
```

Logging entry/exit of `TryOpenModVerificationWindow` is the smallest decisive
probe for whether a no-load run is blocked by the modal or never activated
Continue.

## Prevention

- Treat `modsettings.lsx` and PAK scans as disk preflight, not proof of the
  current process's active ModManager set.
- Log the live `GetMissingAddons` output before an automated bypass.
- Require UUID and `Version64` equality; do not infer a PAK update from raw
  file MD5.
- Detect and report active-LSX versus internal-PAK folder discrepancies.
- Replace repeating Escape/Space/coordinate input with main-thread semantic
  controller calls.
- Count a continue attempt as successful only after `TryLoad`,
  `COsiris::Load`, and `LoadSession`, never after a key or click was merely
  posted.
