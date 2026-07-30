"""Offline validation for BG3SE compatibility scenario manifests."""

import json
import shutil
import subprocess
from pathlib import Path

import pytest


ROOT = Path(__file__).resolve().parents[2]
SCENARIOS_DIR = ROOT / "tools" / "bg3se_harness" / "scenarios"
CATALOG_PATH = ROOT / "tools" / "bg3se_harness" / "catalog" / "popular_mods.json"
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
