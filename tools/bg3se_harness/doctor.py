"""Prerequisite verifier for bg3se-harness.

Checks that all required paths, permissions, and tools are available.
Reports actionable diagnostics as JSON.

Usage:
    bg3se-harness doctor
"""

from __future__ import annotations

import json
import os
import subprocess
import sys
from pathlib import Path

import stat

from .config import (
    BG3_APP_BUNDLE, BG3_EXEC, DEPLOYED_DYLIB, HARNESS_CONFIG_DIR,
    INSERT_DYLIB, MOD_CRASH_SANITY_CHECK_DIR, MODS_DIR, MODSETTINGS_PATH,
    SAVES_DIR, SOCKET_PATH, PROJECT_ROOT,
)
from . import launch as launch_mod
from . import patch as patch_mod


def _check(name, passed, detail=None, fix=None, severity="info", code=None):
    """Build a check result dict.

    severity: 'critical' (blocks launch), 'warning' (proceed with caution),
              'info' (diagnostic only).
    """
    result = {"name": name, "passed": passed, "severity": severity}
    if code:
        result["code"] = code
    if detail:
        result["detail"] = detail
    if fix and not passed:
        result["fix"] = fix
    return result


def run_doctor():
    """Run all diagnostic checks. Returns dict with checks array and summary."""
    checks = []

    # 1. BG3 app bundle
    checks.append(_check(
        "bg3_app_bundle",
        BG3_APP_BUNDLE.exists(),
        detail=str(BG3_APP_BUNDLE),
        fix="Install BG3 via Steam",
        severity="critical",
    ))

    # 2. BG3 binary
    checks.append(_check(
        "bg3_binary",
        BG3_EXEC.exists(),
        detail=str(BG3_EXEC),
        severity="critical",
    ))

    # 3. SE dylib built
    dylib_built = (PROJECT_ROOT / "build/lib/libbg3se.dylib").exists()
    checks.append(_check(
        "se_dylib_built",
        dylib_built,
        fix="Run: bg3se-harness build",
        severity="critical",
    ))

    # 4. SE dylib deployed
    checks.append(_check(
        "se_dylib_deployed",
        DEPLOYED_DYLIB.exists(),
        detail=str(DEPLOYED_DYLIB),
        fix="Run: bg3se-harness build (auto-deploys)",
        severity="critical",
    ))

    # 5. Binary patched
    patched = False
    try:
        patched = patch_mod.is_patched()
    except Exception:
        pass
    checks.append(_check(
        "binary_patched",
        patched,
        fix="Run: bg3se-harness patch",
        severity="critical",
    ))

    # 6. insert_dylib available
    checks.append(_check(
        "insert_dylib",
        INSERT_DYLIB.exists(),
        detail=str(INSERT_DYLIB),
        fix="Build insert_dylib from tools/vendor/insert_dylib/",
        severity="critical",
    ))

    # 7. Mods directory
    checks.append(_check(
        "mods_directory",
        MODS_DIR.exists(),
        detail=str(MODS_DIR),
        fix="Launch BG3 at least once to create Larian directories",
        severity="warning",
    ))

    # 8. modsettings.lsx
    modsettings_ok = MODSETTINGS_PATH.exists()
    checks.append(_check(
        "modsettings_lsx",
        modsettings_ok,
        detail=str(MODSETTINGS_PATH),
        fix="Launch BG3 at least once",
        severity="warning",
    ))

    # 9. Save directory
    checks.append(_check(
        "save_directory",
        SAVES_DIR.exists(),
        detail=str(SAVES_DIR),
        severity="info",
    ))

    # 10. Harness config dir writable
    try:
        HARNESS_CONFIG_DIR.mkdir(parents=True, exist_ok=True)
        test_file = HARNESS_CONFIG_DIR / ".doctor_test"
        test_file.write_text("ok")
        test_file.unlink()
        config_ok = True
    except OSError:
        config_ok = False
    checks.append(_check(
        "harness_config_writable",
        config_ok,
        detail=str(HARNESS_CONFIG_DIR),
        severity="critical",
    ))

    # 11. Game running?
    game_running = launch_mod.is_running()
    checks.append(_check(
        "game_running",
        game_running,
        detail="BG3 process detected" if game_running else "BG3 not running",
        severity="info",
    ))

    # 12. Socket alive?
    socket_alive = launch_mod.socket_alive()
    checks.append(_check(
        "se_socket",
        socket_alive,
        detail=SOCKET_PATH,
        severity="info",
    ))

    # 13. Accessibility permission (for menu automation)
    accessibility_ok = False
    try:
        result = subprocess.run(
            ["osascript", "-e",
             'tell application "System Events" to get name of first process'],
            capture_output=True, text=True, timeout=5,
        )
        accessibility_ok = result.returncode == 0
    except (subprocess.TimeoutExpired, OSError):
        pass
    checks.append(_check(
        "accessibility_permission",
        accessibility_ok,
        fix="System Settings > Privacy & Security > Accessibility > enable terminal app",
        severity="warning",
    ))

    # 14. BG3MacModManager installed?
    mmgr_installed = False
    mmgr_detail = "Not found"
    for app_dir in [Path.home() / "Applications", Path("/Applications")]:
        mmgr_path = app_dir / "BG3 Mac Mod Manager.app"
        if mmgr_path.exists():
            mmgr_installed = True
            mmgr_detail = str(mmgr_path)
            break
    checks.append(_check(
        "bg3macmodmanager",
        mmgr_installed,
        detail=mmgr_detail,
        fix="Optional: https://github.com/ShaiLaric/BG3MacModManager",
        severity="info",
    ))

    # 15. NoLauncher defaults set?
    nolauncher = False
    try:
        result = subprocess.run(
            ["defaults", "read", "com.larian.bg3", "NoLauncher"],
            capture_output=True, text=True,
        )
        nolauncher = result.stdout.strip() == "1"
    except OSError:
        pass
    checks.append(_check(
        "no_launcher_bypass",
        nolauncher,
        fix="Run: defaults write com.larian.bg3 NoLauncher 1",
        severity="warning",
    ))

    # 16. ModCrashSanityCheck directory (Patch 8+ footgun)
    sanity_exists = MOD_CRASH_SANITY_CHECK_DIR.exists()
    checks.append(_check(
        "mod_crash_sanity_check",
        not sanity_exists,
        detail="Not present (good)" if not sanity_exists else str(MOD_CRASH_SANITY_CHECK_DIR),
        fix=(
            "Delete this directory — since Patch 8, BG3 deactivates externally-managed "
            f"mods when it exists: rm -rf \"{MOD_CRASH_SANITY_CHECK_DIR}\""
        ),
        severity="warning",
    ))

    # 17. modsettings.lsx file locking (chflags uchg)
    modsettings_locked = False
    if MODSETTINGS_PATH.exists():
        try:
            modsettings_locked = bool(os.stat(MODSETTINGS_PATH).st_flags & stat.UF_IMMUTABLE)
        except (OSError, AttributeError):
            pass
    checks.append(_check(
        "modsettings_unlocked",
        not modsettings_locked,
        detail="Locked (chflags uchg)" if modsettings_locked else "Writable",
        fix=f'Unlock: chflags nouchg "{MODSETTINGS_PATH}"',
        severity="warning",
    ))

    # 18. Steam readiness
    steam = launch_mod.steam_readiness()
    steam_ok = steam["status"] == "ready"
    steam_detail = f"status={steam['status']}"
    steam_fix = None
    if steam["status"] == "absent":
        steam_fix = "Open Steam and wait for the Library to load"
    elif steam["status"] == "starting":
        steam_fix = "Wait for Steam IPC to finish initializing"
    checks.append(_check(
        "steam_readiness",
        steam_ok,
        detail=steam_detail,
        fix=steam_fix,
        severity="critical",
        code="steam_not_ready" if not steam_ok else None,
    ))

    # 19. Memory pressure
    mem = launch_mod.memory_pressure_check()
    mem_ok = mem["classification"] in ("pass", "warning")
    mem_detail = f"classification={mem['classification']}"
    if mem["free_percent"] is not None:
        mem_detail += f", free={mem['free_percent']}%"
    mem_severity = "info"
    if mem["classification"] == "critical":
        mem_severity = "critical"
    elif mem["classification"] == "warning":
        mem_severity = "warning"
    elif mem["classification"] == "unavailable":
        mem_severity = "warning"
    checks.append(_check(
        "memory_pressure",
        mem_ok,
        detail=mem_detail,
        fix="Close memory-intensive applications" if not mem_ok else None,
        severity=mem_severity,
        code="memory_pressure_critical" if mem["classification"] == "critical" else None,
    ))

    # 20. Windowed-mode profile attestation
    windowed = launch_mod.check_windowed_mode()
    windowed_ok = windowed.get("windowed", False)
    windowed_detail = "FakeFullscreenEnabled=0 (Windowed)" if windowed_ok else windowed.get("error", "unknown")
    checks.append(_check(
        "windowed_mode",
        windowed_ok,
        detail=windowed_detail,
        fix=(
            "In BG3: Options > Video > Display Mode > Windowed, then quit normally. "
            "This writes FakeFullscreenEnabled=0 to graphicSettings.lsx."
        ),
        severity="warning",
        code="window_mode_unverified" if not windowed_ok else None,
    ))

    # Summary
    passed = sum(1 for c in checks if c["passed"])
    total = len(checks)
    failed_critical = [
        c for c in checks if not c["passed"] and c.get("severity") == "critical"
    ]
    launch_blocked = len(failed_critical) > 0

    return {
        "checks": checks,
        "passed": passed,
        "total": total,
        "all_passed": passed == total,
        "launch_blocked": launch_blocked,
        "failed_critical": [c["name"] for c in failed_critical],
    }


def cmd_doctor(args):
    """CLI handler for doctor command."""
    result = run_doctor()
    print(json.dumps(result, indent=2))

    for check in result["checks"]:
        icon = "OK" if check["passed"] else "FAIL"
        sev = check.get("severity", "info")
        line = f"  [{icon}] {check['name']}"
        if sev != "info":
            line += f" [{sev}]"
        if "detail" in check:
            line += f" — {check['detail']}"
        print(line, file=sys.stderr)
        if not check["passed"] and "fix" in check:
            print(f"         Fix: {check['fix']}", file=sys.stderr)

    print(f"\n  {result['passed']}/{result['total']} checks passed", file=sys.stderr)
    if result.get("launch_blocked"):
        print(
            f"  LAUNCH BLOCKED: {', '.join(result['failed_critical'])}",
            file=sys.stderr,
        )
    return 0 if not result.get("launch_blocked") else 1
