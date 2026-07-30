"""Mod compatibility test runner.

Orchestrates mod install + save restore + launch + test + report for
automated compatibility verification of popular BG3SE mods.

Scenarios are defined as JSON manifests in catalog/scenarios/.

Usage:
    bg3se-harness compat list              # Available scenarios
    bg3se-harness compat run <scenario>    # Run a scenario end-to-end
    bg3se-harness compat matrix            # Run all scenarios
"""

from __future__ import annotations

import json
import os
import re
import shutil
import sys
import tempfile
import time
from pathlib import Path

from .config import CATALOG_DIR, SCENARIOS_DIR, REPORTS_DIR


def _load_scenarios():
    """Load all scenario manifests from the scenarios/ directory."""
    scenarios = {}
    if not SCENARIOS_DIR.exists():
        return scenarios
    for f in SCENARIOS_DIR.glob("*.json"):
        try:
            data = json.loads(f.read_text())
            data["_file"] = f.name
            scenarios[f.stem] = data
        except (json.JSONDecodeError, OSError) as exc:
            # A corrupt manifest must not silently vanish from the catalog —
            # the caller would see "scenario not found" with no diagnostic.
            print(
                f"warning: skipping unreadable scenario {f.name}: {exc}",
                file=sys.stderr,
            )
    return scenarios


def _load_popular_mods():
    """Load the popular mods catalog."""
    path = CATALOG_DIR / "popular_mods.json"
    if not path.exists():
        return {}
    try:
        data = json.loads(path.read_text())
        mods = data.get("mods", {})
        return mods if isinstance(mods, dict) else {}
    except (json.JSONDecodeError, OSError):
        return {}


def list_scenarios():
    """List available compatibility test scenarios."""
    scenarios = _load_scenarios()
    catalog = _load_popular_mods()

    items = []
    for name, data in scenarios.items():
        items.append({
            "name": name,
            "description": data.get("description", ""),
            "mods": data.get("mods", []),
            "save_fixture": data.get("save_fixture", ""),
        })

    # Also list catalog mods that could be scenarios
    for mod_key, mod_info in catalog.items():
        if mod_key not in scenarios:
            items.append({
                "name": mod_key,
                "description": f"{mod_info['name']} (catalog entry, no scenario manifest)",
                "mods": [mod_key],
                "save_fixture": "",
                "has_manifest": False,
            })

    return {"scenarios": items, "count": len(items)}


def _parse_assertion_output(output, tail_limit=500):
    """Parse the last compat sentinel in console output."""
    text = output or ""
    pass_marker = "BG3SE_COMPAT_PASS"
    fail_marker = "BG3SE_COMPAT_FAIL:"
    pass_at = text.rfind(pass_marker)
    fail_at = text.rfind(fail_marker)

    if pass_at >= 0 and pass_at > fail_at:
        return {"success": True, "output": text[-tail_limit:]}
    if fail_at >= 0:
        error = text[fail_at + len(fail_marker):].splitlines()[0].strip()
        return {
            "success": False,
            "error": error or "assertion failed without an error message",
            "output": text[-tail_limit:],
        }
    return {
        "success": False,
        "error": "no sentinel in output",
        "output_tail": text[-tail_limit:],
    }


def _find_installed_mod(registry, mod_name):
    """Return ``(uuid, entry)`` using the catalog-name match used by vet_mod."""
    name_lower = mod_name.lower()
    for uuid, entry in registry.items():
        if name_lower in (entry.get("name") or "").lower():
            return uuid, entry
    return None, None


def _baseline_path(scenario_name):
    from .config import PROJECT_ROOT
    return PROJECT_ROOT / "docs" / "compat-reports" / "baseline" / f"{scenario_name}.json"


def _catalog_error(catalog):
    """Return why the catalog is unavailable, or None for a valid catalog."""
    if catalog:
        return None
    path = CATALOG_DIR / "popular_mods.json"
    if not path.exists():
        return f"catalog missing: {path}"
    try:
        data = json.loads(path.read_text())
    except (json.JSONDecodeError, OSError) as exc:
        return f"catalog unreadable: {exc}"
    if not isinstance(data, dict) or not isinstance(data.get("mods", {}), dict):
        return "catalog has an invalid mods mapping"
    return None


def run_scenario(
    scenario_name,
    launch=False,
    auto_install=False,
    save_baseline=False,
):
    """Run a compatibility test scenario.

    Live launch/socket work is opt-in through *launch*.  Missing mods are only
    downloaded when *auto_install* is true.
    """
    scenarios = _load_scenarios()
    catalog = _load_popular_mods()

    # Try manifest first, then catalog entry
    if scenario_name in scenarios:
        scenario = scenarios[scenario_name]
    elif scenario_name in catalog:
        mod_info = catalog[scenario_name]
        scenario = {
            "description": f"Auto-generated scenario for {mod_info['name']}",
            "mods": [scenario_name],
            "assertions": [
                "assert(Ext.Utils.Version() ~= nil, 'SE loaded')",
                f"assert(Ext.Mod.IsModLoaded ~= nil, 'Ext.Mod available')",
            ],
        }
    else:
        available = sorted(list(scenarios.keys()) + list(catalog.keys()))
        return {
            "error": f"Scenario '{scenario_name}' not found",
            "available": available,
        }

    run_id = f"{scenario_name}_{int(time.time())}"
    report_dir = REPORTS_DIR / run_id
    report_dir.mkdir(parents=True, exist_ok=True)

    results = {
        "scenario": scenario_name,
        "run_id": run_id,
        "description": scenario.get("description", ""),
        "report_dir": str(report_dir),
        "steps": [],
        "started_at": time.strftime("%Y-%m-%d %H:%M:%S"),
    }

    def log_step(name, result):
        step = {"success": result.get("success", "error" not in result), **result, "name": name}
        results["steps"].append(step)
        status = "SKIP" if step.get("status") == "not_run" else ("OK" if step["success"] else "FAIL")
        print(f"  [{status}] {name}", file=sys.stderr)
        return step["success"]

    print(f"Running compat scenario: {scenario_name}", file=sys.stderr)

    original_mod_keys = scenario.get("mods", [])
    if not isinstance(original_mod_keys, list):
        original_mod_keys = []
    requires_mcm = any(
        catalog.get(mod_key, {}).get("requires_mcm") is True
        for mod_key in original_mod_keys
    )
    effective_mod_keys = list(original_mod_keys)
    if requires_mcm:
        effective_mod_keys = ["mcm", *effective_mod_keys]
    # dict.fromkeys keeps first occurrence, so the prepended mcm stays first.
    effective_mod_keys = list(dict.fromkeys(effective_mod_keys))
    results["mods"] = effective_mod_keys
    catalog_error = _catalog_error(catalog)
    log_step("dependency_resolution", {
        "success": catalog_error is None,
        "requires_mcm": requires_mcm,
        "mcm_injected": requires_mcm and "mcm" not in original_mod_keys,
        "original_mods": original_mod_keys,
        "effective_mods": effective_mod_keys,
        "error_type": "catalog_unavailable" if catalog_error else None,
        "error": catalog_error,
    })

    # Check prerequisites.
    from .doctor import run_doctor
    try:
        doctor_result = run_doctor()
    except Exception as exc:
        doctor_result = {"checks": [], "passed": 0, "total": 0, "error": str(exc)}
    if not isinstance(doctor_result, dict):
        doctor_result = {
            "checks": [],
            "passed": 0,
            "total": 0,
            "error": "doctor returned an invalid response",
        }
    critical_checks = ["bg3_app_bundle", "bg3_binary", "se_dylib_deployed", "mods_directory"]
    checks = doctor_result.get("checks")
    checks_by_name = {
        check.get("name"): check
        for check in checks or []
        if isinstance(check, dict)
    }
    critical_passed = all(
        name in checks_by_name and checks_by_name[name].get("passed") is True
        for name in critical_checks
    )
    log_step("prerequisites", {
        "success": critical_passed,
        "passed": doctor_result.get("passed", 0),
        "total": doctor_result.get("total", len(checks or [])),
        "critical_checks": critical_checks,
        "error_type": "doctor_unavailable" if doctor_result.get("error") else None,
        "error": doctor_result.get("error"),
    })

    if not critical_passed:
        results["success"] = False
        results["error"] = "Critical prerequisites not met"
        _save_report(report_dir, results)
        return results

    # Resolve installed/enabled state and optionally install missing mods.
    from .mod_manager.registry import load_registry
    try:
        registry = load_registry()
        registry_error = None
    except (OSError, ValueError, TypeError) as exc:
        registry = {}
        registry_error = str(exc)
    if not isinstance(registry, dict):
        registry = {}
        registry_error = "registry returned an invalid response"

    mod_state_changed = False
    for mod_key in effective_mod_keys:
        mod_info = catalog.get(mod_key, {})
        mod_name = mod_info.get("name", mod_key)

        if registry_error:
            log_step(f"mod_check_{mod_key}", {
                "success": False,
                "mod_name": mod_name,
                "error_type": "registry_unavailable",
                "error": registry_error,
            })
            continue

        installed_uuid, entry = _find_installed_mod(registry, mod_name)
        if not installed_uuid:
            if not auto_install:
                log_step(f"mod_check_{mod_key}", {
                    "success": False,
                    "mod_name": mod_name,
                    "state": "missing",
                    "note": "not installed; rerun with --auto-install",
                })
                continue

            nexus_id = mod_info.get("nexus_id")
            if not nexus_id:
                log_step(f"mod_check_{mod_key}", {
                    "success": False,
                    "mod_name": mod_name,
                    "state": "missing",
                    "error_type": "missing_nexus_id",
                    "error": f"No Nexus mod ID is configured for {mod_name}.",
                })
                continue

            from .mod_manager.nexus import download_file
            from .mod_manager.installer import install_local

            temp_dir = tempfile.mkdtemp(prefix=f"bg3se-compat-{mod_key}-")
            try:
                # An explicit catalog file_id overrides Nexus primary-file
                # auto-pick (e.g. mods whose newest primary is a translation).
                download = download_file(
                    nexus_id, temp_dir, file_id=mod_info.get("file_id"),
                )
            except Exception as exc:
                download = {
                    "success": False,
                    "error_type": "download_unavailable",
                    "message": str(exc),
                }
            if not download.get("success"):
                log_step(f"mod_check_{mod_key}", {
                    "success": False,
                    "mod_name": mod_name,
                    "state": "download_failed",
                    "error_type": download.get("error_type", "download_failed"),
                    "error": download.get("message") or download.get("error"),
                    "download": download,
                    "temp_dir": temp_dir,
                    "cleanup": "temp_dir_retained_for_debugging",
                })
                continue

            paks = download.get("paks") or []
            if not paks:
                log_step(f"mod_check_{mod_key}", {
                    "success": False,
                    "mod_name": mod_name,
                    "state": "downloaded_no_paks",
                    "error_type": "no_paks",
                    "note": "download completed but contained no installable .pak files",
                    "download": download,
                    "temp_dir": temp_dir,
                    "cleanup": "temp_dir_retained_for_debugging",
                })
                continue

            install_results = []
            for pak_path in paks:
                try:
                    install_result = install_local(pak_path, enable=True)
                except Exception as exc:
                    install_result = {"error": str(exc)}
                install_results.append({"pak": pak_path, **install_result})
                if install_result.get("installed") is True:
                    mod_state_changed = True

            install_success = all(
                result.get("installed") is True and "error" not in result
                for result in install_results
            )
            if install_success:
                shutil.rmtree(temp_dir, ignore_errors=True)
            log_step(f"mod_check_{mod_key}", {
                "success": install_success,
                "mod_name": mod_name,
                "state": "installed" if install_success else "install_failed",
                "download": download,
                "installs": install_results,
                "temp_dir": temp_dir,
                "cleanup": (
                    "temp_dir_removed" if install_success
                    else "temp_dir_retained_for_debugging"
                ),
            })
            continue

        from .mod_manager.modsettings import read_mod_order, add_mod
        try:
            mod_order = read_mod_order()
        except (OSError, ValueError, TypeError) as exc:
            mod_order = [{"error": str(exc)}]
        if not isinstance(mod_order, list):
            mod_order = [{"error": "modsettings returned an invalid response"}]

        if mod_order and isinstance(mod_order[0], dict) and "error" in mod_order[0]:
            log_step(f"mod_check_{mod_key}", {
                "success": False,
                "mod_name": mod_name,
                "uuid": installed_uuid,
                "state": "installed_unknown_enablement",
                "error_type": "modsettings_unavailable",
                "error": mod_order[0]["error"],
            })
            continue

        if any(mod.get("uuid") == installed_uuid for mod in mod_order):
            log_step(f"mod_check_{mod_key}", {
                "success": True,
                "mod_name": mod_name,
                "uuid": installed_uuid,
                "state": "installed_enabled",
            })
            continue

        try:
            enable_result = add_mod(
                uuid=installed_uuid,
                name=entry.get("name") or installed_uuid,
                folder=entry.get("folder") or entry.get("name") or installed_uuid,
                version=entry.get("version") or "36028797018963968",
                md5=entry.get("md5", ""),
            )
        except Exception as exc:
            enable_result = {"error": str(exc)}
        enabled = "error" not in enable_result
        registry_result = None
        if enabled:
            from .mod_manager.registry import set_mod_enabled
            try:
                registry_result = set_mod_enabled(installed_uuid, True)
            except Exception as exc:
                registry_result = {"updated": False, "error": str(exc)}
            if not (
                isinstance(registry_result, dict)
                and registry_result.get("updated") is True
            ):
                enabled = False
        if enabled:
            # This branch is only reached when the mod was absent from the
            # load order, so any successful enable changes mod state — a
            # running game must be restarted to pick it up.
            mod_state_changed = True
        log_step(f"mod_check_{mod_key}", {
            "success": enabled,
            "mod_name": mod_name,
            "uuid": installed_uuid,
            "state": "enabled" if enabled else "enable_failed",
            "enable_result": enable_result,
            "registry_result": registry_result,
        })

    results["mod_state_changed"] = mod_state_changed

    # Restore the named fixture only when it actually exists.
    save_fixture = scenario.get("save_fixture")
    if save_fixture:
        from .savegames import list_fixtures, restore
        try:
            fixtures_result = list_fixtures()
        except Exception as exc:
            fixtures_result = {"error": str(exc), "fixtures": []}
        if not isinstance(fixtures_result, dict):
            fixtures_result = {
                "error": "fixture catalog returned an invalid response",
                "fixtures": [],
            }
        fixture_names = {
            fixture.get("name")
            for fixture in fixtures_result.get("fixtures", [])
            if isinstance(fixture, dict)
        }
        if "error" in fixtures_result:
            log_step("restore_save", {
                "success": False,
                "error_type": "fixture_catalog_unavailable",
                "error": fixtures_result["error"],
            })
        elif save_fixture not in fixture_names:
            log_step("restore_save", {
                "success": False,
                "fixture": save_fixture,
                "note": f"fixture missing: {save_fixture}",
            })
        else:
            try:
                restore_result = restore(save_fixture)
            except Exception as exc:
                restore_result = {
                    "success": False,
                    "error_type": "restore_unavailable",
                    "error": str(exc),
                }
            log_step("restore_save", restore_result)

    # Launch lifecycle and live checks are explicitly opt-in.
    assertions = scenario.get("assertions", [])
    live_ready = False
    launched_game = False
    expected_pid = None
    log_since = None
    launch_mod = None

    if launch:
        from . import launch as launch_mod
        log_since = time.strftime("%Y-%m-%d %H:%M:%S")
        cleanup_ok = True

        if mod_state_changed:
            was_running = launch_mod.is_running()
            if was_running:
                launch_mod.kill_existing(force_all=True)
            still_running = launch_mod.is_running()
            cleanup_ok = not still_running
            log_step("stale_mod_state_cleanup", {
                "success": cleanup_ok,
                "was_running": was_running,
                "still_running": still_running,
                "note": "running game stopped because mod state changed",
            })

        if cleanup_ok and not mod_state_changed and launch_mod.is_running() and launch_mod.socket_alive():
            live_ready = True
            log_step("launch_health", {
                "success": True,
                "socket_connected": True,
                "reused_running_game": True,
            })
        elif cleanup_ok:
            launched_game = True
            try:
                from .cli import _launch_until_socket
                _, health, attempts = _launch_until_socket(
                    continue_game=True,
                    load_save=None,
                    extra_flags=None,
                    skip_videos=True,
                    headless=False,
                    timeout=launch_mod.default_timeout(continue_game=True),
                    retries=1,
                )
                live_ready = bool(
                    health.get("socket_connected")
                    and health.get("session_loaded")
                )
                expected_pid = health.get("pid")
                log_step("launch_health", {
                    **health,
                    "success": live_ready,
                    "attempts": attempts,
                    "reused_running_game": False,
                })
            except Exception as exc:
                log_step("launch_health", {
                    "success": False,
                    "error_type": "launch_unavailable",
                    "error": str(exc),
                    "reused_running_game": False,
                })
        else:
            log_step("launch_health", {
                "success": False,
                "error_type": "stale_game_still_running",
                "note": "launch blocked because the stale running game could not be stopped",
                "reused_running_game": False,
            })
    else:
        log_step("launch_health", {
            "success": False,
            "status": "not_run",
            "note": "launch disabled; rerun with --launch",
            "reused_running_game": False,
        })

    # Every post-launch check runs inside this try; the finally block quits
    # any game this run launched, so a pipeline bug (or Ctrl-C) can never
    # orphan a live BG3 process.
    try:
        # Verify socket identity before trusting any Lua assertion.
        if assertions and live_ready:
            from .console import Console
            try:
                with Console() as c:
                    identity_output = c.send("!identity")
                    identity = launch_mod.parse_identity_json(
                        (identity_output or "").encode("utf-8", errors="replace")
                    )
                    session_init = identity.get("session_init") if identity else None
                    pid = identity.get("pid") if identity else None
                    identity_ok = bool(
                        identity
                        and pid is not None
                        and session_init == "complete"
                        and (expected_pid is None or pid == expected_pid)
                    )
                    log_step("identity", {
                        "success": identity_ok,
                        "pid": pid,
                        "expected_pid": expected_pid,
                        "session_init": session_init,
                        "raw_output": (identity_output or "")[-500:],
                        "error": (
                            None if identity_ok
                            else "identity JSON unavailable or did not match the live session"
                        ),
                    })

                    if not identity_ok:
                        for i, assertion_lua in enumerate(assertions):
                            log_step(f"assertion_{i}", {
                                "success": False,
                                "status": "not_run",
                                "lua": assertion_lua,
                                "note": (
                                    f"identity not ready: pid={pid!r}, "
                                    f"session_init={session_init or 'unavailable'}"
                                ),
                            })
                    else:
                        for i, assertion_lua in enumerate(assertions):
                            command = (
                                "local __ok, __err = pcall(function() "
                                f"{assertion_lua} end); "
                                "Ext.Print(__ok and 'BG3SE_COMPAT_PASS' or "
                                "('BG3SE_COMPAT_FAIL: ' .. tostring(__err)))"
                            )
                            try:
                                output = c.send(command)
                                parsed = _parse_assertion_output(output)
                                log_step(f"assertion_{i}", {
                                    **parsed,
                                    "lua": assertion_lua,
                                })
                            except Exception as exc:
                                log_step(f"assertion_{i}", {
                                    "success": False,
                                    "lua": assertion_lua,
                                    "error": str(exc),
                                })
            except Exception as exc:
                log_step("identity", {
                    "success": False,
                    "error_type": "socket_unavailable",
                    "error": f"Socket connection failed: {exc}",
                    "pid": None,
                    "session_init": None,
                })
                for i, assertion_lua in enumerate(assertions):
                    log_step(f"assertion_{i}", {
                        "success": False,
                        "status": "not_run",
                        "lua": assertion_lua,
                        "note": "identity handshake unavailable",
                    })
        elif assertions:
            reason = "live session unavailable" if launch else "launch disabled; rerun with --launch"
            for i, assertion_lua in enumerate(assertions):
                log_step(f"assertion_{i}", {
                    "success": False,
                    "status": "not_run",
                    "lua": assertion_lua,
                    "note": reason,
                })

        # Scan only the log window opened immediately before launch.
        if launch and log_since:
            log_path = Path.home() / "Library/Application Support/BG3SE/logs/latest.log"
            for mod_key in effective_mod_keys:
                mod_name = catalog.get(mod_key, {}).get("name", mod_key)
                if not log_path.exists():
                    log_step(f"log_scan_{mod_key}", {
                        "success": False,
                        "error_type": "log_unavailable",
                        "mod": mod_name,
                        "since": log_since,
                        "note": f"log file unavailable: {log_path}",
                    })
                    continue
                log_errors, log_warnings, scan_error = _scan_log_for_mod(
                    mod_name,
                    since_timestamp=log_since,
                )
                # A failed read must not read as "scanned clean".
                log_step(f"log_scan_{mod_key}", {
                    "success": not log_errors and scan_error is None,
                    "mod": mod_name,
                    "since": log_since,
                    "errors": log_errors,
                    "warnings": log_warnings,
                    "error_type": "log_scan_failed" if scan_error else None,
                    "error": scan_error,
                })

        # Capture and crash checks are meaningful only for a live run.
        if live_ready:
            try:
                from .screenshot import capture
                ss_result = capture(output=str(report_dir / "screenshot.jpg"))
                log_step("screenshot", {"success": "error" not in ss_result, **ss_result})
            except Exception as exc:
                log_step("screenshot", {
                    "success": False,
                    "error_type": "screenshot_unavailable",
                    "error": str(exc),
                })

            try:
                from .crashlog import get_crash_report
                crash_data = get_crash_report(ring=False, tail=20)
                has_crash = bool(crash_data.get("signal"))
                log_step("crashlog", {"success": not has_crash, **crash_data})
            except Exception as exc:
                log_step("crashlog", {
                    "success": False,
                    "error_type": "crashlog_unavailable",
                    "error": str(exc),
                })
    finally:
        if launched_game:
            try:
                quit_result = launch_mod.quit_game(force=False)
            except Exception as exc:
                quit_result = {"success": False, "error": str(exc)}
            log_step("quit_game", quit_result)

    # Finalize
    passed_steps = sum(1 for s in results["steps"] if s.get("success"))
    total_steps = len(results["steps"])
    results["success"] = all(s.get("success") for s in results["steps"])
    results["summary"] = f"{passed_steps}/{total_steps} steps passed"
    results["finished_at"] = time.strftime("%Y-%m-%d %H:%M:%S")

    _save_report(report_dir, results)
    if save_baseline and results["success"]:
        baseline_path = _baseline_path(scenario_name)
        try:
            baseline_path.parent.mkdir(parents=True, exist_ok=True)
            shutil.copy2(str(report_dir / "report.json"), str(baseline_path))
            results["baseline"] = str(baseline_path)
        except OSError as exc:
            results["success"] = False
            results["baseline_error"] = str(exc)
            _save_report(report_dir, results)
    return results


def run_matrix(launch=False, auto_install=False):
    """Run all scenarios and produce a summary."""
    catalog = _load_popular_mods()
    scenarios = _load_scenarios()
    all_names = sorted(set(list(scenarios.keys()) + list(catalog.keys())))

    matrix_results = []
    for name in all_names:
        result = run_scenario(
            name,
            launch=launch,
            auto_install=auto_install,
        )
        matrix_results.append({
            "scenario": name,
            "success": result.get("success", False),
            "summary": result.get("summary", ""),
            "run_id": result.get("run_id", ""),
        })

    passed = sum(1 for r in matrix_results if r["success"])
    return {
        "matrix": matrix_results,
        "passed": passed,
        "total": len(matrix_results),
        "all_passed": passed == len(matrix_results),
    }


def diff_scenario(scenario_name):
    """Compare the latest scenario report with its saved baseline."""
    baseline_path = _baseline_path(scenario_name)
    run_candidates = []
    if REPORTS_DIR.exists():
        prefix = f"{scenario_name}_"
        for directory in REPORTS_DIR.iterdir():
            if not directory.is_dir() or not directory.name.startswith(prefix):
                continue
            suffix = directory.name[len(prefix):]
            if suffix.isdigit() and (directory / "report.json").exists():
                run_candidates.append((int(suffix), directory / "report.json"))

    available_baselines = []
    if baseline_path.parent.exists():
        available_baselines = sorted(path.stem for path in baseline_path.parent.glob("*.json"))
    available_runs = []
    if REPORTS_DIR.exists():
        available_runs = sorted(
            (
                directory.name
                for directory in REPORTS_DIR.iterdir()
                if directory.is_dir() and (directory / "report.json").exists()
            ),
            reverse=True,
        )

    if not baseline_path.exists():
        return {
            "success": False,
            "error_type": "missing_baseline",
            "error": f"No baseline report exists for scenario '{scenario_name}'.",
            "baseline_exists": False,
            "available_baselines": available_baselines,
            "available_runs": available_runs,
        }
    if not run_candidates:
        return {
            "success": False,
            "error_type": "missing_run",
            "error": f"No run report exists for scenario '{scenario_name}'.",
            "baseline_exists": True,
            "available_baselines": available_baselines,
            "available_runs": available_runs,
        }

    current_path = max(run_candidates, key=lambda item: item[0])[1]
    try:
        baseline = json.loads(baseline_path.read_text())
        current = json.loads(current_path.read_text())
    except (json.JSONDecodeError, OSError) as exc:
        return {
            "success": False,
            "error_type": "report_unavailable",
            "error": str(exc),
            "baseline": str(baseline_path),
            "current": str(current_path),
            "available_baselines": available_baselines,
            "available_runs": available_runs,
        }

    if (
        not isinstance(baseline, dict)
        or not isinstance(current, dict)
        or not isinstance(baseline.get("steps"), list)
        or not isinstance(current.get("steps"), list)
    ):
        return {
            "success": False,
            "error_type": "report_unavailable",
            "error": "baseline or current report has an invalid steps list",
            "baseline": str(baseline_path),
            "current": str(current_path),
            "available_baselines": available_baselines,
            "available_runs": available_runs,
        }

    baseline_steps = {
        step.get("name"): step
        for step in baseline.get("steps", [])
        if isinstance(step, dict) and step.get("name")
    }
    current_steps = {
        step.get("name"): step
        for step in current.get("steps", [])
        if isinstance(step, dict) and step.get("name")
    }

    regressions = []
    fixes = []
    for name in sorted(baseline_steps.keys() & current_steps.keys()):
        baseline_success = baseline_steps[name].get("success") is True
        current_success = current_steps[name].get("success") is True
        change = {
            "name": name,
            "baseline_success": baseline_success,
            "current_success": current_success,
        }
        if baseline_success and not current_success:
            regressions.append(change)
        elif not baseline_success and current_success:
            fixes.append(change)

    added_steps = sorted(current_steps.keys() - baseline_steps.keys())
    removed_steps = sorted(baseline_steps.keys() - current_steps.keys())
    return {
        "success": True,
        "scenario": scenario_name,
        "baseline": str(baseline_path),
        "current": str(current_path),
        "regressions": regressions,
        "fixes": fixes,
        "added_steps": added_steps,
        "removed_steps": removed_steps,
        "regressed": bool(regressions or removed_steps),
    }


def vet_mod(source, no_launch=False, output_path=None):
    """Vet a single mod: install, detect SE requirement, probe, generate report.

    source: Nexus mod ID (int), catalog key (str), or path to .pak file.

    Returns a compat report dict with status, errors, warnings, and API usage.
    """
    from . import launch as launch_mod

    catalog = _load_popular_mods()
    report = {
        "source": source,
        "bg3se_version": _read_se_version(),
        "timestamp": time.strftime("%Y-%m-%dT%H:%M:%S"),
        "status": "unknown",
        "se_required": False,
        "load_success": False,
        "bootstrap_executed": False,
        "apis_used": [],
        "errors": [],
        "warnings": [],
        "notes": "",
    }

    # Resolve source to mod info
    mod_info = None
    pak_path = None

    if source in catalog:
        mod_info = catalog[source]
        report["mod_name"] = mod_info["name"]
        report["nexus_id"] = mod_info.get("nexus_id")
        report["apis_used"] = mod_info.get("apis_used", [])
        print(f"Vetting catalog mod: {mod_info['name']}", file=sys.stderr)
    elif os.path.isfile(source) and Path(source).suffix.lower() == ".pak":
        pak_path = source
        report["mod_name"] = Path(source).stem
        print(f"Vetting local PAK: {source}", file=sys.stderr)
    else:
        try:
            nexus_id = int(source)
            report["nexus_id"] = nexus_id
            report["mod_name"] = f"nexus:{nexus_id}"
            print(f"Vetting Nexus mod #{nexus_id}", file=sys.stderr)
        except ValueError:
            report["status"] = "error"
            report["errors"].append(f"Cannot resolve source: {source!r}")
            return report

    # Step 1: Check if mod is installed (via PAK or modsettings)
    from .mod_manager.registry import load_registry
    registry = load_registry()
    installed_uuid = None

    if mod_info and mod_info.get("nexus_id"):
        for uuid, entry in registry.items():
            if mod_info["name"].lower() in (entry.get("name") or "").lower():
                installed_uuid = uuid
                break

    if pak_path:
        from .mod_manager.pak_inspector import PakReader, PakInspectorError
        try:
            with PakReader(pak_path) as pak:
                info = pak.get_mod_info()
                report["se_required"] = pak.contains_script_extender()
                if info.get("uuid"):
                    installed_uuid = info["uuid"]
                    report["mod_name"] = info.get("name") or report["mod_name"]
        except (PakInspectorError, OSError) as e:
            report["errors"].append(f"PAK read failed: {e}")

    # Step 2: Check SE socket and game status
    game_running = launch_mod.is_running()
    socket_ok = launch_mod.socket_alive()

    if not game_running and not no_launch:
        report["warnings"].append("Game not running. Launch with: bg3se-harness launch --continue")
        report["status"] = "needs_launch"
        _save_vet_report(report, output_path)
        return report

    if not socket_ok:
        report["warnings"].append("SE socket not connected — game may be starting or SE not loaded")
        report["status"] = "no_socket"
        _save_vet_report(report, output_path)
        return report

    # Step 3: Probe via socket
    from .console import Console
    try:
        with Console() as c:
            # Check SE loaded
            version = c.send("Ext.Print(Ext.Utils.Version())")
            report["bg3se_version_live"] = version.strip() if version else "unknown"

            # Check mod loaded (if we know the UUID)
            if installed_uuid and _valid_uuid(installed_uuid):
                is_loaded = c.send(
                    f'local ok, res = pcall(Ext.Mod.IsModLoaded, "{installed_uuid}"); '
                    f'Ext.Print(tostring(ok and res))'
                )
                report["load_success"] = "true" in (is_loaded or "").lower()

                bootstrap_check = c.send(
                    f'local ok = pcall(function() return Ext.Mod.GetMod("{installed_uuid}") end); '
                    f'Ext.Print(tostring(ok))'
                )
                report["bootstrap_executed"] = "true" in (bootstrap_check or "").lower()
            elif installed_uuid:
                report["warnings"].append(f"Invalid UUID format, skipping probe: {installed_uuid!r}")

    except (ConnectionRefusedError, FileNotFoundError, OSError) as e:
        report["errors"].append(f"Console connection failed: {e}")
        report["status"] = "socket_error"
        _save_vet_report(report, output_path)
        return report

    # Step 4: Parse latest log for errors related to this mod
    log_errors, log_warnings, scan_error = _scan_log_for_mod(
        report.get("mod_name", ""),
        since_timestamp=report["timestamp"].replace("T", " "),
    )
    report["errors"].extend(log_errors)
    report["warnings"].extend(log_warnings)
    if scan_error:
        # A failed scan means the log was never verified — surface it as a
        # warning so the verdict can be at most "partial", never "working".
        report["warnings"].append(f"log scan incomplete: {scan_error}")

    # Step 5: Determine status
    if report["errors"]:
        report["status"] = "broken"
    elif report["load_success"] and not report["warnings"]:
        report["status"] = "working"
    elif report["load_success"]:
        report["status"] = "partial"
    else:
        report["status"] = "not_loaded"

    _save_vet_report(report, output_path)
    return report


_UUID_RE = re.compile(
    r'^[0-9a-fA-F]{8}-[0-9a-fA-F]{4}-[0-9a-fA-F]{4}-'
    r'[0-9a-fA-F]{4}-[0-9a-fA-F]{12}$'
)


def _valid_uuid(s):
    """Return True if s is a well-formed UUID (hex-dash format)."""
    return bool(s and _UUID_RE.match(s))


def _read_se_version():
    """Read SE version from version.h."""
    try:
        from .config import PROJECT_ROOT
        version_h = PROJECT_ROOT / "src/core/version.h"
        if version_h.exists():
            for line in version_h.read_text().splitlines():
                if "BG3SE_VERSION" in line and '"' in line:
                    return line.split('"')[1]
    except Exception:
        pass
    return "unknown"


def _scan_log_for_mod(mod_name, since_timestamp=None):
    """Scan latest.log for errors/warnings mentioning a mod.

    If *since_timestamp* is given (ISO format "YYYY-MM-DD HH:MM:SS"), only
    lines with timestamps at or after that value are considered.

    Returns ``(errors, warnings, scan_error)``. *scan_error* is None when the
    log was actually read; otherwise it describes the read failure so callers
    can distinguish "scanned clean" from "could not scan".
    """
    errors = []
    warnings = []
    scan_error = None
    if not mod_name:
        return errors, warnings, scan_error
    log_path = Path.home() / "Library/Application Support/BG3SE/logs/latest.log"
    if not log_path.exists():
        return errors, warnings, scan_error

    try:
        lines = log_path.read_text(errors="replace").splitlines()
        mod_lower = mod_name.lower()
        for line in lines[-500:]:
            if since_timestamp and line.startswith("["):
                ts_end = line.find("]")
                if ts_end > 1:
                    line_ts = line[1:ts_end].split(".")[0]
                    if line_ts < since_timestamp:
                        continue
            lower = line.lower()
            if mod_lower and mod_lower not in lower:
                continue
            if "error" in lower or "failed" in lower:
                errors.append(line.strip()[:200])
            elif "warn" in lower:
                warnings.append(line.strip()[:200])
    except OSError as exc:
        scan_error = f"could not read {log_path.name}: {exc}"

    return errors[:20], warnings[:20], scan_error


def _save_vet_report(report, output_path=None):
    """Save a vet report to docs/compat-reports/."""
    from .config import PROJECT_ROOT
    if output_path:
        out = Path(output_path)
    else:
        reports_dir = PROJECT_ROOT / "docs" / "compat-reports"
        reports_dir.mkdir(parents=True, exist_ok=True)
        mod_slug = report.get("mod_name", "unknown").lower().replace(" ", "_")[:40]
        out = reports_dir / f"{mod_slug}_{int(time.time())}.json"

    out.write_text(json.dumps(report, indent=2))
    print(f"Report saved: {out}", file=sys.stderr)


def _save_report(report_dir, results):
    """Save report JSON to report directory."""
    report_path = report_dir / "report.json"
    report_path.write_text(json.dumps(results, indent=2))


# ============================================================================
# CLI handler
# ============================================================================

def cmd_compat(args):
    """CLI handler for compat subcommands."""
    subcmd = args.compat_command

    if subcmd == "list":
        result = list_scenarios()
        print(json.dumps(result, indent=2))
        return 0

    elif subcmd == "run":
        result = run_scenario(
            args.scenario,
            launch=getattr(args, "launch", False),
            auto_install=getattr(args, "auto_install", False),
            save_baseline=getattr(args, "save_baseline", False),
        )
        print(json.dumps(result, indent=2))
        return 0 if result.get("success") else 1

    elif subcmd == "matrix":
        result = run_matrix(
            launch=getattr(args, "launch", False),
            auto_install=getattr(args, "auto_install", False),
        )
        print(json.dumps(result, indent=2))
        return 0 if result.get("all_passed") else 1

    elif subcmd == "diff":
        result = diff_scenario(args.scenario)
        print(json.dumps(result, indent=2))
        return 0 if result.get("success") and not result.get("regressed") else 1

    elif subcmd == "vet":
        result = vet_mod(
            args.source,
            no_launch=getattr(args, "no_launch", False),
            output_path=getattr(args, "output", None),
        )
        print(json.dumps(result, indent=2))
        return 0 if result.get("status") == "working" else 1

    return 1
