# Wave Campaign Progress — Parity Closure + CLI Automation + Top-10 Vetting

Plan: `docs/plans/2026-07-28-001-feat-parity-closure-codex-wave-campaign-plan.md`
Append-only. Each entry: date, wave/goal, action, evidence, verdict.

---

## 2026-07-28 — Wave 0: Truth & Baseline

**Housekeeping (Claude)** — DONE
- Committed campaign artifacts (wave plan, e2e review, CLI goal spec, May plans, progress.json) as `f701dc1`.
- `.gitignore` extended: `.screenshots/`, `.subdaimon-output/`, `__pycache__/`, `*.pyc`. Stale tracked `.pyc` removed.
- `gold.*.log` / `network.*.log` already covered by existing `*.log` rule.

**Goal 0.2 — osiris_call_by_id fail-closed (from Codex review 2026-04-02)** — ALREADY RESOLVED
- Verified `src/injector/main.c:2659-2670`: the unsafe `pfn_InternalCall` fallback is gone; function fails closed with explanatory comment ("InternalCall takes COsiParameterList*, NOT OsiArgumentDesc*"). Fixed between 2026-04-02 review and the May 16 commits. No work needed.

**Goal 0.1 — documentation reconciliation (codex docs profile)** — DONE (2026-07-28, ~126k tokens)
- ROADMAP.md, CLAUDE.md, README.md, agent_docs/api-status.md now tell one story: **v0.37.1**, overall **~94.7%** (unweighted mean of supported-surface matrix rows, source cited), honest per-namespace numbers (Types 9/15=60%, Math 57/59=96.6%, Level 15/21=71%, Audio 13/17=76%), Stats "function-count parity" + stub footnote, Entity stub footnote (incl. stubbed component writes), Ext.UI/Debugger moved outside the denominator with the scope-corrected exclusion statement (Ext.UI excluded; Debugger, replication, Virtual Textures, Input Injection deferred).
- Known residual for Wave 3: Ext.Localization still listed 100% (GetLanguage/CreateHandle) while the Windows baseline expects GetTranslatedString/UpdateTranslatedString — reconcile when Goal 3.2 implements them.

**Wave 0 exit gate** — ✅ PASSED (2026-07-28)
- Offline suites green (55+41+23+23), docs agree on numerator/denominator/exclusions, progress doc live, Goal 0.2 verified already-fixed. Wave 1 unblocked.

---

## 2026-07-28 — Wave 1: Boot Unblock

**Goal 1.1 live retest #1** (17:42 local) — MAJOR FINDINGS, hypothesis falsified
- Command: `test --headless --continue --tier 1` (clean stopped state, reconciled registry: all 14 PAKs, 10 enabled).
- **Session 1 (17:42:10): `-continueGame` WORKED** — save auto-loaded (player UUIDs discovered, Osiris events streaming, no menu input needed) — then **crashed at 17:42:41**, ~31s after load: `gui::HotbarSystem::Update` + 3076, EXC_BAD_ACCESS `KERN_INVALID_ADDRESS at 0x10` (null + field offset), GameThread, vanilla BG3 frames (`.ips` 2026-07-28-174248). **Identical signature to May 16 → mod-registry-reconciliation hypothesis FALSIFIED.**
- Session 2 (17:42:44): watchdog relaunch reached socket at menu in 6.3s; harness ran Tier 1 there — **100/109 passed, 9 failed** (failure list lost to output truncation; rerun needed) — hid window, restored graphics, exited 0.
- Test-honesty note for Goal 2.4: the pipeline reported success without a loaded session; Wave 1's gate additionally requires `session_loaded` evidence, and tier-count drift (109 vs documented 93) needs doc sync.
- Revised stall model: menu stall is *conditional* — first boot auto-continues fine; the stall likely occurs on post-crash relaunch (safe-mode/mod-verification dialog path). Codex researcher's bypass ladder (full text: `/tmp/codex-researcher-a1UiqaAX`): (1) boundary-proving logs, (2) `SavegameManager::QuickLoad()` one-shot, (3) `OnContinueGameCommand` SavegameManager leaf, (4) `OnSkipModValidationAndStartGame` continuation for the dialog case, (5) save-list-async timing, (6) `SetTargetState`, (7) ECS component injection (last).
- **Next probe (decisive control): vanilla launch (SE unpatched) + `-continueGame` on the same save → crash?** Splits SE-caused corruption vs mod/save incompatibility.

**Goal 1.1 vanilla control run** (18:0x local) — TWO BREAKTHROUGHS
- Vanilla (unpatched) + `-continueGame`: game parks at "Press any key" splash — video/splash skip is SE-provided automation; the flag alone does not bypass it. System Events keystroke dismisses the splash (raw input path) but Return/arrow keys do NOT reach the Noesis menu — input asymmetry confirmed with real window focus.
- **Breakthrough 1 — CGEvent clicks DO land** when the window is visible + `activate_bg3()` + cursor warp: clicked Continue successfully at computed coords (window 1920x1080 at origin, `is_onscreen:false` in Quartz). The May "clicks don't land" verdict is bounded to hidden/offscreen windows. Menu automation is unblocked.
- **Breakthrough 2 — root cause surfaced**: clicking Continue opened BG3's **Mod Verification dialog**: six mods "Missing or Disabled" — IN_Core_1_03, Camp Event Overhaul, Better Inventory UI, ACT1 Capes and Cloaks, Origin Dialog Tags, Facial Animations — despite all six enabled in modsettings.lsx (untouched since May 16) and clean PAK scans. The main-menu banner ("load order is likely being reset… invalid meta.lsx") states the mechanism: the game resets/loses the load order at boot. Save is from 2025-12-23 and references those mods; loading without them matches the `HotbarSystem::Update` null-deref exactly. Hypothesis v2: **SE's `-continueGame` path loads without surfacing/honoring this dialog → crash; not an SE code bug.**
- Enabled all six via CGEvent checkbox clicks, clicked Start Game → **save loaded and stayed alive 240s+, fully playable** (Underdark/Sussur Tree, 4-member party, hotbar rendering correctly, no crash). **The save is NOT corrupt and the hotbar renders fine when mod state is consistent.**
- ⚠️ **Destructive side effect discovered**: confirming the Mod Verification dialog made BG3 **rewrite `modsettings.lsx`, silently dropping the three SE-dependent mods** (CommunityLibrary, 5eSpells, Combat Extender) — 11 module entries → 8. Vanilla BG3 prunes SE-dependent mods it cannot validate without the extender. Post-dialog snapshot preserved at `scratchpad/modsettings-after-verification-dialog.lsx`.
  - **Harness requirement (Wave 1 code change)**: snapshot + restore `modsettings.lsx` around launches exactly as `graphicSettings.lsx` is already handled, and never run a vanilla control without that guard.
- Game force-quit immediately after the observation window to prevent an autosave baking the reduced mod set into the save.

**Root-cause model v3 (evidence-backed)**
- The main-menu banner ("Your load order is likely being reset…") plus the verification dialog show BG3 considered 6 enabled mods missing/changed at boot. SE's `-continueGame` path loads the save **without surfacing or satisfying that dialog**, so the save's hotbar references content from mods the engine did not actually load → null deref in `gui::HotbarSystem::Update`. The crash is a *consequence of unresolved mod verification*, not an SE memory bug — consistent with the faulting frames being pure vanilla code.
- **Consequence for Wave 1 strategy**: the menu-bypass RE track is demoted. CGEvent clicks are proven to land on a visible+activated window, so the practical path is *satisfy the menu and dialog with clicks, then hide the window after `session_loaded`* — no RE required. The `SavegameManager::QuickLoad` ladder stays as fallback.

**Controlled A/B — SE IS THE CAUSE** (18:57, decisive)
- Same save, same BG3-blessed 8-mod config, only variable = the SE dylib:
  - **Vanilla (unpatched)**: loaded, playable, stable 240s+.
  - **SE-patched + `-continueGame`**: crash at **~35s**, identical `gui::HotbarSystem::Update +3076`, `EXC_BAD_ACCESS` at `0x10`, GameThread (`.ips` 2026-07-28-185709).
- **Root-cause model v3 falsified.** Mod verification was already satisfied by the game itself; mods were not the variable. Crash reproduces with BG3SE injected and disappears without it → **BG3SE causes the hotbar crash**. Prior May-16 attribution to mod-state mismatch was wrong; that fix was necessary housekeeping but not the cure.
- Prime suspects (crash is in a UI system iterating a WorldView containing three one-frame components — `ecl::party::GroupsChangedEventOneFrameComponent`, `PartyChangedEventOneFrameComponent`, `gui::registration::VMPassiveUnregisteredOneFrameComponent`): SE's recently added **one-frame-component event polling** (~10 events from the Qedeshot swarm), entity create/destroy hooks with their deferred queue, component TypeId/layout registration, or the ExecuteFunctor Dobby hook. Consistent ~31–35s latency suggests a specific event firing rather than random corruption.
- **Bisection underway** using the Issue #65 diagnostic toggles found in `src/injector/main.c` (`BG3SE_NO_HOOKS` skips all Dobby hooks; `BG3SE_MINIMAL` skips all subsystem init). Harness passes environment through (`launch.py:414`).

**Methodology fix — trust the game's own log, not SE's**
- First `NO_HOOKS` probe reported "alive 120s" but BG3 never left `Init` (never loaded) — a false negative. SE-side markers ("Discovered player UUID") cannot verify sessions in hooks-off arms because the code that logs them is disabled.
- **SE-independent indicator adopted**: BG3's own `network.*.log` records `GameStateMachine` transitions (`CLIENT/SERVER STATE SWAP … LoadSession → LoadLevel → PrepareRunning`). Probe script `scratchpad/se_bisect3.sh` now gates every verdict on reaching a running session, and drives splash/menu itself (keystroke + CGEvent Continue click) so hooks-off arms can load at all.
- Re-reading the logs with this indicator: the 19:02 `NO_HOOKS` run **did** reach `PrepareRunning` and **still crashed** with the identical signature. **Dobby hooks are exonerated.**
- Also learned: SE's hooks are what make `-continueGame` actually auto-load — vanilla parks at the splash, so the flag alone never bypasses it. Any hooks-off arm must be driven manually.

**Current suspect set** (SE surface still active under `NO_HOOKS` — load-time init): game-memory *writes* are the standouts — `focus_hack` forcing the BaseApp `0x142` flag and `global_switches` writing into the GlobalSwitches struct (a wrong offset would corrupt neighbours and surface later in an unrelated system), plus the ImGui Metal backend / NSView swizzling, the CGEventTap input hooks, and the GCD console poll timer. Probe B (`BG3SE_MINIMAL`, all subsystem init skipped) is in flight to split init-vs-injection.

**focus_hack exonerated as sole cause** (19:1x re-read of NO_HOOKS run log)
- `bg3se_2026-07-28_19-03-08.log` (the NO_HOOKS run that crashed): focus_hack polled 20× and every attempt logged `BaseApp::s_AppInstance not yet set (NULL)` — **no `Forced focus` write ever occurred**, yet the crash reproduced with the identical signature. The two earlier crashed runs (17:42, 18:56) *did* write the flag, so focus_hack is not necessary for the crash. Combined with the hooks-off result: **both Dobby hooks and the focus_hack memory write are exonerated.**
- Surviving suspect surface (constructor-time init active under NO_HOOKS): CGEventTap input hooks, ImGui NSView swizzling + Metal backend, focusless_input splash autodismiss (NSEvent injection — active, `BG3SE_AUTO_DISMISS_SPLASH=1` is the harness default), GCD console poll timer, video_skip settings edits, and the static Mach-O patch itself.
- Probe B (`BG3SE_MINIMAL`) verdict: INCONCLUSIVE — never reached a session; its "ALIVE at 150s" is additionally unreliable because the harness watchdog can relaunch after a crash (pgrep can't distinguish survivor from replacement).
- Mod state confound ruled out for next runs: current `modsettings.lsx` matches the blessed 8-mod snapshot byte-for-name.
- **Codex fleet dispatched (user-directed)**: debugger (enumerate SE surface under NO_HOOKS + disassemble faulting site imageOffset 40039100 + rank suspects + design decisive arms), syseng (true-headless CLI architecture: in-process input injection, modsettings guard, honest session evidence), reviewer (adversarial audit of all three claims + full launch timeline + probe-script holes). Reports land in `docs/bugs/codex-*-2026-07-28.md`.

**🎯 ROOT CAUSE FOUND — stale code-patch offsets patch the middle of `HotbarSystem::Update`** (19:46, codex debugger + Claude verification)
- Codex debugger report: `docs/bugs/codex-debugger-nohooks-2026-07-28.md`. Mechanism: the game updated to build **4.1.1.7209685** (offsets were derived on 4.1.1.6995620); the three sentinel probes validate *data* addresses only, so stale *code* offsets sailed through. `OFFSET_GET_CLASS 0x0262f184` (staticdata_manager.c:33) is no longer `ImmutableDataHeadmaster::Get<eoc::ClassDescriptions>` (now `0x02614874`) — it is **`gui::HotbarSystem::Update+0xacc`** (function starts `0x10262e6b8`, per nm). Dobby installs a mid-function trampoline there; when the hotbar loop reaches it, execution resumes with the wrong stack frame, the saved `WorldView*` reads back as 0, and the code faults at `Update+3076` on `[x12,#0x10]` → the exact `KERN_INVALID_ADDRESS 0x10` in all three .ips (all with `x12=0`). The ~31–35s latency is just time-to-reach-the-patched-block, not delayed corruption.
- **`BG3SE_NO_HOOKS` was never authoritative**: it skips only the four Osiris hooks in `install_hooks()`, then jumps to `init_subsystems`, which still runs `staticdata_manager_init()` (6 Dobby patches) and `video_skip_init()` (1 patch). The NO_HOOKS run therefore still carried the fatal patch — the "Dobby hooks exonerated" conclusion was wrong *in the interesting direction*: hooks were exonerated, but the mis-aimed patch wasn't a hook at all in the intended sense.
- Claude independently verified: the define, the symbol addresses via `nm` on the installed binary, and the ASLR math in the crashed run's log (`0x10262f184 + slide 0x27f0000 = 0x104e1f184`, the address StaticData logged patching).
- **Full staleness audit (nm, build 7209685)**: ALL six StaticData code targets are stale — GetFeats `0x01b752b4→0x01b59814`, Background `0x02994834→0x0297a068`, Origin `0x0341c42c→0x03401c84`, Class `0x0262f184→0x02614874`, Progression `0x03697f0c→0x0367d764`, ActionResource `0x011a4494→0x011889f4` — plus Ext.Net `ADDR_GETMESSAGE 0x1063d5998→0x1063c4550` (`net::MessageFactory::GetFreeMessage`). VideoSkip's `BinkManager::LoadVideo` is the only correct hardcoded code patch. Every SE session on the current build has been installing up to seven blind patches; the Class one happened to detonate first.
- **Evidence correction (from the same report)**: `.ips 190312` belongs to **PID 36512**, whose SE log is `bg3se_2026-07-28_19-02-36.log` — in that (crashed) process focus_hack **did** write `Forced focus: 0 -> 1` and focusless input injected zero events. The `19-03-08.log` I earlier greped (20× NULL polls, no write) is **PID 37698, the post-crash relaunch**. My "focus_hack exonerated" entry above is therefore retracted as *unproven by that run* — though it's now moot: the mid-function patch is sufficient to explain all three crashes. Lesson: pair logs to crashes by PID + `procLaunch`, never by filename timestamp adjacency.
- **Fix plan (Wave 1, implementing now)**: (1) update the seven stale targets to nm-derived build-7209685 addresses; (2) make `BG3SE_NO_HOOKS` authoritative over StaticData/VideoSkip code patches and fix its false "ALL Dobby hooks SKIPPED" log; (3) add an offline harness test that runs `nm` against the installed binary and asserts every hardcoded code-patch target still resolves to its intended symbol (kills this bug class in CI); (4) leave `version_detect` expected-build and functor hooks untouched — functor offsets are still old-build and must stay version-gated off until re-derived. Decisive test after rebuild: `launch --continue` soak ≥240s with session evidence.

**✅ FIX IMPLEMENTED AND CONFIRMED — hotbar crash resolved** (20:0x)
- Changes: seven stale code-patch targets corrected to nm-derived build-7209685 addresses (`staticdata_manager.c`: GetFeats, GetAllFeats, five `Get<T>`; `protocol.h`: `ADDR_GETMESSAGE` → `net::MessageFactory::GetFreeMessage`), stale `OFFSET_MSTATE_PTR` data pointer corrected (`0x083c4a68→0x08947ba0`), `BG3SE_NO_HOOKS` made authoritative over StaticData/VideoSkip code patches with honest log lines (`main.c`), and new offline audit `tests/harness/test_offset_audit.py` (10 cases: parses live source defines, asserts each resolves via `nm` to its intended symbol — kills the stale-offset bug class in CI; skips when the binary is absent).
- Offline: rebuild clean, tier0 41/41, harness pytest 65/65 (55 + 10 new).
- **Live soak (PID-tracked, per methodology-audit rules): session loaded at t=30s via BG3's own network log; PID 43318 alive after 300s — ~10× the former crash latency. VERDICT: fix holds.**
- Lateral-strategy report landed (`docs/bugs/codex-lateral-strategy-2026-07-28.md`): 9 ranked force-multipliers. Adopted now: symbol-derived offset audit (embryo of its UUID-manifest proposal 3). Queued for Wave 1/2: crash-PC↔hook-target proximity miner (prop 5, S effort), LLDB Dobby-API census during confirmation runs (prop 1), executable-page attestation (prop 2). Deferred: batch hook-admission refactor (prop 7, needs approval), null-image matrix + export quarantine (props 8-9, residual-only).

**Address-space migration to build 4.1.1.7209685** (20:0x–20:1x, follow-through on the tier-1 Stats failures)
- Post-fix tier1 rerun still 100/109; the 9 failures decoded to one cause: **`RPGStats::m_ptr` stale** (`0x1089c5730` → actual `0x1089cd730`) — `Ext.Stats.Get` returned nil for everything. Widening the audit exposed the full blast radius of the game update:
  - **Data singletons (+0x8000, nm-verified, fixed)**: RPGStats::m_ptr (stats_manager.c + fixed_string.c), esv::EocServer::m_ptr `→0x1089968b8`, ecl::EocClient::m_ptr `→0x108994968`, Spell/Status/Boost prototype managers, InterruptPrototypeManager `→0x1089ba8f0` (old value was an ADRP guess, not the symbol), ResourceManager noted (level/audio/resource resolve at runtime), GlobalStringTable `→0x108af4cd8` (re-derived by hand-decoding the ADRP+LDR in `ls::gst::Get`; the installed FAT binary's arm64 slice moved to `0xf558000`).
  - **Code addresses (nm-verified, fixed)**: `ls::FixedString::Create` `→0x1064a8a74`, `ecs::EntityStorageContainer::TryGet` `→0x10635ac94`, `eoc::SpellPrototype::Init` `→0x101f56cb4`, LEGACY_IsInCombat/GetCombatFromGuid.
  - **TypeIds regenerated**: `tools/extract_typeids.py` rerun → 1,999 macros for the new build (old header had 2,002; the update **deleted 3 components** — esv Turn(Started|Ended)EventOneFrame, esv::stats::LevelChangedOneFrame — pruned from `generated_component_registry.c`). Spot-check had shown old TypeId addresses now hold *other symbols'* data (+0x8000), meaning component TypeIndex reads since the game update could silently return the wrong component's index.
  - **Shifts are NOT uniform**: `BaseApp::s_AppInstance` did not move (why focus_hack kept working). So no blind delta-patching: every address individually symbol-derived; unverifiable globals (PassivePrototypeManager ptr, OFFSET_MEMORY_MANAGER, GLOBAL_SWITCHES_PTR_VA) left at old values and marked UNVERIFIED pending a Ghidra pass.
  - **Audit hardened**: `test_offset_audit.py` now 24 cases covering every symbol-derivable hardcoded address. Offline: tier0 41/41, pytest 79/79.
- Tier-1 live rerun on the migrated build in flight — expecting the 9 Stats failures to flip green.

**Offline baseline gate** — GREEN (2026-07-28)
- `src/core/version.h` = **0.37.1** (ground truth for doc reconciliation).
- pytest tests/harness: **55 passed** (2.67s). tier0: **41/41**. tests_nexus: **OK**. tests_wiki: **OK**.
- No C/ObjC changes yet this wave, so no rebuild required for the baseline.

**Parallel research track: plasma-ai/fractal integration (user-requested 2026-07-28)** — COMPLETE
- Two subagent reports (architecture survey + insertion-point analysis) synthesized into `docs/plans/2026-07-28-002-feat-fractal-orchestration-integration-plan.md`.
- Verdicts: **ADOPT** Ghidra component-size extraction swarm as Wave 3 pilot Goal 3.4 (≥800/922 sizes, ≤$20, ≤4h, output confined to `ghidra/offsets/`, never builds/deploys/launches game); **DEFER** code-wave orchestration (until pilot verdict), vetting fan-out, crash bisection; **REJECT** live-game orchestration (tmux nodes lack GUI/Accessibility — origin plan's Claude-only rule confirmed structurally).
- Key findings: Claude Code native backend with hard budget caps (G2 pass); action space is LLM-mediated only — no deterministic exit-code gating (offline gates stay in setup.sh/Claude); branch-only worktree isolation leaves the shared dylib deploy target and single BG3 instance unprotected (design rule: fractal nodes never build or touch the game); Ghidra HTTP bridge is single-threaded, so swarm value is parse/recovery, not parallel queries.

---

## 2026-07-28 (evening 5): Lua state race — ROOT CAUSE + lua_gate fix

**Crash under analysis:** PID 97193 (driver run 20:41), `EXC_BAD_ACCESS at 0x0` on ServerWorker thread, PC=0 — jump through NULL function pointer, ~100s into a healthy session, mid `AutomatedDialogStarted` callback dispatch. Ring buffer empty; SE log ends on the dispatch line.

**Root cause (verified in code):** the Lua VM is shared by four threads with no mutual exclusion:
- game/ServerWorker thread — `fake_Event` tick block + `dispatch_event_to_lua` (`src/injector/main.c`)
- GCD console poll timer — `console_poll` every 100ms (its `polling` atomic only prevented SELF-re-entry)
- Metal render thread — `lua_imgui_fire_event` (`src/lua/lua_imgui.c`)
- input hook thread — `lua_hotkey_callback` (`src/input/lua_input.c`)

Console commands sent during a dialog-storm save (Zhentarim dungeon, dozens of Osiris callback dispatches/ms) ran Lua on the GCD thread concurrently with ServerWorker dispatches → corrupted VM internals. Smoking gun: `Ext.Stats.GetAll` probe returned `Error: table index is nil` from a chunk with no computed table keys, then the ServerWorker died on the same dispatch path that had succeeded hundreds of times earlier in-session.

**Why the earlier 300s soak passed:** nobody touched the console during it. Historical in-game console testing survived on low event rates — luck dosed by frequency, not correctness.

**Answer to "did the address migration cause this?":** No. Latent since the GCD poll timer was introduced; the migration is unrelated to this crash class.

**Fix:** new `src/lua/lua_gate.c/h` — recursive pthread mutex (pthread_once init). Applied at every thread entry point: GCD timer handler (trylock — skip poll, retry next tick), `fake_Event` tick block, `dispatch_event_to_lua`, `shutdown_lua` (lock across `lua_close`), `lua_imgui_fire_event`, `lua_hotkey_callback`, `callback_registry_invoke`. Recursive so nested same-thread dispatch is safe; `orig_Event` is called OUTSIDE the gate to avoid serializing game logic against the console.

**Validation in progress:** session_driver --launch --soak 180, then hammer the socket console with stats probes during the dialog storm — the exact sequence that killed 97193.

---

## 2026-07-28 (evening 6): lua_gate validated; two environmental red herrings; MAP_JIT trampoline bug found + fixed

**lua_gate validation — PASSED under killer conditions.** PID 63319: session in 39s, then 10 consecutive socket stats probes mid-dialog-storm (all clean: StatsObject returned, GetAll=15,774 stats) plus full in-session tier-1 run: **109/109 passed** (was 100/109). The 9 stats failures were pure test timing (tier 1 previously ran at the menu, before stats init). Offline gates still green (tier-0 41/41, pytest 79 passed).

**Red herring 1 — PID 63319 "death" was not a crash.** `runningboardd: termination reported by proc_exit`, no crash report, no SE shutdown log. Cause chain: system-wide jetsam storm (100 JETSAM_MEMORY_IDLE_EXIT kills in ~6min, including ReportCrash itself) had killed Steam earlier; both 21:0x runs logged `failed lookup: com.valvesoftware.steam.ipctool` at launch. BG3 direct-launched without a Steam client self-exits after a DRM grace window (~151s observed). Lesson: **a report-less exit + dead ReportCrash proves nothing about crashes; check `log show` (`/usr/bin/log` — plain `log` is a zsh builtin that silently fails).**

**Red herring 2 — with Steam freshly restarted, direct launch bounced** (clean exit 0 at 1.3s = SteamAPI_RestartAppIfNecessary) and the harness attempt-2 raced Steam's own relaunch → two instances ~1s apart.

**Real bug found via that race:** `EXC_BAD_ACCESS KERN_PROTECTION_FAILURE` writing trampoline base, `arm64_hook_at_offset` ← `install_feat_getfeats_safe_hook` (PID 83926, 21:18:02). `arm64_alloc_near` allocates trampolines with MAP_JIT, but the trampoline build wrote without `pthread_jit_write_protect_np(0)`. Normally masked; an unlucky ASLR slide forced the far-trampoline fallback where the write faults. **Fixed:** JIT write gate bracketed around the trampoline build in `src/hooks/arm64_hook.c` (arm64-only, re-protected before `sys_icache_invalidate`).

**Validation in progress:** driver --launch --soak 300 with Steam fully up.

---

## 2026-07-28 (evening 7): four-agent synthesis; tier-2 "defect" was a socket identity mixup; P0/P1 hardening implemented

**Codex fleet reports** (all in `docs/bugs/`): reviewer (`codex-review-uncommitted-2026-07-28.md`), debugger (`codex-debug-sessioninit-2026-07-28.md`), syseng (`codex-syseng-launch-lifecycle-2026-07-28.md`), plus the Claude doc-proposal (`claude-md-update-proposal-2026-07-28.md`).

**Debugger verdict — the tier-2 24/54 failure cluster was never a session-init defect.** The manual menu load (PID 2556) initialized identically to `-continueGame` (15,774 stats, 2,081 TypeIds, SessionLoaded fired, tier-1 109/109 in that process). The failures ran against PID 27477 — a fresh menu process that rebound `/tmp/bg3se.sock` after 2556's overlay crash; `Load: 0, Event: 0` at its shutdown. Bonus find: 2556 had TWO libbg3se.dylib images loaded (build tree + app bundle) — two static universes in one process.

**Reviewer verdict on the lua_gate — right idea, incomplete.** Unguarded entries (fake_InitGame/fake_Load, functor hooks, shutdown event, native ImGui cleanup), stale-pointer capture before locking (hotkey/imgui/dispatch), and a logging-mutex ABBA inversion if the Log callback were gated naively. MAP_JIT fix and driver quoting: confirmed correct. Overlay coalescing: sound except a clearOutput ordering race.

**Implemented tonight (all builds green, tier-0 41/41):**
- **Gate completion:** fake_InitGame + fake_Load Lua sections, functor event dispatch (`functor_hooks.c`), destructor Shutdown event, `lua_imgui_cleanup_refs` — all gated; every native entry now resolves its lua_State UNDER the gate.
- **Shutdown lifecycle:** `shutdown_lua` clears every published state pointer while holding the gate (`lua_input_clear_state`, `lua_imgui_set_lua_state(NULL)`, `events_shutdown_log_callback`, `functor_hooks_shutdown`) before `lua_close`; waiters re-resolve to NULL instead of entering a freed VM.
- **Logging inversion fix:** `log_write_v` snapshots callbacks under `g_config.mutex`, invokes them after release; `log_event_callback` now takes the gate safely. Lock order documented: Lua gate → logging mutex.
- **Identity handshake:** new `!identity` console builtin (JSON: pid, version, game_state, session_init, stats_ready, dylib image) so test clients verify who they're talking to before running live tiers.
- **Duplicate-image guard:** constructor elects exactly one dylib image per process via env (BG3SE_LOADED_PID); second image logs + disables itself; destructor no-ops on duplicates.
- **Misc:** functor install now honors BG3SE_NO_HOOKS; console poll logs starvation after 50 consecutive gate misses; overlay clearOutput drops pending lines in the append sync domain; GameStateChanged LoadSession now fires only after a successful orig_Load.
- **Docs:** architecture.md (lua_gate table, MAP_JIT section, poll-timer rewrite), development.md (test counts 41/79/109/67, macOS debugging gotchas), CLAUDE.md (counts, structure, offset-table build stamp), agent_docs/harness.md (session driver, Steam environment, headless truth).

**In flight:** harness-lifecycle subagent implementing the syseng design offline (launch-attempt record + Steam-bounce adoption, socket peer-PID verification, exactly-once graphics restore, Steam/memory preflights, config.lsf windowed attestation, driver rework + lifecycle tests).

**Note:** a stale `steam_appid.txt` containing 480 (Spacewar, Dec 2025) sits at the game install ROOT; ours (1086940) next to the binary is correct. Steamworks reads appid from process cwd — the root file could matter for Steam-relaunched instances. Left untouched pending user decision.

**Windowed-mode ground truth CORRECTED (22:47).** User set Options → Video → Windowed in-game and quit. `config.lsf` untouched (mtime Nov 2025) — the earlier theory was wrong. Instead `graphicSettings.lsx` gained a NEW 17th MapKey: `FakeFullscreenEnabled = 0` (absent = borderless fake-fullscreen, 0 = windowed). Consequences: (1) doctor/preflight can verify windowed mode semantically by parsing the XML — no sha256 attestation of config.lsf needed; (2) the harness must NEVER whole-file-restore graphicSettings.lsx from a pre-windowed snapshot, or it silently reverts the user to fullscreen — transient restores must be per-key (ScreenWidth/Height only). Harness-lifecycle agent re-specced accordingly mid-flight; agent_docs corrected.

## 2026-07-28 (late evening) — Codex pre-commit review resolved

Codex (gpt-5.6-sol, high reasoning) reviewed the full v0.38.0 diff and returned 7 ship
blockers + 5 should-fixes + 2 nits (docs/bugs/codex-precommit-v0380-2026-07-28.md). All
resolved before commit:

- **Version contract**: BG3_KNOWN_VERSION + the three layout sentinels were still
  pre-migration (6995620) while offsets were 7209685 — the exact-version gate was
  rejecting the supported build. All now coherent at 4.1.1.7209685. The eleven
  functor code patches (stripped locals, un-auditable) gate on their own
  FUNCTOR_ADDRS_VERIFIED_BUILD and stay disabled until re-derived via Ghidra;
  a guard test pins the gate's independence from the global version match.
- **Bonus find (mine, not Codex's)**: resource_manager.c was also un-migrated —
  ResourceManager::m_ptr and ResourceContainer::GetResource re-derived via nm and
  added to the offset audit (the GetResource one was a wrong-address CALL).
- **Harness enforcement**: !identity is now actually consumed by wait_for_socket
  (PID match, fail-closed peer check), adoption/cleanup validate exact executable +
  start-time identity (PID reuse can't be SIGTERMed), headless launch hard-gates on
  windowed mode, detached monitor recognizes the Steam bounce, steam_relaunch_timeout
  is a real terminal phase, --allow-memory-pressure exists, driver settle window
  requires full-window stability.
- **C-side**: BG3SE_DISABLE destructor now inert (s_image_active election flag),
  ImGui callback ref resolved under the Lua gate (registry-slot reuse race),
  overlay clear linearized as a sentinel in the append stream, identity JSON
  path-escaped with truncation fallback, log-callback unregister lifetime documented.

Offline gate after resolution: 41 C + 144 pytest (was 130), build clean.
Deferred (documented, not ship-blocking): functor hooks disabled on 7209685 until
Ghidra re-derivation; PassivePrototypeManager unverified (no nm symbol); deeper
integration-level monitor tests.
