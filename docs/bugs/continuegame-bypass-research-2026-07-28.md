I’ll trace the requested documents and code paths, then cross-check symbols/history if useful. I’m also using the bg3se-macos Ghidra skill because the question directly concerns reverse-engineering this port; I’ll only use its read-only guidance.
The key distinction is emerging: the dylib’s “game state” is an inferred Osiris lifecycle, not the client menu state machine. At the menu it remains `Init (2)` by construction; `LoadSession (7)` is only inferred once `COsiris::Load` fires, then `Running (13)` is set by deferred initialization. That makes a raw write to this tracker ineffective—it changes reporting, not BG3’s engine state.
The evidence also separates two different “continueGame” paths: the documented Objective-C method is the Larian launcher bridge and merely closes the launcher modal; with `NoLauncher=1`, it is not the in-game Continue handler. The useful target is the `GameStateInit` flag-consumption path or the `DCMainMenu` command/savegame manager path—not `-[LariLauncherViewController continueGame]`.
## Answer

The highest-probability explanation is that `-continueGame` selects or primes the main-menu Continue flow but does not execute the final save-load action. The binary has a distinct in-game handler, `gui::DCMainMenu::OnContinueGameCommand`, followed by mod validation and `StartGame`; the documented Objective-C `continueGame` method is only the launcher-modal path.

The recommended target is therefore the first UI-independent `ecl::SavegameManager` call made by `OnContinueGameCommand`, not a write to the existing BG3SE state tracker.

Confidence: high on the architectural conclusion; medium on the exact blocking gate until the current functions are decompiled.

## Evidence

- The harness passes exactly one of `-continueGame` or `-loadSaveGame` to the ARM64 process: [launch.py:386](/Users/tomdimino/Desktop/Programming/game-modding/bg3/bg3se-macos/tools/bg3se_harness/launch.py:386).
- Runtime tests reached the visible main menu while the socket listened but Lua did not respond: [headless-cli-goal-progress.md:85](/Users/tomdimino/Desktop/Programming/game-modding/bg3/bg3se-macos/docs/bugs/headless-cli-goal-progress.md:85).
- `-continueGame` highlights Continue without activating it: [noesis-input-bypass-re.md:9](/Users/tomdimino/Desktop/Programming/game-modding/bg3/bg3se-macos/docs/bugs/noesis-input-bypass-re.md:9).
- Corrected coordinates, foreground activation, Retina handling, and enhanced CGEvents still produced no state change: [headless-cli-goal-progress.md:227](/Users/tomdimino/Desktop/Programming/game-modding/bg3/bg3se-macos/docs/bugs/headless-cli-goal-progress.md:227), [headless-cli-goal-progress.md:304](/Users/tomdimino/Desktop/Programming/game-modding/bg3/bg3se-macos/docs/bugs/headless-cli-goal-progress.md:304).
- `GameStateInit` validates that the two save flags are mutually exclusive, proving the flags are parsed before state-machine progression: [CLI_FLAGS.md:14](/Users/tomdimino/Desktop/Programming/game-modding/bg3/bg3se-macos/ghidra/offsets/CLI_FLAGS.md:14).
- The launcher `continueGame` bridge only sets a launch flag and closes its modal: [CLI_FLAGS.md:25](/Users/tomdimino/Desktop/Programming/game-modding/bg3/bg3se-macos/ghidra/offsets/CLI_FLAGS.md:25). With `NoLauncher=1`, it is not the in-game Continue implementation.
- A real Mod Verification flow has already been observed after Continue, and it can block until required mods are acknowledged: [headless-cli-goal-progress.md:374](/Users/tomdimino/Desktop/Programming/game-modding/bg3/bg3se-macos/docs/bugs/headless-cli-goal-progress.md:374).
- The local state tracker explicitly infers state from Osiris rather than reading the engine’s client state machine: [game_state.c:1](/Users/tomdimino/Desktop/Programming/game-modding/bg3/bg3se-macos/src/game/game_state.c:1).

## Details

### 1. Ranked stall hypotheses

| Rank | Hypothesis | Supporting evidence | Contradicting/limiting evidence | Decisive check |
|---|---|---|---|---|
| 1 | `-continueGame` primes a menu selection or pending action but does not call the in-game Continue command | The flag visibly highlights Continue without activating it. Current `nm` exposes separate `DCMainMenu::OnContinueGameCommand`, `StartGame`, and `GameStateInitMenu` functions. | The flag is described as “Auto-continue,” but that description came from string/registry interpretation rather than successful runtime proof. | Decompile `GameStateInit::{Enter,Update,Do}` and `GameStateInitMenu::{Enter,Update}`. Find the flag field and determine whether it calls the handler/load manager or merely chooses menu state/focus. |
| 2 | Mod Verification or missing-addon validation intercepts Continue | A Mod Verification window has been reached in live testing. Current symbols include `GetMissingAddons`, `OnSkipModValidationAndStartGame`, and `OnCloseModVerificationWindowCommand`. | After registry reconciliation, preflight reports zero issues and all six high-confidence save-required mods active: [headless-cli-goal-progress.md:455](/Users/tomdimino/Desktop/Programming/game-modding/bg3/bg3se-macos/docs/bugs/headless-cli-goal-progress.md:455). That reduces, but does not eliminate, this hypothesis. | Decompile `OnContinueGameCommand`, `GetMissingAddons`, and `OnSkipModValidationAndStartGame`; identify the branch that opens verification and its exact continuation callback. |
| 3 | Save enumeration/synchronization is not ready when the flag is consumed | Current symbols include `OnSyncingSaves`, `InternalOnSavegameListUpdated`, `RequestGatherSavedFiles`, `HasSaveGames`, and `GetLastSaveGame`. This suggests an asynchronous save-list readiness gate. | The visible Continue button implies at least some save metadata eventually exists, and manual UI progression has reached validation/load. | Check whether `GameStateInitMenu` consumes the flag before `InternalOnSavegameListUpdated`, and whether a pending flag is retried from that callback. |
| 4 | A `MainMenuConfirm` or profile/controller confirmation requires an answer | The binary has `ecl::PlayerManager::strMainMenuConfirm`; the RE clue list includes `MainMenuConfirm`. | This may only be a `FixedString`/message identifier, not a game state. No existing trace proves it is the active blocker. | Xref `strMainMenuConfirm`; identify the message-box type, callback, and accepted `EMessageBoxAnswer`. |
| 5 | Story/mod validation aborts or defers the load before Osiris begins | The prior successful UI path later crashed after `LevelLoaded`, and inconsistent mod state was proven to affect loading. | At the menu, `COsiris::Load` never fires. The documented crash occurs after load begins, so it explains post-load instability better than the initial menu stall. | Watch for entry into `SavegameManager::TryLoad`, `GameStateLoadSession`, or `COsiris::Load`. If none occurs, story loading is downstream and cannot be the current blocker. |
| 6 | Focus/input consumption is the only blocker | The focus gate and missing Noesis keyboard focus explain why synthetic Return/Space fail: [noesis-input-bypass-re.md:58](/Users/tomdimino/Desktop/Programming/game-modding/bg3/bg3se-macos/docs/bugs/noesis-input-bypass-re.md:58). | It does not explain why a command-line auto-load feature would require UI input. It is irrelevant once the dylib invokes the load API directly. | Treat only as a fallback diagnostic, not the primary bypass. |

### 2. What the dylib actually observes

The BG3SE tracker is not the client `GameStateMachine`.

| Moment | BG3SE tracker | Source |
|---|---:|---|
| Dylib/Lua initialization and visible menu | `Init (2)` internally | `game_state_init()` assigns it unconditionally: [game_state.c:78](/Users/tomdimino/Desktop/Programming/game-modding/bg3/bg3se-macos/src/game/game_state.c:78). |
| `Ext.Utils.GetGameState()` before any transition | `Unknown (0)` | The separate Lua-event cache starts at zero and is updated only when an event fires: [lua_events.c:482](/Users/tomdimino/Desktop/Programming/game-modding/bg3/bg3se-macos/src/lua/lua_events.c:482). |
| `COsiris::Load` begins | `LoadSession (7)` | `fake_Load` calls `game_state_on_session_loading()` before the original load: [main.c:2503](/Users/tomdimino/Desktop/Programming/game-modding/bg3/bg3se-macos/src/injector/main.c:2503). |
| Deferred session initialization completes | `Running (13)` | The deferred tick fires stats/session events and then marks Running: [main.c:2383](/Users/tomdimino/Desktop/Programming/game-modding/bg3/bg3se-macos/src/injector/main.c:2383). |
| Save/pause/reset callbacks | Values `14`, `12`, `3`, etc. exist | They are defined in [game_state.h:20](/Users/tomdimino/Desktop/Programming/game-modding/bg3/bg3se-macos/src/game/game_state.h:20), but repository search finds no active callers for most of them. |

There is no `MainMenu` value and no callback for `MainMenuComponent`. Therefore:

- Writing `g_current_state = 7` would only falsify BG3SE’s reporting.
- Calling `game_state_on_session_loading()` would only emit a Lua event.
- Neither operation creates `SavegameLoadComponent`, selects a save, initializes client/server load states, or calls `SavegameManager`.
- A real forced transition requires calling the engine state machine or save manager with valid objects.

### 3. Ghidra queries once the bridge is live

Start with:

```bash
PYTHONPATH=tools python3 -m bg3se_harness ghidra status
```

#### Flag-consumption chain

```bash
PYTHONPATH=tools python3 -m bg3se_harness ghidra search-strings "-continueGame"
PYTHONPATH=tools python3 -m bg3se_harness ghidra search-strings "-loadSaveGame"
PYTHONPATH=tools python3 -m bg3se_harness ghidra search-strings "These commands should be mutually exclusive"
PYTHONPATH=tools python3 -m bg3se_harness ghidra search-functions "GameStateInit"
```

For every returned string address:

```bash
PYTHONPATH=tools python3 -m bg3se_harness ghidra xrefs 0xSTRING_ADDRESS
PYTHONPATH=tools python3 -m bg3se_harness ghidra decompile 0xCALLER_ADDRESS
```

Current installed-binary `nm` gives these especially useful VAs:

```bash
PYTHONPATH=tools python3 -m bg3se_harness ghidra decompile 0x102fa6030  # GameStateInit::Enter
PYTHONPATH=tools python3 -m bg3se_harness ghidra decompile 0x102fa6838  # GameStateInit::Update
PYTHONPATH=tools python3 -m bg3se_harness ghidra decompile 0x102fa6910  # GameStateInit::Do
PYTHONPATH=tools python3 -m bg3se_harness ghidra decompile 0x102fa90c0  # GameStateInitMenu::Enter
PYTHONPATH=tools python3 -m bg3se_harness ghidra decompile 0x102fa93b8  # GameStateInitMenu::Update
```

Look for:

- The object/field holding `continueGame` and `loadSaveGame`.
- Whether the field is cleared before the save list is ready.
- A branch to `GameStateInitMenu`, `DCMainMenu`, `SavegameManager`, or a message-box routine.
- Whether `-loadSaveGame` and `-continueGame` converge on the same leaf.
- Main-thread or frame-update requirements.
- A deferred callback registered on save-list completion.

#### Canonical Continue and validation chain

```bash
PYTHONPATH=tools python3 -m bg3se_harness ghidra decompile 0x1023e10f8  # DCMainMenu::OnContinueGameCommand
PYTHONPATH=tools python3 -m bg3se_harness ghidra decompile 0x1023e374c  # DCMainMenu::StartGame
PYTHONPATH=tools python3 -m bg3se_harness ghidra decompile 0x1023e38f8  # OnSkipModValidationAndStartGame
PYTHONPATH=tools python3 -m bg3se_harness ghidra decompile 0x1023e29d8  # Close verification
PYTHONPATH=tools python3 -m bg3se_harness ghidra xrefs 0x1089fb398     # strMainMenuConfirm
```

Look for:

- How the latest save is obtained.
- Which save index/name is stored in `DCMainMenu`.
- Calls to `GetMissingAddons`, `TryLoad`, `LoadGame`, or `StartGame`.
- The exact branch that displays Mod Verification.
- Whether `OnSkipModValidationAndStartGame` merely sets a boolean or directly calls `StartGame`.
- Whether `StartGame()` is safe without the `AnyView`/Noesis arguments.

#### UI-independent save-load entry points

```bash
PYTHONPATH=tools python3 -m bg3se_harness ghidra decompile 0x10312c8b4  # HasSaveGames
PYTHONPATH=tools python3 -m bg3se_harness ghidra decompile 0x10312cad4  # GetLastSaveGame
PYTHONPATH=tools python3 -m bg3se_harness ghidra decompile 0x103125abc  # GetSavegameIndex
PYTHONPATH=tools python3 -m bg3se_harness ghidra decompile 0x103125d40  # LoadGame(index,...)
PYTHONPATH=tools python3 -m bg3se_harness ghidra decompile 0x103125f20  # LoadGame(name,...)
PYTHONPATH=tools python3 -m bg3se_harness ghidra decompile 0x1031278e4  # TryLoad
PYTHONPATH=tools python3 -m bg3se_harness ghidra decompile 0x1031275f4  # QuickLoad
PYTHONPATH=tools python3 -m bg3se_harness ghidra decompile 0x10312885c  # GetMissingAddons
```

Look for:

- Return type/layout of `GetLastSaveGame`.
- Whether `QuickLoad` means newest quicksave or newest save of any type.
- Required `ModuleSettings`, callback, and string-view parameters.
- Whether `TryLoad` owns validation and confirmation.
- Whether the public-looking `LoadGame` overload is the cleanest stable leaf.

#### State-machine/ECS fallback

```bash
PYTHONPATH=tools python3 -m bg3se_harness ghidra decompile 0x102fb29d8  # ecl::GameStateMachine::SetTargetState
PYTHONPATH=tools python3 -m bg3se_harness ghidra decompile 0x102fb01c8  # ecl::GameStateLoadSession::Enter
PYTHONPATH=tools python3 -m bg3se_harness ghidra decompile 0x104a14b78  # esv::GameStateLoadSession::Enter
PYTHONPATH=tools python3 -m bg3se_harness ghidra decompile 0x102fabe18  # Add SavegameLoadComponent
```

Look for the state object’s owner/lifetime, the save descriptor consumed by `Enter`, and which fields the 32-byte `SavegameLoadComponent` requires.

Important: these current VAs differ from the older addresses in `noesis-input-bypass-re.md`. For example, current `SetTargetState` is `0x102fb29d8`, not `0x102fcd1a4`, and current `ecl::EocClient::m_ptr` is `0x108994968`. Any implementation must be version-gated and ASLR-adjusted.

### 4. Non-Ghidra options available now

#### Best strict bypass probe: `SavegameManager::QuickLoad`

Read-only `nm` inspection found:

- `ecl::SavegameManager::m_ptr` — `0x1089fc738`
- `CanLoad()` — `0x10312638c`
- `QuickLoad()` — `0x1031275f4`
- `GetLastSaveGame()` — `0x10312cad4`
- Two `LoadGame()` overloads and `TryLoad()`

`QuickLoad()` is the cheapest direct-load experiment because it appears to require only `this` in `x0`:

1. Wait for the singleton to become non-null.
2. Dispatch on the game/main thread.
3. Call `CanLoad()`.
4. If true, call `QuickLoad()` once.
5. Observe `fake_Load`.

Limitation: it may choose the latest quicksave rather than BG3’s overall “Continue” save. It is a diagnostic proving that the menu can be bypassed, not necessarily the final implementation.

These are local `t`/`b` symbols, not public exports. `nm -gU` exposes no useful SavegameManager/DCMainMenu API, so `dlsym()` is unlikely to work. Resolve them as version-specific main-binary VA plus ASLR slide, following the same pattern already used for `GlobalSwitches`: [global_switches.c:13](/Users/tomdimino/Desktop/Programming/game-modding/bg3/bg3se-macos/src/game/global_switches.c:13).

Calling main-binary code does not patch `__TEXT`; it is distinct from installing a main-binary hook. Still use version checks and safe pointer validation.

#### Osiris

No load-game Osiris call is present in the repository’s discovered API surface; only `SavegameLoaded` listeners appear. More importantly, at the menu:

- `RegisterDIVFunctions` may not have fired.
- There is no server session/context.
- Osiris story functions are therefore not a reliable way to initiate the first load.

Osiris is useful as an observable success signal, not as the initiating mechanism.

#### The five documented Noesis approaches

1. **Force focus flag:** quickest overall because it is already implemented and wired at [main.c:2159](/Users/tomdimino/Desktop/Programming/game-modding/bg3/bg3se-macos/src/injector/main.c:2159). It remains UI input, so it does not satisfy the requested direct-load design.
2. **Direct `Noesis::Keyboard::KeyDown`:** still UI-dependent and requires a valid focused element.
3. **Call `GameStateMachine::SetTargetState`:** direct, but unsafe until a correctly initialized `GameStateLoadSession` object and save payload are known.
4. **Execute the DCMainMenu command:** preserves canonical validation behavior, but requires locating the live `DCMainMenu` instance. Decompilation may reveal a singleton or an easier leaf.
5. **Inject `SavegameLoadComponent`:** highest risk; its size is known, but its fields/save identity are not.

For the strict requirement, `QuickLoad()` is cheaper than approaches 3–5. The likely production solution is the `SavegameManager` leaf reached from `OnContinueGameCommand`.

### 5. Recommended experiment order

1. **Control: prove which boundary is missing.**  
   Run `-continueGame` with logs for `GameStateInit`, save-list readiness, `SavegameManager::TryLoad`, and existing `fake_Load`.  
   Success signal: any entry into `TryLoad`/`LoadGame`; if absent, the block is entirely client/menu-side.

2. **Direct `QuickLoad()` one-shot.**  
   Trigger only after `SavegameManager::m_ptr != NULL` and `CanLoad() == true`.  
   Success signal: `COsiris::Load` hook fires, BG3SE logs `Init (2) -> LoadSession (7)`, then `Running (13)`.

3. **Reproduce Continue exactly without UI.**  
   After decompiling `OnContinueGameCommand`, call its UI-independent SavegameManager leaf with the result of `GetLastSaveGame()` or its index.  
   Success signal: the same load transition as manual Continue, using the actual newest save rather than only a quicksave.

4. **Test the validation split.**  
   If step 3 opens/stalls on Mod Verification, invoke the continuation used by `OnSkipModValidationAndStartGame`, or call the post-validation load leaf directly—but only when harness preflight reports no missing required mods.  
   Success signal: load begins without a visible verification dialog.

5. **Test the asynchronous save-list hypothesis.**  
   Issue the request from `InternalOnSavegameListUpdated` or after `HasSaveGames()` flips true, rather than at dylib construction.  
   Success signal: a previously ignored request begins loading as soon as enumeration completes.

6. **State-machine call only if SavegameManager cannot be isolated.**  
   Construct/reuse the exact `GameStateLoadSession` object observed in decompilation and call `SetTargetState` on the client state machine.  
   Success signal: `MainMenuComponent` disappears, `GameStateLoadSessionComponent` appears, then server load begins.

7. **ECS component injection last.**  
   Reproduce all fields written by the engine before adding `SavegameLoadComponent`.  
   Success signal: component-added queries advance both client and server state machines without a crash.

## Related

The final implementation should expose a one-shot request with three guards: exact supported game version, execution on the correct game thread, and idempotence. Its authoritative success criterion should be the existing `COsiris::Load → LoadSession (7) → Running (13)` path—not socket acceptance alone, since the socket can listen while the game is still stalled at the menu. No files were modified.
