"""Tier H tests for the offline compatibility pipeline.

Regression: _scan_log_for_mod read last 500 lines without filtering
by timestamp, so stale errors from prior sessions polluted vet reports.
"""
import io
import json
import zipfile
from pathlib import Path
from types import SimpleNamespace

import pytest


LOG_LINES = [
    "[2026-05-01 10:00:00.000] [ERROR] [Mod    ] TestMod: old error from days ago",
    "[2026-05-01 10:00:01.000] [WARN ] [Mod    ] TestMod: old warning",
    "[2026-05-03 14:00:00.000] [ERROR] [Mod    ] TestMod: fresh error",
    "[2026-05-03 14:00:01.000] [WARN ] [Mod    ] TestMod: fresh warning",
    "[2026-05-03 14:00:02.000] [INFO ] [Mod    ] TestMod: loaded successfully",
]


@pytest.fixture
def fake_home(monkeypatch, tmp_path):
    """Redirect Path.home() so _scan_log_for_mod finds our fake log."""
    log_dir = tmp_path / "Library" / "Application Support" / "BG3SE" / "logs"
    log_dir.mkdir(parents=True)
    log_path = log_dir / "latest.log"
    log_path.write_text("\n".join(LOG_LINES))

    monkeypatch.setattr(Path, "home", classmethod(lambda cls: tmp_path))
    return log_path


def test_scan_without_timestamp_returns_all(fake_home):
    from bg3se_harness.compat import _scan_log_for_mod
    errors, warnings, scan_error = _scan_log_for_mod("TestMod")
    assert len(errors) == 2
    assert len(warnings) == 2
    assert scan_error is None


def test_scan_with_timestamp_filters_old(fake_home):
    from bg3se_harness.compat import _scan_log_for_mod
    errors, warnings, scan_error = _scan_log_for_mod("TestMod", since_timestamp="2026-05-03 00:00:00")
    assert len(errors) == 1
    assert "fresh error" in errors[0]
    assert len(warnings) == 1
    assert "fresh warning" in warnings[0]
    assert scan_error is None


def test_scan_with_future_timestamp_returns_none(fake_home):
    from bg3se_harness.compat import _scan_log_for_mod
    errors, warnings, scan_error = _scan_log_for_mod("TestMod", since_timestamp="2027-01-01 00:00:00")
    assert len(errors) == 0
    assert len(warnings) == 0
    assert scan_error is None


def test_scan_read_failure_is_reported_not_silent(fake_home, monkeypatch):
    # A locked/unreadable log must be distinguishable from "scanned clean".
    from bg3se_harness.compat import _scan_log_for_mod

    def boom(self, *args, **kwargs):
        raise OSError("resource locked")

    monkeypatch.setattr(Path, "read_text", boom)
    errors, warnings, scan_error = _scan_log_for_mod("TestMod")
    assert errors == []
    assert warnings == []
    assert scan_error is not None
    assert "resource locked" in scan_error


def _zip_bytes(entries):
    payload = io.BytesIO()
    with zipfile.ZipFile(payload, "w") as archive:
        for name, content in entries.items():
            archive.writestr(name, content)
    return payload.getvalue()


def _mock_download(monkeypatch, uri, payload, file_id=77):
    from bg3se_harness.mod_manager import nexus

    monkeypatch.setattr(nexus, "get_download_links", lambda *args, **kwargs: {
        "success": True,
        "file_id": file_id,
        "links": [{"uri": uri}],
    })

    requests = []

    class FakeResponse(io.BytesIO):
        # Mirror the attributes a urlopen response exposes so a future
        # header/status read in download_file fails here too, not just live.
        status = 200
        headers = {}

    def fake_urlopen(request, timeout):
        requests.append(request)
        return FakeResponse(payload)

    monkeypatch.setattr(nexus.urllib.request, "urlopen", fake_urlopen)
    return nexus, requests


def test_download_file_extracts_flattened_zip_paks(monkeypatch, tmp_path):
    payload = _zip_bytes({
        "nested/First.pak": b"first",
        "Second.PAK": b"second",
        "readme.txt": b"ignored",
    })
    nexus, requests = _mock_download(
        monkeypatch,
        "https://cdn.example.test/Mod%20Bundle.zip?token=secret",
        payload,
    )

    result = nexus.download_file(123, str(tmp_path))

    assert result == {
        "success": True,
        "file_id": 77,
        "archive": str(tmp_path / "Mod Bundle.zip"),
        "paks": [
            str(tmp_path / "First.pak"),
            str(tmp_path / "Second.PAK"),
        ],
    }
    assert (tmp_path / "First.pak").read_bytes() == b"first"
    assert (tmp_path / "Second.PAK").read_bytes() == b"second"
    assert requests[0].get_header("User-agent").startswith("bg3se-harness/")


def test_download_file_quotes_literal_spaces_in_cdn_uri(monkeypatch, tmp_path):
    # Real Nexus CDN links contain unencoded spaces (observed live 2026-07-30);
    # http.client raises InvalidURL unless they are re-quoted first.
    nexus, requests = _mock_download(
        monkeypatch,
        "https://cdn.example.test/3474/9162/Mod Configuration Menu 1.40.1.pak"
        "?expires=1785439015&md5=zw2kLDmhbTLJkfyw7CZvVw&user_id=268171597",
        b"LSPK" + b"\x00" * 16,
    )

    result = nexus.download_file(9162, str(tmp_path))

    assert result["success"] is True
    assert result["paks"] == [str(tmp_path / "Mod Configuration Menu 1.40.1.pak")]
    requested_url = requests[0].full_url
    assert " " not in requested_url
    assert "Mod%20Configuration%20Menu%201.40.1.pak" in requested_url
    assert "md5=zw2kLDmhbTLJkfyw7CZvVw" in requested_url


def test_download_file_sniffs_extensionless_zip(monkeypatch, tmp_path):
    # Observed live 2026-07-30: Expansion Level 20's CDN filename is a bare
    # UUID with no extension, but the payload is a ZIP.
    payload = _zip_bytes({"Expansion.pak": b"expansion"})
    nexus, _ = _mock_download(
        monkeypatch,
        "https://cdn.example.test/0abcef52-56f5-441a-a044-ef6099db1c53?x=1",
        payload,
    )

    result = nexus.download_file(279, str(tmp_path))

    assert result["success"] is True
    assert result["archive"] == str(tmp_path / "0abcef52-56f5-441a-a044-ef6099db1c53")
    assert result["paks"] == [str(tmp_path / "Expansion.pak")]


def test_download_file_sniffs_extensionless_pak(monkeypatch, tmp_path):
    nexus, _ = _mock_download(
        monkeypatch,
        "https://cdn.example.test/deadbeef-0000?x=1",
        b"LSPK" + b"\x00" * 16,
    )

    result = nexus.download_file(1, str(tmp_path))

    assert result["success"] is True
    assert result["archive"] is None
    assert result["paks"] == [str(tmp_path / "deadbeef-0000")]


def test_download_file_rejects_pak_named_non_pak_content(monkeypatch, tmp_path):
    # An HTTP 200 with an HTML challenge page saved under a .pak name must
    # not be reported as a successful PAK download.
    nexus, _ = _mock_download(
        monkeypatch,
        "https://cdn.example.test/Fake.pak?token=secret",
        b"<html>challenge</html>",
    )

    result = nexus.download_file(123, str(tmp_path))

    assert result["success"] is False
    assert result["error_type"] == "invalid_archive"
    assert "LSPK" in result["message"]


def test_download_file_rejects_zip_path_traversal(monkeypatch, tmp_path):
    payload = _zip_bytes({"../escape.pak": b"unsafe"})
    nexus, _ = _mock_download(
        monkeypatch,
        "https://cdn.example.test/unsafe.zip",
        payload,
    )

    result = nexus.download_file(123, str(tmp_path))

    assert result["success"] is False
    assert result["error_type"] == "unsafe_archive"
    assert not (tmp_path.parent / "escape.pak").exists()


def test_download_file_keeps_unsupported_archive(monkeypatch, tmp_path):
    nexus, _ = _mock_download(
        monkeypatch,
        "https://cdn.example.test/Some%20Mod.7z?token=secret",
        b"7z payload",
    )

    result = nexus.download_file(123, str(tmp_path))

    assert result["success"] is False
    assert result["error_type"] == "unsupported_archive"
    assert result["path"] == str(tmp_path / "Some Mod.7z")
    assert Path(result["path"]).read_bytes() == b"7z payload"


@pytest.mark.parametrize(
    ("output", "success", "error", "tail_key"),
    [
        ("noise\nBG3SE_COMPAT_PASS\n", True, None, "output"),
        ("BG3SE_COMPAT_FAIL: boom\n", False, "boom", "output"),
        ("ordinary Lua output", False, "no sentinel in output", "output_tail"),
    ],
)
def test_assertion_sentinel_parsing(output, success, error, tail_key):
    from bg3se_harness.compat import _parse_assertion_output

    result = _parse_assertion_output(output)

    assert result["success"] is success
    # The sentinel cases carry "output"; the no-sentinel case deliberately
    # uses "output_tail" — consumers must not assume a uniform shape.
    assert tail_key in result
    if error is not None:
        assert result["error"] == error
    else:
        assert "error" not in result


def _passing_doctor():
    names = ["bg3_app_bundle", "bg3_binary", "se_dylib_deployed", "mods_directory"]
    return {
        "all_passed": True,
        "checks": [{"name": name, "passed": True} for name in names],
        "passed": len(names),
        "total": len(names),
    }


def _scenario_step(result, name):
    return next(step for step in result["steps"] if step["name"] == name)


def test_requires_mcm_is_injected_first(monkeypatch, tmp_path):
    from bg3se_harness import compat
    from bg3se_harness import doctor
    from bg3se_harness.mod_manager import modsettings, registry

    monkeypatch.setattr(compat, "REPORTS_DIR", tmp_path / "reports")
    monkeypatch.setattr(compat, "_load_scenarios", lambda: {
        "dependent": {"mods": ["dependent", "mcm"], "assertions": []},
    })
    monkeypatch.setattr(compat, "_load_popular_mods", lambda: {
        "mcm": {"name": "Mod Configuration Menu", "nexus_id": 1, "requires_mcm": False},
        "dependent": {"name": "Dependent Mod", "nexus_id": 2, "requires_mcm": True},
    })
    monkeypatch.setattr(doctor, "run_doctor", _passing_doctor)
    monkeypatch.setattr(registry, "load_registry", lambda: {
        "mcm-uuid": {"name": "Mod Configuration Menu", "folder": "MCM"},
        "dep-uuid": {"name": "Dependent Mod", "folder": "Dependent"},
    })
    monkeypatch.setattr(modsettings, "read_mod_order", lambda: [
        {"uuid": "mcm-uuid"},
        {"uuid": "dep-uuid"},
    ])

    result = compat.run_scenario("dependent")

    dependency = _scenario_step(result, "dependency_resolution")
    assert dependency["effective_mods"] == ["mcm", "dependent"]
    assert dependency["requires_mcm"] is True


def test_auto_install_skip_and_install_decisions(monkeypatch, tmp_path):
    from bg3se_harness import compat
    from bg3se_harness import doctor
    from bg3se_harness.mod_manager import installer, nexus, registry

    monkeypatch.setattr(compat, "REPORTS_DIR", tmp_path / "reports")
    monkeypatch.setattr(compat, "_load_scenarios", lambda: {
        "test_mod": {"mods": ["test_mod"], "assertions": []},
        "pinned_mod": {"mods": ["pinned_mod"], "assertions": []},
    })
    monkeypatch.setattr(compat, "_load_popular_mods", lambda: {
        "test_mod": {"name": "Test Mod", "nexus_id": 99, "requires_mcm": False},
        "pinned_mod": {
            "name": "Pinned Mod",
            "nexus_id": 77,
            "file_id": 777,
            "requires_mcm": False,
        },
    })
    monkeypatch.setattr(doctor, "run_doctor", _passing_doctor)
    monkeypatch.setattr(registry, "load_registry", lambda: {})

    calls = []

    def fake_download(mod_id, dest_dir, file_id=None):
        calls.append(("download", mod_id, dest_dir, file_id))
        return {
            "success": True,
            "file_id": 12,
            "archive": None,
            "paks": [str(tmp_path / "TestMod.pak")],
        }

    def fake_install(path, enable):
        calls.append(("install", path, enable))
        return {"installed": True, "uuid": "test-uuid", "name": "Test Mod"}

    monkeypatch.setattr(nexus, "download_file", fake_download)
    monkeypatch.setattr(installer, "install_local", fake_install)

    skipped = compat.run_scenario("test_mod", auto_install=False)
    skipped_step = _scenario_step(skipped, "mod_check_test_mod")
    assert skipped_step["success"] is False
    assert skipped_step["note"] == "not installed; rerun with --auto-install"
    assert calls == []

    installed = compat.run_scenario("test_mod", auto_install=True)
    installed_step = _scenario_step(installed, "mod_check_test_mod")
    assert installed_step["success"] is True
    assert installed_step["state"] == "installed"
    assert calls[0][0:2] == ("download", 99)
    assert calls[0][3] is None
    assert calls[1][0] == "install"
    assert calls[1][2] is True

    calls.clear()
    pinned = compat.run_scenario("pinned_mod", auto_install=True)
    pinned_step = _scenario_step(pinned, "mod_check_pinned_mod")
    assert pinned_step["success"] is True
    assert calls[0][0:2] == ("download", 77)
    assert calls[0][3] == 777


def test_enable_step_fails_when_registry_update_fails(monkeypatch, tmp_path):
    # Regression: add_mod success used to mask a set_mod_enabled failure, and
    # an enable that succeeded did not count as a mod-state change.
    from bg3se_harness import compat, doctor
    from bg3se_harness.mod_manager import modsettings, registry

    monkeypatch.setattr(compat, "REPORTS_DIR", tmp_path / "reports")
    monkeypatch.setattr(compat, "_load_scenarios", lambda: {
        "test_mod": {"mods": ["test_mod"], "assertions": []},
    })
    monkeypatch.setattr(compat, "_load_popular_mods", lambda: {
        "test_mod": {"name": "TestMod", "nexus_id": 99, "requires_mcm": False},
    })
    monkeypatch.setattr(doctor, "run_doctor", _passing_doctor)
    monkeypatch.setattr(registry, "load_registry", lambda: {
        "test-uuid": {"name": "TestMod", "folder": "TestMod"},
    })
    monkeypatch.setattr(modsettings, "read_mod_order", lambda: [])
    monkeypatch.setattr(modsettings, "add_mod", lambda **kwargs: {"added": True})
    monkeypatch.setattr(
        registry, "set_mod_enabled",
        lambda uuid, enabled: {"updated": False, "uuid": uuid, "reason": "not found"},
    )

    result = compat.run_scenario("test_mod")
    step = _scenario_step(result, "mod_check_test_mod")
    assert step["success"] is False
    assert step["state"] == "enable_failed"
    assert result["mod_state_changed"] is False

    monkeypatch.setattr(
        registry, "set_mod_enabled",
        lambda uuid, enabled: {"updated": True, "uuid": uuid, "enabled": True},
    )
    result = compat.run_scenario("test_mod")
    step = _scenario_step(result, "mod_check_test_mod")
    assert step["success"] is True
    assert step["state"] == "enabled"
    assert result["mod_state_changed"] is True


def test_quit_runs_even_when_a_post_launch_check_raises(monkeypatch, tmp_path, fake_home):
    # Regression: an exception between launch and quit (here a Ctrl-C during
    # the identity handshake) used to orphan the launched game process.
    from bg3se_harness import cli, compat, console, doctor, launch
    from bg3se_harness.mod_manager import modsettings, registry

    monkeypatch.setattr(compat, "REPORTS_DIR", tmp_path / "reports")
    monkeypatch.setattr(compat, "_load_scenarios", lambda: {
        "test_mod": {"mods": ["test_mod"], "assertions": ["assert(true)"]},
    })
    monkeypatch.setattr(compat, "_load_popular_mods", lambda: {
        "test_mod": {"name": "TestMod", "nexus_id": 99, "requires_mcm": False},
    })
    monkeypatch.setattr(doctor, "run_doctor", _passing_doctor)
    monkeypatch.setattr(registry, "load_registry", lambda: {
        "test-uuid": {"name": "TestMod", "folder": "TestMod"},
    })
    monkeypatch.setattr(modsettings, "read_mod_order", lambda: [{"uuid": "test-uuid"}])
    monkeypatch.setattr(launch, "is_running", lambda: False)
    monkeypatch.setattr(launch, "socket_alive", lambda: False)
    monkeypatch.setattr(cli, "_launch_until_socket", lambda **kwargs: (
        None,
        {"socket_connected": True, "session_loaded": True, "pid": 42},
        1,
    ))

    quits = []

    def fake_quit(force=False):
        quits.append(force)
        return {"success": True}

    monkeypatch.setattr(launch, "quit_game", fake_quit)

    class ExplodingConsole:
        def __enter__(self):
            raise KeyboardInterrupt

        def __exit__(self, *exc):
            return None

    monkeypatch.setattr(console, "Console", ExplodingConsole)

    with pytest.raises(KeyboardInterrupt):
        compat.run_scenario("test_mod", launch=True)

    assert quits == [False]


def test_mod_install_nexus_installs_each_downloaded_pak(monkeypatch, tmp_path, capsys):
    from bg3se_harness import mod_cli
    from bg3se_harness.mod_manager import installer, nexus

    temp_dir = tmp_path / "download"
    monkeypatch.setattr(mod_cli.tempfile, "mkdtemp", lambda **kwargs: str(temp_dir))
    monkeypatch.setattr(nexus, "download_file", lambda mod_id, dest_dir: {
        "success": True,
        "file_id": 22,
        "archive": str(temp_dir / "bundle.zip"),
        "paks": [str(temp_dir / "One.pak"), str(temp_dir / "Two.pak")],
    })

    calls = []

    def fake_install(path, enable):
        calls.append((path, enable))
        return {"installed": True, "uuid": Path(path).stem}

    monkeypatch.setattr(installer, "install_local", fake_install)
    args = SimpleNamespace(
        mod_command="install",
        source="nexus:123",
        no_enable=True,
        links_only=False,
    )

    assert mod_cli.cmd_mod(args) == 0
    result = json.loads(capsys.readouterr().out)
    assert result["success"] is True
    assert [item["pak"] for item in result["installs"]] == [
        str(temp_dir / "One.pak"),
        str(temp_dir / "Two.pak"),
    ]
    assert calls == [
        (str(temp_dir / "One.pak"), False),
        (str(temp_dir / "Two.pak"), False),
    ]


def test_live_assertions_use_identity_and_sentinel_protocol(
    monkeypatch,
    tmp_path,
    fake_home,
):
    from bg3se_harness import compat, crashlog, doctor, launch, screenshot
    from bg3se_harness import console
    from bg3se_harness.mod_manager import modsettings, registry

    monkeypatch.setattr(compat, "REPORTS_DIR", tmp_path / "reports")
    monkeypatch.setattr(compat, "_load_scenarios", lambda: {
        "test_mod": {
            "mods": ["test_mod"],
            "assertions": ["assert(true)", "assert(false)", "assert(true)"],
        },
    })
    monkeypatch.setattr(compat, "_load_popular_mods", lambda: {
        "test_mod": {"name": "TestMod", "nexus_id": 99, "requires_mcm": False},
    })
    monkeypatch.setattr(doctor, "run_doctor", _passing_doctor)
    monkeypatch.setattr(registry, "load_registry", lambda: {
        "test-uuid": {"name": "TestMod", "folder": "TestMod"},
    })
    monkeypatch.setattr(modsettings, "read_mod_order", lambda: [{"uuid": "test-uuid"}])
    monkeypatch.setattr(launch, "is_running", lambda: True)
    monkeypatch.setattr(launch, "socket_alive", lambda: True)
    monkeypatch.setattr(screenshot, "capture", lambda **kwargs: {"path": kwargs["output"]})
    monkeypatch.setattr(crashlog, "get_crash_report", lambda **kwargs: {"signal": None})

    sent = []
    responses = iter([
        '{"pid": 321, "session_init": "complete"}',
        "BG3SE_COMPAT_PASS",
        "BG3SE_COMPAT_FAIL: assertion exploded",
        "Lua returned without marker",
    ])

    class FakeConsole:
        def __enter__(self):
            return self

        def __exit__(self, *exc):
            return None

        def send(self, command):
            sent.append(command)
            return next(responses)

    monkeypatch.setattr(console, "Console", FakeConsole)

    result = compat.run_scenario("test_mod", launch=True)

    assert sent[0] == "!identity"
    assert "pcall(function() assert(true) end)" in sent[1]
    assert "BG3SE_COMPAT_PASS" in sent[1]
    assert _scenario_step(result, "identity")["pid"] == 321
    assert _scenario_step(result, "assertion_0")["success"] is True
    assert _scenario_step(result, "assertion_1")["error"] == "assertion exploded"
    assert _scenario_step(result, "assertion_2")["error"] == "no sentinel in output"


def _write_report(path, steps):
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(json.dumps({"steps": steps}))


def test_diff_scenario_reports_regressions_fixes_and_step_changes(monkeypatch, tmp_path):
    from bg3se_harness import compat

    baseline = tmp_path / "baseline" / "sample.json"
    reports = tmp_path / "reports"
    _write_report(baseline, [
        {"name": "regressed_step", "success": True},
        {"name": "fixed_step", "success": False},
        {"name": "removed_step", "success": True},
    ])
    _write_report(reports / "sample_100" / "report.json", [
        {"name": "regressed_step", "success": False},
        {"name": "fixed_step", "success": True},
        {"name": "added_step", "success": True},
    ])
    monkeypatch.setattr(compat, "REPORTS_DIR", reports)
    monkeypatch.setattr(compat, "_baseline_path", lambda scenario: baseline)

    result = compat.diff_scenario("sample")

    assert result["success"] is True
    assert result["regressed"] is True
    assert [item["name"] for item in result["regressions"]] == ["regressed_step"]
    assert [item["name"] for item in result["fixes"]] == ["fixed_step"]
    assert result["added_steps"] == ["added_step"]
    assert result["removed_steps"] == ["removed_step"]


def test_diff_scenario_missing_baseline_lists_existing_reports(monkeypatch, tmp_path):
    from bg3se_harness import compat

    baseline = tmp_path / "baseline" / "missing.json"
    reports = tmp_path / "reports"
    _write_report(reports / "missing_100" / "report.json", [])
    other_baseline = baseline.parent / "other.json"
    _write_report(other_baseline, [])
    monkeypatch.setattr(compat, "REPORTS_DIR", reports)
    monkeypatch.setattr(compat, "_baseline_path", lambda scenario: baseline)

    result = compat.diff_scenario("missing")

    assert result["success"] is False
    assert result["error_type"] == "missing_baseline"
    assert result["available_baselines"] == ["other"]
    assert result["available_runs"] == ["missing_100"]


def test_diff_scenario_missing_run_lists_existing_baselines(monkeypatch, tmp_path):
    from bg3se_harness import compat

    baseline = tmp_path / "baseline" / "sample.json"
    _write_report(baseline, [])
    reports = tmp_path / "reports"
    reports.mkdir()
    monkeypatch.setattr(compat, "REPORTS_DIR", reports)
    monkeypatch.setattr(compat, "_baseline_path", lambda scenario: baseline)

    result = compat.diff_scenario("sample")

    assert result["success"] is False
    assert result["error_type"] == "missing_run"
    assert result["available_baselines"] == ["sample"]
    assert result["available_runs"] == []
