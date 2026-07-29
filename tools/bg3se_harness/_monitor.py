"""Background health monitor for headless launch.

Spawned as a detached subprocess by `launch --background`. Polls the
SE socket, writes health JSON to HEALTH_FILE, and optionally hides
the BG3 window.

Writes a human-readable stage log to MONITOR_LOG that can be tailed
to watch the boot sequence in real time:
    tail -f /tmp/bg3se_monitor.log

Usage (internal -- called by cli.py):
    python3 -m bg3se_harness._monitor <pid> <timeout> <headless:0|1>
"""

import json
import os
import subprocess
import sys
import time

sys.path.insert(0, os.path.join(os.path.dirname(__file__), ".."))

from bg3se_harness.config import HEALTH_FILE, MONITOR_LOG
from bg3se_harness.launch import (
    ProcessTracker,
    wait_for_socket,
    hide_window,
    move_window_offscreen,
    quit_game,
    restore_headless_graphics,
)


def _log(msg, stages):
    """Write a timestamped line to the monitor log and track in stages list."""
    ts = time.strftime("%H:%M:%S")
    elapsed = time.monotonic() - _start_time
    line = f"[{ts}] +{elapsed:6.1f}s  {msg}"
    stages.append({"t": round(elapsed, 1), "msg": msg})
    try:
        with open(MONITOR_LOG, "a") as f:
            f.write(line + "\n")
    except OSError:
        pass

_start_time = 0.0


def _move_window_offscreen_by_pid(pid, retries=3):
    """Move BG3 window off-screen, targeting by Unix PID."""
    script = f'''
tell application "System Events"
  repeat with p in (every process whose unix id is {pid})
    repeat with w in windows of p
      set position of w to {{-10000, -10000}}
    end repeat
  end repeat
end tell
'''
    for attempt in range(retries):
        try:
            r = subprocess.run(
                ["osascript", "-e", script],
                capture_output=True, text=True, timeout=5,
            )
            if r.returncode == 0:
                return {"success": True, "method": "position_offscreen_by_pid"}
        except (subprocess.TimeoutExpired, OSError):
            pass
        if attempt < retries - 1:
            time.sleep(2)
    return {"success": False, "error": "all retries failed"}


class _TrackerProcess:
    """Adapter giving ProcessTracker a poll()/pid interface for wait_for_socket.

    Without a Popen handle, the real exit code is unavailable. When the
    tracked PID dies, returncode is set to 0 so that the bounce state
    machine in wait_for_socket can recognize a potential Steam exit-0
    restart. The bounded adoption window and identity enforcement in
    try_adopt() prevent false-positive adoptions from crashes.
    """

    def __init__(self, tracker):
        self._tracker = tracker
        self.pid = tracker.current_pid
        self.returncode = None

    def poll(self):
        if self._tracker.is_process_alive():
            self.pid = self._tracker.current_pid
            return None
        self.returncode = 0
        return 0


def _write_health(health_data):
    """Write health JSON atomically."""
    try:
        tmp = HEALTH_FILE.with_suffix(".tmp")
        with open(tmp, "w") as f:
            f.write(json.dumps(health_data, indent=2))
            f.flush()
            os.fsync(f.fileno())
        os.replace(str(tmp), str(HEALTH_FILE))
    except OSError:
        try:
            HEALTH_FILE.write_text(json.dumps(health_data, indent=2))
        except OSError:
            pass


def main():
    global _start_time
    if len(sys.argv) < 4:
        print("Usage: _monitor.py <pid> <timeout> <headless:0|1> [skip_videos:0|1] [auto_dismiss:0|1]",
              file=sys.stderr)
        sys.exit(1)

    pid = int(sys.argv[1])
    timeout = int(sys.argv[2])
    headless = sys.argv[3] == "1"
    skip_videos = sys.argv[4] == "1" if len(sys.argv) > 4 else False
    auto_dismiss = sys.argv[5] == "1" if len(sys.argv) > 5 else False

    _start_time = time.monotonic()
    stages = []
    graphics_restored = False

    try:
        with open(MONITOR_LOG, "w") as f:
            f.write(f"=== BG3SE Monitor (pid {pid}, timeout {timeout}s, headless={headless}) ===\n")
    except OSError:
        pass

    tracker = ProcessTracker.from_record()
    if tracker is None or tracker.current_pid != pid:
        tracker = ProcessTracker(steam_preflight="unknown")
        tracker.current_pid = pid
        tracker.phase = "direct_running"
        tracker.lineage = [{"pid": pid, "role": "direct", "observed_exitcode": None}]

    proc = _TrackerProcess(tracker)

    _log(f"monitor_started  pid={pid} timeout={timeout}s headless={headless}", stages)

    health_data = {
        "status": "monitoring",
        "pid": pid,
        "launch_id": tracker.launch_id,
        "phase": tracker.phase,
        "started_at": time.time(),
    }
    _write_health(health_data)

    log_dir = os.path.expanduser("~/Library/Application Support/BG3SE/logs")
    latest_log = os.path.join(log_dir, "latest.log")
    if os.path.exists(latest_log):
        mtime = os.path.getmtime(latest_log)
        if mtime > time.time() - 5:
            _log("dylib_detected   BG3SE log file present (fresh)", stages)
        else:
            _log("dylib_stale      BG3SE log from previous session", stages)
    else:
        _log("dylib_pending    waiting for BG3SE log file...", stages)

    if skip_videos:
        _log("video_skip       BG3SE_SKIP_VIDEOS=1 (in-process Bink hook)", stages)

    if auto_dismiss:
        _log("auto_dismiss     BG3SE_AUTO_DISMISS_SPLASH=1 (in-process dismiss)", stages)

    if headless:
        time.sleep(5)
        current_pid = tracker.current_pid
        if current_pid and tracker.is_process_alive():
            mr = _move_window_offscreen_by_pid(current_pid)
            if mr.get("success"):
                _log("window_offscreen BG3 moved off-screen for headless boot", stages)
                tracker.window_contained = True
            else:
                _log(f"offscreen_failed {mr.get('error', 'unknown')}", stages)
        else:
            _log("offscreen_skip   process not alive for window move", stages)

    _log("socket_polling   waiting for SE socket to respond...", stages)

    health = wait_for_socket(
        timeout=timeout, dismiss_splash=True, process=proc,
        tracker=tracker,
    )
    health["pid"] = tracker.current_pid
    health["launch_id"] = tracker.launch_id
    health["phase"] = tracker.phase
    health["lineage"] = tracker.lineage

    if health.get("socket_connected"):
        elapsed_ms = health.get("elapsed_ms", 0)
        _log(f"socket_ready     connected in {elapsed_ms}ms", stages)
    elif health.get("stage") == "process_exited":
        code = health.get("exitcode", "?")
        _log(f"process_exited   BG3 died during boot (exit code {code})", stages)
    else:
        elapsed_ms = health.get("elapsed_ms", 0)
        _log(f"socket_timeout   no response after {elapsed_ms}ms", stages)

    if headless:
        is_terminal = (
            not health.get("socket_connected")
            and tracker.phase not in ("waiting_for_steam_relaunch", "adopted_running")
        )

        if health.get("socket_connected"):
            _log("hiding_window    requesting System Events hide...", stages)
            hr = hide_window()
            health["headless"] = {
                "requested": True,
                "hidden": hr.get("success", False),
                **hr,
            }
            if hr.get("success"):
                _log("window_hidden    BG3 running in background", stages)
            else:
                _log(f"hide_failed      {hr.get('error', 'unknown')}", stages)
            if not graphics_restored:
                restore_result = restore_headless_graphics(
                    reason="background_hide_complete",
                )
                health["headless"]["graphics_restore"] = restore_result
                graphics_restored = True
                if restore_result.get("success"):
                    _log("graphics_restore restored headless launch settings", stages)
                else:
                    _log(f"restore_failed   {restore_result.get('error', 'unknown')}", stages)
        elif is_terminal:
            health["headless"] = {
                "requested": True,
                "hidden": False,
                "reason": health.get("stage", "socket_not_connected"),
            }
            if health.get("stage") != "process_exited":
                _log("cancel_game      headless boot failed; force-quitting BG3", stages)
                health["headless"]["cancel"] = quit_game(force=True)
            if not graphics_restored:
                restore_result = restore_headless_graphics(
                    reason="background_socket_not_connected",
                )
                health["headless"]["graphics_restore"] = restore_result
                graphics_restored = True
                if restore_result.get("success"):
                    _log("graphics_restore restored headless launch settings", stages)
                else:
                    _log(f"restore_failed   {restore_result.get('error', 'unknown')}", stages)

    health["boot_stages"] = stages
    health["graphics_restored"] = graphics_restored
    _write_health(health)

    status = "ready" if health.get("socket_connected") else "failed"
    _log(f"monitor_done     status={status}", stages)

    try:
        with open(MONITOR_LOG, "a") as f:
            f.write(f"=== Monitor complete ({status}) ===\n")
    except OSError:
        pass


if __name__ == "__main__":
    main()
