# Documentation Update Proposal -- 2026-07-28 Evening Session

Proposed insertions and replacements for agent-facing docs. Each section names the target file, the anchor (section heading or line content to locate), and the exact markdown to insert or replace.

---

## 1. agent_docs/architecture.md

### 1a. Module Structure tree -- INSERT new entries

**Anchor:** Inside the `src/` tree block, after the `├── hooks/` line.

**Current:**
```
├── hooks/          # Legacy hook stubs (actual hooks in main.c)
```

**Replace with:**
```
├── hooks/          # ARM64 safe hooking + legacy stubs
│   └── arm64_hook.c/h     # Skip-and-redirect hooks, MAP_JIT trampolines
```

### 1b. Module Structure tree -- INSERT lua_gate into lua/ listing

**Anchor:** Inside the `src/` tree block, the `├── lua/` line.

**Current:**
```
├── lua/            # Lua API modules (lua_ext, lua_json, lua_osiris, lua_stats, lua_events, lua_logging, lua_level, lua_audio)
```

**Replace with:**
```
├── lua/            # Lua API modules + thread gate
│   ├── lua_gate.c/h        # Recursive mutex serializing all Lua entry points
│   └── lua_ext, lua_json, lua_osiris, lua_stats, lua_events, lua_logging, lua_level, lua_audio
```

### 1c. Key Files -- INSERT lua_gate and arm64_hook

**Anchor:** After the `src/game/focus_hack.c` Key Files entry.

**Insert:**
```
- `src/lua/lua_gate.c/h` - Recursive pthread mutex serializing all `lua_State` access across threads (see Lua Thread Safety below)
- `src/hooks/arm64_hook.c/h` - ARM64 skip-and-redirect inline hooks with MAP_JIT trampoline allocation
```

### 1d. Console Poll Timer (GCD) -- REPLACE section

**Anchor:** Section heading `## Console Poll Timer (GCD)`.

**Replace entire section with:**
```
## Console Poll Timer (GCD)

The socket console (`/tmp/bg3se.sock`) is polled by a GCD dispatch timer at 100ms intervals, independent of Osiris events. Without this, the socket only responded during gameplay (when `fake_Event()` fired Osiris hooks). The timer acquires the Lua gate via `lua_gate_trylock()` -- if another thread holds the gate, the tick is skipped (retry next 100ms).

Cleanup: timer cancelled in `shutdown_lua()` before Lua state destruction.
```

### 1e. NEW SECTION -- INSERT after Console Poll Timer

**Anchor:** After the Console Poll Timer (GCD) section, before `## Platform Notes`.

**Insert:**
```
## Lua Thread Safety (lua_gate)

The Lua VM is single-threaded, but four code paths enter it from different threads:

| Thread | Entry point | Gate strategy |
|--------|-------------|---------------|
| Game / ServerWorker | `fake_Event` tick block, `dispatch_event_to_lua` | `lua_gate_lock()` |
| GCD console timer | `console_poll` (100ms) | `lua_gate_trylock()` -- skip on contention |
| Metal render thread | `lua_imgui_fire_event` | `lua_gate_lock()` |
| Input hook thread | `lua_hotkey_callback` | `lua_gate_lock()` |
| Network callbacks | `callback_registry_invoke` | `lua_gate_lock()` |

**Any new code path that calls into the shared `lua_State` MUST acquire the gate.** The mutex is recursive (same-thread re-entry is safe for nested dispatch). Implementation: `src/lua/lua_gate.c/h` (pthread_mutex_t with PTHREAD_MUTEX_RECURSIVE, pthread_once init).

History: before lua_gate (v0.37.1), the GCD timer used a bare atomic flag that only prevented self-re-entry, not cross-thread races. The race was latent and surfaced under high Osiris event rates (2026-07-28 crash, PID 97193).
```

### 1f. NEW SECTION -- INSERT after Lua Thread Safety, before Platform Notes

**Anchor:** Before `## Platform Notes`.

**Insert:**
```
## MAP_JIT Trampolines (ARM64)

`arm64_alloc_near()` (`src/hooks/arm64_hook.c`) allocates trampoline pages with `MAP_JIT`. On Apple Silicon, MAP_JIT pages are execute-protected by default; writing requires bracketing with:
```c
pthread_jit_write_protect_np(0);  // enable writes
// ... write trampoline instructions ...
pthread_jit_write_protect_np(1);  // re-protect
sys_icache_invalidate(tramp, size);
```
Without the write gate, the trampoline build faults with `KERN_PROTECTION_FAILURE`. This is normally masked because `arm64_alloc_near` usually lands within +/-128MB (near branch); the far-trampoline fallback (absolute branch, 4 instructions) exercises the write path and faults under unlucky ASLR slides.
```

---

## 2. agent_docs/development.md

### 2a. Test count -- Built-in Console Commands table

**Anchor:** The `!test` row in the Built-in Console Commands table.

**Current:**
```
| `!test [filter]` | Run Tier 1 regression tests (93 tests, always works). Optional filter: `!test Stats`, `!test Parity` |
| `!test_ingame [filter]` | Run Tier 2 tests (54 tests, needs loaded save). Tests Entity, Level, Audio, Net, IMGUI, StaticData, Osi dispatch, EntityEvents, Parity |
```

**Replace with:**
```
| `!test [filter]` | Run Tier 1 regression tests (109 tests, always works). Optional filter: `!test Stats`, `!test Parity` |
| `!test_ingame [filter]` | Run Tier 2 tests (67 tests, needs loaded save). Tests Entity, Level, Audio, Net, IMGUI, StaticData, Osi dispatch, EntityEvents, Parity |
```

### 2b. Test Suite header and table

**Anchor:** `### Test Suite (229 tests)`

**Replace the heading and table (4 rows):**

**Current:**
```
### Test Suite (229 tests)

Four tiers, 229 total tests. Offline tiers (0 + H) run in CI. In-game tiers (1 + 2) are Lua C string constants registered via `BG3SE_AddTest(tier, name, fn)`.

| Tier | Command | Tests | Requires |
|------|---------|-------|----------|
| 0 | `./build/bin/bg3se_test_tier0` | 41 | None (CI-safe) |
| H | `PYTHONPATH=tools pytest tests/harness/ -v` | 41 | Python 3.12 (CI-safe) |
| 1 | `!test` | 93 | Console only (no save needed) |
| 2 | `!test_ingame` | 54 | Loaded save game |
```

**Replace with:**
```
### Test Suite (314 tests)

Four tiers, 314 total tests. Offline tiers (0 + H) run in CI. In-game tiers (1 + 2) are Lua C string constants registered via `BG3SE_AddTest(tier, name, fn)`.

| Tier | Command | Tests | Requires |
|------|---------|-------|----------|
| 0 | `./build/bin/bg3se_test_tier0` | 82 | None (CI-safe) |
| H | `PYTHONPATH=tools pytest tests/harness/ -v` | 56 | Python 3.12 (CI-safe) |
| 1 | `!test` | 109 | Console only (no save needed) |
| 2 | `!test_ingame` | 67 | Loaded save game |
```

### 2c. NEW SECTION -- INSERT at end of Debugging section

**Anchor:** After `- Osiris events logged with `[Osiris]` prefix` (end of ## Debugging).

**Insert:**
```

### Debugging Gotchas (macOS)

- **`log` is a zsh builtin.** Use `/usr/bin/log show` explicitly; bare `log` silently does nothing useful.
- **Missing .ips proves nothing.** If `ReportCrash` was jetsam-killed (common under memory pressure -- 100 JETSAM_MEMORY_IDLE_EXIT kills in 6 min observed), no crash report is written. Pair crash reports to sessions by PID + `procLaunch` timestamp, never by filename adjacency.
- **Steam-less sessions self-exit.** Without Steam running (or with `steam_appid.txt` absent), BG3 exits cleanly via `proc_exit` after ~90-150s (Steamworks DRM grace timeout). No crash report is generated. Place `steam_appid.txt` next to the game binary to suppress `SteamAPI_RestartAppIfNecessary`.
- **Windowed mode is NOT controlled by `graphicSettings.lsx` on macOS.** The macOS build's `graphicSettings.lsx` has only 16 MapKeys (no window-mode key). Window mode persists in binary `PlayerProfiles/Public/config.lsf` -- set once via in-game Options > Video. The harness's `HEADLESS_GRAPHICS_ENTRIES` controls Fullscreen/FakeFullscreen flags only.
```

---

## 3. CLAUDE.md

### 3a. Test count line

**Anchor:** Line containing `Use `!test` to run Tier 1 regression tests (93 tests`.

**Current:**
```
Use `!test` to run Tier 1 regression tests (93 tests, always works). Use `!test_ingame` for Tier 2 tests (54 tests, needs loaded save). Use `Debug.*` helpers for memory probing. 82 offline tests (41 C + 41 pytest) run via CI.
```

**Replace with:**
```
Use `!test` to run Tier 1 regression tests (109 tests, always works). Use `!test_ingame` for Tier 2 tests (67 tests, needs loaded save). Use `Debug.*` helpers for memory probing. 138 offline tests (82 C + 56 pytest) run via CI.
```

### 3b. Structure listing -- INSERT lua_gate and arm64_hook

**Anchor:** The `- `ghidra/offsets/` - Reverse-engineered offsets documentation` line in ## Structure.

**Insert before that line:**
```
- `src/lua/lua_gate.c/h` - Recursive mutex serializing all Lua VM entry points (thread safety)
- `src/hooks/arm64_hook.c/h` - ARM64 inline hooks with MAP_JIT trampolines
```

### 3c. Key Offsets table -- STALE WARNING

**Anchor:** Section `## Key Offsets (Ghidra-verified)`.

**Insert after the section heading, before the table:**
```
**Build 4.1.1.7209685 (current).** Offsets were migrated 2026-07-28; prior values (build 6995620) are invalid. Every hardcoded address is validated by `tests/harness/test_offset_audit.py` against `nm` on the installed binary.

```

---

## 4. agent_docs/harness.md

### 4a. NEW SUBSECTION -- INSERT after Headless Mode, before Game Inspection

**Anchor:** After the `### Headless Mode` paragraph, before `### Game Inspection`.

**Insert:**
```
### Session Driver (`scripts/session_driver.sh`)
Stall-detecting wrapper that drives BG3 from launch through a running session. Each game state has a stall budget; on timeout it captures diagnostics (screenshot + SE log + game state) into a JSONL file and attempts bounded recovery (menu Continue click from live window geometry). Verdicts: `SESSION_RUNNING` (exit 0), `GAME_DIED` (exit 1), `STUCK` (exit 2), `REPLACED` (exit 3, PID changed mid-drive). Note: BSD `find -newermt` rejects `@epoch` syntax silently -- use a human-readable timestamp.

### Steam Environment
- **`steam_appid.txt`** next to the game binary suppresses `SteamAPI_RestartAppIfNecessary`. Without Steam running, direct-launched sessions self-exit after ~90-150s (DRM grace) with a clean `proc_exit` and no crash report.
- With Steam running but no `steam_appid.txt`, direct launch exits 0 at ~1.3s and Steam relaunches the game ~10s later (bounce).
```
