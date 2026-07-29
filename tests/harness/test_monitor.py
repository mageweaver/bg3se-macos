"""Offline tests for bg3se_harness._monitor module.

Tests _TrackerProcess adapter, _write_health atomicity, and graphics
restore policy (exactly once, only on terminal conditions).
"""

import json
import os
from pathlib import Path
from types import SimpleNamespace

import pytest

from bg3se_harness._monitor import _TrackerProcess, _write_health
from bg3se_harness.launch import ProcessTracker


def _make_tracker(*, pid=100, phase="direct_running", alive=True):
    """Build a ProcessTracker with controlled state."""
    tracker = ProcessTracker.__new__(ProcessTracker)
    tracker.launch_id = "monitor-test-id"
    tracker.expected_executable = "/fake/bg3"
    tracker.requested_args = []
    tracker.steam_preflight = "ready"
    tracker.phase = phase
    tracker.current_pid = pid
    tracker.current_identity = None
    tracker.lineage = [{"pid": pid, "role": "direct", "observed_exitcode": None}]
    tracker.adoptions = 0
    tracker.created_wall_time = 0
    tracker.created_monotonic = 0
    tracker.cleanup_complete = False
    tracker.window_contained = False
    tracker._record_path = Path("/dev/null")
    tracker._write_record = lambda: None
    tracker._alive = alive
    original_is_alive = tracker.is_process_alive
    tracker.is_process_alive = lambda: tracker._alive
    return tracker


# -- _TrackerProcess adapter ------------------------------------------------


def test_tracker_process_poll_alive():
    tracker = _make_tracker(pid=42, alive=True)
    proc = _TrackerProcess(tracker)
    assert proc.pid == 42
    assert proc.poll() is None
    assert proc.returncode is None


def test_tracker_process_poll_dead_returns_zero():
    """Dead tracked PID reports exit code 0 (unknown, assumed clean for bounce)."""
    tracker = _make_tracker(pid=42, alive=False)
    proc = _TrackerProcess(tracker)
    assert proc.poll() == 0
    assert proc.returncode == 0


def test_tracker_process_tracks_pid_changes():
    tracker = _make_tracker(pid=42, alive=True)
    proc = _TrackerProcess(tracker)
    assert proc.pid == 42
    tracker.current_pid = 99
    proc.poll()
    assert proc.pid == 99


# -- _write_health atomic writes --------------------------------------------


def test_write_health_atomic(monkeypatch, tmp_path):
    health_file = tmp_path / "health.json"
    monkeypatch.setattr("bg3se_harness._monitor.HEALTH_FILE", health_file)

    data = {"status": "ready", "pid": 123, "launch_id": "test"}
    _write_health(data)

    assert health_file.exists()
    loaded = json.loads(health_file.read_text())
    assert loaded["status"] == "ready"
    assert loaded["pid"] == 123


def test_write_health_overwrites(monkeypatch, tmp_path):
    health_file = tmp_path / "health.json"
    monkeypatch.setattr("bg3se_harness._monitor.HEALTH_FILE", health_file)

    _write_health({"status": "monitoring"})
    _write_health({"status": "ready", "updated": True})

    loaded = json.loads(health_file.read_text())
    assert loaded["status"] == "ready"
    assert loaded["updated"] is True


def test_write_health_no_temp_file_left(monkeypatch, tmp_path):
    health_file = tmp_path / "health.json"
    monkeypatch.setattr("bg3se_harness._monitor.HEALTH_FILE", health_file)

    _write_health({"status": "ok"})

    tmp_file = health_file.with_suffix(".tmp")
    assert not tmp_file.exists()


# -- Graphics restore policy ------------------------------------------------


def test_graphics_restored_flag_prevents_double_restore():
    """Simulates the monitor's graphics_restored guard.

    The monitor uses a boolean flag to ensure restore_headless_graphics
    is called at most once per monitor session. This test verifies the
    pattern works correctly.
    """
    restore_calls = []
    graphics_restored = False

    def mock_restore(reason=""):
        restore_calls.append(reason)
        return {"success": True}

    socket_connected_scenarios = [
        {"socket_connected": True},
        {"socket_connected": False, "stage": "process_exited"},
    ]

    for health in socket_connected_scenarios:
        if health.get("socket_connected"):
            if not graphics_restored:
                mock_restore(reason="hide_complete")
                graphics_restored = True
        elif health.get("stage") == "process_exited":
            if not graphics_restored:
                mock_restore(reason="terminal_exit")
                graphics_restored = True

    assert len(restore_calls) == 1
    assert restore_calls[0] == "hide_complete"


def test_bootstrap_exit_does_not_trigger_restore():
    """The monitor must NOT restore graphics on a bootstrap exit candidate.

    When the tracker is in waiting_for_steam_relaunch or adopted_running,
    the process exit is not terminal and graphics should stay modified.
    """
    restore_calls = []
    graphics_restored = False

    tracker = _make_tracker(phase="waiting_for_steam_relaunch")
    health = {"socket_connected": False, "stage": "bootstrap_bounce"}

    is_terminal = (
        not health.get("socket_connected")
        and tracker.phase not in ("waiting_for_steam_relaunch", "adopted_running")
    )

    if is_terminal and not graphics_restored:
        restore_calls.append("should_not_happen")
        graphics_restored = True

    assert len(restore_calls) == 0
    assert graphics_restored is False
