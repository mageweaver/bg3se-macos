"""Tier H tests for savegames snapshot/restore behavior.

Regressions covered:
- restore() destroyed existing saves via shutil.rmtree without backup.
- restore() invented a save directory name the game cannot load: BG3 requires
  <Profile>-<id>__<DisplayName>/<DisplayName>.lsv, and -continueGame hangs at
  0% on anything else (observed live 2026-07-30). Restore must write back into
  the recorded source directory, and back up outside the game's save tree.
"""
import json
import shutil
from pathlib import Path

import pytest

SOURCE_DIR = "Tamarru-123__Ebonlake Grotto - 27h 19m"


@pytest.fixture
def save_env(monkeypatch, tmp_path):
    """Set up fake save dirs and a source-tracked fixture."""
    saves_dir = tmp_path / "saves"
    saves_dir.mkdir()
    fixtures_dir = tmp_path / "fixtures"
    fixtures_dir.mkdir()

    fixture = fixtures_dir / "test_fixture"
    fixture.mkdir()
    (fixture / "save.lsv").write_text("fixture_data")

    import bg3se_harness.savegames as sg
    (fixture / sg.FIXTURE_META_NAME).write_text(
        json.dumps({"source_dir_name": SOURCE_DIR})
    )
    monkeypatch.setattr(sg, "SAVES_DIR", saves_dir)
    monkeypatch.setattr(sg, "SAVE_FIXTURES_DIR", fixtures_dir)
    return saves_dir, fixtures_dir


def test_restore_targets_source_dir_and_backs_up_outside_saves(save_env):
    saves_dir, fixtures_dir = save_env
    import bg3se_harness.savegames as sg

    existing = saves_dir / SOURCE_DIR
    existing.mkdir()
    (existing / "save.lsv").write_text("original_data")

    result = sg.restore("test_fixture")
    assert result.get("success") is True
    assert result["save_name"] == SOURCE_DIR

    # Backup landed under the fixtures area, never in the game's save tree.
    assert [d for d in saves_dir.iterdir()] == [existing]
    backups = list((fixtures_dir / "_restore_backups").iterdir())
    assert len(backups) == 1
    assert (backups[0] / "save.lsv").read_text() == "original_data"

    assert (existing / "save.lsv").read_text() == "fixture_data"
    # The meta sidecar must not leak into the restored save directory.
    assert not (existing / sg.FIXTURE_META_NAME).exists()


def test_restore_works_without_existing_save(save_env):
    saves_dir, _ = save_env
    import bg3se_harness.savegames as sg

    result = sg.restore("test_fixture")
    assert result.get("success") is True

    dest = saves_dir / SOURCE_DIR
    assert dest.exists()
    assert (dest / "save.lsv").read_text() == "fixture_data"
    assert result["backup"] is None


def test_restore_refuses_fixture_without_source_metadata(save_env):
    saves_dir, fixtures_dir = save_env
    import bg3se_harness.savegames as sg

    legacy = fixtures_dir / "legacy_fixture"
    legacy.mkdir()
    (legacy / "save.lsv").write_text("legacy_data")

    result = sg.restore("legacy_fixture")
    assert "error" in result
    assert sg.FIXTURE_META_NAME in result["error"]
    assert list(saves_dir.iterdir()) == []


def test_snapshot_records_source_dir(save_env, monkeypatch):
    saves_dir, fixtures_dir = save_env
    import bg3se_harness.savegames as sg

    source = saves_dir / SOURCE_DIR
    source.mkdir()
    (source / "save.lsv").write_text("live_data")

    result = sg.snapshot("new_fixture", source_dir_name=SOURCE_DIR)
    assert result.get("success") is True

    meta = json.loads(
        (fixtures_dir / "new_fixture" / sg.FIXTURE_META_NAME).read_text()
    )
    assert meta["source_dir_name"] == SOURCE_DIR


def test_restore_aborts_when_backup_move_fails(save_env, monkeypatch):
    saves_dir, _ = save_env
    import bg3se_harness.savegames as sg

    existing = saves_dir / SOURCE_DIR
    existing.mkdir()
    (existing / "save.lsv").write_text("original_data")

    def failing_move(src, dst):
        raise OSError("disk full")

    monkeypatch.setattr(sg.shutil, "move", failing_move)

    result = sg.restore("test_fixture")
    assert "error" in result
    # Fail closed: the user's save is untouched, nothing was restored.
    assert (existing / "save.lsv").read_text() == "original_data"


def test_snapshot_backs_up_existing_fixture(save_env):
    saves_dir, fixtures_dir = save_env
    import bg3se_harness.savegames as sg

    source = saves_dir / SOURCE_DIR
    source.mkdir()
    (source / "save.lsv").write_text("new_data")

    result = sg.snapshot("test_fixture", source_dir_name=SOURCE_DIR)
    assert result.get("success") is True

    baks = [
        d for d in fixtures_dir.iterdir()
        if d.name.startswith("test_fixture.bak.")
    ]
    assert len(baks) == 1
    assert (baks[0] / "save.lsv").read_text() == "fixture_data"
    assert (fixtures_dir / "test_fixture" / "save.lsv").read_text() == "new_data"


def test_clone_of_fixture_keeps_restore_metadata(save_env):
    _, fixtures_dir = save_env
    import bg3se_harness.savegames as sg

    result = sg.clone("test_fixture", "clone_fixture")
    assert result.get("success") is True

    # The clone restores to the same game save directory as its source.
    meta = json.loads(
        (fixtures_dir / "clone_fixture" / sg.FIXTURE_META_NAME).read_text()
    )
    assert meta["source_dir_name"] == SOURCE_DIR


def test_restore_backups_dir_is_not_listed_as_fixture(save_env):
    _, fixtures_dir = save_env
    import bg3se_harness.savegames as sg

    (fixtures_dir / "_restore_backups").mkdir()
    names = [f["name"] for f in sg.list_fixtures()["fixtures"]]
    assert "_restore_backups" not in names
    assert "test_fixture" in names


def test_scan_archive_for_mod_markers_separates_high_and_low_confidence(monkeypatch, tmp_path):
    import bg3se_harness.savegames as sg

    lsv = tmp_path / "save.lsv"
    lsv.write_bytes(b"")

    class FakePak:
        def __init__(self, path):
            self.path = path

        def __enter__(self):
            return self

        def __exit__(self, *args):
            return None

        def list_files(self):
            return ["meta.lsf", "Globals.lsf"]

        def read_file(self, name):
            if name == "meta.lsf":
                return b"FolderA_uuid-a and uuid-a"
            return b"Waypoints"

    monkeypatch.setattr(sg, "PakReader", FakePak)

    result = sg._scan_archive_for_mod_markers(lsv, {
        "uuid-a": {
            "uuid": "uuid-a",
            "name": "Mod A",
            "folder": "FolderA_uuid-a",
            "version": "1",
        },
        "uuid-way": {
            "uuid": "uuid-way",
            "name": "Waypoints",
            "folder": "Waypoints_uuid-way",
            "version": "1",
        },
    })

    by_uuid = {mod["uuid"]: mod for mod in result["mods"]}
    assert by_uuid["uuid-a"]["confidence"] == "high"
    assert by_uuid["uuid-way"]["confidence"] == "low"


def test_save_mods_uses_only_high_confidence_markers_for_required(monkeypatch, tmp_path):
    import bg3se_harness.savegames as sg

    save_dir = tmp_path / "Char__Save"
    save_dir.mkdir()
    lsv = save_dir / "Save.lsv"
    lsv.write_bytes(b"")

    monkeypatch.setattr(sg, "_find_save_dir", lambda name=None, continue_latest=False: save_dir)
    monkeypatch.setattr(sg, "_load_known_mods", lambda: {
        "uuid-a": {"uuid": "uuid-a", "name": "Mod A", "folder": "FolderA_uuid-a"},
        "uuid-way": {"uuid": "uuid-way", "name": "Waypoints", "folder": "Waypoints_uuid-way"},
    })
    monkeypatch.setattr(sg, "_read_save_info_json", lambda path: {"Save Name": "Save"})
    monkeypatch.setattr(sg, "_scan_archive_for_mod_markers", lambda path, known: {
        "scanned_files": ["meta.lsf"],
        "unreadable_files": [],
        "mods": [
            {"uuid": "uuid-a", "name": "Mod A", "confidence": "high", "markers": []},
            {"uuid": "uuid-way", "name": "Waypoints", "confidence": "low", "markers": []},
        ],
    })

    from bg3se_harness.mod_manager import modsettings
    monkeypatch.setattr(modsettings, "read_mod_order", lambda: [
        {"uuid": "uuid-a", "name": "Mod A"},
    ])

    result = sg.save_mods(continue_latest=True)

    assert result["required_count"] == 1
    assert [mod["uuid"] for mod in result["required_mods"]] == ["uuid-a"]
    assert [mod["uuid"] for mod in result["low_confidence_candidates"]] == ["uuid-way"]
    assert result["comparison"]["missing_from_active"] == []
