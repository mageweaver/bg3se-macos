# BG3SE Test Harness

37-command Python CLI for autonomous BG3 operation. All commands emit JSON to stdout.

## Invocation

```bash
PYTHONPATH=tools python3 -m bg3se_harness <command> [args]
```

## Command Groups

### Lifecycle (build → patch → launch → test)
- `status` — game/socket/patch state (no side effects)
- `build` — cmake + deploy dylib to Steam folder
- `patch` / `unpatch` — insert_dylib Mach-O patching
- `launch [--headless] [--continue|--save NAME]` — full pipeline, skips intro videos
- `test [filter] [--tier 2]` — run Tier 1 or Tier 2 Lua tests

### Headless Mode
`--headless` applies a transient 1280x720 size via graphicSettings.lsx and hides the window via System Events after socket connects (~3.3s); the transient keys are restored automatically. **Display mode is the user's in-game choice, stored as `FakeFullscreenEnabled` in graphicSettings.lsx** (absent = borderless fake-fullscreen; `0` = windowed — set once via Options → Video → Display Mode → Windowed, verified 2026-07-28). The harness verifies the key at preflight but never writes it, and restores must be per-key so they can't clobber it. Socket responds at main menu (GCD poll timer, independent of Osiris events).

### Session Driver (`scripts/session_driver.sh`)
Stall-detecting wrapper that drives BG3 from launch through a running session. Each game state has a stall budget; on timeout it captures diagnostics (screenshot + SE log + game state) into a JSONL file and attempts bounded recovery. Verdicts: `SESSION_RUNNING` (0), `GAME_DIED` (1), `STUCK` (2), `REPLACED` (3, PID changed mid-drive), `PREFLIGHT_FAILED` (4, Steam/memory/windowed gate), `AMBIGUOUS_ATTACH` (5, zero or multiple candidate processes). Note: BSD `find -newermt` rejects `@epoch` syntax silently — use a human-readable timestamp.

### Steam Environment
- **`steam_appid.txt`** (1086940) next to the game binary suppresses `SteamAPI_RestartAppIfNecessary`. Without Steam running, direct-launched sessions self-exit after ~90-150s (DRM grace) with a clean `proc_exit` and no crash report.
- With Steam running but no `steam_appid.txt`, direct launch exits 0 at ~1.3s and Steam relaunches the game ~10s later (bounce) — the relaunched PID differs and rebinds `/tmp/bg3se.sock`; verify with `!identity` before trusting the socket.

### Game Inspection (requires running game + socket)
- `run "<lua>"` / `eval script.lua` / `watch script.lua` — Lua execution
- `entity <GUID>` / `stats <name>` / `components` / `probe <addr>` — data inspection
- `dump spells|items|...` / `events --subscribe X` — bulk extract / stream
- `screenshot` — JPEG capture (1568px max, Claude-safe)

### Mod Management
- `mod list|install|enable|disable|remove|info|order|search|backup`
- `mod changelog|versions|updated` — Nexus Mods API (stdlib urllib, no deps)

### Web Integrations
- `wiki spell|item|verify|clear-cache` — bg3.wiki parsing (24h file cache)

### Menu Automation (Vision OCR + CGEvent)
- `menu detect` — OCR main menu buttons → JSON
- `menu click "Continue"` / `menu click-fraction X Y` — coordinate-based clicks
- `menu geometry [--capture]` — debug coordinate systems

### Diagnostics & RE
- `crashlog` — parse mmap ring buffer (no socket needed)
- `benchmark "<lua>"` — per-call timing
- `doctor` — verify all prerequisites
- `flags` — 40 discovered BG3 CLI flags
- `ghidra decompile|search-strings|search-functions|xrefs` — Ghidra HTTP bridge
- `parity scan|missing` — Windows BG3SE API comparison
- `compat list|run|vet` — mod compatibility testing

## Architecture

```
tools/bg3se_harness/
├── cli.py              # argparse dispatch
├── config.py           # paths, timeouts
├── build.py            # cmake + deploy
├── patch.py            # insert_dylib + codesign
├── launch.py           # launch + socket health + headless graphics
├── console.py          # Unix socket IPC
├── test_runner.py      # test execution + JSON output
├── menu.py             # Vision OCR + CGEvent clicks
├── wiki.py             # bg3.wiki client (24h cache)
├── ghidra.py           # Ghidra HTTP bridge
├── mod_cli.py          # mod subcommand router
└── mod_manager/        # Nexus API, installer, modsettings, registry
```

## Design Conventions
- **JSON on stdout** — every command. Exit 1 on failure.
- **Error envelope** — `{"success": false, "error_type": str, "message": str}`
- **Stdlib-only HTTP** — `urllib.request`, no pip dependencies
- **Idempotent patch** — checks otool before patching, SHA-256 hash tracking

## When to Use
- **Claude testing BG3SE**: `launch --headless` → `run` / `test` → `screenshot`
- **Mod vetting**: `mod install` → `compat vet` → check logs
- **RE sessions**: `ghidra decompile` + `probe` + `entity`
- **CI/offline**: `build` + Tier 0/H tests (no game needed)

Full reference: `docs/harness.md`
