# Harness Pipeline E2E Test Analysis

*Codex GPT-5.5 researcher review — 2026-05-03*

## Mock Architecture

### Recommended Fixtures

1. **`fake_socket`** — Thread-based Unix domain socket server that sends scripted test output lines. Enables socket communication testing without BG3.
2. **`fake_process`** — `SimpleNamespace` with configurable `poll()`, `returncode`, `pid`. Tests crash detection and process lifecycle.
3. **`fake_subprocess`** — Monkeypatch `subprocess.run`/`Popen` to return scripted results for CMake, insert_dylib, osascript, codesign.
4. **`fake_filesystem`** — `tmp_path` + monkeypatched path constants. Tests mod install/enable/disable without real PAK files.
5. **`fake_http`** — Existing pattern in `tests_wiki.py` and `tests_nexus.py` should be reused for wiki/Nexus CLI tests.

## Pipeline Integration Tests

### Build -> Verify -> Deploy

- Missing `build/` returns stage `build` and nonzero CLI result.
- CMake failure preserves last 500 chars of stderr.
- `file` output with only `arm64` fails universal verification.
- Missing dylib after successful build fails deploy.
- Deploy copies to configured app bundle path.
- Stale CMake cache or wrong SDK should be represented by a scripted CMake stderr fixture and asserted as surfaced JSON.

### Patch -> Launch -> Socket

- Missing BG3 executable fails before insert_dylib.
- Missing `insert_dylib` fails after patch-state check.
- First insert attempt fails, retry with `--strip-codesig` succeeds.
- `otool` verification missing `libbg3se` causes patch failure.
- Process exits before socket responds returns `stage="process_exited"`, not generic timeout.
- Socket accepts but never returns version should not count as connected.
- Socket returns ANSI-colored version string; parser strips ANSI.

### Socket -> Test Runner

- Parses pass/fail/summary.
- Parses `[SLOW Nms]` token.
- Handles malformed unrelated lines.
- Handles partial output with no summary by deriving summary from test lines.
- Empty output should set `all_passed=False`.
- Socket connection errors return structured error, not exception.

### Headless Mode

- `launch --headless` calls `wait_for_socket(dismiss_splash=True)` before `hide_window`.
- If socket fails, `hide_window` is not called.
- `test --headless` hides only after socket connection and before `run_tests`.
- Crash during boot returns process exit details and does not hide.
- Splash dismiss attempts stop after first successful socket response.

### Mod Install -> Enable -> Launch -> Vet

- `mod install local.pak --no-enable` calls installer with `enable=False`.
- `mod enable <name>` resolves registry name to UUID before touching modsettings.
- `compat run` should install declared local mods or hard-fail with actionable missing artifact, not silently log manual install as success.
- `requires_mcm` in catalog/manifests should enforce MCM presence in scenario setup.
- `compat vet` should launch when `no_launch=False`, or rename the option to match current behavior.
- Log scanning should be launch-scoped by timestamp/marker.

## Regression Tests for Known Bugs

1. **Headless early-hide bug**: Assert call order: `launch`, `wait_for_socket`, `hide_window`. Assert `hide_window` not called before splash dismissal path.

2. **Socket timeout vs process exit**: Fake process `poll()` returns `7`. Expected result: `{"stage": "process_exited", "exitcode": 7}`.

3. **Stale crash/latest log**: Create old `latest.log` with mod error. Start run marker after file mtime. Vet should ignore old lines.

4. **`[SLOW ...]` token**: `PASS: Foo.Bar (1200ms) [SLOW 1000ms] [1/1]` must parse.

5. **`mod enable` name support**: Registry contains `{"uuid-1": {"name": "My Mod"}}`. `mod enable "My Mod"` must call `enable_mod("uuid-1")` or `add_mod(...)`.

## CI/CD Readiness

### Recommended Tiers

| Tier | Runner | BG3 Required | Runtime | Scope |
|------|--------|-------------|---------|-------|
| `offline-fast` | Linux + macOS | No | <30s | Parser tests, fake filesystem/subprocess/socket, mocked HTTP |
| `macos-integration` | GitHub Actions macOS | No | ~1min | AppleScript/CGEvent wrappers via mocks, macOS path handling |
| `local-live` | Developer machine | Yes | ~5min | Build/patch/launch/socket/test against real game |
| `full-e2e` | Self-hosted macOS | Yes | ~15min | All mods, save fixtures, compat matrix, Accessibility permission |

**Gating env vars:** `BG3SE_LIVE=1`, `BG3_APP_PATH`, `NEXUS_API_KEY`

## Concrete pytest Test Cases

```python
import argparse
import json
import socket
import threading
import time
from pathlib import Path
from types import SimpleNamespace

import pytest

from bg3se_harness import cli, launch, test_runner


def test_parse_test_output_accepts_slow_token():
    raw = "  PASS: Core.Slow (1200ms) [SLOW 1000ms] [1/1]\n=== Results: 1/1 passed, 0 failed, 0 skipped (1200ms) ==="
    tests, summary = test_runner.parse_test_output(raw)
    assert tests == [{
        "name": "Core.Slow",
        "status": "pass",
        "ms": 1200,
        "error": None,
        "index": 1,
        "total": 1,
    }]
    assert summary["passed"] == 1


def test_parse_test_output_derives_summary_when_missing(monkeypatch):
    raw = "  PASS: Core.Print (2ms) [1/2]\n  FAIL: Core.Bad (3ms) - nope [2/2]"
    tests, summary = test_runner.parse_test_output(raw)
    assert summary is None
    assert len(tests) == 2


def test_run_tests_returns_structured_socket_error(monkeypatch):
    class BrokenConsole:
        def __init__(self, *a, **k): pass
        def __enter__(self): raise FileNotFoundError("/tmp/bg3se.sock")
        def __exit__(self, *a): pass

    monkeypatch.setattr(test_runner, "Console", BrokenConsole)
    result = test_runner.run_tests(tier=1)
    assert result["all_passed"] is False
    assert result["tests"] == []
    assert "Socket connection failed" in result["error"]


def test_wait_for_socket_reports_process_exit(monkeypatch):
    proc = SimpleNamespace(returncode=42)
    proc.poll = lambda: 42
    result = launch.wait_for_socket(timeout=10, process=proc)
    assert result["socket_connected"] is False
    assert result["stage"] == "process_exited"
    assert result["exitcode"] == 42


def test_wait_for_socket_dismisses_before_socket_only(monkeypatch):
    calls = []
    ticks = iter([0, 6, 6.1, 7])

    monkeypatch.setattr(launch.time, "monotonic", lambda: next(ticks))
    monkeypatch.setattr(launch.time, "sleep", lambda _: None)
    monkeypatch.setattr(launch, "_try_dismiss_splash", lambda n: calls.append(("dismiss", n)))

    class FakeSocket:
        def settimeout(self, _): pass
        def connect(self, _): raise FileNotFoundError()
        def close(self): pass

    monkeypatch.setattr(launch.socket, "socket", lambda *a, **k: FakeSocket())
    result = launch.wait_for_socket(timeout=7, dismiss_splash=True)

    assert result["socket_connected"] is False
    assert calls == [("dismiss", 1)]


def test_cmd_launch_headless_hides_after_socket(monkeypatch):
    calls = []

    monkeypatch.setattr(cli.build_mod, "build", lambda: calls.append("build") or {"success": True})
    monkeypatch.setattr(cli.build_mod, "verify", lambda: calls.append("verify") or {"verified": True})
    monkeypatch.setattr(cli.build_mod, "deploy", lambda: calls.append("deploy") or {"deployed": True})
    monkeypatch.setattr(cli.patch_mod, "patch", lambda: calls.append("patch") or {"success": True})
    monkeypatch.setattr(cli.launch_mod, "launch", lambda **k: calls.append("launch") or SimpleNamespace(pid=123, poll=lambda: None))
    monkeypatch.setattr(cli.launch_mod, "default_timeout", lambda *a: 1)
    monkeypatch.setattr(cli.launch_mod, "wait_for_socket", lambda **k: calls.append("wait") or {"socket_connected": True})
    monkeypatch.setattr(cli.launch_mod, "hide_window", lambda: calls.append("hide") or {"success": True})

    args = argparse.Namespace(headless=True, timeout=1, continue_game=False, save=None, skip_videos=True)
    rc = cli.cmd_launch(args)

    assert rc == 0
    assert calls.index("wait") < calls.index("hide")


def test_cmd_launch_headless_does_not_hide_on_socket_failure(monkeypatch):
    monkeypatch.setattr(cli.build_mod, "build", lambda: {"success": True})
    monkeypatch.setattr(cli.build_mod, "verify", lambda: {"verified": True})
    monkeypatch.setattr(cli.build_mod, "deploy", lambda: {"deployed": True})
    monkeypatch.setattr(cli.patch_mod, "patch", lambda: {"success": True})
    monkeypatch.setattr(cli.launch_mod, "launch", lambda **k: SimpleNamespace(pid=123, poll=lambda: None))
    monkeypatch.setattr(cli.launch_mod, "default_timeout", lambda *a: 1)
    monkeypatch.setattr(cli.launch_mod, "wait_for_socket", lambda **k: {"socket_connected": False})
    monkeypatch.setattr(cli.launch_mod, "hide_window", lambda: pytest.fail("hide_window should not be called"))

    args = argparse.Namespace(headless=True, timeout=1, continue_game=False, save=None, skip_videos=True)
    assert cli.cmd_launch(args) == 1


def test_cmd_build_stops_on_cmake_failure(monkeypatch):
    monkeypatch.setattr(cli.build_mod, "build", lambda: {"success": False, "error": "SDK missing"})
    assert cli.cmd_build(argparse.Namespace()) == 1


def test_cmd_build_fails_non_universal_binary(monkeypatch):
    monkeypatch.setattr(cli.build_mod, "build", lambda: {"success": True})
    monkeypatch.setattr(cli.build_mod, "verify", lambda: {"verified": False, "arm64": True, "x86_64": False})
    monkeypatch.setattr(cli.build_mod, "deploy", lambda: {"deployed": True})
    assert cli.cmd_build(argparse.Namespace()) == 1


def test_cmd_test_process_exit_is_reported_not_confused_with_timeout(monkeypatch):
    monkeypatch.setattr(cli.build_mod, "build", lambda: {"success": True})
    monkeypatch.setattr(cli.build_mod, "verify", lambda: {"verified": True})
    monkeypatch.setattr(cli.build_mod, "deploy", lambda: {"deployed": True})
    monkeypatch.setattr(cli.patch_mod, "patch", lambda: {"success": True})
    monkeypatch.setattr(cli.launch_mod, "launch", lambda **k: SimpleNamespace(pid=55, poll=lambda: 9, returncode=9))
    monkeypatch.setattr(cli.launch_mod, "default_timeout", lambda *a: 1)
    monkeypatch.setattr(cli.launch_mod, "wait_for_socket", lambda **k: {"socket_connected": False, "stage": "process_exited", "exitcode": 9})

    args = argparse.Namespace(headless=False, tier=1, filter=None, continue_game=True, save=None, skip_videos=True)
    assert cli.cmd_test(args) == 1


def test_mod_enable_name_should_resolve_registry_before_modsettings(monkeypatch):
    from bg3se_harness import mod_cli

    seen = {}
    def fake_enable(uuid):
        seen["uuid"] = uuid
        return {"enabled": True, "uuid": uuid}

    monkeypatch.setattr("bg3se_harness.mod_manager.modsettings.enable_mod", fake_enable)
    args = argparse.Namespace(mod_command="enable", name="My Mod")

    # Expected future behavior: resolve "My Mod" to UUID before calling enable_mod.
    # This currently fails because cmd_mod passes the name directly.
    mod_cli.cmd_mod(args)
    assert seen["uuid"] != "My Mod"


def test_save_restore_should_backup_existing_harness_restore(tmp_path, monkeypatch):
    from bg3se_harness import savegames

    saves = tmp_path / "saves"
    fixtures = tmp_path / "fixtures"
    existing = saves / "Harness__Base"
    fixture = fixtures / "Base"
    existing.mkdir(parents=True)
    fixture.mkdir(parents=True)
    (existing / "old.lsv").write_text("old")
    (fixture / "new.lsv").write_text("new")

    monkeypatch.setattr(savegames, "SAVES_DIR", saves)
    monkeypatch.setattr(savegames, "SAVE_FIXTURES_DIR", fixtures)

    result = savegames.restore("Base")
    assert result["success"] is True

    # Expected future behavior: backup path returned and old data preserved.
    assert "backup" in result


def test_compat_log_scan_should_ignore_stale_lines(tmp_path, monkeypatch):
    from bg3se_harness import compat

    log_dir = tmp_path / "Library/Application Support/BG3SE/logs"
    log_dir.mkdir(parents=True)
    latest = log_dir / "latest.log"
    latest.write_text("[old] My Mod error from previous run\n")

    monkeypatch.setattr(Path, "home", lambda: tmp_path)
    errors, warnings = compat._scan_log_for_mod("My Mod")

    # Expected future behavior: scan should accept a run start timestamp/marker and ignore old lines.
    assert errors == []
```

## Recommended Next Steps

1. Convert `tools/bg3se_harness/tests.py` into pytest or add a new `tests/harness/` pytest suite.
2. Add fixture-driven tests for `cli.cmd_launch`, `cli.cmd_test`, `launch.wait_for_socket`, `test_runner.parse_test_output`, `mod_cli.cmd_mod`, `savegames.restore`, and `compat.vet_mod`.
3. Refactor modules to accept injectable path/process/socket/menu dependencies where monkeypatching currently needs module internals.
4. Split CI into `offline-fast`, `macos-mocked`, `local-live`, and `self-hosted-full-e2e`.
