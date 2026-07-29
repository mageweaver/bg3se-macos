"""Offline tests for launch-lifecycle hardening (spec cases 1-14).

All process, clock, Steam, socket-peer, filesystem, and memory-pressure
inputs are injected/faked. These tests never enumerate or signal live
BG3/Steam processes.
"""

import json
import os
import struct
import time
import xml.etree.ElementTree as ET
from pathlib import Path
from types import SimpleNamespace

import pytest

from bg3se_harness import launch


# -- Helpers -----------------------------------------------------------------


def _make_tracker(*, phase="direct_running", pid=100, steam="ready",
                  adoptions=0, lineage=None, lifetime_offset=0,
                  expected_executable="/fake/Baldur's Gate 3"):
    """Build a ProcessTracker with controlled state (no I/O)."""
    tracker = launch.ProcessTracker.__new__(launch.ProcessTracker)
    tracker.launch_id = "test-launch-id"
    tracker.expected_executable = expected_executable
    tracker.requested_args = ["-continueGame"]
    tracker.steam_preflight = steam
    tracker.phase = phase
    tracker.current_pid = pid
    tracker.current_identity = {"start_time": "Mon Jul 28 12:00:00 2026",
                                "executable": expected_executable}
    tracker.lineage = lineage or [{"pid": pid, "role": "direct",
                                   "observed_exitcode": None}]
    tracker.adoptions = adoptions
    tracker.created_wall_time = time.time()
    tracker.created_monotonic = time.monotonic() - lifetime_offset
    tracker.cleanup_complete = False
    tracker.window_contained = False
    tracker._record_path = Path("/dev/null")
    tracker._write_record = lambda: None
    return tracker


# -- 1. Direct PID remains alive --------------------------------------------


def test_direct_pid_alive_stays_in_direct_running():
    """A tracker in direct_running with no exit event stays in that phase."""
    tracker = _make_tracker()
    tracker.phase = "direct_running"
    assert tracker.phase == "direct_running"
    assert tracker.current_pid == 100


# -- 2. Steam bounce adoption -----------------------------------------------


def test_steam_bounce_exit0_qualifies_for_adoption(monkeypatch):
    tracker = _make_tracker(lifetime_offset=1.0)
    is_bounce = tracker.record_direct_exit(0)
    assert is_bounce is True
    assert tracker.phase == "waiting_for_steam_relaunch"


def test_steam_bounce_adopt_successor(monkeypatch):
    tracker = _make_tracker(phase="waiting_for_steam_relaunch")
    monkeypatch.setattr(
        launch, "get_process_identity",
        lambda pid: ("/fake/Baldur's Gate 3", "Mon Jul 28 12:00:10 2026"),
    )
    adopted = tracker.try_adopt(200)
    assert adopted is True
    assert tracker.current_pid == 200
    assert tracker.phase == "adopted_running"
    assert tracker.adoptions == 1
    assert len(tracker.lineage) == 2
    assert tracker.lineage[-1]["role"] == "steam_relaunch"


# -- 3. Bounce independent of steam_appid.txt -------------------------------


def test_bounce_qualifies_with_valid_exit_and_lifetime(monkeypatch):
    """Bounce detection depends on exit code + lifetime, not appid file."""
    tracker = _make_tracker(lifetime_offset=1.0)
    is_bounce = tracker.record_direct_exit(0)
    assert is_bounce is True


# -- 4. Nonzero exit: no adoption -------------------------------------------


def test_nonzero_exit_no_adoption():
    tracker = _make_tracker(lifetime_offset=1.0)
    is_bounce = tracker.record_direct_exit(1)
    assert is_bounce is False
    assert tracker.phase == "process_exited"


def test_exit0_too_slow_no_adoption():
    tracker = _make_tracker(lifetime_offset=10.0)
    is_bounce = tracker.record_direct_exit(0)
    assert is_bounce is False
    assert tracker.phase == "process_exited"


# -- 5. Mismatched executable rejected --------------------------------------


def test_mismatched_executable_rejected(monkeypatch):
    tracker = _make_tracker(phase="waiting_for_steam_relaunch")
    monkeypatch.setattr(
        launch, "get_process_identity",
        lambda pid: ("/usr/bin/some_other_app", "Mon Jul 28 12:00:10 2026"),
    )
    adopted = tracker.try_adopt(200)
    assert adopted is False
    assert tracker.phase == "waiting_for_steam_relaunch"


def test_unavailable_executable_rejected(monkeypatch):
    """Adoption fails closed when executable is unavailable (None)."""
    tracker = _make_tracker(phase="waiting_for_steam_relaunch")
    monkeypatch.setattr(
        launch, "get_process_identity",
        lambda pid: (None, "Mon Jul 28 12:00:10 2026"),
    )
    adopted = tracker.try_adopt(200)
    assert adopted is False


def test_adoption_requires_exact_executable_match(monkeypatch):
    """Adoption compares the full executable path, not just the basename."""
    tracker = _make_tracker(
        phase="waiting_for_steam_relaunch",
        expected_executable="/steam/apps/Baldur's Gate 3",
    )
    monkeypatch.setattr(
        launch, "get_process_identity",
        lambda pid: ("/other/path/Baldur's Gate 3", "Mon Jul 28 12:00:10 2026"),
    )
    adopted = tracker.try_adopt(200)
    assert adopted is False


def test_adoption_rejects_outside_relaunch_window(monkeypatch):
    """Adoption fails if the tracker has been alive longer than STEAM_RELAUNCH_GRACE_S."""
    tracker = _make_tracker(
        phase="waiting_for_steam_relaunch",
        lifetime_offset=launch.STEAM_RELAUNCH_GRACE_S + 5,
    )
    monkeypatch.setattr(
        launch, "get_process_identity",
        lambda pid: ("/fake/Baldur's Gate 3", "Mon Jul 28 12:00:10 2026"),
    )
    adopted = tracker.try_adopt(200)
    assert adopted is False


# -- 6. Max adoptions -------------------------------------------------------


def test_max_adoptions_rejected():
    tracker = _make_tracker(
        phase="waiting_for_steam_relaunch",
        adoptions=launch.STEAM_MAX_ADOPTIONS_PER_LAUNCH,
    )
    adopted = tracker.try_adopt(300)
    assert adopted is False


# -- 7. Steam relaunch timeout ----------------------------------------------


def test_no_candidate_stays_waiting():
    tracker = _make_tracker(phase="waiting_for_steam_relaunch")
    assert tracker.phase == "waiting_for_steam_relaunch"
    tracker.set_failed("steam_relaunch_timeout")
    assert tracker.phase == "steam_relaunch_timeout"


# -- 8. Socket peer verification --------------------------------------------


def test_verify_socket_peer_match():
    sock = SimpleNamespace()
    pid_bytes = struct.pack("i", 100)
    sock.getsockopt = lambda level, opt, size: pid_bytes
    ok, peer = launch.verify_socket_peer(sock, 100)
    assert ok is True
    assert peer == 100


def test_verify_socket_peer_mismatch():
    sock = SimpleNamespace()
    pid_bytes = struct.pack("i", 999)
    sock.getsockopt = lambda level, opt, size: pid_bytes
    ok, peer = launch.verify_socket_peer(sock, 100)
    assert ok is False
    assert peer == 999


def test_verify_socket_peer_oserror_is_unverified():
    """getsockopt failures yield (False, None) -- unverified, not pass."""
    sock = SimpleNamespace()
    sock.getsockopt = lambda *a: (_ for _ in ()).throw(OSError("no peer"))
    ok, peer = launch.verify_socket_peer(sock, 100)
    assert ok is False
    assert peer is None


# -- 9. Steam absent/starting blocks launch ---------------------------------


def test_steam_absent_blocks_launch(monkeypatch, tmp_path):
    monkeypatch.setattr(launch, "find_bg3_processes", lambda: [])
    monkeypatch.setattr(launch, "_read_pid_file", lambda: None)
    monkeypatch.setattr(
        launch, "steam_readiness",
        lambda: {"status": "absent", "steam_process": False},
    )
    session = launch.launch()
    assert session.direct_process is None
    assert session.tracker.phase == "steam_not_ready"


def test_steam_starting_blocks_launch(monkeypatch, tmp_path):
    monkeypatch.setattr(launch, "find_bg3_processes", lambda: [])
    monkeypatch.setattr(launch, "_read_pid_file", lambda: None)
    monkeypatch.setattr(
        launch, "steam_readiness",
        lambda: {"status": "starting", "steam_process": True, "ipc_registered": False},
    )
    session = launch.launch()
    assert session.direct_process is None
    assert session.tracker.phase == "steam_not_ready"


# -- 10. Windowed-mode check (graphicSettings.lsx) --------------------------


def _write_settings(path, entries):
    """Write a minimal graphicSettings.lsx with given entries."""
    children = ET.Element("children")
    for key, value in entries.items():
        node = ET.SubElement(children, "node", id="ConfigEntry")
        ET.SubElement(node, "attribute", id="MapKey", type="FixedString", value=key)
        ET.SubElement(node, "attribute", id="Type", type="int32", value="0")
        ET.SubElement(node, "attribute", id="Value", type="int32", value=str(value))

    root = ET.Element("save")
    region = ET.SubElement(root, "region", id="Config")
    config = ET.SubElement(region, "node", id="Config")
    config.append(children)
    ET.ElementTree(root).write(path, xml_declaration=True, encoding="unicode")


def _patch_graphics_path(monkeypatch, tmp_path):
    settings_path = tmp_path / "graphicSettings.lsx"
    monkeypatch.setattr(launch, "GRAPHIC_SETTINGS_PATH", settings_path)
    return settings_path


def test_check_windowed_mode_key_absent(monkeypatch, tmp_path):
    settings_path = _patch_graphics_path(monkeypatch, tmp_path)
    _write_settings(settings_path, {"ScreenWidth": 1920, "ScreenHeight": 1080})
    result = launch.check_windowed_mode()
    assert result["windowed"] is False
    assert result["key_present"] is False
    assert "not found" in result.get("error", "")


def test_check_windowed_mode_key_value_1(monkeypatch, tmp_path):
    settings_path = _patch_graphics_path(monkeypatch, tmp_path)
    _write_settings(settings_path, {
        "ScreenWidth": 1920, "FakeFullscreenEnabled": 1,
    })
    result = launch.check_windowed_mode()
    assert result["windowed"] is False
    assert result["key_present"] is True
    assert result["value"] == "1"


def test_check_windowed_mode_key_value_0(monkeypatch, tmp_path):
    settings_path = _patch_graphics_path(monkeypatch, tmp_path)
    _write_settings(settings_path, {
        "ScreenWidth": 1920, "FakeFullscreenEnabled": 0,
    })
    result = launch.check_windowed_mode()
    assert result["windowed"] is True
    assert result["key_present"] is True
    assert result["value"] == "0"


def test_check_windowed_mode_file_missing(monkeypatch, tmp_path):
    monkeypatch.setattr(launch, "GRAPHIC_SETTINGS_PATH", tmp_path / "missing.lsx")
    result = launch.check_windowed_mode()
    assert result["windowed"] is False
    assert "not found" in result.get("error", "")


# -- 11. graphicSettings keeps only width/height ----------------------------


def test_headless_transient_entries_only_width_height():
    entries = launch.HEADLESS_TRANSIENT_GRAPHICS_ENTRIES
    assert set(entries.keys()) == {"ScreenWidth", "ScreenHeight"}
    assert "Fullscreen" not in entries
    assert "FakeFullscreenEnabled" not in entries
    assert "FakeFullscreen" not in entries


def test_headless_graphics_entries_alias():
    assert launch.HEADLESS_GRAPHICS_ENTRIES is launch.HEADLESS_TRANSIENT_GRAPHICS_ENTRIES


# -- 12. Memory pressure thresholds -----------------------------------------


def _stub_memory_pressure(monkeypatch, output, returncode=0, *, timeout=False,
                          not_found=False):
    import subprocess
    original_run = subprocess.run

    def fake_run(cmd, **kw):
        if cmd and cmd[0] == "/usr/bin/memory_pressure":
            if not_found:
                raise FileNotFoundError("/usr/bin/memory_pressure")
            if timeout:
                raise subprocess.TimeoutExpired(cmd, 3)
            return SimpleNamespace(
                stdout=output, stderr="", returncode=returncode,
            )
        return original_run(cmd, **kw)

    monkeypatch.setattr(subprocess, "run", fake_run)


def test_memory_pressure_pass_20pct(monkeypatch):
    _stub_memory_pressure(
        monkeypatch,
        "System-wide memory free percentage: 20%\n",
    )
    result = launch.memory_pressure_check()
    assert result["classification"] == "pass"
    assert result["free_percent"] == 20


def test_memory_pressure_warning_10pct(monkeypatch):
    _stub_memory_pressure(
        monkeypatch,
        "System-wide memory free percentage: 10%\n",
    )
    result = launch.memory_pressure_check()
    assert result["classification"] == "warning"
    assert result["free_percent"] == 10


def test_memory_pressure_warning_19pct(monkeypatch):
    _stub_memory_pressure(
        monkeypatch,
        "System-wide memory free percentage: 19%\n",
    )
    result = launch.memory_pressure_check()
    assert result["classification"] == "warning"
    assert result["free_percent"] == 19


def test_memory_pressure_critical_9pct(monkeypatch):
    _stub_memory_pressure(
        monkeypatch,
        "System-wide memory free percentage: 9%\n",
    )
    result = launch.memory_pressure_check()
    assert result["classification"] == "critical"
    assert result["free_percent"] == 9


def test_memory_pressure_unavailable_not_found(monkeypatch):
    _stub_memory_pressure(monkeypatch, "", not_found=True)
    result = launch.memory_pressure_check()
    assert result["classification"] == "unavailable"


def test_memory_pressure_unavailable_timeout(monkeypatch):
    _stub_memory_pressure(monkeypatch, "", timeout=True)
    result = launch.memory_pressure_check()
    assert result["classification"] == "unavailable"


def test_memory_critical_blocks_launch(monkeypatch):
    monkeypatch.setattr(launch, "find_bg3_processes", lambda: [])
    monkeypatch.setattr(launch, "_read_pid_file", lambda: None)
    monkeypatch.setattr(
        launch, "steam_readiness", lambda: {"status": "ready"},
    )
    monkeypatch.setattr(
        launch, "memory_pressure_check",
        lambda: {"classification": "critical", "free_percent": 5},
    )
    session = launch.launch()
    assert session.direct_process is None
    assert session.tracker.phase == "memory_pressure_critical"


def test_memory_critical_overridable(monkeypatch, tmp_path):
    monkeypatch.setattr(launch, "find_bg3_processes", lambda: [])
    monkeypatch.setattr(launch, "_read_pid_file", lambda: None)
    monkeypatch.setattr(
        launch, "steam_readiness", lambda: {"status": "ready"},
    )
    monkeypatch.setattr(
        launch, "memory_pressure_check",
        lambda: {"classification": "critical", "free_percent": 5},
    )
    monkeypatch.setattr(launch, "clean_socket", lambda: None)
    monkeypatch.setattr(launch, "ensure_no_launcher", lambda: None)
    monkeypatch.setattr(launch, "ensure_skip_videos", lambda: None)
    monkeypatch.setattr(launch, "get_process_identity", lambda pid: ("/fake/bg3", "now"))

    import subprocess as _sp

    class FakePopen:
        def __init__(self, *a, **k):
            self.pid = 999
            self.returncode = None
            self.bg3se_headless_graphics = None
        def poll(self):
            return None

    monkeypatch.setattr(_sp, "Popen", lambda *a, **k: FakePopen())
    session = launch.launch(allow_memory_pressure=True)
    assert session.direct_process is not None
    assert session.pid == 999


# -- 13. Stale monitor / launch_id ------------------------------------------


def test_tracker_serialization_roundtrip(tmp_path):
    record_path = tmp_path / "bg3se_pid.json"

    tracker = launch.ProcessTracker(
        launch_id="test-abc",
        steam_preflight="ready",
    )
    tracker.current_pid = 123
    tracker.phase = "socket_ready"
    tracker.lineage = [{"pid": 123, "role": "direct", "observed_exitcode": None}]
    tracker._record_path = record_path
    tracker._write_record()

    loaded = launch.ProcessTracker.from_record(record_path)
    assert loaded is not None
    assert loaded.launch_id == "test-abc"
    assert loaded.phase == "socket_ready"
    assert loaded.current_pid == 123


def test_tracker_from_record_rejects_legacy(tmp_path):
    record_path = tmp_path / "bg3se_pid.json"
    record_path.write_text(json.dumps({"pid": 123, "launched_at": 1000}))
    loaded = launch.ProcessTracker.from_record(record_path)
    assert loaded is None


def test_tracker_from_record_missing_file(tmp_path):
    loaded = launch.ProcessTracker.from_record(tmp_path / "nonexistent.json")
    assert loaded is None


# -- 14. ProcessTracker state machine edge cases ----------------------------


def test_steam_not_ready_prevents_adoption():
    tracker = _make_tracker(steam="absent", lifetime_offset=1.0)
    is_bounce = tracker.record_direct_exit(0)
    assert is_bounce is False


def test_double_adoption_rejected(monkeypatch):
    tracker = _make_tracker(phase="waiting_for_steam_relaunch", adoptions=1)
    monkeypatch.setattr(
        launch, "get_process_identity",
        lambda pid: ("/fake/Baldur's Gate 3", "now"),
    )
    adopted = tracker.try_adopt(300)
    assert adopted is False


def test_set_socket_ready():
    tracker = _make_tracker(phase="adopted_running")
    tracker.set_socket_ready()
    assert tracker.phase == "socket_ready"


def test_set_failed():
    tracker = _make_tracker()
    tracker.set_failed("ambiguous_relaunch")
    assert tracker.phase == "ambiguous_relaunch"


def test_launch_session_facade():
    tracker = _make_tracker(pid=42)
    proc = SimpleNamespace(pid=42, returncode=None)
    session = launch.LaunchSession(tracker, proc)
    assert session.pid == 42
    assert session.launch_id == "test-launch-id"
    assert session.returncode is None


def test_launch_session_no_process():
    tracker = _make_tracker(pid=None)
    session = launch.LaunchSession(tracker, None)
    assert session.pid is None
    assert session.direct_process is None


# -- Doctor severity checks (calls _check directly) -------------------------


def test_doctor_check_severity_levels():
    from bg3se_harness.doctor import _check

    critical = _check("test", False, severity="critical", code="test_fail")
    assert critical["severity"] == "critical"
    assert critical["code"] == "test_fail"

    warning = _check("test", True, severity="warning")
    assert warning["severity"] == "warning"
    assert "code" not in warning

    info = _check("test", True, severity="info")
    assert info["severity"] == "info"


def test_doctor_severity_filtering():
    """Verify that critical failures are correctly identified from a checks list."""
    from bg3se_harness.doctor import _check

    checks = [
        _check("passing_critical", True, severity="critical"),
        _check("failing_critical", False, severity="critical", code="b_fail"),
        _check("failing_warning", False, severity="warning"),
        _check("passing_info", True, severity="info"),
    ]
    failed_critical = [
        c for c in checks if not c["passed"] and c.get("severity") == "critical"
    ]
    assert len(failed_critical) == 1
    assert failed_critical[0]["name"] == "failing_critical"


def test_doctor_no_critical_failures_means_no_block():
    from bg3se_harness.doctor import _check

    checks = [
        _check("a", True, severity="critical"),
        _check("b", False, severity="warning"),
    ]
    failed_critical = [
        c for c in checks if not c["passed"] and c.get("severity") == "critical"
    ]
    assert len(failed_critical) == 0


# -- Stale snapshot safety ---------------------------------------------------


def _patch_restore_paths(monkeypatch, tmp_path):
    settings_path = tmp_path / "graphicSettings.lsx"
    restore_path = tmp_path / "graphic_settings_headless_restore.json"
    monkeypatch.setattr(launch, "GRAPHIC_SETTINGS_PATH", settings_path)
    monkeypatch.setattr(launch, "HEADLESS_GRAPHICS_RESTORE_PATH", restore_path)
    monkeypatch.setattr(launch, "HARNESS_CONFIG_DIR", tmp_path)
    return settings_path, restore_path


def test_stale_snapshot_with_extra_keys_is_deleted(monkeypatch, tmp_path):
    settings_path, restore_path = _patch_restore_paths(monkeypatch, tmp_path)
    _write_settings(settings_path, {
        "ScreenWidth": 1280, "ScreenHeight": 720, "FakeFullscreenEnabled": 0,
    })
    stale_snapshot = {
        "path": str(settings_path),
        "entries": {
            "ScreenWidth": {"existed": True, "index": 0, "node_attrib": {"id": "ConfigEntry"},
                            "attributes": [{"id": "MapKey", "type": "FixedString", "value": "ScreenWidth"},
                                           {"id": "Value", "type": "int32", "value": "1920"}]},
            "ScreenHeight": {"existed": True, "index": 1, "node_attrib": {"id": "ConfigEntry"},
                             "attributes": [{"id": "MapKey", "type": "FixedString", "value": "ScreenHeight"},
                                            {"id": "Value", "type": "int32", "value": "1080"}]},
            "Fullscreen": {"existed": False},
            "FakeFullscreenEnabled": {"existed": False},
        },
    }
    restore_path.write_text(json.dumps(stale_snapshot))

    result = launch.restore_headless_graphics(reason="test_stale")
    assert result["success"] is True
    assert result.get("stale_snapshot_deleted") is True
    assert "Fullscreen" in result.get("extra_keys", [])
    assert not restore_path.exists()


def test_valid_snapshot_restores_normally(monkeypatch, tmp_path):
    settings_path, restore_path = _patch_restore_paths(monkeypatch, tmp_path)
    _write_settings(settings_path, {
        "ScreenWidth": 1920, "ScreenHeight": 1080, "FakeFullscreenEnabled": 0,
    })

    prepared = launch.prepare_headless_graphics()
    assert prepared["success"] is True
    assert restore_path.exists()

    restored = launch.restore_headless_graphics(reason="test_valid")
    assert restored["success"] is True
    assert not restore_path.exists()


def test_restore_preserves_fakefullscreenenabled(monkeypatch, tmp_path):
    settings_path, restore_path = _patch_restore_paths(monkeypatch, tmp_path)
    _write_settings(settings_path, {
        "ScreenWidth": 1920, "ScreenHeight": 1080, "FakeFullscreenEnabled": 0,
    })

    launch.prepare_headless_graphics()
    launch.restore_headless_graphics(reason="test_preserve")

    tree = ET.parse(settings_path)
    values = {}
    for node in tree.getroot().iter("node"):
        if node.get("id") != "ConfigEntry":
            continue
        attrs = {attr.get("id"): attr.get("value") for attr in node.findall("attribute")}
        key = attrs.get("MapKey")
        if key:
            values[key] = attrs.get("Value")

    assert values["ScreenWidth"] == "1920"
    assert values["ScreenHeight"] == "1080"
    assert values["FakeFullscreenEnabled"] == "0"


# -- parse_identity_json -----------------------------------------------------


def test_parse_identity_json_clean():
    raw = b'{"pid": 123, "version": "v0.38.0", "game_state": "Running"}'
    result = launch.parse_identity_json(raw)
    assert result is not None
    assert result["pid"] == 123


def test_parse_identity_json_with_noise():
    raw = b'some log noise\n\x1b[32m{"pid": 456, "version": "v0.38.0"}\x1b[0m\nmore noise'
    result = launch.parse_identity_json(raw)
    assert result is not None
    assert result["pid"] == 456


def test_parse_identity_json_empty():
    assert launch.parse_identity_json(b"") is None
    assert launch.parse_identity_json(b"no json here") is None


# -- Headless launch windowed-mode gate --------------------------------------


def test_headless_launch_refused_without_windowed_mode(monkeypatch, tmp_path):
    """launch(headless=True) must check windowed mode before prepare_headless_graphics."""
    monkeypatch.setattr(launch, "find_bg3_processes", lambda: [])
    monkeypatch.setattr(launch, "_read_pid_file", lambda: None)
    monkeypatch.setattr(launch, "steam_readiness", lambda: {"status": "ready"})
    monkeypatch.setattr(
        launch, "memory_pressure_check",
        lambda: {"classification": "pass", "free_percent": 50},
    )
    monkeypatch.setattr(launch, "clean_socket", lambda: None)
    monkeypatch.setattr(launch, "ensure_no_launcher", lambda: None)
    monkeypatch.setattr(
        launch, "check_windowed_mode",
        lambda: {"windowed": False, "error": "FakeFullscreenEnabled not found"},
    )
    monkeypatch.setattr(
        launch, "prepare_headless_graphics",
        lambda: pytest.fail("prepare_headless_graphics should not be called"),
    )
    session = launch.launch(headless=True)
    assert session.direct_process is None
    assert session.tracker.phase == "window_mode_unverified"


def test_headless_launch_proceeds_when_windowed(monkeypatch, tmp_path):
    """launch(headless=True) proceeds when windowed mode is confirmed."""
    monkeypatch.setattr(launch, "find_bg3_processes", lambda: [])
    monkeypatch.setattr(launch, "_read_pid_file", lambda: None)
    monkeypatch.setattr(launch, "steam_readiness", lambda: {"status": "ready"})
    monkeypatch.setattr(
        launch, "memory_pressure_check",
        lambda: {"classification": "pass", "free_percent": 50},
    )
    monkeypatch.setattr(launch, "clean_socket", lambda: None)
    monkeypatch.setattr(launch, "ensure_no_launcher", lambda: None)
    monkeypatch.setattr(launch, "ensure_skip_videos", lambda: None)
    monkeypatch.setattr(
        launch, "check_windowed_mode",
        lambda: {"windowed": True, "key_present": True, "value": "0"},
    )
    monkeypatch.setattr(
        launch, "prepare_headless_graphics",
        lambda: {"success": True},
    )
    monkeypatch.setattr(launch, "get_process_identity", lambda pid: ("/fake/bg3", "now"))

    import subprocess as _sp

    class FakePopen:
        def __init__(self, *a, **k):
            self.pid = 888
            self.returncode = None
            self.bg3se_headless_graphics = None
        def poll(self):
            return None

    monkeypatch.setattr(_sp, "Popen", lambda *a, **k: FakePopen())
    session = launch.launch(headless=True)
    assert session.direct_process is not None
    assert session.pid == 888
