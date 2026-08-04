"""Wave 7 contract manifest integrity + parity scan --contract scoring.

The manifest (docs/parity-100/contract.json) is the scoring denominator for
the Wave 7 campaign: every Windows-registered contract classified as
implemented / behavioral_gap / matched_upstream_todo / excluded. These tests
enforce the Phase 0 invariant — zero unclassified entries — and pin the
verified planning-phase counts so silent regressions of the manifest fail CI.
"""

import json
from pathlib import Path

import pytest

from bg3se_harness.parity import CONTRACT_PATH, contract_report

VALID_CLASSES = {"implemented", "behavioral_gap", "matched_upstream_todo", "excluded"}
VALID_KINDS = {"function", "proxy_method", "event", "alias"}


@pytest.fixture(scope="module")
def manifest():
    assert CONTRACT_PATH.exists(), f"contract manifest missing: {CONTRACT_PATH}"
    with open(CONTRACT_PATH) as f:
        return json.load(f)


def test_manifest_schema(manifest):
    assert manifest["version"] == 1
    assert manifest["namespaces"], "manifest has no namespaces"
    for ns_name, ns in manifest["namespaces"].items():
        assert ns.get("contracts"), f"{ns_name} has no contracts"
        for entry in ns["contracts"]:
            assert entry.get("name"), f"{ns_name}: unnamed contract"
            assert entry.get("class") in VALID_CLASSES, (
                f"{ns_name}.{entry['name']}: invalid class {entry.get('class')!r}"
            )
            assert entry.get("kind") in VALID_KINDS, (
                f"{ns_name}.{entry['name']}: invalid kind {entry.get('kind')!r}"
            )


def test_zero_unclassified(manifest):
    report = contract_report()
    assert "error" not in report, report.get("unclassified", report.get("error"))
    assert report["total_contracts"] > 0


def test_summary_matches_entries(manifest):
    """The pre-baked summary block must agree with the per-entry tallies."""
    report = contract_report()
    for key in ("implemented", "behavioral_gap", "matched_upstream_todo", "excluded"):
        assert manifest["summary"][key] == report[key], (
            f"summary.{key}: JSON says {manifest['summary'][key]}, "
            f"entries say {report[key]}"
        )


def test_upstream_todo_is_only_construct(manifest):
    todos = [
        f"{ns_name}.{e['name']}"
        for ns_name, ns in manifest["namespaces"].items()
        for e in ns["contracts"]
        if e["class"] == "matched_upstream_todo"
    ]
    assert todos == ["Ext.Types.Construct"], todos


def test_entity_contract_counts(manifest):
    """Pin the per-function diff: 26 Windows Entity registrations, 19
    behavioral after Wave 7 A1 (HandleToUuid, UuidToHandle,
    GetAllEntitiesWithUuid, GetRegisteredComponentTypes,
    GetEntitiesAroundPosition). The tracing quartet stays a gap: the A8
    prototype is a flat bounded log, not the Windows trace tree."""
    ns = manifest["namespaces"].get("Ext.Entity")
    assert ns is not None, "Ext.Entity missing from manifest"
    contracts = ns["contracts"]
    assert len(contracts) == 26, f"Entity block should have 26 entries, got {len(contracts)}"
    implemented = {e["name"] for e in contracts if e["class"] == "implemented"}
    assert len(implemented) == 19, sorted(implemented)
    for name in ("HandleToUuid", "UuidToHandle", "GetAllEntitiesWithUuid",
                 "GetRegisteredComponentTypes", "GetEntitiesAroundPosition"):
        assert name in implemented, f"{name} should be implemented (Wave 7 A1)"
    gaps = {e["name"] for e in contracts if e["class"] == "behavioral_gap"}
    assert gaps == {"Create", "Destroy", "EnableTracing", "GetTrace",
                    "ClearTrace", "OnSystemUpdate", "OnSystemPostUpdate"}, sorted(gaps)


def test_types_contract_counts(manifest):
    """Pin the Phase 0 correction: 14 registrations, 10 implemented, 3 gaps."""
    ns = manifest["namespaces"].get("Ext.Types")
    assert ns is not None, "Ext.Types missing from manifest"
    contracts = ns["contracts"]
    assert len(contracts) == 14, f"Types block should have 14 entries, got {len(contracts)}"
    gaps = {e["name"] for e in contracts if e["class"] == "behavioral_gap"}
    assert gaps == {"GetHashSetValueAt", "AddCustomFunction", "AddCustomProperty"}, gaps
    report = contract_report()
    assert report["namespaces"]["Ext.Types"]["behavioral_percent"] == 76.9


def test_net_has_no_gaps(manifest):
    """Wave 7 A6 closed PlayerHasExtender's GUID branch — Net is gap-free."""
    gaps = [
        e["name"]
        for ns_name, ns in manifest["namespaces"].items()
        if ns_name.startswith("Ext.Net")
        for e in ns["contracts"]
        if e["class"] == "behavioral_gap"
    ]
    assert gaps == [], gaps
    net = manifest["namespaces"]["Ext.Net"]["contracts"]
    phe = next(e for e in net if e["name"] == "PlayerHasExtender")
    assert phe["class"] == "implemented"
