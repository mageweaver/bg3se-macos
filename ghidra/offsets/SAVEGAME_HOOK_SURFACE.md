# Savegame hook surface — static feasibility evidence

**Date:** 2026-08-03  
**Game build:** 4.1.1.7209685  
**Scope:** Wave 7 E1.1 static analysis only; the game was not launched and no save was modified.

## Static verdict

**PROMISING.** The ARM64 game slice contains the same high-value seam used by Windows BG3SE:

```text
0x104b51a9c  esv::OsirisVariableHelper::SavegameVisit(eoc::SavegameVisitor*)
```

It has a clean, non-inlined function entry that can be intercepted by the port's MAP_JIT ARM64 hook implementation. Static direct-branch analysis finds exactly two callers:

| Direction | Call instruction | Return address | Containing function |
|---|---:|---:|---|
| write | `0x104d2cd38` | `0x104d2cd3c` | `esv::SaveSystem::DoSaveFlow(WorldView<...>&)` |
| read | `0x104d8a01c` | `0x104d8a020` | `esv::SessionLoadSystem::Update(WorldView<...>&, GameTime const&)` |

Both callers put an `eoc::SavegameVisitor*` in `x1`. The callee loads `[x1 + 0xb0]`, obtains that object's vtable, and calls slot `+0x70`. Independent wrapper symbols prove that `eoc::SavegameVisitor + 0xb0` is the nested LSF/ObjectVisitor and that vtable slot `+0x70` is `EnterRegion`. This is instruction-level evidence for the same visitor seam, in both directions, not merely a suggestive symbol name.

The callee then enters the engine's `OsirisVariableHelper` region, resolves its variable manager, calls `ls::VariableManager::Visit(ls::ObjectVisitor*)` at `0x106228908`, and exits the region through visitor slot `+0x80`. The target is therefore part of actual payload traversal, not just save/load lifecycle notification.

This is only the **static half** of E1.1. The plan gate, “repeatable observation of both directions on disposable saves = go,” is not passed until the runtime experiment below succeeds. The sidecar fallback is therefore **not selected by this spike**.

## Reference mechanism on Windows

The Windows implementation installs a pre-hook on `esv::OsirisVariableHelper::SavegameVisit`:

- `BG3Extender/Extender/Server/ScriptExtenderServer.cpp:63` installs the hook.
- `ScriptExtenderServer.cpp:276-280` receives `(OsirisVariableHelper*, SavegameVisitor*)`, checks `visitor->LSFVisitor`, and passes that nested visitor to the extender serializer.
- `BG3Extender/Extender/Shared/SavegameSerializer.inl:6-28` enters `ScriptExtenderSave`, visits the version, and selects read versus write with `ObjectVisitor::IsReading()`.
- `SavegameSerializer.inl:31-47` serializes PersistentVars, user vars, mod vars, and persistent timers together; lines 51 onward implement the PersistentVars node structure.
- `BG3Extender/GameDefinitions/Misc.h:11-25` defines the visitor relationship and the function type as `bool (OsirisVariableHelper*, SavegameVisitor*)`.

The macOS symbol, visitor forwarding behavior, and one write plus one read caller are a close structural match.

## Binary and address derivation

The executable was resolved through `scripts/find_bg3.sh`, as used by the launch/deploy scripts:

```text
/Users/tomdimino/Library/Application Support/Steam/steamapps/common/
Baldurs Gate 3/Baldur's Gate 3.app/Contents/MacOS/Baldur's Gate 3
```

`otool -f` re-derived the arm64 slice, rather than assuming the supplied value:

```text
cputype  16777228
offset   257261568 decimal = 0x0f558000
```

For this build:

```text
preferred image base = 0x100000000
fat file offset      = 0x0f558000 + (preferred_va - 0x100000000)
runtime address      = runtime Mach-O header + (preferred_va - 0x100000000)
```

Useful conversions for the recommended seam are:

```text
preferred VA   0x104b51a9c
image RVA      0x04b51a9c
fat file offset 0x140a9a9c
```

The Ghidra HTTP bridge at `127.0.0.1:8080` refused the connection. Evidence below was produced with `nm -arch arm64 -n`, `c++filt`, `strings`, the repository ADRP/string scanners, direct ARM64 `BL` decoding, `otool`, and LLDB's offline disassembler. LLDB opened the thin arm64 file as data; it did not run it.

All relevant functions below are lowercase local `t` symbols. A separate `nm -arch arm64 -gU | c++filt` search returned none of them, so “not exported” means they are unavailable through normal `dlsym` lookup even though the local symbol table preserves their names.

The name scan found no macOS counterpart literally named `SavegameSerializer` or a generic `SaveLoad` serializer callback. The useful surface is expressed through `SavegameVisitor`, `ObjectVisitor`, `LSFObjectVisitor`, the server save/load systems, and the Osiris variable helper listed below.

## Write-side candidates

“Entry-hookable” means the first 16 bytes contain no PC-relative instruction and can be relocated verbatim by the current `arm64_hook_at_offset(..., 0, ...)` implementation. It does not imply that `arm64_safe_hook()` chooses the correct offset; see the hook warning below.

| Rank | Preferred VA | Symbol | Export / virtual surface | Prologue hookability | Likely arguments and value | Confidence |
|---:|---:|---|---|---|---|---|
| 1 | `0x104b51a9c` | `esv::OsirisVariableHelper::SavegameVisit(eoc::SavegameVisitor*)` | local `t`; nonvirtual/direct `BL`; no class vtable symbol | **Yes, explicit offset 0**; clean 16-byte stack-save window | `x0=self`, `x1=SavegameVisitor*`; nested LSF/ObjectVisitor at `x1+0xb0`; ideal place to append a named visitor region before the original | **Very high** |
| 2 | `0x104d328c4` | `esv::SaveSystem::SaveEntities(WorldView<...>&, Level*, LevelCacheDesc const&, eoc::SavegameVisitor&)` | local `t`; direct calls; no demonstrated writable slot | **Yes, offset 0**; first 16 bytes are register saves | member ABI: `x0=self`, `x1=WorldView*`, `x2=Level*`, `x3=LevelCacheDesc*`, `x4=SavegameVisitor*` (confirmed by the entry moves) | High, write-only backup |
| 3 | `0x104d2af00` | `esv::SaveSystem::DoSaveFlow(WorldView<...>&)` | local `t`; directly called by ECS registration helper; `SaveSystem` has a read-only vtable, but this call is direct | **Yes, offset 0**; clean register-save window | `x0=self`, `x1=WorldView*`; owns the broader write flow and calls rank 2 and then rank 1 | High for observation; medium for serialization injection |
| 4 | `0x1062a88c4` | `ls::savegame::Framework::Save(MemWriteStream&, Span<EntityHandle const> const&, Span<int const> const&)` | local `t`; non-exported | **Yes, offset 0**; clean 16-byte stack/register window | member-like `x0=Framework*`, then stream and spans; called twice inside `SaveEntities` | High for ECS payload observation; low for a custom top-level extender region |
| 5 | `0x10312681c` | `ecl::SavegameManager::SaveGame(STDString const&, ESaveGameType, int, ScratchBuffer const*)` | local `t`; client manager vtable exists in read-only data, no writable slot established | **Yes, offset 0**; clean 16-byte stack/register window | `x0=self`, `x1=name/path string`, `w2=save type`, `w3=int`, `x4=scratch buffer` | Medium; request boundary, before server visitor exists |
| 6 | `0x1060ef73c` | `ls::SavegameVisitor::Save(Path const&)` | local `t`; virtual-forwarding thunk; visitor vtable is in read-only data | Technically replaceable at 0, but the whole 16-byte function is a tail thunk; brittle and not recommended | `x0=SavegameVisitor*`, `x1=Path*`; forwards nested visitor (`+0xb0`) to vtable `+0x50` | High ABI evidence; medium hook target |
| 7 | `0x1064cfa40` | `ls::LSFObjectVisitor::Save(Path const&)` | local `t`; virtual implementation; vtable in read-only data | Entry's first 16 bytes are relocatable; short tail wrapper requires care | `x0=LSFObjectVisitor*`, `x1=Path*`; selects flags and tails to the 3-argument save at `0x1064d5684` | Medium; too low-level and may see unrelated LSF writes |
| 8 | `0x104d2fe50` | `esv::SaveSystem::DoSaveMeta(WorldView<...>&, SaveWorldRequestComponent&)` | local `t`; no writable slot demonstrated | **Yes, offset 0** | `x0=self`, `x1=SaveWorldRequestComponent*`; directly references `SaveInfo.json` | High for metadata observation; low for persistence payload |
| 9 | `0x100c8c8a8` | `Eoc::SaveGame(STDString const&)` | local `t`; nonvirtual wrapper | **No with the current relocator at entry**: begins with PC-relative `ADRP` and tail-branches | `x0=Eoc self`, `x1=string`; wrapper replaces `x0` with the global savegame manager and preserves `x1` | High identity; low hook quality |

Other write-path symbols retained by the binary include:

```text
0x104d2e0e8  esv::SaveSystem::DoSaveToCache(...)
0x104d39ee8  esv::SavegameManager::SaveGameProcessor::OnWTCompletion()
0x104d3a090  esv::SavegameManager::SaveGameProcessor::ExecuteWTKernel()
0x104d3f2a0  esv::SavegameManager::CreateSavegameVisitor(ObjectVisitorBuffer*)
0x104d3fb78  esv::SavegameManager::QueueSaveGameProcessing(SaveGameContext&&)
0x106499964  ls::FileFormatIO::SaveFile(Path const&, bool)
0x1064e6aac  ls::DefaultObjectVisitor::Save(Path const&)
0x1064f253c  ls::DefaultObjectVisitor::SaveToMemory(MemWriteStream&)
```

## Read-side candidates

| Rank | Preferred VA | Symbol | Export / virtual surface | Prologue hookability | Likely arguments and value | Confidence |
|---:|---:|---|---|---|---|---|
| 1 | `0x104b51a9c` | `esv::OsirisVariableHelper::SavegameVisit(eoc::SavegameVisitor*)` | local `t`; nonvirtual/direct `BL`; no class vtable symbol | **Yes, explicit offset 0** | same ABI as write; read caller passes its prepared visitor after `PreVisit()` and `VisitVersionHash()` | **Very high** |
| 2 | `0x104d897b0` | `esv::SessionLoadSystem::Update(WorldView<...>&, GameTime const&)` | local `t`; direct ECS call; class vtable is read-only | **Yes, offset 0**; clean register-save window | `x0=self`, `x1=WorldView*`, `x2=GameTime*`; creates/prepares the read visitor and calls rank 1 | High for lifecycle/owner-thread observation; medium for injection |
| 3 | `0x104aef950` | `esv::LoadProtocol::LoadSavegame(STDString const&, ModuleSettings const*)` | local `t`; `LoadProtocol` vtable exists in read-only data; four direct callers | **Yes, offset 0**; clean 16-byte stack/register window | `x0=self`, `x1=save path/name`, `x2=ModuleSettings*`; best path-bearing read breadcrumb | High, but earlier than visitor traversal |
| 4 | `0x103125f20` | `ecl::SavegameManager::LoadGame(STDString const&, ModuleSettings const*)` | local `t`; client manager vtable is read-only | **Yes, offset 0**; clean stack/register window | `x0=self`, `x1=name/path string`, `x2=ModuleSettings*` | Medium; client request boundary |
| 5 | `0x1060ef74c` | `ls::SavegameVisitor::Load(Path const&)` | local `t`; virtual-forwarding thunk; vtable in read-only data | Technically replaceable at 0, but the entire 16-byte function is a tail thunk; not recommended | `x0=SavegameVisitor*`, `x1=Path*`; forwards nested visitor to vtable `+0x58` | High ABI evidence; medium hook target |
| 6 | `0x1060ef75c` | `ls::SavegameVisitor::Load(Path const&, FileReader&)` | local `t`; virtual-forwarding thunk | Same short-thunk caveat | `x0=SavegameVisitor*`, `x1=Path*`, `x2=FileReader*`; forwards through vtable `+0x60` | High ABI evidence; medium hook target |
| 7 | `0x1064cfa58` | `ls::LSFObjectVisitor::Load(Path const&)` | local `t`; virtual implementation; vtable is read-only | **Yes, offset 0**; conventional 16-byte stack/register window | `x0=LSFObjectVisitor*`, `x1=Path*`; constructs a `FileReader` then calls the reader overload | Medium; too low-level and may see unrelated LSF reads |
| 8 | `0x100c8c8bc` | `Eoc::LoadGame(STDString const&)` | local `t`; nonvirtual wrapper | **No with the current relocator at entry**: begins with `ADRP` and tail-branches | `x0=Eoc self`, `x1=string`; wrapper replaces `x0` with the global savegame manager and preserves `x1` | High identity; low hook quality |

Other load-path symbols retained by the binary include:

```text
0x103125d40  ecl::SavegameManager::LoadGame(SaveGameIndex, ModuleSettings const*, StringView)
0x1031275f4  ecl::SavegameManager::QuickLoad()
0x1031278e4  ecl::SavegameManager::TryLoad(SaveGameIndex, ModuleSettings const*, Function<...> const*, StringView, bool)
0x104d3f468  esv::SavegameManager::FinishLoadGame()
0x104d3fd80  esv::SavegameManager::LoadLevelCacheEntries(eoc::SavegameVisitor*)
0x1064cfacc  ls::LSFObjectVisitor::Load(Path const&, FileReader&)
0x1064d4910  ls::LSFObjectVisitor::PrepareForRead()
0x1064e79f4  ls::DefaultObjectVisitor::Load(Path const&)
0x1064ea24c  ls::DefaultObjectVisitor::Load(Path const&, FileReader&, FixedString const&)
```

## String and xref evidence

String VAs are preferred VAs in the arm64 slice. References were recovered with the repository ADRP/string-pointer tools and assigned to the nearest enclosing `nm` symbol.

| String VA | String | Referencing instruction(s) / function | Directional evidence |
|---:|---|---|---|
| `0x107bf28d2` | `.lsv` | `0x101c7a7e4-0x101c7a7e8`, `eoc::SaveOpenHelper::SaveOpenHelper(STDString const&, EFileAccess, bool)` at `0x101c7a430`; another client CloudManager ref | archive open boundary; access mode determines direction |
| `0x107b4adb3` | `*.lsv` | no direct ADRP+ADD ref found | likely used indirectly or through a pointer pool; not ranked |
| `0x107cc2271` | `SaveInfo.json` | `0x104d30170`, `0x104d301a0` inside `esv::SaveSystem::DoSaveMeta` at `0x104d2fe50`; additional client cross-save reads | confirms metadata writer candidate |
| `0x107cc611f` | `Globals.lsf` | `0x104d2cb6c` in `DoSaveFlow`; `0x104d3a31c` in `SaveGameProcessor::ExecuteWTKernel`; `0x104d8b87c` in `SessionLoadSystem::Update` | direct write/read symmetry around the ranked outer functions |
| `0x107d48fe2` | `meta.lsf` | `0x104d2c4d8` in `DoSaveFlow`; `0x104d3a308` in `ExecuteWTKernel`; `0x104aefbdc` in `LoadProtocol::LoadSavegame` | independent confirmation of write and load protocol paths |
| `0x107ca55de` | `StoryGlobals.lsf` | discovered by strings scan; no high-confidence direct ref assigned | supporting filename only |
| `0x107cab30c` | `LoadSavegame` | `0x104aef9a0` in `LoadProtocol::LoadSavegame` | names the server load protocol operation |
| `0x107cab319` | `* ServerLoadProtocol: Load savegame request: %s` | `0x104aefa18` / `0x104aefac8` in `LoadProtocol::LoadSavegame` | path-bearing load log site |
| `0x107c03b2c` | `-loadSaveGame` | command-line initialization path | supports the harness' explicit-save load experiment |

Also present were `metadata.lsf`, `config.lsf`, `Savegames`, `SaveCopyFinalSaveGame`, and `SaveLevel_SaveFile`; they did not produce a better two-direction visitor boundary than the Osiris helper.

## Why the recommended seam is hookable

The first instructions at `0x104b51a9c` are a normal, non-inlined prologue:

```asm
0x104b51a9c  sub  sp, sp, #0x40
0x104b51aa0  stp  x22, x21, [sp, #0x10]
0x104b51aa4  stp  x20, x19, [sp, #0x20]
0x104b51aa8  stp  x29, x30, [sp, #0x30]
0x104b51aac  add  x29, sp, #0x30
0x104b51ab0  mov  x19, x1
0x104b51ab4  mov  x20, x0
0x104b51ab8  ldr  x0, [x1, #0xb0]
0x104b51abc  ldr  x8, [x0]
0x104b51ac0  ldr  x8, [x8, #0x70]
...
0x104b51acc  blr  x8
```

The build-gate bytes for the first 16 bytes are:

```text
ff 03 01 d1  f6 57 01 a9  f4 4f 02 a9  fd 7b 03 a9
```

The first four instructions are all position-independent stack/register operations. A near replacement overwrites 4 bytes; a far replacement uses the helper's 16-byte absolute branch. In either case the trampoline can replay the overwritten entry instructions safely and continue at the corresponding boundary.

### Important `arm64_safe_hook()` warning

Use:

```c
arm64_hook_at_offset(target, 0, replacement, &original);
```

Do **not** use `arm64_safe_hook()` for this target without fixing its selector. `arm64_analyze_prologue()` currently records the first apparently safe offset while scanning and does not revise it when a later PC-relative instruction is discovered. On this function it can retain `+4`. Hooking at `+4` is incorrect: the live call executes `sub sp, sp, #0x40`, and the returned “original” trampoline replays the skipped `sub` a second time, corrupting the stack.

This warning follows directly from `src/hooks/arm64_decode.c:355-367` and the trampoline's skipped-instruction replay in `src/hooks/arm64_hook.c:308-350`. Explicit offset 0 avoids the double execution. The helper's MAP_JIT write-gate, absolute-branch fallback, instruction-cache invalidation, and `vm_protect` behavior are otherwise suitable for this clean entry (`arm64_hook.c:34-86`, `129-145`, `151-203`, `259-397`).

## Visitor ABI evidence available before E1.2

The wrapper cluster at `0x1060ef72c` independently establishes the nested pointer and initial vtable slots:

| Wrapper | Preferred VA | Operation on nested `[visitor + 0xb0]` |
|---|---:|---|
| `ls::SavegameVisitor::IsReading() const` | `0x1060ef72c` | tail-dispatch vtable `+0x48` |
| `ls::SavegameVisitor::Save(Path const&)` | `0x1060ef73c` | tail-dispatch vtable `+0x50` |
| `ls::SavegameVisitor::Load(Path const&)` | `0x1060ef74c` | tail-dispatch vtable `+0x58` |
| `ls::SavegameVisitor::Load(Path const&, FileReader&)` | `0x1060ef75c` | tail-dispatch vtable `+0x60` |
| `ls::SavegameVisitor::TryEnterRegion(...)` | `0x1060ef76c` | tail-dispatch vtable `+0x68` |
| `ls::SavegameVisitor::EnterRegion(...)` | `0x1060ef77c` | tail-dispatch vtable `+0x70` |

`ls::ObjectVisitor::IsReading() const` at `0x1064e5eac` is only four instructions:

```asm
ldr  w8, [x0, #0x8]
cmp  w8, #0
cset w0, eq
ret
```

Therefore a breadcrumb-only hook can classify direction without calling another engine function: after validating the nested pointer, `*(uint32_t *)(lsf + 8) == 0` is the same result as `IsReading()`.

## Export, vtable, and registration assessment

### Exports

None of the ranked save/load functions is exported. They are local `t` symbols and must be resolved by version-gated main-image RVA plus entry-byte validation (and eventually a signature fallback), not `dlsym`.

### Virtual dispatch

The best seam is nonvirtual: both relevant sites use direct `BL` instructions and no `vtable for esv::OsirisVariableHelper` symbol exists. This is acceptable because the main-binary entry is inline-hookable.

Related vtables do exist:

```text
0x1086ade40  vtable for eoc::SavegameVisitor
0x1087fc5d0  vtable for esv::LoadProtocol
0x108804010  vtable for esv::SaveSystem
0x108804a68  vtable for esv::SessionLoadSystem
0x10883e518  vtable for ls::SavegameVisitor
0x108856118  vtable for ls::LSFObjectVisitor
```

They lie in `__DATA_CONST` (`vmaddr 0x1083a0000`, segment flag `0x10`, `SG_READ_ONLY`). No safely writable runtime vtable slot was established. The visitor thunks identify virtual slots, but vtable patching is not the recommended route.

### Registration or callback API

No ideal public serialization-registration callback was found.

The binary does contain ten local template instantiations named `ls::savegame::Framework::RegisterComponents<Loader, ...>` (examples begin at `0x1044b1478`, `0x1044c5714`, and `0x1044e2898`) and many internal loader/type-context records. These are fixed, compile-time ECS component registrations; the only central non-template operation found was local `ls::savegame::Framework::Save` at `0x1062a88c4`. There is no exported generic “register serializer/handler” function, no recovered callback list suitable for appending an arbitrary named ObjectVisitor region, and no writable dispatch table proven by the static scan.

Lifecycle notifications such as the port's Osiris `SavegameLoaded` event occur after load and do not expose the read visitor or a save-write callback. They cannot substitute for this seam.

**Registration verdict:** none demonstrated. The direct `OsirisVariableHelper::SavegameVisit` entry remains the best surface.

## Required runtime experiment (follow-up session; do not mutate data in this spike)

### Temporary breadcrumb hook

Build a temporary, build-gated diagnostic hook only. It must not enter a region, write visitor data, call Lua, or change a save. Install it before any save is loaded:

```c
typedef bool (*SavegameVisitProc)(void *self, void *visitor);
static SavegameVisitProc s_original;

static bool SavegameVisitBreadcrumb(void *self, void *visitor)
{
    void *caller = __builtin_return_address(0);
    void *lsf = visitor ? *(void **)((uint8_t *)visitor + 0xb0) : NULL;
    uint32_t mode = lsf ? *(uint32_t *)((uint8_t *)lsf + 8) : UINT32_MAX;

    // Append one JSONL record to /tmp/bg3se_savegame_hook.jsonl:
    // monotonic sequence, timestamp, pthread id, self, visitor, lsf,
    // caller RVA, mode, and direction (mode == 0 means READ).
    breadcrumb_log("pre", self, visitor, lsf, caller, mode);
    bool result = s_original(self, visitor);
    breadcrumb_log("post", self, visitor, lsf, caller, mode);
    return result;
}
```

Installation requirements:

1. Resolve main-image RVA `0x04b51a9c` only for build 4.1.1.7209685.
2. Compare all 16 entry bytes with the byte gate above; fail closed on mismatch.
3. Call `arm64_hook_at_offset(target, 0, SavegameVisitBreadcrumb, (void **)&s_original)`.
4. Normalize `caller` against the runtime Mach-O header. Expected caller RVAs are `0x04d2cd3c` for write and `0x04d8a020` for read.
5. Use a reentrancy guard and allocation-minimal, thread-safe logging. Record the owning thread; do not assume it is the main thread.

### Disposable-save procedure and exact harness commands

The installed fixture is named `vetting_base`; its recorded source is `Ebonlake Grotto - 27h 19m`. Use that fixture only as the starting state. The new disposable manual save slot for this experiment is exactly **`Harness__vetting_base`**; do not overwrite the fixture source slot.

```bash
cd /Users/tomdimino/Desktop/Programming/game-modding/bg3/bg3se-macos
rm -f /tmp/bg3se_savegame_hook.jsonl

# Restore the known starting fixture, then load its recorded source save.
PYTHONPATH=tools python3 -m bg3se_harness save restore vetting_base
PYTHONPATH=tools python3 -m bg3se_harness launch \
  --save "Ebonlake Grotto - 27h 19m" --timeout 180
```

After the session is fully loaded, use BG3's Save Game UI once to create a **new** manual save named `Harness__vetting_base`. Wait for save completion. This is the write observation; the harness currently has no save-write command, so this one operator action must not be disguised as automation.

```bash
PYTHONPATH=tools python3 -m bg3se_harness quit
PYTHONPATH=tools python3 -m bg3se_harness launch \
  --save "Harness__vetting_base" --timeout 180
```

That launch is the read observation. Once loaded, overwrite only `Harness__vetting_base` through the Save Game UI, quit, and launch it again. Repeat the overwrite/quit/load sequence until there are **three independent write/read pairs**. Then inspect the dedicated breadcrumb file:

```bash
rg '"direction":"(WRITE|READ)"' /tmp/bg3se_savegame_hook.jsonl
PYTHONPATH=tools python3 -m bg3se_harness crashlog --ring
PYTHONPATH=tools python3 -m bg3se_harness quit
```

Use the BG3 UI to delete `Harness__vetting_base` after the experiment if cleanup is desired; do not shell-delete a save directory.

### Pass/fail gate

The runtime half passes only if all of the following hold:

- Each of three saves produces a pre/post pair at caller RVA `0x04d2cd3c`, with non-null visitor and nested LSF visitor, and `mode != 0` (`WRITE`).
- Each of three loads produces a pre/post pair at caller RVA `0x04d8a020`, with non-null visitor and nested LSF visitor, and `mode == 0` (`READ`).
- The hook records thread identity and ordering; the original returns normally; no crash-ring or game-log regression appears.
- Observations are tied only to the disposable slot and are repeatable across separate processes, not merely save-menu or post-load lifecycle events.

If write is observed but read is not, retry the same experiment with breadcrumb-only hooks at `SessionLoadSystem::Update` and `LoadProtocol::LoadSavegame` to determine where traversal stops. If read is observed but write is not, bracket `SaveSystem::DoSaveFlow` and `SaveEntities`. If the recommended entry hook cannot be installed safely despite the byte gate, E1.1 is no-go for the current helper until the entry-hook implementation is corrected; do not fall back to arbitrary mid-function patching.

## Gate conclusion

| Question | Static answer |
|---|---|
| Candidate save-write visitor boundary? | **Yes** — `OsirisVariableHelper::SavegameVisit`, directly called by `DoSaveFlow`. |
| Candidate load-read visitor boundary? | **Yes** — the same function, directly called by `SessionLoadSystem::Update`. |
| Exported? | No. Resolve by build-gated main-image RVA/signature. |
| Writable virtual slot? | Not needed; no safe writable slot was demonstrated. |
| Clean `arm64_hook` prologue? | **Yes at entry offset 0**; do not use the current automatic offset selection. |
| Visitor/stream/path arguments? | Shared seam: `x1=SavegameVisitor*`, nested ObjectVisitor at `+0xb0`; path-bearing backups are `LoadProtocol::LoadSavegame` and visitor `Save/Load(Path)`. |
| Registration/callback API? | None suitable was found. Internal template component registration is not an extender serialization callback. |
| Static plan verdict | **PROMISING**. Proceed to the breadcrumb-only disposable-save runtime gate. |

## Runtime experiment procedure

Build and launch every experiment process with the spike explicitly enabled:

```bash
export BG3SE_SAVEGAME_SPIKE=1
```

Restore and load the `vetting_base` fixture using the commands above, then create
the disposable manual save `Harness__vetting_base`. A successful write emits an
armed line followed by an entry line of this form in `bg3se.log`:

```text
[SavegameHook] armed: build=4.1.1.7209685 target=<address> env=BG3SE_SAVEGAME_SPIKE=1
[SavegameHook] entry call=<N> self=<pointer> visitor=<non-null> nested=<non-null> nested_read=ok mode=<nonzero> mode_read=ok direction=WRITE caller_rva=0x4d2cd3c thread=<id>
```

Quit and launch `Harness__vetting_base` with the same environment. Its load must
emit the same entry format with `mode=0` and `direction=READ`. Repeat the
load caller RVA must be `0x4d8a020`. Repeat the save/quit/load sequence for
three independent pairs, using only
`Harness__vetting_base`; inspect the crash ring as described above and do not
launch with this environment variable during normal use.

The E1.1 runtime gate is **go** only when both directions are observed
repeatably, with non-null visitors, successful safe reads, normal original
returns, and no crash/log regression. A missing direction, unreadable visitor,
failed byte/hook gate, or regression is **no-go** until investigated; write-only
observation does not pass the gate.
