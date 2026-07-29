# Codex fleet context — 2026-07-28 evening session state

Shared brief for Codex agents. Read this first; deep history is in
`docs/bugs/wave-campaign-progress.md` (esp. entries "evening 5" and "evening 6").

## Today's confirmed bugs and fixes (all uncommitted, working tree)

1. **Lua state cross-thread race (FIXED, validated).** Four threads entered one
   `lua_State` unsynchronized: game/ServerWorker (`fake_Event` +
   `dispatch_event_to_lua`, src/injector/main.c), GCD console poll timer,
   Metal render thread (`lua_imgui_fire_event`), input hotkeys. Crash: PID 97193,
   20:43:40 .ips, PC=0 on ServerWorker. Fix: `src/lua/lua_gate.c/h` recursive
   pthread mutex at every entry (trylock in the GCD timer). Validated: 10 socket
   stats probes mid-dialog-storm + tier-1 109/109 in-session, twice.

2. **MAP_JIT trampoline write fault (FIXED, unvalidated).** `arm64_alloc_near`
   allocates MAP_JIT; trampoline build wrote without
   `pthread_jit_write_protect_np(0)`. Crash: PID 83926, 21:18:02 .ips,
   KERN_PROTECTION_FAILURE in `arm64_hook_at_offset` ← `install_feat_getfeats_safe_hook`
   when ASLR forced the far-trampoline fallback. Fix: JIT gate bracketed in
   `src/hooks/arm64_hook.c`.

3. **Overlay console NSTextView SIGBUS (FIXED, unvalidated).** PID 2556,
   21:40:33 .ips: `-[BG3SEConsoleView appendOutput:]_block_invoke` →
   `scrollToEndOfDocument:` → TextKit2 `synchronizeTextLayoutManagers` SIGBUS
   under tier-2 output flood (one main-queue block + full relayout per line).
   Fix in `src/overlay/overlay.m`: force TextKit 1 (`(void)_outputView.layoutManager`),
   coalesce lines into batched flush, cap storage at 500KB, `scrollRangeToVisible:`.

## Environmental findings (not code bugs, but need harness/driver handling)

- **Steam-less sessions self-exit** at ~90-151s via clean `proc_exit` (Steamworks
  grace timeout). No crash report; ReportCrash was jetsam-killed. 100 jetsam
  idle-exits in 6 min — memory pressure is routine on this machine.
- **Steam bounce:** with Steam running, direct launch exits 0 at 1.3s
  (`SteamAPI_RestartAppIfNecessary`) and Steam relaunches with same args ~10s
  later. Harness monitor interpreted the bounce as exit → restored fullscreen
  graphics → Steam relaunch went fullscreen (user's screen taken over).
  Mitigation applied: `steam_appid.txt` (1086940) placed next to the game binary.
- **Windowed mode was never actually controlled by the harness.**
  `HEADLESS_GRAPHICS_ENTRIES` in `tools/bg3se_harness/launch.py` upserts
  Fullscreen/FakeFullscreen keys into `graphicSettings.lsx`, but the macOS
  build's file carries only 16 MapKeys (no window-mode key). Real window mode
  persists in binary `PlayerProfiles/Public/config.lsf`.

## Open in-game defect (NOT yet diagnosed)

Tier-2 in-session run (PID 2556, save loaded MANUALLY from menu after a Steam
relaunch) failed 24/54 with a coherent cluster: `Osi.GetHostCharacter()` nil,
Ext.Level singletons nil, StaticData not ready, `Parity.Stats.CanonicalCounts got 0`
(while a console probe seconds earlier returned 15,774), TypeIds 69 instead of
>1500. Hypothesis: `deferred_session_init_tick()` (runs in `fake_Event`,
src/injector/main.c) never completed / SessionLoaded never fired on that
instance; SE log `bg3se_2026-07-28_21-4x` shows
"Stats system not yet ready (m_ptr is NULL - will retry at SessionLoaded)" at 21:40:27.
Also note "Lua context changed: Server -> None" around the manual load.

## Evidence locations

- Crash reports: `~/Library/Logs/DiagnosticReports/Baldur's Gate 3-2026-07-28-{204340,211802,214033}.ips`
- SE session logs: `~/Library/Application Support/BG3SE/logs/`
- Driver diagnoses: `docs/bugs/session-driver-diagnosis.jsonl`
- Driver: `scripts/session_driver.sh` (stall detection, recovery ladder, Steam-bounce wait)
- Uncommitted changes: `git diff` + untracked `src/lua/lua_gate.*`, `tests/harness/test_offset_audit.py`

## Constraints

- Do NOT launch the game, send input events, or touch the Steam install.
- Offline analysis only; recommend fixes with file:line specificity.
