# BG3SE macOS `BG3SE_NO_HOOKS` crash diagnosis

Date: 2026-07-28
Scope: static source analysis, three `.ips` reports, associated SE logs, and ARM64 disassembly. The game was not launched.

## Executive conclusion

The crash has a direct, high-confidence cause: `BG3SE_NO_HOOKS=1` does **not** skip all Dobby hooks. It skips only the four hooks installed locally by `install_hooks()` for `COsiris::InitGame`, `COsiris::Load`, `COsiris::Event`, and `RegisterDIVFunctions`, then jumps to `init_subsystems`. That path still calls `staticdata_manager_init()`, which installs six code hooks, and `video_skip_init()`, which installs a seventh when `BG3SE_SKIP_VIDEOS` is enabled.

One StaticData offset is stale in exactly the way needed to cause this crash:

| Item | Unslid VA / image offset |
|---|---:|
| Hardcoded alleged `ImmutableDataHeadmaster::Get<ClassDescriptions>()` | `0x10262f184` / `0x0262f184` |
| Actual symbol in the installed game binary | `0x102614874` / `0x02614874` |
| Difference | `0x1a910` |
| Start of `gui::HotbarSystem::Update` | `0x10262e6b8` |
| Crash instruction | `0x10262f2bc` |

The alleged `Get<ClassDescriptions>` target is actually instruction `HotbarSystem::Update+0xacc`, just `0x138` bytes (78 instructions) before the fault. Every crashing run's SE log confirms that Dobby successfully patched this address after ASLR. When vanilla execution reaches the mid-function patch, Dobby treats it as a function entry and runs `hook_GetClass()` plus its "original function" trampoline with the wrong function boundary and stack frame. The resumed Hotbar code then loads its saved `WorldView*` from the hook/trampoline stack, obtains zero, and faults on `NULL + 0x10`.

This also explains why all visible faulting frames are vanilla and why the top two frames both symbolize as `HotbarSystem::Update`: the corrupt transition resumes in vanilla code after entering through an SE-installed mid-function trampoline.

The one-frame ECS components in the `WorldView` signature determine when Hotbar refresh work runs after a save load. They are not the null object at the fault. The faulting register is `x12`, and at that instruction `x12` is intended to be the `WorldView*` itself.

## Symptom

All three reports fault on the GameThread at the same image offset:

| Report | Configuration | PID | Frame 0 image offset | `x12` | Exception |
|---|---|---:|---:|---:|---|
| `174248.ips` | SE full configuration | 78545 | 40039100 (`0x0262f2bc`) | 0 | `KERN_INVALID_ADDRESS at 0x10` |
| `185709.ips` | SE, game-blessed eight mods | 19766 | 40039100 (`0x0262f2bc`) | 0 | `KERN_INVALID_ADDRESS at 0x10` |
| `190312.ips` | `BG3SE_NO_HOOKS=1` | 36512 | 40039100 (`0x0262f2bc`) | 0 | `KERN_INVALID_ADDRESS at 0x10` |

Vanilla with the same save and eight-mod configuration remains stable for at least 240 seconds. The reproducible delta is therefore the injected SE dylib.

## Root cause

### `BG3SE_NO_HOOKS` has narrower scope than its name and log message

The gate in [`src/injector/main.c`](../../src/injector/main.c) is:

```c
if (no_hooks) {
    LOG_HOOKS_INFO("BG3SE_NO_HOOKS=1: ALL Dobby hooks SKIPPED. ...");
    hooks_installed = 1;
    goto init_subsystems;
}
```

This is at `main.c:3173-3183`. The jump bypasses only the four Dobby calls between that gate and the `init_subsystems` label at `main.c:3312`. It then reaches:

- `staticdata_manager_init(binary_base)` at `main.c:3373`;
- `video_skip_init(binary_base)` at `main.c:3404`;
- `functor_hooks_init(L)` at `main.c:3411-3419` if and only if the game version is an exact match.

The observed game version is `4.1.1.7209685`, while the offsets were verified for `4.1.1.6995620`. Three data sentinel reads pass, so `version_detect_addresses_safe()` enables all address-dependent initializers. The code correctly applies the stricter `version_detect_matches()` gate to Functor code patches, but not to the StaticData or VideoSkip code patches.

In other words, the log line saying "ALL Dobby hooks SKIPPED" is false for the process as a whole. It means only "the four Osiris hooks in this function were skipped."

### StaticData installs the bad patch

[`src/staticdata/staticdata_manager.c`](../../src/staticdata/staticdata_manager.c) defines:

```c
#define OFFSET_GET_CLASS 0x0262f184
```

`staticdata_manager_init()` unconditionally calls:

```c
install_feat_getfeats_safe_hook(main_binary_base);
install_get_manager_hooks(main_binary_base);
```

`install_get_manager_hooks()` then performs five standard `DobbyHook()` calls, including:

```c
target = (uint8_t*)main_binary_base + OFFSET_GET_CLASS;
DobbyHook(target, (void*)hook_GetClass, (void**)&g_orig_GetClass);
```

Relevant source locations are `staticdata_manager.c:33`, `686-729`, and `1035-1058`.

The current game's symbol table places the intended function at:

```text
0x102614874  eoc::ClassDescriptions const*
             ls::ImmutableDataHeadmaster::Get<eoc::ClassDescriptions>() const
```

Its real first instructions operate on a Headmaster passed in `x0` and load the `ClassDescriptions` TypeId:

```asm
102614874: ldrsw x8, [x0, #0x8]
102614878: cbz   w8, 0x1026148c0
10261487c: adrp  x9, ...
102614880: ldr   x9, [...]  ; TypeId<ClassDescriptions, ImmutableDataHeadmaster>
```

That is structurally unrelated to the alleged target's Hotbar query-argument setup. The analyzed arm64 Mach-O UUID is `9A647311-E263-3FF2-AF98-111CEDCB3034`, exactly matching the `slice_uuid` in all three crash reports, so this is the binary slice that produced the reports.

The hardcoded offset instead resolves inside:

```text
0x10262e6b8  gui::HotbarSystem::Update(...)
0x10262f184  HotbarSystem::Update + 0xacc
```

The three relevant logs prove that this precise address was patched in each crashing process:

| Run | Log and PID | Runtime hook address | ASLR slide | Normalized VA |
|---|---|---:|---:|---:|
| Full SE | `17-42-10.log`, PID 78545 | `0x104e1f184` | `0x27f0000` | `0x10262f184` |
| Blessed mods | `18-56-21.log`, PID 19766 | `0x1075e7184` | `0x4fb8000` | `0x10262f184` |
| `NO_HOOKS` | `19-02-36.log`, PID 36512 | `0x103377184` | `0x0d48000` | `0x10262f184` |

The logs also report successful installation of the Feat hook and the Background, Origin, Progression, and ActionResource `Get<T>` hooks. Under the observed harness defaults, VideoSkip installs one more Dobby hook. Thus the `NO_HOOKS` run still had at least seven SE code patches, not zero.

### Concrete corruption mechanism

At the wrongly patched location, the original Hotbar instructions begin:

```asm
10262f180: str  w8,  [sp, #0x10]
10262f184: add  x4,  sp, #0x270     ; patched as alleged Get<ClassDescriptions>
10262f188: sub  x5,  x29, #0xd0
10262f18c: stp  xzr, x9, [sp]
10262f190: mov  x0,  x21
10262f194: bl   ecs::_private::QueryIterator<Modified, Alive>::QueryIterator(...)
10262f198: str  x19, [sp, #0x260]
```

This is argument setup for a Hotbar ECS `QueryIterator` constructor, not a function prologue. Dobby nevertheless redirects it to:

```c
static void* hook_GetClass(void* headmaster) {
    ...
    void* result = g_orig_GetClass ? g_orig_GetClass(headmaster) : NULL;
    ...
    return result;
}
```

Calling the generated "original function" trampoline for a patch in the middle of Hotbar resumes the relocated Hotbar instructions as though they belonged to an independently called function. The stack pointer is now the hook/trampoline call's stack, not the original Hotbar frame expected by the resumed instructions. Hotbar later reads `[sp + 0x110]` and receives zero.

The fault is therefore deterministic once execution reaches this Hotbar path; the 30-second-scale delay is the time until save-load/UI/ECS activity reaches the patched block, not evidence that the original memory corruption necessarily occurred 30 seconds earlier.

## Evidence

### A. Code active under `BG3SE_NO_HOOKS=1`

The constructor at `main.c:3511-3605` performs logging, crash handling, mod detection, Lua initialization, version detection, image enumeration, Osiris discovery, `install_hooks()`, and dyld callback registration. `NO_HOOKS` is not checked until `install_hooks()` after most of that work has already happened.

The table describes the observed `NO_HOOKS` run with the harness defaults (`BG3SE_AUTO_DISMISS_SPLASH=1`, `BG3SE_SKIP_VIDEOS=1`).

| Component | Active under `NO_HOOKS`? | Game memory / render path / input queue effects |
|---|---|---|
| `BG3SE_DISABLE` constructor kill switch | Not set in these runs | If truthy, returns before every other side effect. This is the true "dylib present, zero SE initialization" control. |
| Logging, mod detection, Lua runtime, API registration, path/timer/game-state registries | Yes | Primarily SE heap, files, and Lua state. Mod session loading does not occur without the Osiris Event/Load path. |
| Osiris symbol resolution and function-cache pointer update | Yes | `dlsym`/address bookkeeping; no inline patch. It occurs before the `NO_HOOKS` gate. |
| Four Osiris inline hooks (`InitGame`, `Load`, `Event`, `RegisterDIVFunctions`) | **No** | The only Dobby calls actually bypassed by `NO_HOOKS`. |
| Deferred session init, EntityWorld/TypeId discovery, Stats-loaded work, mod session events, normal Osiris tick loop | **No** | Driven by `fake_Event`/`fake_Load`; those hooks are absent. This agrees with the established trace. |
| Base entity/stats/prototype initializer calls | Yes | Resolve and cache hardcoded game function/global addresses. They do not perform the deferred world/TypeId/session discovery in this run. |
| **StaticData initializer** | **Yes** | **Patches game `__TEXT`: one Feat hook plus five `Get<T>` Dobby hooks. The Class hook patches Hotbar and is the root cause.** |
| Template manager | Yes | Despite log text saying "Auto-capture hooks installed," the current implementation uses direct reads of three singleton pointer globals and installs no hook (`template_manager.c:243-271`). It can read game memory on init and later API access. |
| Resource, level, audio, localization initializers | Yes | Cache/read address-dependent game globals and function pointers. Audio additionally calls into the game only when `BG3SE_MUTE_AUDIO` is present. No evidence ties these to the faulting site. |
| VideoSkip | Yes when `BG3SE_SKIP_VIDEOS` is truthy; yes in all three logs | Patches `BinkManager::LoadVideo` in game `__TEXT`, can suppress intro videos and alter startup/render timing. It is not gated by `NO_HOOKS`. |
| Functor hooks | Conditionally reachable, but **not installed in these runs** | Requires `version_detect_matches()`. Logs say it was skipped because the installed build does not exactly match the expected build. |
| `global_switches` | **No caller; inactive** | Its implementation would read a singleton and write `SkipSplashScreen` at `+0x6ac`, but repository-wide call-site search finds no invocation outside `global_switches.c`. Merely including its header in `main.c` causes no write. |
| CGEventTap input system | Yes | A listen-only session event tap on the main run loop observes keyboard/mouse input, forwards state to ImGui helpers, evaluates hotkeys, and queues Lua key events. It returns every event unchanged and does not inject or consume game input. With `fake_Event` absent, `input_poll()` does not drain the Lua queue. F11 can lazily initialize ImGui. |
| Focusless input object | Yes | `focusless_input_init()` itself changes only SE state. With AutoDismiss enabled, its GCD timer can directly call the game's `LSMTLView` `keyDown:`, `keyUp:`, `mouseMoved:`, `mouseDown:`, and `mouseUp:` methods, thereby injecting into the game input path. In the actual PID 36512 crash log the timer stopped at socket-ready after **zero attempts**, so it injected no events. |
| Focus hack | Yes when AutoDismiss is truthy | Polls the game's `BaseApp::s_AppInstance` and writes its focus byte at `BaseApp+0x142`. The actual PID 36512 log records `Forced focus: 0 -> 1`. It is not gated by `NO_HOOKS`. |
| Console socket poll timer | Yes | A 100 ms `QOS_CLASS_USER_INTERACTIVE` GCD timer calls `console_poll(L)`. It touches SE Lua/socket state and can execute arbitrary Lua supplied by a connected client. It has no inherent direct game write, but commands can call APIs that do. Its atomic guard prevents overlapping timer callbacks, not all possible Lua access from other threads. |
| ImGui Metal backend / CAMetalLayer interception | Not initialized by the constructor; **lazy and ungated** | `Ext.IMGUI.Show()` or an observed F11 calls `imgui_metal_init()`, which globally swizzles `CAMetalLayer.nextDrawable`; after setup it swizzles the concrete drawable's `present`. If activated it directly changes frame/drawable lifecycle. None of the crash-process logs contains "Initializing ImGui Metal backend", so it was not active before these crashes. |
| ImGui NSView input swizzling | Not active in these crashes; lazy and ungated | Installed only after the Metal backend captures a game window and completes setup. It replaces keyboard/mouse methods on the game's concrete view class and can consume input while ImGui captures it. No installation log appears in the crash processes. |
| Crashlog POSIX handlers | Yes | Allocates/maps SE crash logs and replaces `SIGSEGV`, `SIGBUS`, and `SIGABRT` handlers while preserving/chaining prior handlers. No game data/render/input mutation before a crash. |
| Mach exception handler | Yes | Swaps task exception ports for `EXC_BAD_ACCESS` and `EXC_BAD_INSTRUCTION` and runs a listener thread. It acts when an exception occurs and then forwards; it does not explain the pre-fault null. |
| dyld add-image callback | Yes | Watches for `libOsiris`; repeated `install_hooks()` calls are stopped by `hooks_installed`. No render/input write itself. |

Two naming/logging issues deserve cleanup:

1. `BG3SE_NO_HOOKS` is presence-based, so even `BG3SE_NO_HOOKS=0` activates it.
2. Template logging says hooks were installed even though the current implementation only reads globals.

### B. ARM64 disassembly and null dataflow

The decimal image offset converts as:

```text
40039100 = 0x0262f2bc
0x100000000 + 0x0262f2bc = 0x10262f2bc
```

The relevant disassembly was obtained from the supplied executable with `xcrun llvm-objdump --macho --disassemble --demangle`, selecting the full mangled Hotbar symbol with `--dis-symname` and filtering to the address windows. The direct fault window is:

```asm
10262f2ac: adrp  x8, ...
10262f2b0: ldr   x8, [x8, #0xb48]  ; TypeId<Spec<..., ValueComponent,
                                     ; QueryTypeAddedTag, AliveTag>>::m_TypeIndex
10262f2b4: ldrsw x8, [x8]
10262f2b8: ldr   x12, [sp, #0x110]
10262f2bc: ldp   x10, x9, [x12, #0x10]  ; fault: x12 == 0
10262f2c0: ldr   x9, [x9]
10262f2c4: mov   w11, #0x58
10262f2c8: madd  x8, x8, x11, x9
10262f2cc: ldp   x9, x19, [x12]
10262f2d0: ldr   x9, [x9, #0x3f0]
10262f2d4: add   x10, x10, #0x498
```

All three reports have `x12=0` and `x9=0`. The faulting instruction dereferences `x12` first, at field offset `0x10`, matching the exception address `0x10`. `x9` would have faulted at the next instruction but was not the first invalid base.

The Hotbar prologue identifies the spilled value:

```asm
10262e6d4: sub  sp, sp, #0x400
10262e6d8: mov  x19, x0
10262e6ec: str  x1, [sp, #0x110]   ; save incoming WorldView&
10262e6f0: ldr  x8, [x1, #0x18]   ; immediately use it successfully
```

Under normal entry, ARM64 `x1` is the `ecs::WorldView<...>&` argument. The function successfully dereferences it at `+0x18`, so the caller did not pass a null WorldView. Before the crash, the only store to `[sp,#0x110]` in this function is the prologue store; the later store at `0x1026320c4` is after the faulting site. The zero is therefore not a normal mutation of the saved argument. It is consistent with resumed Hotbar code using the wrong stack after the mid-function hook/trampoline transition.

The fields used around the query setup are internal WorldView/query infrastructure:

- `[WorldView+0x18]` leads to the query-registry/type-index table;
- `[WorldView+0x10]`, later adjusted by `+0x498`, supplies a storage-view-like query input;
- `[WorldView+0x0]`, then `+0x3f0`, supplies a world/cache-like query input.

The exact class names of those internal fields are not available from symbols alone, but the pointer identity is clear: `x12` is the `WorldView*`, not a `ValueComponent*` or one-frame event component.

#### Relationship to the one-frame components

Hotbar begins by checking the signature's UI/party event queries:

```text
0x10262e700  GroupsChangedEventOneFrameComponent, Added + Alive
0x10262e7a8  PartyChangedEventOneFrameComponent, Persistent + Alive
0x10262e840  VMPassiveUnregisteredOneFrameComponent, Added + Alive
```

The crash site later begins:

```text
0x10262f2b0  ValueComponent, Added + Alive
```

Thus party/passive one-frame state can cause or shape a Hotbar refresh during post-load UI registration, explaining when the bad patch is reached. The fault is not a missing instance of any of those components: the code faults while building a query from the WorldView facade, before it obtains a per-entity `ValueComponent`.

#### Caller context from frames 1 and 2

In `190312.ips`:

- frame 0: `HotbarSystem::Update+3076`, image offset `0x0262f2bc`;
- frame 1: `HotbarSystem::Update+1780`, image offset `0x0262edac`;
- frame 2: `GameUI::Update+23952`, image offset `0x02530928` (return after the virtual call).

Frame 1 is:

```asm
10262ed9c: bl   Noesis::Cast(...)
10262eda0: str  x0, [sp, #0x90]
10262eda4: ldr  x0, [x0, #0xe0]
10262eda8: bl   Noesis::BaseCollection::Count() const
10262edac: str  w0, [sp, #0xe8]
```

The duplicate Hotbar frame is not normal source-level recursion. It is consistent with a saved/stale link register and an unwinder crossing the Dobby replacement/trampoline entry made in the middle of the same function.

`GameUI::Update` obtains and saves a WorldView, then dispatches its UI systems virtually:

```asm
10252acb8: ldr   x8, [sp, #0xa8]
10252acbc: ldr   x8, [x8]
10252acc8: str   x8, [sp, #0xf8]       ; saved WorldView*
...
1025308f8: ldr   x19, [x8, #0x19f8]   ; UI system list
102530904: ldrsw x8, [x8, #0x1a04]    ; count
102530910: ldr   x8, [x19], #8
102530914: ldr   x8, [x8]             ; update function
102530918: mov   x0, x19               ; system object
10253091c: ldr   x1, [sp, #0xf8]      ; same WorldView*
102530924: blr   x8
102530928: add   x19, x19, #0x38
```

This caller context independently shows that `x1` should be a reusable WorldView passed to each UI system. A legitimate null would have failed at Hotbar's first `[x1+0x18]` load, not only after the bad patch point.

### Evidence bookkeeping correction

The `190312.ips` body records:

```text
captureTime: 2026-07-28 19:03:08.7666
procLaunch:  2026-07-28 19:02:36.2770
pid:         36512
```

`bg3se_2026-07-28_19-02-36.log` is PID 36512 and is the associated SE log. `bg3se_2026-07-28_19-03-08.log` is PID 37698, begins after the crash capture, and belongs to a later process.

Consequences:

- In the actual `NO_HOOKS` crash process, FocuslessInput stopped at socket-ready after zero attempts, so it injected no key or mouse events.
- In that same process, FocusHack did write `Forced focus: 0 -> 1`.
- The 20 null FocusHack attempts and absence of a write in `19-03-08.log` describe the later PID, not the crashed PID 36512.

This correction does not compete with the direct StaticData hook collision. It only means the `190312` run cannot independently prove that the focus-byte write is unnecessary. The exact code patch inside Hotbar remains sufficient to explain all three crashes.

## Ranked surviving suspects

With the hook-target collision established, "suspect" is mostly relevant to additional defects that could survive after the primary fix.

| Rank | Component | Assessment and concrete mechanism |
|---:|---|---|
| 1 | **Stale StaticData `Get<ClassDescriptions>` hook** | **Established root cause.** It patches Hotbar mid-function, creates an invalid function/trampoline boundary, resumes Hotbar with the wrong stack, nulls the effective saved WorldView, and faults exactly `0x138` bytes later. |
| 2 | Other StaticData code hooks plus permissive sentinel gating | High risk even if they did not produce this exact PC. The Feat and other four `Get<T>` offsets also come from the older build and are patched on a version mismatch based only on three readable data sentinels. Any stale target can patch an unrelated function and fail when that function eventually executes. |
| 3 | VideoSkip hook | Active and outside the `NO_HOOKS` gate. A stale Bink offset could corrupt another code location; a valid hook can also change splash/video/UI timing. There is no direct address or log evidence connecting it to Hotbar, and no "Suppressing intro video" line in the crash logs. |
| 4 | FocusHack / splash AutoDismiss | The focus-byte write can alter app focus and Noesis/input/view-model registration timing; direct NSEvent calls could perturb UI state. For PID 36512, the write occurred but the NSEvent timer injected zero events. Neither mechanism explains why the exact patched Hotbar address is followed by the exact null WorldView spill. |
| 5 | Console GCD polling and externally submitted Lua | A high-QoS timer can perturb scheduling, and a connected client's commands can call game-facing APIs from the poll queue. It can affect when UI/ECS state changes, but there is no evidence it directly corrupts the Hotbar stack. |
| 6 | CGEventTap | Listen-only and returns events unchanged. It can update SE/ImGui state, queue Lua input, or activate a hotkey, but does not inject into the game's event queue. It is a weak timing/input observer unless F11 or Ctrl+backtick is actually pressed. |
| 7 | ImGui CAMetalLayer/NSView swizzling | A plausible mechanism in runs where initialized: global `nextDrawable`/`present` interception can alter drawable lifecycle, and view swizzles can consume input. It is not active in the analyzed crash logs, so it is not a surviving explanation for these crashes. |
| 8 | Crashlog/Mach handlers | They alter process exception plumbing, not normal GameThread dataflow. They become active at the fault and are useful for recording it; no evidence indicates they create the pre-fault null. |

## Recommended fix

1. Make `BG3SE_NO_HOOKS` authoritative across the entire process. A true diagnostic no-hooks mode must skip StaticData, VideoSkip, Functor, and any future code-patching initializer, not just the four Osiris calls.

2. Immediately disable all StaticData code hooks on non-exact game versions. Apply `version_detect_matches()` to every code patch. Readable sentinel data proves only that those three data addresses are mapped; it cannot validate unrelated function entry offsets.

3. Do not ship a blind replacement of `OFFSET_GET_CLASS` with `0x02614874` without validating the target function's ABI and prologue for build `4.1.1.7209685`. That value is the correct current symbol, but all six StaticData targets should be re-derived and verified together.

4. Add a target validator before every hardcoded code patch:

   - require the exact Mach-O UUID/build version;
   - verify that the target is a function start, not merely executable/readable memory;
   - compare an expected instruction signature/prologue;
   - fail closed and log the normalized target plus nearest symbol on mismatch.

5. Rename or correct misleading state:

   - `BG3SE_NO_OSIRIS_HOOKS` if narrow behavior is retained, or broaden it to match `NO_HOOKS`;
   - change the "ALL Dobby hooks SKIPPED" log;
   - change Template's "Auto-capture hooks installed" message to "global pointer capture configured."

## Minimal decisive experiment sequence

No game was launched during this analysis. The following is the recommended next runtime matrix, ordered by information gain in light of the newly identified direct collision.

### Existing environment inventory

Repository-wide `getenv` search found:

| Variable | Semantics | Relevant effect |
|---|---|---|
| `BG3SE_DISABLE` | Truthy string; `"0"` is off | Constructor returns before logging/crash/Lua/hook initialization. |
| `BG3SE_NO_HOOKS` | Presence-based | Skips only four Osiris hooks; `"0"` still turns it on. |
| `BG3SE_MINIMAL` | Presence-based | Checked only inside deferred session init; irrelevant when `fake_Event` is absent. The prior probe was inconclusive and should not be reused as evidence. |
| `BG3SE_AUTO_DISMISS_SPLASH` | Truthy; `"0"` is off | Enables both FocusHack and FocuslessInput timer. |
| `BG3SE_SKIP_VIDEOS` | Truthy; `"0"` is off | Enables the VideoSkip Dobby hook. |
| `BG3SE_MUTE_AUDIO` | Presence-based | Calls `audio_pause_all()` after audio init; `"0"` still enables the attempt. |
| `BG3SE_FORCE_ADDRESSES` | Truthy; `"0"` is off | Overrides the address-safety check. |
| `BG3SE_NO_NET` | Presence-based | Disables deferred network hook work; normally moot without session init. |
| `BG3SE_LOG_LEVEL`, `BG3SE_LOG_FORMAT`, `BG3SE_LOG_COLOR`, `BG3SE_LOG_OUTPUT`, `BG3SE_LOG_MODULES` | Value-based logging configuration | No intended game-memory effect. |
| `HOME`, `LANG` | Standard process environment | Paths and localization, not diagnostic gates. |

There are no existing environment gates for StaticData hooks, CGEventTap, ImGui, FocusHack independently of AutoDismiss, the console timer, or crash handlers.

The harness defaults matter: `launch.py:386-418` defaults `skip_videos=True` and `auto_dismiss=True`, then unconditionally overwrites the child environment with `BG3SE_SKIP_VIDEOS=1` and `BG3SE_AUTO_DISMISS_SPLASH=1`. Setting AutoDismiss to `0` in the parent environment is therefore ineffective unless the harness call uses `auto_dismiss=False`. `--no-skip-videos` already exists for VideoSkip.

### Ordered arms

1. **Zero-side-effect control, no code change:** run the same save/eight-mod scenario with `BG3SE_DISABLE=1` for at least 240 seconds. Expected: stable. This distinguishes mere dylib mapping/DYLD effects from constructor activity and is the cleanest injected-dylib control. Because this deliberately disables the Lua console socket, configure the harness/monitor not to treat missing socket readiness as a launch failure.

2. **Primary confirmation, one small env gate:** add `BG3SE_NO_STATICDATA_HOOKS` as a truthy guard around `staticdata_manager_init(binary_base)`. Run with `BG3SE_NO_HOOKS=1`, the same save/mods, and otherwise unchanged defaults. Expected: the Hotbar crash disappears. Confirm in the SE log that none of the six StaticData hook-install lines appears.

3. **Specific causal confirmation:** narrow the new gate so it skips only the Class `DobbyHook()` at `staticdata_manager.c:705-711`, leaving the other StaticData hooks installed. Expected: the `0x0262f2bc` crash disappears. This arm isolates the exact collision from the broader StaticData subsystem.

4. **No-code residual-hook arm:** with the Class/StaticData gate retained, add `--no-skip-videos` or otherwise ensure `BG3SE_SKIP_VIDEOS=0`. Expected: no change if StaticData was the only cause. This tests the remaining non-Osiris Dobby hook without confounding it with the known Hotbar patch.

5. **No-code input/focus arm:** with the Class/StaticData gate retained, launch through a harness path that passes `auto_dismiss=False`, and verify the child log has no Splash AutoDismiss or FocusHack polling. Merely exporting `BG3SE_AUTO_DISMISS_SPLASH=0` is insufficient while the harness overwrites it with `1`. Expected: stable either way; this separates focus/input timing from the root fix.

6. **One-line CGEventTap gate:** add a truthy `BG3SE_NO_INPUT_TAP` check before `input_init()`, keeping console/Lua initialization. Expected: no difference. Verify that "Input system initialized (CGEventTap active)" is absent.

7. **ImGui gate only if logs show activation:** add `BG3SE_NO_IMGUI` at `imgui_metal_init()` if a future crashing arm logs "Initializing ImGui Metal backend." The current reports already show that ImGui was lazy and uninitialized, so running this arm now has near-zero information gain.

8. **Console timer and crash handlers last:** if and only if the StaticData-specific arm still crashes at a different site, add separate gates for the 100 ms console poll timer and `crashlog_init()`. These are lower-value arms because neither writes the Hotbar path during ordinary execution, and disabling crash handling reduces diagnostic evidence.

Do not combine arms 2 through 6 in the first confirmation run. A single-factor StaticData arm yields the decisive result. After that result, combinations can be used to define a safe temporary operating configuration.

## Prevention

- Treat code offsets and data offsets as different risk classes. Data readability sentinels must never authorize code patching.
- Add a startup hook manifest to the log: component, normalized VA, runtime VA, nearest symbol, exact-version status, and installation result.
- Make diagnostic toggles truthy/falsey consistently; avoid presence-based variables whose `=0` value still enables them.
- Add a static or CI hook-target audit against the supported Mach-O. Every intended hook offset must equal a function start and resolve to the expected demangled symbol.
- Add a regression test asserting that `BG3SE_NO_HOOKS=1` reaches no `DobbyHook()` or `arm64_safe_hook()` call anywhere in the dylib.
- Keep PID, `procLaunch`, and crash `captureTime` in the evidence index so logs cannot be paired by filename time alone.

## Final assessment

The crash is not a vanilla Hotbar null-handling bug exposed merely by SE timing. The SE dylib patches the middle of `HotbarSystem::Update` because `OFFSET_GET_CLASS` is stale, and `BG3SE_NO_HOOKS` unintentionally leaves that patch active. The exact patch address, crash address, register state, caller dataflow, duplicate Hotbar frame, and installation logs all converge on the same mechanism.
