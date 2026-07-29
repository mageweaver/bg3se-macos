"""Offline tests for macOS headless graphics setting mutation."""

import json
import xml.etree.ElementTree as ET

from bg3se_harness import launch


def _write_settings(path, entries):
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


def _entry_attrs(path, key):
    tree = ET.parse(path)
    for node in tree.getroot().iter("node"):
        if node.get("id") != "ConfigEntry":
            continue
        attrs = {attr.get("id"): dict(attr.attrib) for attr in node.findall("attribute")}
        if attrs.get("MapKey", {}).get("value") == key:
            return attrs
    return None


def _entry_values(path):
    values = {}
    tree = ET.parse(path)
    for node in tree.getroot().iter("node"):
        if node.get("id") != "ConfigEntry":
            continue
        attrs = {attr.get("id"): attr.get("value") for attr in node.findall("attribute")}
        key = attrs.get("MapKey")
        if key:
            values[key] = attrs.get("Value")
    return values


def _patch_paths(monkeypatch, tmp_path):
    settings_path = tmp_path / "graphicSettings.lsx"
    restore_path = tmp_path / "graphic_settings_headless_restore.json"
    monkeypatch.setattr(launch, "GRAPHIC_SETTINGS_PATH", settings_path)
    monkeypatch.setattr(launch, "HEADLESS_GRAPHICS_RESTORE_PATH", restore_path)
    monkeypatch.setattr(launch, "HARNESS_CONFIG_DIR", tmp_path)
    return settings_path, restore_path


def test_prepare_inserts_only_width_height_and_restore_removes_them(monkeypatch, tmp_path):
    settings_path, restore_path = _patch_paths(monkeypatch, tmp_path)
    _write_settings(settings_path, {"ScreenWidth": 1920, "ScreenHeight": 1080})

    prepared = launch.prepare_headless_graphics()
    assert prepared["success"] is True
    assert prepared["snapshot_created"] is True
    assert restore_path.exists()

    values = _entry_values(settings_path)
    assert "Fullscreen" not in values
    assert "FakeFullscreenEnabled" not in values
    assert "FakeFullscreen" not in values
    assert values["ScreenWidth"] == "1280"
    assert values["ScreenHeight"] == "720"

    restored = launch.restore_headless_graphics(reason="test")
    assert restored["success"] is True
    assert not restore_path.exists()

    values = _entry_values(settings_path)
    assert "Fullscreen" not in values
    assert "FakeFullscreenEnabled" not in values
    assert "FakeFullscreen" not in values
    assert values["ScreenWidth"] == "1920"
    assert values["ScreenHeight"] == "1080"


def test_headless_transient_entries_has_no_display_mode_keys():
    entries = launch.HEADLESS_TRANSIENT_GRAPHICS_ENTRIES
    assert "Fullscreen" not in entries
    assert "FakeFullscreenEnabled" not in entries
    assert "FakeFullscreen" not in entries
    assert entries["ScreenWidth"] == 1280
    assert entries["ScreenHeight"] == 720


def test_second_prepare_does_not_overwrite_existing_restore_snapshot(monkeypatch, tmp_path):
    settings_path, restore_path = _patch_paths(monkeypatch, tmp_path)
    _write_settings(settings_path, {"ScreenWidth": 1920, "ScreenHeight": 1080})

    assert launch.prepare_headless_graphics()["success"] is True
    first_snapshot = json.loads(restore_path.read_text())

    _write_settings(settings_path, {"ScreenWidth": 3440, "ScreenHeight": 1440})
    second = launch.prepare_headless_graphics()
    assert second["success"] is True
    assert second["snapshot_created"] is False
    assert json.loads(restore_path.read_text()) == first_snapshot

    restored = launch.restore_headless_graphics(reason="test")
    assert restored["success"] is True
    values = _entry_values(settings_path)
    assert values["ScreenWidth"] == "1920"
    assert values["ScreenHeight"] == "1080"


def test_restore_without_restore_file_is_noop_success(monkeypatch, tmp_path):
    settings_path, restore_path = _patch_paths(monkeypatch, tmp_path)
    _write_settings(settings_path, {"ScreenWidth": 1920})
    assert not restore_path.exists()

    restored = launch.restore_headless_graphics(reason="test")
    assert restored["success"] is True
    assert restored["noop"] is True
    assert restored["restored"] is False
    assert _entry_values(settings_path)["ScreenWidth"] == "1920"
