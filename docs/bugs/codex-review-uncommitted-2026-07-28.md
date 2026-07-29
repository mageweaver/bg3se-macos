# Review of uncommitted changes — 2026-07-28 evening

Scope: the complete tracked `git diff` plus the requested untracked files
`src/lua/lua_gate.c`, `src/lua/lua_gate.h`,
`tests/harness/test_offset_audit.py`, and `scripts/session_driver.sh`.
The other untracked diagnosis/evidence documents were treated as review inputs,
not implementation changes. No game launch or input automation was performed.

## Critical Issues

- [`src/injector/main.c:2477`] The new Lua gate does not cover all top-level
  entries into the shared `lua_State`.

  `fake_InitGame()` calls `luaL_dostring()`, `events_fire()`, and
  `load_mod_scripts()` at lines 2477-2492 without taking the gate.
  `fake_Load()` does the same through `game_state_on_session_loading()`,
  `luaL_dostring()`, `events_fire()`, and `load_mod_scripts()` at lines
  2520-2542. `load_mod_scripts()` reaches the `lua_pcall()` in
  `try_load_lua_file()` at `src/injector/main.c:620`. These engine-hook paths
  can overlap the GCD console timer, Metal callbacks, and input callbacks.

  There are two more unguarded top-level families:

  - The functor Dobby hooks call `events_fire_execute_functor()` and
    `events_fire_after_execute_functor()` directly from the hooked game thread
    at `src/stats/functor_hooks.c:70-78`. They are dormant on the current
    version mismatch, but become live as soon as the exact-version gate is
    updated.
  - The destructor fires `EVENT_SHUTDOWN` without the gate at
    `src/injector/main.c:3641-3645`.

  `lua_imgui_cleanup_refs()` is also an unguarded Lua API entry at
  `src/lua/lua_imgui.c:2400-2425`. Its normal Lua-facing call path is nested
  under an admitted Lua call, but `imgui_objects_shutdown()` calls it during
  native teardown, so it cannot rely on that invariant.

  **Suggestion:** make gate admission a property of every native-to-Lua
  boundary, not selected `lua_pcall()` sites. Gate `fake_InitGame()`,
  `fake_Load()`, functor dispatch, shutdown-event dispatch, and native ImGui
  reference cleanup. Prefer one small wrapper that locks, resolves the live
  state under the lock, invokes a callback, and unlocks, so future entry points
  cannot omit either lifecycle validation or serialization.

- [`src/injector/main.c:2249`] Shutdown can close Lua ahead of callers that
  already decided the state was live and are blocked waiting for the gate.

  `shutdown_lua()` cancels the timer asynchronously, takes the gate, closes
  `L`, and only then assigns `L = NULL` (lines 2249-2267). The following paths
  test or capture their Lua pointer *before* taking the gate:

  - `dispatch_event_to_lua()` tests global `L` at line 2796, locks at line
    2798, and later passes the possibly-null post-shutdown global to
    `lua_rawgeti()` at line 2809.
  - `lua_hotkey_callback()` captures `s_hotkey_lua_state` and a registry
    reference at `src/input/lua_input.c:35-46`, then blocks on the gate.
    `input_shutdown()` only clears the separate `s_lua_state` in
    `src/input/input_hooks.m:379-407`; it never clears
    `s_hotkey_lua_state` or the Lua hotkey table. A waiter can therefore use a
    freed state after shutdown wins the mutex.
  - `lua_imgui_fire_event()` captures `s_imgui_lua_state` before locking at
    `src/lua/lua_imgui.c:2324-2344`. That pointer is never cleared, so a
    waiting render callback has the same use-after-free window.
  - The registered logging callback retains `g_log_callback_L`
    (`src/lua/lua_events.c:1851-1868`) and it is not unregistered or cleared
    before `lua_close()`.

  The recursive mutex serializes execution but does not make these published
  pointers or subsystem callbacks lifetime-safe. `dispatch_source_cancel()`
  also does not wait for an already-running GCD handler, although that handler
  happens to recheck global `L` after a successful trylock.

  **Suggestion:** introduce a lifecycle state protected by the Lua gate.
  Native callbacks should acquire the gate first and resolve/revalidate the
  live `lua_State` only while holding it. During shutdown, first disable and
  drain input/render/log/timer producers, then under the gate clear every
  published Lua pointer and registry-backed callback, and only then close the
  VM. The shutdown event itself must run inside the same admitted lifetime.

- [`src/lua/lua_events.c:1834`] Log-to-Lua dispatch is both unguarded and
  incompatible with the current logging lock.

  `log_event_callback()` can be invoked by any thread that logs, and calls
  `events_fire_log()` without the Lua gate at lines 1834-1845. More seriously,
  the C logger invokes callbacks while holding the non-recursive
  `g_config.mutex` (`src/core/logging.c:582-633`). Existing gated Lua paths log
  while holding the Lua gate, establishing the order:

  `Lua gate -> logging mutex`

  Adding the missing gate directly in `log_event_callback()` would establish
  the reverse order:

  `logging mutex -> Lua gate`

  Two threads can then ABBA-deadlock. A Lua Log handler that logs again can
  also self-deadlock on `g_config.mutex`; the unsynchronized
  `g_log_event_dispatching` flag does not help because the recursive log call
  blocks before callback recursion is reached.

  **Suggestion:** never run callbacks while `g_config.mutex` is held. Snapshot
  active callback entries under the logging mutex, release it, then invoke
  them. For Lua specifically, queue immutable log records onto the designated
  Lua owner/dispatcher rather than entering Lua synchronously from arbitrary
  logging threads. Document and enforce one global lock order.

## Improvements

- [`src/injector/main.c:3437`] `BG3SE_NO_HOOKS=1` does not actually suppress
  every code patch as its diagnostic contract claims.

  The no-hooks branch says it is “skipping ALL code patches” at lines
  3183-3196, and correctly gates StaticData and VideoSkip. Functor hook
  installation at lines 3432-3446 checks only `version_detect_matches()`, so it
  will still install Dobby hooks whenever the expected build is updated to
  match. Those offsets at `src/stats/functor_types.h:377-385` are also absent
  from `AUDITED_OFFSETS` in `tests/harness/test_offset_audit.py:28-77`, despite
  that test's claim to cover every main-binary Dobby code patch.

  This is currently dormant because the expected build is older than the
  installed build, but it can turn both failures on with a future version
  constant update.

  **Suggestion:** require `!no_hooks && version_detect_matches()` for functor
  installation, and add every functor patch address to the offset audit (or
  explicitly retire the hooks). Add a test that enumerates patch-site defines
  so the audit cannot silently omit a new Dobby call family.

- [`src/overlay/overlay.m:656`] `clearOutput` is not ordered with the pending
  output buffer, so text queued before a clear can reappear after it.

  `appendOutput:` releases `@synchronized(self)` at line 670 before enqueuing
  its flush block. A concurrent `clearOutput` can enqueue its main-queue block
  in that gap, after which the append thread enqueues `flushPendingOutput`.
  The main queue then clears the view and flushes the pre-clear line. Clearing
  also neither empties `pendingLines` nor invalidates an already-enqueued
  flush.

  The swap of `pendingLines` under `@synchronized` in
  `flushPendingOutput` is otherwise sound: appends that arrive during rendering
  get a fresh array and schedule a later drain. The storage trim followed by
  `scrollRangeToVisible:NSMakeRange(storage.length, 0)` at lines 699-707 uses
  the post-trim length and is valid.

  **Suggestion:** make clear a generation/barrier operation under the same
  synchronization domain: discard pending lines, increment an output
  generation, and have queued flushes drop batches from an older generation.
  Enqueue the clear while preserving that ordering.

- [`src/injector/main.c:2232`] Dropping every failed GCD `trylock` permits
  indefinite console-command starvation.

  `pthread_mutex_trylock()` does not queue a waiter or provide fairness. At the
  menu, where `fake_Event()` may not run, sustained Lua activity from render,
  input, logging, or long callbacks can make every 100 ms timer attempt miss.
  No “work pending” state survives a miss, so socket commands have no bounded
  service latency. During active gameplay, the blocking `console_poll()` in
  `fake_Event()` at lines 2897-2903 usually masks the problem, but it does not
  cover the menu/control-plane case the GCD timer was added to solve.

  **Suggestion:** retain a pending-poll flag and let the next admitted Lua
  owner service it, or use a single serialized Lua executor with queued console
  work. At minimum, count consecutive misses and expose maximum command
  latency so starvation is diagnosable.

- [`scripts/session_driver.sh:140`] The Steam bounce handling can latch onto
  the transient direct-launch PID and then report `GAME_DIED` or `REPLACED`
  instead of waiting for Steam's replacement.

  The 30-second wait runs only while no process exists (lines 140-146). If
  `game_pid` samples the short-lived pre-restart process, the progress loop
  immediately treats its disappearance at lines 162-170 as terminal. This is
  exactly the lifecycle the preceding comment says the script should absorb.

  **Suggestion:** add a launch-settling phase that accepts disappearance or PID
  replacement during the Steam restart window and commits to a PID only after
  it remains stable (and preferably after a fresh log appears). Once settled,
  retain the current strict replacement detection.

## Minor/Style

- [`scripts/session_driver.sh:25`] Option values and the soak duration are not
  validated.

  `--soak` and `--diagnosis` read `$2` unconditionally at lines 28-29, so a
  missing value terminates under `set -u` without a useful verdict. A
  non-integer or negative soak is accepted and later makes the numeric test at
  line 206 emit an error or silently skip the requested soak. Unknown options
  are silently ignored.

  **Suggestion:** reject missing values and unknown flags, and validate
  `SOAK` against `^[0-9]+$` before launch.

- [`scripts/session_driver.sh:154`] The recovery count is global even though
  stalls are budgeted per state.

  `RECOVERIES` is never reset on a state transition at lines 173-177. Three
  successful recoveries in earlier, unrelated states leave no recovery for a
  later state. A global cap is reasonable, but it should be named and
  documented as such; if the intent is a per-state ladder, reset the count
  when `STATE` changes and retain a separate global cap under the overall
  deadline.

## Lua entry-point audit

The following classification covers every current `lua_pcall` and
`lua_rawgeti` caller found under `src/`, plus teardown operations that mutate
the Lua registry:

| Caller family | Expected thread/context | Gate classification |
| --- | --- | --- |
| `try_load_lua_file` (`main.c:620`) | `fake_InitGame` / `fake_Load` game hooks; also nested `Ext.Require` | **Unsafe at the two top-level engine-hook callers**; nested Lua calls inherit admission |
| `dispatch_event_to_lua` (`main.c:2809,2854`) | Osiris/game thread | Explicitly gated, but shutdown-unsafe because liveness is tested before locking |
| `console_poll` (`console.c:382,392`) | GCD timer or `fake_Event` | Inherits an outer gate in both current callers |
| `lua_hotkey_callback` (`lua_input.c:46,48`) | Input event-tap thread | Explicitly gated, but captures a stale-able state before locking |
| `lua_imgui_fire_event` (`lua_imgui.c:2344,2390`) | Metal/render thread | Explicitly gated, but captures a stale-able state before locking |
| `callback_registry_invoke` / retrieval (`callback_registry.c:123,302`) | Network/message processing from `fake_Event` | Explicit gate in invoke; current outer caller is recursively gated |
| Entity deferred callbacks (`entity_events.c:648,673`) | `fake_Event` tick | Inherits the outer `fake_Event` gate |
| Timer callbacks (`timer.c:343,346,756,766`) | `fake_Event` tick | Inherits the outer `fake_Event` gate |
| Ordinary `lua_events.c` callback dispatch | `fake_Event`, Osiris bridge, or explicitly gated native dispatcher | Safe only when reached through those admitted callers |
| Log event callback (`lua_events.c:1845`) | Any logging thread | **Unguarded**, with logging-lock inversion risk |
| Functor events (`functor_hooks.c:70-78`) | Hooked game execution thread | **Unguarded** when version-matched hooks are enabled |
| Module-load events / scripts (`main.c:2477-2542`) | `InitGame` / `Load` hook threads | **Unguarded** |
| Shutdown event (`main.c:3641-3645`) | Dylib teardown thread | **Unguarded** |
| `lua_imgui_cleanup_refs` (`lua_imgui.c:2400-2425`) | Lua API or native ImGui teardown | Nested calls are safe; native teardown is **unguarded** and retains a stale state |
| `lua_osiris.c`, `lua_json.c`, `lua_level.c`, `lua_math.c`, `custom_functions.c`, and ImGui argument-parsing `rawgeti` calls | Lua C functions invoked by Lua itself | Nested on the currently admitted Lua thread; no independent native entry |

## Validation and areas with no finding

- **ARM64 MAP_JIT/JIT write protection:** no correctness defect found in the
  reviewed change. `arm64_hook_at_offset()` allocates the trampoline at
  `src/hooks/arm64_hook.c:314`, disables per-thread JIT write protection at
  line 327, performs every trampoline write synchronously on that same caller
  thread at lines 334, 339, 346, and 349, and re-enables protection at line
  354. There is no early return between the two gates. The later
  `arm64_write_instruction()` / `arm64_write_instructions()` calls at lines
  363 and 373 write the original executable hook point, not the MAP_JIT
  trampoline. Re-protection therefore precedes every subsequent early return.
- **Overlay synchronization:** aside from the clear barrier race above, the
  `@synchronized(self)` state transition is consistent. A line cannot be lost
  during the batch swap, and the post-trim scroll range is computed from the
  current storage length.
- **Driver quoting and NUL handling:** paths containing spaces are quoted in
  the shell paths reviewed. `find ... -print0 | xargs -0 ls -t` preserves
  spaces, and the host BSD `xargs` was verified not to invoke `ls` on empty
  input. `bash -n scripts/session_driver.sh` passed.
- **Offset audit:** all 24 cases collect successfully. The fixture was not
  executed because it reads the installed game binary, which was outside the
  offline review constraint. Its principal coverage gap is the functor patch
  family described above.
- **Build/tests:** a clean out-of-tree Clang CMake build completed successfully
  with `BUILD_IMGUI_TEST=OFF` and `BUILD_TESTS=ON`.
  `bg3se_test_tier0` passed 41/41 tests. The harness suite excluding the
  installed-binary offset audit passed 55 tests. `git diff --check` passed.
- The remaining tracked updates (entity/static-data/stats/fixed-string/network
  offsets, generated component IDs, generated TypeIds, CMake integration, and
  ImGui test linkage) compiled successfully and did not reveal an additional
  correctness issue in static review.

## Overall assessment

Do not ship the Lua gate as complete in its current form. The mutex fixes
several observed concurrent calls, but top-level entries remain outside it and
shutdown does not provide a safe lifetime boundary. The logging callback must
be redesigned with the logging mutex before it can be safely gated. The JIT
write-protection fix is correctly scoped to the writing thread; the overlay
coalescing is sound except for clear ordering; and the driver needs a stable
Steam-relaunch PID phase plus argument validation.
