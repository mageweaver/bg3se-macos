# Deferred Session Init diagnosis — 2026-07-28

## Verdict

The reported manual-menu-load defect is a false attribution. The human load in
PID 2556 did not miss deferred session initialization. It followed the same
`COsiris::Load` -> pending -> first Osiris event -> `SessionLoaded` path as the
working `-continueGame` run and completed successfully.

The 24-failure tier-2 cluster came from a different, newly started process:
PID 27477. That process had not loaded a save. Its `COsiris::Load`,
`COsiris::InitGame`, and `COsiris::Event` counters were all zero. It replaced
the pathname of the process-global `/tmp/bg3se.sock`, so the test retry
connected to the menu/startup process instead of the already initialized
PID 2556.

There is also a separate launch defect: PID 2556 had two copies of the same
BG3SE dylib loaded (the build-tree copy and the deployed app-bundle copy).
That can create two sets of dylib statics, including two Lua states and two
console servers, inside one process. It is real and must be prevented, but it
is not the direct explanation for the observed 15,774-versus-zero pair; those
two measurements are logged by different PIDs.

## Deferred-init state machine

The init state is independent of `ServerGameState`:

- `s_session_init_state` starts `IDLE` in `src/injector/main.c:2288-2294`.
- The only normal arming call is `request_deferred_session_init()` after a
  successful `COsiris::Load` (`result != 0`) in
  `src/injector/main.c:2515-2551`.
- A first load changes `IDLE -> PENDING`. A later save reload changes
  `COMPLETE -> PENDING` explicitly in `src/injector/main.c:2296-2305`.
- `fake_Event()` calls `deferred_session_init_tick()` on every Osiris event in
  `src/injector/main.c:2894-2943`.
- `fake_InitGame()` does not arm initialization. It is only a fallback
  consumer: if `fake_Load()` has already made the state pending, it runs the
  same tick function in `src/injector/main.c:2494-2505`.
- The pending tick discovers the entity world, retries TypeIds, validates
  stats, captures static data, restores persistent variables, transitions to
  Running, fires `SessionLoaded`, and marks `COMPLETE` in
  `src/injector/main.c:2319-2404`.

The game-state tracker is observational, not an arming mechanism:

- `game_state_init()` starts at `Init` (`src/game/game_state.c:78-88`).
- `fake_Load()` calls `game_state_on_session_loading()` before the original
  load, producing `* -> LoadSession`
  (`src/injector/main.c:2519-2528`,
  `src/game/game_state.c:110-114`).
- The deferred tick calls `game_state_on_session_loaded()`, producing
  `LoadSession -> Running`
  (`src/injector/main.c:2389-2395`,
  `src/game/game_state.c:116-123`).
- Reset, save, pause, and combat notifications in
  `src/game/game_state.c:125-169` never change the deferred-init state.

Therefore there is no separate “human menu load” transition to miss.
`-continueGame` and clicking a save both converge on hooked
`COsiris::Load`. The only genuine missing-path condition would be a load
mechanism that never calls this `COsiris::Load` hook (or returns false), but
PID 2556's log proves that neither happened.

One unrelated state-tracker weakness is visible in the code:
`game_state_on_session_loading()` runs before the original load result is
known. If `orig_Load` returns false, the tracker remains at `LoadSession`
although deferred init is not armed. Moving the transition after a successful
return, or rolling it back on failure, would make diagnostics more accurate.
It did not affect this incident because all observed loads returned 1.

The comment at `src/injector/main.c:2481-2484` is also stale: `fake_Load()`
sets `LoadSession`, not `Running`; only `deferred_session_init_tick()` sets
`Running`.

## Log sequence diff

### Working `-continueGame`: PID 97193

Source:
`~/Library/Application Support/BG3SE/logs/bg3se_2026-07-28_20-41-18.log`

| Time | Log line | Event |
|---|---:|---|
| 20:41:18.198 | 5414 | Startup stats probe finds `m_ptr == NULL`; this is expected before a session load. |
| 20:41:42.040 | 5541 | First `COsiris::Load` call. |
| 20:41:42.041 | 5542 | `Init -> LoadSession`. |
| 20:41:42.266 | 5543 | Load returns 1. |
| 20:41:42.274 | 5573 | Deferred init requested. |
| 20:41:42.362-42.527 | 5577-5580 | Second load returns 1 and requests pending again. |
| 20:41:47.187 | 5581 | Deferred init starts from the Event tick. |
| 20:41:47.215-47.232 | 12140-16645 | 1,999 generated / 2,081 valid TypeIds are discovered. |
| 20:41:47.232-47.235 | 16647-16698 | SessionLoaded stats validation finds 15,774 objects and marks stats ready. |
| 20:41:47.236 | 16721-16723 | `LoadSession -> Running`; `SessionLoaded` fires. |
| 20:41:47.798 | 16729 | Deferred init completes in 611 ms. |

### Human menu load: PID 2556

Source:
`~/Library/Application Support/BG3SE/logs/bg3se_2026-07-28_21-29-04.log`

| Time | Log line | Event |
|---|---:|---|
| 21:37:06.781 | 5525-5526 | First `COsiris::Load`; `Init -> LoadSession`. |
| 21:37:07.001 | 5527 | Load returns 1. |
| 21:37:07.096-07.263 | 5561-5564 | Second load returns 1; deferred init requested. |
| 21:37:12.399 | 5565 | Deferred init starts from the Event tick. |
| 21:37:12.435 | 16631 | SessionLoaded stats validation runs. |
| 21:37:12.435-12.436 | 16644-16679 | Stats count is 15,774 and stats are marked ready. |
| 21:37:12.435 | 16627-16629 | TypeId discovery has completed with 2,081 valid components. |
| 21:37:12.436 | 16702-16704 | `LoadSession -> Running`; `SessionLoaded` fires and the mod handler runs. |
| 21:37:12.944 | 16710 | Deferred init completes in 546 ms. |
| 21:40:20.225 | 22133 | The same process later reports 109/109 tests passed. |

The sequences are materially identical. `InitGame` did not need to fire in
either case because the Event hook consumed the pending request.

### The failing cluster: PID 27477, not PID 2556

Source:
`~/Library/Application Support/BG3SE/logs/bg3se_2026-07-28_21-40-27.log`

- Line 12 identifies `Running in process ... (PID: 27477)`.
- Line 5412 is the startup-only `m_ptr == NULL` message.
- No `COsiris::Load called`, `Deferred session init requested`,
  `Deferred session init starting`, stats-ready, or `SessionLoaded` entry
  exists.
- Tier-2 starts at 21:40:32.898, about five seconds after process startup.
- Lines 8139 and following report 43/67 passed and 24 failed.
- Shutdown lines 8169-8171 report `InitGame: 0`, `Load: 0`, and `Event: 0`.

The cross-process handoff is visible at the boundary:

1. PID 2556 accepts `! test_ingame` at 21:40:26.351
   (`bg3se_2026-07-28_21-29-04.log:22239-22240`).
2. PID 2556 crashes at capture time 21:40:27.601; the crash report gives
   `procLaunch=21:29:03.9981` and `pid=2556`.
3. PID 27477 initializes at 21:40:27.881
   (`bg3se_2026-07-28_21-40-27.log:6-12`).
4. Its `console_init()` unlinks and rebinds the same pathname.
5. The test client reconnects at 21:40:32.898 and runs tier-2 against PID
   27477 at the menu.

This is possible because every instance unconditionally removes
`/tmp/bg3se.sock` before binding (`src/console/console.c:158-194`), while both
the C client contract and harness hard-code that single path
(`src/console/console.h:29-32`,
`tools/bg3se_harness/config.py:6-9`). An older server socket may still be
listening through its open file descriptor, but new pathname connections go
to the most recent binder.

## Why stats were 15,774 and then zero

There is no stats-data contradiction:

- PID 2556 completed session init and read 15,774 objects.
- PID 27477 had not loaded a session, so `stats_manager_ready()` was false.
  `Ext.Stats.GetAll()` deliberately returns an empty table in that condition
  (`src/lua/lua_stats.c:489-500`).
- The accompanying symptoms all have the same pre-session explanation:
  Osiris functions were never enumerated, no host existed, level and static
  data singletons were absent, and generated TypeId globals were not in their
  loaded-session state.

The `Lua context changed: Server -> None` line is not a state teardown and
does not identify a client Lua VM. A normal console command saves the current
context enum, temporarily sets it to Server, executes on the same `lua_State`,
and restores the saved value (`src/console/console.c:583-613`). In the working
20:41 log the exact line follows the startup `Ext.GetVersion()` health probe.
Context itself is only a process/dylib-global enum
(`src/lua/lua_context.c:17-18,40-57`). The implementation creates one
`lua_State` at `src/injector/main.c:2068-2075` and merely relabels it Server
or Client while loading scripts (`src/injector/main.c:2004-2039`).

There was, however, a separate source of multiple Lua states in PID 2556:
the crash report's `usedImages` contains two `libbg3se.dylib` entries with the
same UUID, one from the build tree and one from the Steam app bundle. It also
contains one `BG3SE-ExcHandler` thread from each image. Because constructor
state such as `L`, `s_session_init_state`, and the console globals is
dylib-local (`src/injector/main.c:401-403,2288-2294,3535-3581`), loading two
physical images can create two runtimes in one PID. The crash's active
console/Lua/overlay frames are from the deployed image. This duplicate-load
condition should be eliminated, but the failed 24-test result is still
unambiguously PID 27477's pre-session runtime.

## Root cause

Primary root cause: the test/harness identifies a session through a global
socket pathname rather than a verified `(PID, dylib image, session-init
generation)` identity. A replacement BG3 process rebound that pathname, and
tier-2 ran against it before any load.

Contributing cause: the launch setup allowed both the build-tree and deployed
BG3SE dylibs to load into PID 2556, making singleton state and socket
ownership ambiguous even within one process.

Not a cause: a missing human-menu-load branch in
`deferred_session_init_tick()`, `fake_Load()`, or `game_state.c`.

## Recommended fix

1. **Make the socket PID-specific.** Bind `/tmp/bg3se-<pid>.sock` (and expose
   the chosen path in the log/health file) instead of having every constructor
   unlink `/tmp/bg3se.sock`. Change `src/console/console.h:29-32` and
   `src/console/console.c:158-213`; update
   `tools/bg3se_harness/config.py:8` and console clients accordingly. If a
   compatibility alias is retained, only a supervisor should update it.

2. **Add an identity/readiness handshake.** The socket greeting or a
   machine-readable command should return at least PID, process launch time,
   dylib UUID/path, current `ServerGameState`, deferred-init state, and a
   monotonically increasing session generation. Refuse live tier-1/tier-2
   unless the response matches the intended PID and reports
   `SESSION_INIT_COMPLETE` plus `Running`.

3. **Fix PID selection in the driver.** `scripts/session_driver.sh:78` uses
   `pgrep ... | head -1`, which is unsafe with a Steam bounce or replacement.
   Track the exact launch/relaunch PID and bind SE log, network log, socket,
   screenshots, and crash report to that PID. Treat a PID change as a new
   session requiring a fresh readiness gate, never as a transparent retry.

4. **Prevent duplicate dylib loading.** Use exactly one injection mechanism:
   either the patched `@loader_path/libbg3se.dylib` or
   `DYLD_INSERT_LIBRARIES`, not both. Add a constructor-level process-global
   guard (outside per-image statics, for example a named `shm_open`/lock keyed
   by PID) that logs both image paths and disables the later copy before it
   creates Lua, hooks, timers, exception handlers, overlay, or sockets.

5. **Harden diagnostics, not the successful load path.** Log PID and session
   generation on every deferred-init transition. Move
   `game_state_on_session_loading()` after a successful `orig_Load`, or roll
   back on failure. Correct the stale InitGame comment. No new
   manual-menu-specific arming branch is warranted by this evidence.

## Validation criteria for the fix

- A human menu load and `-continueGame` both show
  `IDLE/COMPLETE -> PENDING -> COMPLETE`, exactly one `SessionLoaded`, stats
  count greater than 10,000, and more than 1,500 TypeIds.
- Starting or crashing a second BG3 process cannot redirect an existing test
  client to its socket.
- The harness aborts with an identity/readiness error if the target PID
  changes or has not reached `SESSION_INIT_COMPLETE`.
- A crash report contains exactly one BG3SE image and one BG3SE exception
  handler.
- The console context can transition `None -> Server -> None` during a health
  probe without being interpreted as Lua-state loss.
