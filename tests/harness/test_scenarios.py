"""Offline validation for BG3SE compatibility scenario manifests."""

import json
import re
import shutil
import subprocess
from pathlib import Path

import pytest


ROOT = Path(__file__).resolve().parents[2]
SCENARIOS_DIR = ROOT / "tools" / "bg3se_harness" / "scenarios"
CATALOG_PATH = ROOT / "tools" / "bg3se_harness" / "catalog" / "popular_mods.json"
BASELINE_DIR = ROOT / "docs" / "compat-reports" / "baseline"
REQUIRED_KEYS = {
    "description",
    "mods",
    "save_fixture",
    "assertions",
    "requires_save",
}
EXPECTED_SCENARIOS = {
    "5e_spells",
    "always_show_approvals",
    "auto_send_food",
    "camp_event_notifications",
    "combat_extender",
    "community_library",
    "expansion_lvl20",
    "mcm",
    "more_reactive_companions",
    "party_limit_begone",
}
LUAC = shutil.which("luac")


def load_catalog():
    return json.loads(CATALOG_PATH.read_text())["mods"]


def load_scenarios():
    return {
        path.stem: json.loads(path.read_text())
        for path in sorted(SCENARIOS_DIR.glob("*.json"))
    }


def test_scenario_manifest_schema():
    catalog = load_catalog()
    scenarios = load_scenarios()

    assert set(scenarios) == EXPECTED_SCENARIOS
    for name, scenario in scenarios.items():
        assert REQUIRED_KEYS <= scenario.keys(), f"{name}: missing required keys"
        assert isinstance(scenario["description"], str) and scenario["description"].strip()
        assert isinstance(scenario["mods"], list) and scenario["mods"]
        assert all(mod in catalog for mod in scenario["mods"])
        assert scenario["save_fixture"] == "vetting_base"
        assert scenario["requires_save"] is True
        assert isinstance(scenario["assertions"], list) and scenario["assertions"]
        assert all(
            isinstance(assertion, str) and assertion.strip()
            for assertion in scenario["assertions"]
        )


def test_mcm_dependency_is_runner_injected():
    for name, scenario in load_scenarios().items():
        if name == "mcm":
            assert scenario["mods"] == ["mcm"]
        else:
            assert all("mcm" not in mod.lower() for mod in scenario["mods"])


def test_load_order_assertions_use_normalized_name_or_uuid():
    """Load-order checks must not regress to raw-name matching.

    Wave 5 established two robust patterns: name matching after gsub
    normalization (case/whitespace/underscore/hyphen stripped), or exact UUID
    equality for mods whose PAK metadata hides the public name (Party Limit
    Begone ships as a Gustav override, Camp Event Notifications as
    KvCampEvents, Expansion Level 20 as bare "Expansion").
    """
    for name, scenario in load_scenarios().items():
        for index, assertion in enumerate(scenario["assertions"]):
            if "GetLoadOrder" not in assertion or "load order" not in assertion:
                continue
            uses_gsub = "string.gsub" in assertion
            uses_uuid = re.search(r"u\s*==\s*'[0-9a-f]{8}-[0-9a-f-]{27}'", assertion)
            assert uses_gsub or uses_uuid, (
                f"{name} assertion {index} matches load order by raw name — "
                f"use gsub normalization or an exact UUID"
            )


def test_vetted_catalog_mods_have_complete_stamps():
    for key, mod in load_catalog().items():
        if mod.get("status") != "working":
            continue
        for field in ("vetted_version", "vetted_date", "vetted_evidence"):
            assert mod.get(field), f"{key}: vetted mod missing {field}"
        assert re.search(r"\d+/\d+", mod["vetted_evidence"]), (
            f"{key}: vetted_evidence '{mod['vetted_evidence']}' lacks an N/N count"
        )
        assert re.fullmatch(r"v\d+\.\d+\.\d+", mod["vetted_version"]), (
            f"{key}: vetted_version '{mod['vetted_version']}' is not vX.Y.Z"
        )


def test_baselines_parse_and_cover_every_scenario():
    baselines = {p.stem for p in BASELINE_DIR.glob("*.json")}
    assert baselines == EXPECTED_SCENARIOS, (
        f"baseline/scenario mismatch: {baselines ^ EXPECTED_SCENARIOS}"
    )
    for path in BASELINE_DIR.glob("*.json"):
        data = json.loads(path.read_text())
        assert data.get("success") is True, f"{path.name}: baseline is not a passing run"
        assert isinstance(data.get("steps"), list) and data["steps"], (
            f"{path.name}: baseline has no steps"
        )
        assert re.fullmatch(r"\d+/\d+ steps passed", data.get("summary", "")), (
            f"{path.name}: unexpected summary '{data.get('summary')}'"
        )


def test_max_osiris_listeners_is_512():
    header = (ROOT / "src" / "lua" / "lua_osiris.h").read_text()
    match = re.search(r"#define\s+MAX_OSIRIS_LISTENERS\s+(\d+)", header)
    assert match, "MAX_OSIRIS_LISTENERS not found in lua_osiris.h"
    assert int(match.group(1)) == 512, (
        f"MAX_OSIRIS_LISTENERS is {match.group(1)}, expected 512 — "
        f"64 was exhausted by real mod stacks (Expansion Level 20, Wave 5)"
    )


@pytest.mark.skipif(LUAC is None, reason="luac is not available on PATH")
def test_scenario_assertions_compile_as_lua():
    for name, scenario in load_scenarios().items():
        for index, assertion in enumerate(scenario["assertions"]):
            source = f"return function() {assertion} end\n"
            result = subprocess.run(
                [LUAC, "-p", "-"],
                input=source,
                text=True,
                capture_output=True,
                check=False,
            )
            assert result.returncode == 0, (
                f"{name} assertion {index} is invalid Lua:\n{result.stderr}"
            )
