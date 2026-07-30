# Wave 2 main-menu stall: dylib-diff causality audit

Date: 2026-07-29  
Audit range: `91313f0..ebb61e8` (`91313f0` is the 2026-07-29 13:42
`origin/main` baseline)

## Symptom

The 13:39 launch passed `-continueGame`, skipped the version-gated functor
hooks, called `COsiris::Load`, and transitioned from `Init` to `LoadSession`
about 20 seconds after launch
(`bg3se_2026-07-29_13-39-12.log:5521,6025-6031`). The 18:14 and 19:32
launches installed the Wave 2 functor hooks but remained at the main menu with
no `COsiris::Load` call and no game-state transition.

The source audit covers every `src/` change in `a0bc648`, `4bec7ae`,
`a0f3ee6`, `2fcd6a2`, and `ebb61e8`. The full repository diff is 29 files,
2,806 insertions, and 551 deletions. Only the following source files changed:

- `src/core/offset_table.{c,h}`
- `src/entity/component_property.{c,h}`
- `src/injector/main.c`
- `src/lua/lua_events.{c,h}`, `src/lua/lua_ext.c`, `src/lua/lua_stats.c`
- `src/mod/mod_loader.{c,h}`
- `src/stats/functor_hooks.c`, `functor_types.h`,
  `prototype_managers.c`, and `stats_manager.{c,h}`

No Noesis/UI-input implementation, game-state tracker, client/server startup,
network startup, or timer implementation changed.

## Verdict

**The code and logs do not support a direct causal path from any Wave 2
feature body to failure of `-continueGame`. The evidence currently favors the
pre-existing main-menu activation flake.**

There is one changed boot-time side effect that cannot be exonerated by static
analysis alone: `a0f3ee6` changed the exact-build gate so ten Dobby inline
hooks are now installed. Hook installation mutates executable code, so an
incorrect target, a Dobby installation side effect, or an unobserved early
invocation remains technically possible. That is a weak indirect plausibility,
not evidence that the hooks caused the menu stall.

The hook *bodies* have no normal main-menu execution path. They are nine
`esv::functor::ExecuteStatsFunctors` combat/gameplay-context overloads plus
`ProcessDealDamageFunctors`; they do not intercept Noesis input,
`GameStateInit`, either game-state machine, `COsiris::Load`, or EocServer
startup. The known bad ABI in `a0f3ee6` caused a later save-loaded status tick
to crash, and `ebb61e8` fixed that ABI. Both the pre-fix 18:14 dylib and the
post-fix 19:32 dylib stalled *before* save loading, so that known runtime hook
bug does not explain the menu symptom.

Confidence is **moderate**, not high, because there are only two stalled runs,
no functor-hook-only A/B run, and no per-hook entry counters proving that all
ten targets were untouched at the menu.

## Boot and pre-session execution audit

| Commit/change | What executes before a session | What does not execute until called | Menu-stall relevance |
|---|---|---|---|
| `a0bc648` component writes | `component_property_init()` copies layout descriptors into a dylib-owned static array and marks generated layouts with a boolean (`src/entity/component_property.c:51-81,89-104`; call site `src/entity/entity_system.c:1092-1098`). | All game-memory writes require Lua assignment through `component_proxy_newindex()` (`src/entity/component_property.c:486-631,676-695`). | No game memory is written at boot. The new flag changes only BG3SE metadata. No UI, state-machine, server-startup, or timer path. |
| `4bec7ae` offset row/status init | The offset table gains one data field, and `prototype_managers_init()` resolves `StatusPrototype::Init` to a function pointer (`src/core/offset_table.c:150-155`; `src/stats/prototype_managers.c:257-274`). | Calling the function and mutating the status manager happens only through status prototype sync (`src/stats/prototype_managers.c:717-776`). | Pointer resolution is not a hook and does not call or dereference the target. |
| `4bec7ae` Stats APIs/load order | Lua startup registers new C functions and TreasureTable/TreasureCategory subtables (`src/lua/lua_stats.c:1346-1428`). The new mod UUID accessor itself is inert (`src/mod/mod_loader.c:582-594`). | `GetStatsLoadedMods`, `GetStatsLoadedBefore`, AddAttribute/enumeration gates, status sync, and treasure getters run only when Lua calls them (`src/lua/lua_stats.c:782-820,1128-1200,1251-1339`). | Registration changes Lua tables only. It does not touch UI or engine state. |
| `4bec7ae` treasure managers | Nothing reads RPGStats `+0x120` or `+0x180` during initialization. | `get_treasure_manager()` is reached only by a treasure getter and then performs guarded reads (`src/stats/stats_manager.c:570-603,924-1079`). | No boot/menu memory touch and no hook installation. |
| `a0f3ee6` functor gate/remaps | The version constant now matches `4.1.1.7209685`, so `main.c` calls `functor_hooks_init()` (`src/stats/functor_types.h:373-395`; `src/injector/main.c:4167-4186`). That function performs ten Dobby patches (`src/stats/functor_hooks.c:289-396`). | Event construction and Lua dispatch occur only after a hooked game function is invoked and only when subscribers exist (`src/stats/functor_hooks.c:70-128`; `src/lua/lua_events.c:1636-1732`). | This is the only material new boot-time mutation and therefore the only credible changed-code suspect, but its targets are server gameplay functions, not menu code. |
| `a0f3ee6` Lua damage events/ExecuteFunctors | Function pointers are registered in Lua as part of the existing Ext API initialization (`src/injector/main.c:923-962`). | The damage event body runs only from `ProcessDealDamageFunctors`; `Ext.Stats.ExecuteFunctors` remains a non-executing stub (`src/lua/lua_stats.c:1110-1125`). | No pre-session engine call or state mutation. |
| `2fcd6a2` Wave 2 tests | Lua parses three additional registration chunks and stores test closures in `BG3SE_Tests`; `BG3SE_AddTest` only appends `{name, fn}` records (`src/lua/lua_ext.c:1371-1384,2567-2605`). | The closure bodies, including component writes, status sync, treasure reads, event subscriptions, and Timer calls, run only after `!test` or `!test_ingame`. | The 19:32 log shows 34 chunks took 1 ms versus 31 chunks/0 ms at 13:39, while total initialization was 51 ms in both logs. At most this is a one-millisecond timing perturbation of an existing flake, not a semantic path. |
| `ebb61e8` hidden result argument | The same ten targets are installed, now pointing at corrected wrapper bodies. | Corrected result forwarding matters only when one of the nine `ExecuteStatsFunctors` hooks is invoked (`src/stats/functor_types.h:403-424`; `src/stats/functor_hooks.c:137-235`). | It fixes the post-load crash and introduces no distinct pre-session behavior. |

### Timers and singleton captures

There is no new production timer. `timer_init()` and the tick update path are
unchanged (`src/injector/main.c:2688-2693,3582-3611`). The Timer expressions
visible in `2fcd6a2` are inside stored test closures, not run during
registration. The pre-existing focusless-input timer is also unchanged; it is
started by `BG3SE_AUTO_DISMISS_SPLASH` at
`src/injector/main.c:2726-2734`.

The StatusPrototypeManager singleton-address capture already existed. Wave 2
adds only a cached `StatusPrototype::Init` code pointer
(`src/stats/prototype_managers.c:251-273`). It does not install an Init hook or
call Init at startup.

## Interference analysis

### Noesis UI and main-menu interaction

No changed source file hooks or calls the input pipeline, main-menu command,
Noesis keyboard, or Noesis command manager. The documented pre-existing
failure is exactly that `-continueGame` can visually select Continue without
giving it keyboard focus or executing its command
(`docs/bugs/noesis-input-bypass-re.md:9-18,58-62`). The same document places
the relevant UI and state-machine functions in unrelated address ranges:
Noesis at `0x100...`, client state machinery at `0x102...`, and server
state machinery at `0x104...`
(`docs/bugs/noesis-input-bypass-re.md:88-111`). The new functor targets are
at `0x10537e8b4` and `0x10577787c..0x105786548`
(`src/stats/functor_types.h:383-395`).

The only conceivable link is indirect: ten Dobby installations alter code
pages and add a few milliseconds of initialization work. A bad Dobby patch
could corrupt unrelated execution, and a timing change could perturb an
already flaky menu-focus race. The successful install return values and
non-overlapping target addresses make either explanation less likely than the
known activation flake.

### `GameStateInit`, `-continueGame`, and the game-state machines

No Wave 2 diff changes argument parsing, `GameStateInit`, the
`ecl::GameStateMachine`, the `esv::GameStateMachine`, or the BG3SE
game-state tracker. The harness still appends the literal `-continueGame`
argument (`tools/bg3se_harness/launch.py:964-975`).

Status sync cannot accidentally drive or block an Init-state transition:
`stats_sync()` refuses to run unless the server is already `Running` or
`Paused` (`src/stats/stats_manager.c:1641-1650`). Treasure reads and component
writes likewise need explicit Lua calls.

### ecl/esv and server startup

The ten hooks are:

- eight two-context `esv::functor::ExecuteStatsFunctors` overloads for
  AttackTarget, AttackPosition, Move, Target, NearbyAttacked,
  NearbyAttacking, Equip, and Source;
- the Interrupt overload with an `EntityWorld` argument; and
- `ProcessDealDamageFunctors`, whose arguments include a functor, entity,
  position, spell state, damage flags, ability, and interrupt data
  (`src/stats/functor_types.h:383-449`).

Those inputs require an active server gameplay world and a stat-functor
execution. A main menu in `Init`, before `COsiris::Load`, has no normal combat,
status-tick, equip, movement-functor, interrupt, or damage-processing path.
The wrappers also take the zero-subscriber fast path straight to the original
function (`src/stats/functor_hooks.c:70-80,143-235,238-283`).

This is consistent with the independently diagnosed 18:23 crash: the Target
hook was first implicated after save loading, when a status tick supplied a
real Target context (`docs/bugs/wave2-functor-crash-analysis.md:41-73,300-311`).
It is not consistent with a hook body preventing the earlier
`Init -> LoadSession` transition. The remaining uncertainty is that the code
does not currently count hook entries unless an event is dispatched.

## StatusPrototype and treasure-manager findings

`eoc::StatusPrototype::Init` at offset `0x01ff7150` is **not hooked**.
At boot, BG3SE computes and stores its runtime address, then logs it
(`src/stats/prototype_managers.c:257-274`). The call sites are confined to
`sync_status_prototype()`, after a manager lookup or insertion
(`src/stats/prototype_managers.c:717-776`), and the higher-level sync guard
prevents this path before `Running`/`Paused`
(`src/stats/stats_manager.c:1641-1650`).

Likewise, RPGStats-relative `+0x120` TreasureCategories and `+0x180`
TreasureTables are constants used only by on-demand read helpers. The helper
first obtains `RPGStats`, validates buffer/capacity/size, and fails closed
(`src/stats/stats_manager.c:570-603`). No manager read, write, singleton
capture, or hook occurs at boot or while merely sitting at the menu.

## Log evidence

### 19:32 stalled session

- All ten hooks installed successfully:
  `bg3se_2026-07-29_19-32-26.log:5523-5534`.
- `StatusPrototype::Init` was only resolved and logged:
  `bg3se_2026-07-29_19-32-26.log:5442-5451`.
- Initialization completed in 51 ms:
  `bg3se_2026-07-29_19-32-26.log:5554`, identical to the good run at
  `bg3se_2026-07-29_13-39-12.log:5541`.
- There are **zero WARN and zero ERROR records in the entire 114,585-line
  log**, not merely near boot.
- There is no `COsiris::Load` call and no game-state transition. The log
  remains responsive at the menu until 21:02.

### Comparison with 13:39

- The good dylib explicitly skipped functor hooks:
  `bg3se_2026-07-29_13-39-12.log:5521`.
- Its first `COsiris::Load` began at line 6025, returned successfully at
  line 6030, and immediately produced `Init (2) -> LoadSession (7)` at
  line 6031. It later reached `Running` at line 18619.
- Neither run emitted a WARN/ERROR during subsystem initialization. The good
  run's later WARN/ERROR messages are mod/test/session messages after the load,
  so they have no comparable counterpart in a run that never leaves the menu.

### Input-path correlation

The unchanged focus/input path supplies a more specific match to the symptom:

- In the good run, FocusHack succeeded, FocuslessInput repeatedly posted
  Escape, Space, and a menu click, and the ninth attempt was followed by
  `COsiris::Load`/`Init -> LoadSession`
  (`bg3se_2026-07-29_13-39-12.log:5574-5576,6005-6031`).
- In the 19:32 run, the same focus hack succeeded and the same input attempts
  were posted (`bg3se_2026-07-29_19-32-26.log:5595-5597,5623-6036`), but no
  load began. The harness then sent `! splash_done` and cancelled the
  auto-dismiss timer at lines 6116-6118.
- The documented behavior is that `-continueGame` may highlight Continue
  without activating it (`docs/bugs/noesis-input-bypass-re.md:9-18,58-62`).

This does not prove coincidence, but it supplies a known mechanism matching
the observed stopping point. The server-combat hooks do not.

## Recommended discriminating test

There is **no functor-hook-only environment switch**. The exact-build gate is
at `src/injector/main.c:4167-4186`. The existing
`BG3SE_NO_HOOKS=1` switch disables all Dobby patches while leaving Lua and
read-only subsystem initialization alive
(`src/injector/main.c:3918-3931`). The harness copies its parent environment
into BG3, so the switch reaches the dylib
(`tools/bg3se_harness/launch.py:977-999`).

The cheapest no-rebuild test is:

```bash
BG3SE_NO_HOOKS=1 PYTHONPATH=tools \
  python3 -m bg3se_harness launch --continue --timeout 180
```

After fully quitting BG3, run the same HEAD normally:

```bash
PYTHONPATH=tools \
  python3 -m bg3se_harness launch --continue --timeout 180
```

Interpret the pair as follows:

| Result | Interpretation |
|---|---|
| Both stall | Functor hooks are not necessary for the failure; the Noesis/menu activation path is favored. |
| Both load | The two observed stalls were a flake; hook installation is not sufficient for failure. |
| No-hooks loads, normal stalls | Some code patch is implicated. Because the other patch sets are unchanged from the 13:39 good dylib, the new functor set becomes the leading suspect, but `BG3SE_NO_HOOKS` is still broader than a functor-only test. |
| Results vary within an arm | Timing/input flake dominates; repeat and record attempt counts. |

Because the bug is already known to be flaky, one pair is a smoke test, not a
causality claim. The cheapest useful sequence is alternating
no-hooks/normal/no-hooks/normal with the same save and mod state.

If the broad gate implicates hooks, add a temporary
`BG3SE_NO_FUNCTOR_HOOKS` condition around only
`src/injector/main.c:4173-4180` and repeat the alternating test. That preserves
the pre-existing Osiris, StaticData, and VideoSkip hooks and isolates the only
new patch set.

## Prevention

- Add a permanent `BG3SE_NO_FUNCTOR_HOOKS` diagnostic gate.
- Add per-target entry counters that increment before subscriber checks, then
  log the first invocation and game state. This will prove whether any target
  can execute before `LoadSession`.
- Treat `10/10 installed` as installation evidence only, not execution
  evidence.
- For menu regressions, record at least alternating hook-on/hook-off trials
  and the FocuslessInput attempt that triggered `COsiris::Load`.

