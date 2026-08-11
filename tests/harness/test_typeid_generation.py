"""Deterministic audit for generated component and replication TypeIds."""

from __future__ import annotations

import re
from pathlib import Path

import pytest

from extract_typeids import (
    COMPONENT_CONTEXT,
    ONE_FRAME_CONTEXT,
    REPLICATED_CONTEXT,
    TypeIdSymbol,
    component_surface,
    curated_authority,
    extract_component_contexts,
    extract_replication_contexts,
    generate_header,
    generate_registration_code,
    migration_report,
    one_frame_authority,
    supported_replication_contexts,
)


ROOT = Path(__file__).resolve().parents[2]
OLD_BUILD = "4.1.1.7209685"
NEW_BUILD = "4.1.1.7398727"
OLD_BINARY = (
    ROOT / "build" / "migration-binaries" / OLD_BUILD / "Baldur's Gate 3"
)
NEW_BINARY = (
    ROOT / "build" / "migration-binaries" / NEW_BUILD / "Baldur's Gate 3"
)

EXPECTED_ADDED = {
    "ls::ugc::CacheModDependenciesSingletonComponent",
    "ls::ugc::CacheModInfoSingletonComponent",
    "ls::ugc::CacheModListSingletonComponent",
    "ls::ugc::PendingModDependencyRequestSingletonComponent",
    "ls::ugc::PendingModInfoRequestSingletonComponent",
    "ls::ugc::PendingModListRequestSingletonComponent",
}
EXPECTED_REMOVED = {"ecl::mod::RequestItemInfoSingletonComponent"}
EXPECTED_DELTA_FAMILIES = {
    0x2FF30: 1689,
    0x2FFD8: 162,
    0x30668: 86,
    0x30278: 42,
    0x30660: 17,
    0x30010: 2,
}
EXPECTED_REPLICATION_VAS = {
    "God": 0x1089329B8,
    "GameObjectVisual": 0x108935B60,
    "AvailableLevel": 0x1089415B0,
    "DisplayName": 0x108944D10,
    "ActionResources": 0x10894A8C0,
    "Stats": 0x10894ABC0,
    "Classes": 0x10894ABD0,
    "EocLevel": 0x10894ABF0,
    "CombatParticipant": 0x10894C700,
}


def _require_frozen_binaries() -> None:
    missing = [path for path in (OLD_BINARY, NEW_BINARY) if not path.is_file()]
    if missing:
        pytest.skip(
            "frozen BG3 migration binaries are not available: "
            + ", ".join(str(path) for path in missing)
        )


def test_typeid_generation_is_deterministic_for_symbol_records() -> None:
    record = TypeIdSymbol(
        component_name="eoc::HealthComponent",
        context=COMPONENT_CONTEXT,
        preferred_va=0x108942290,
        raw_mangled_symbol=(
            "__ZN2ls6TypeIdIN3eoc15HealthComponentEN3ecs22"
            "ComponentTypeIdContextEE11m_TypeIndexE"
        ),
        build_id=NEW_BUILD,
        symbol_type="D",
    )
    header = generate_header([record], build_id=NEW_BUILD)
    registry = generate_registration_code([record], build_id=NEW_BUILD)

    assert header == generate_header([record], build_id=NEW_BUILD)
    assert registry == generate_registration_code([record], build_id=NEW_BUILD)
    assert record.raw_mangled_symbol in registry
    assert COMPONENT_CONTEXT in registry
    assert NEW_BUILD in header and NEW_BUILD in registry
    assert "component_data_shift" not in header + registry


def test_typeid_frozen_binary_migration_and_committed_outputs() -> None:
    _require_frozen_binaries()

    old_records = extract_component_contexts(OLD_BINARY, OLD_BUILD)
    new_records = extract_component_contexts(NEW_BINARY, NEW_BUILD)
    assert new_records == extract_component_contexts(NEW_BINARY, NEW_BUILD)
    surface = component_surface(new_records)
    report = migration_report(old_records, new_records, OLD_BUILD, NEW_BUILD)
    curated_only = curated_authority(new_records)
    one_frame = one_frame_authority(new_records)
    replication = supported_replication_contexts(
        extract_replication_contexts(NEW_BINARY, NEW_BUILD)
    )

    assert len(component_surface(old_records)) == 1999
    assert len(surface) == 2004
    assert len(report.shared) == 1998
    assert set(report.added) == EXPECTED_ADDED
    assert set(report.removed) == EXPECTED_REMOVED
    assert dict(report.delta_families) == EXPECTED_DELTA_FAMILIES

    assert all(record.raw_mangled_symbol for record in surface)
    assert all(record.context == COMPONENT_CONTEXT for record in surface)
    assert all(record.build_id == NEW_BUILD for record in surface)
    assert len({record.component_name for record in surface}) == 2004
    assert len({record.raw_mangled_symbol for record in surface}) == 2004

    assert {record.component_name for record in curated_only} == {
        "ecl::Character",
        "ecl::Item",
        "eoc::rest::LongRestInScriptPhase",
    }
    assert {record.component_name for record in one_frame} == {
        "esv::TurnStartedEventOneFrameComponent",
        "esv::TurnEndedEventOneFrameComponent",
    }
    assert all(record.context == ONE_FRAME_CONTEXT for record in one_frame)
    assert {record.preferred_va for record in one_frame} == {
        0x108946A18,
        0x108946A28,
    }

    curated_source = (ROOT / "src/entity/component_typeid.c").read_text()
    curated_rows = re.findall(
        r'\{\s*"([^"]+)",\s*(\d+),\s*(true|false),\s*'
        r'"([^"]+)",\s*(true|false)\s*\}',
        curated_source.split("static const TypeIdEntry g_KnownTypeIds[] = {", 1)[1]
        .split("// Sentinel", 1)[0],
    )
    generated_authority = {
        (record.component_name, record.context)
        for record in (*surface, *curated_only, *one_frame)
    }
    assert len(curated_rows) == 163
    assert len({row[0] for row in curated_rows}) == 163
    assert sum(row[4] == "true" for row in curated_rows) == 2
    assert all((row[0], row[3]) in generated_authority for row in curated_rows)

    assert len(replication) == 9
    assert {name: record.preferred_va for name, record in replication} == (
        EXPECTED_REPLICATION_VAS
    )
    assert all(record.context == REPLICATED_CONTEXT for _, record in replication)
    assert all(record.raw_mangled_symbol for _, record in replication)
    assert all(record.build_id == NEW_BUILD for _, record in replication)

    expected_header = generate_header(surface, replication, report, NEW_BUILD)
    expected_registry = generate_registration_code(
        surface, curated_only, one_frame, report, NEW_BUILD
    )
    assert expected_header == generate_header(
        surface, replication, report, NEW_BUILD
    )
    assert expected_registry == generate_registration_code(
        surface, curated_only, one_frame, report, NEW_BUILD
    )
    assert (ROOT / "src/entity/generated_typeids.h").read_text() == expected_header
    assert (
        ROOT / "src/entity/generated_component_registry.c"
    ).read_text() == expected_registry


def test_typeid_owned_runtime_sources_do_not_consume_uniform_shift() -> None:
    sources = [
        ROOT / "src/entity/component_typeid.c",
        ROOT / "src/entity/component_typeid.h",
        ROOT / "src/entity/generated_component_registry.c",
        ROOT / "src/entity/replication_flags.c",
        ROOT / "src/entity/replication_flags.h",
    ]
    combined = "\n".join(path.read_text() for path in sources)
    assert "component_data_shift" not in combined
    for old_va in EXPECTED_REPLICATION_VAS.values():
        assert f"0x{old_va:x}" not in (ROOT / "src/entity/replication_flags.c").read_text()
