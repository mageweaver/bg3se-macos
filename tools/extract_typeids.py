#!/usr/bin/env python3
"""Extract build-specific BG3 component and replication TypeId symbols.

The generated sources deliberately retain one preferred virtual address per
symbol.  They must never be migrated with a segment-wide address delta.
"""

from __future__ import annotations

import argparse
import re
import subprocess
import sys
from collections import Counter, defaultdict
from dataclasses import dataclass
from pathlib import Path
from typing import Iterable, Sequence


DEFAULT_BINARY = (
    "/Users/tomdimino/Library/Application Support/Steam/steamapps/common/"
    "Baldurs Gate 3/Baldur's Gate 3.app/Contents/MacOS/Baldur's Gate 3"
)

COMPONENT_CONTEXT = "ecs::ComponentTypeIdContext"
ONE_FRAME_CONTEXT = "ecs::OneFrameComponentTypeIdContext"
REPLICATED_CONTEXT = "ecs::sync::ReplicatedTypeContext"
SYSTEM_CONTEXT = "ecs::SystemsContext"

# These are real ComponentTypeIdContext types used by the curated overlay, but
# their C++ names predate the *Component naming convention.  They are emitted
# as lookup authority without changing the measured 2,004-component surface.
CURATED_NON_SURFACE_COMPONENTS = (
    "ecl::Character",
    "ecl::Item",
    "eoc::rest::LongRestInScriptPhase",
)

CURATED_ONE_FRAME_COMPONENTS = (
    "esv::TurnStartedEventOneFrameComponent",
    "esv::TurnEndedEventOneFrameComponent",
)

# API spelling -> exact C++ component type.  Addresses are always obtained from
# nm; no preferred VA is encoded here.
SUPPORTED_REPLICATION_COMPONENTS = (
    ("God", "eoc::god::GodComponent"),
    ("GameObjectVisual", "eoc::GameObjectVisualComponent"),
    ("AvailableLevel", "eoc::exp::AvailableLevelComponent"),
    ("DisplayName", "eoc::DisplayNameComponent"),
    ("ActionResources", "eoc::ActionResourcesComponent"),
    ("Stats", "eoc::StatsComponent"),
    ("Classes", "eoc::ClassesComponent"),
    ("EocLevel", "eoc::LevelComponent"),
    ("CombatParticipant", "eoc::combat::ParticipantComponent"),
)


SUPPORTED_SYSTEM_TYPES = (
    ("ClientCharacterIconRender", "ecl::CharacterIconRenderSystem"),
    ("ClientCharacterManager", "ecl::CharacterManager"),
    ("ClientEquipmentVisuals", "ecl::EquipmentVisualsSystem"),
    ("PickingHelper", "ecl::PickingHelperManager"),
    ("ClientVisual", "ecl::VisualSystem"),
    ("ClientEffectHandler", "ecl::effect::HandlerSystem"),
    ("ClientVisualsVisibilityState", "ecl::equipment::VisualsVisibilityStateSystem"),
    ("ServerActionResource", "esv::ActionResourceSystem"),
    ("ServerAi", "esv::AiHelpers"),
    ("ServerBodyType", "esv::BodyTypeSystem"),
    ("ServerBoost", "esv::BoostSystem"),
    ("ServerCapabilities", "esv::CapabilitiesSystem"),
    ("ServerDialog", "esv::DialogSystem"),
    ("ServerDisplayName", "esv::DisplayNameSystem"),
    ("ServerDualWielding", "esv::DualWieldingSystem"),
    ("ServerFalling", "esv::FallingSystem"),
    ("ServerGravity", "esv::GravitySystem"),
    ("ServerLeader", "esv::LeaderSystem"),
    ("ServerPartyTeleport", "esv::PartyTeleportSystem"),
    ("ServerPassive", "esv::PassiveSystem"),
    ("ServerPingRequest", "esv::PingRequestSystem"),
    ("ServerPlatform", "esv::PlatformSystem"),
    ("ServerRoll", "esv::RollSystem"),
    ("ServerSpellCooldown", "esv::SpellCooldownSystem"),
    ("ServerSpell", "esv::SpellSystem"),
    ("ServerStats", "esv::StatsSystem"),
    ("ServerTurnOrder", "esv::TurnOrderSystem"),
    ("ServerVisual", "esv::VisualSystem"),
    ("ServerRating", "esv::approval::RatingSystem"),
    ("ServerAttitude", "esv::attitude::UpdateSystem"),
    ("ServerCombat", "esv::combat::System"),
    ("ServerConcentration", "esv::concentration::ConcentrationSystem"),
    ("ServerExperience", "esv::exp::ExperienceSystem"),
    ("ServerFTBZone", "esv::ftb::ZoneSystem"),
    ("ServerGod", "esv::god::GodSystem"),
    ("ServerHit", "esv::hit::HitSystem"),
    ("ServerInterruptDecision", "esv::interrupt::DecisionSystem"),
    ("ServerInterruptManagement", "esv::interrupt::ManagementSystem"),
    ("ServerInterruptRequests", "esv::interrupt::RequestsSystem"),
    ("ServerInventoryCanPlace", "esv::inventory::CanPlaceSystem"),
    ("ServerInventoryReceivalNotification", "esv::inventory::EntityReceivalNotificationSystem"),
    ("ServerInventoryEquipment", "esv::inventory::EquipmentSystem"),
    ("ServerInventoryInteractionRequest", "esv::inventory::InteractionRequestSystem"),
    ("ServerInventoryInteraction", "esv::inventory::InteractionSystem"),
    ("ServerInventoryLocking", "esv::inventory::LockingSystem"),
    ("ServerMagicPocketsTracking", "esv::inventory::MagicPocketsTrackingSystem"),
    ("ServerInventoryManagement", "esv::inventory::ManagementSystem"),
    ("ServerNewInventoryMember", "esv::inventory::NewInventoryMemberSystem"),
    ("ServerInventoryStack", "esv::inventory::StackSystem"),
    ("ServerTradeBuyback", "esv::inventory::TradeBuybackSystem"),
    ("ServerTreasureGeneration", "esv::inventory::TreasureGenerationSystem"),
    ("ServerParty", "esv::party::PartySystem"),
    ("ServerProgression", "esv::progression::ManagementSystem"),
    ("ServerLongRest", "esv::rest::LongRestSystem"),
    ("ServerShortRest", "esv::rest::ShortRestSystem"),
    ("ServerRestore", "esv::restore::RestoreSystem"),
    ("ServerRollSave", "esv::roll::stream::SaveSystem"),
    ("ServerShapeshift", "esv::shapeshift::System"),
    ("ServerSightViewshed", "esv::sight::ViewshedSystem"),
    ("ServerSpellLearning", "esv::spell::LearningSystem"),
    ("ServerCastRequest", "esv::spell_cast::CastRequestSystem"),
    ("ServerStatusRequest", "esv::status::RequestSystem"),
    ("ServerSummonDespawn", "esv::summon::DespawnSystem"),
    ("ServerSummonSpawn", "esv::summon::SpawnSystem"),
    ("ServerTemplateChange", "esv::templates::ChangeSystem"),
    ("AnimationBlueprint", "ls::AnimationBlueprintSystem"),
    ("AnimationSet", "ls::AnimationSetSystem"),
    ("Effect", "ls::EffectsManager"),
    ("Light", "ls::LightSystem"),
    ("SoundRouting", "ls::SoundRoutingSystem"),
    ("VisualChange", "ls::VisualChangeRequestSystem"),
    ("VisualChanged", "ls::VisualChangedSystem"),
    ("Visual", "ls::VisualSystem"),
)


class ExtractionError(RuntimeError):
    """Raised when symbol extraction is incomplete or ambiguous."""


@dataclass(frozen=True, order=True)
class TypeIdSymbol:
    component_name: str
    context: str
    preferred_va: int
    raw_mangled_symbol: str
    build_id: str
    symbol_type: str


@dataclass(frozen=True)
class MigrationReport:
    baseline_build_id: str
    current_build_id: str
    baseline_count: int
    current_count: int
    shared: tuple[str, ...]
    added: tuple[str, ...]
    removed: tuple[str, ...]
    delta_families: tuple[tuple[int, int], ...]


_NM_LINE = re.compile(
    r"^([0-9A-Fa-f]+)\s+([A-Za-z?])\s+(\S+)\s*$"
)


def infer_build_id(binary_path: str | Path) -> str | None:
    """Infer a dotted BG3 build id from a migration-binary path."""

    for part in reversed(Path(binary_path).parts):
        if re.fullmatch(r"\d+\.\d+\.\d+\.\d+", part):
            return part
    return None


def _run_nm(binary_path: str | Path, *, exported_only: bool) -> str:
    command = ["nm", "-arch", "arm64"]
    if exported_only:
        command.append("-gU")
    command.append(str(binary_path))
    result = subprocess.run(command, capture_output=True, text=True, check=False)
    if result.returncode != 0:
        detail = result.stderr.strip() or result.stdout.strip()
        raise ExtractionError(f"nm failed for {binary_path}: {detail}")
    return result.stdout


def _demangle(raw_symbols: Sequence[str]) -> list[str]:
    if not raw_symbols:
        return []
    result = subprocess.run(
        ["c++filt"],
        input="\n".join(raw_symbols) + "\n",
        capture_output=True,
        text=True,
        check=False,
    )
    if result.returncode != 0:
        raise ExtractionError(f"c++filt failed: {result.stderr.strip()}")
    demangled = result.stdout.splitlines()
    if len(demangled) != len(raw_symbols):
        raise ExtractionError(
            "c++filt returned a different number of symbols "
            f"({len(demangled)} != {len(raw_symbols)})"
        )
    return demangled


def parse_nm_typeids(
    nm_output: str,
    build_id: str,
    contexts: Iterable[str],
) -> list[TypeIdSymbol]:
    """Parse exact TypeId<..., Context>::m_TypeIndex symbols from nm text."""

    requested_contexts = tuple(contexts)
    candidates: list[tuple[int, str, str]] = []
    for line in nm_output.splitlines():
        match = _NM_LINE.match(line)
        if not match:
            continue
        preferred_va = int(match.group(1), 16)
        symbol_type = match.group(2)
        raw_symbol = match.group(3)
        if (
            not raw_symbol.startswith("__ZN2ls6TypeIdI")
            or not raw_symbol.endswith("11m_TypeIndexE")
            or raw_symbol.startswith("__ZGV")
        ):
            continue
        candidates.append((preferred_va, symbol_type, raw_symbol))

    raw_symbols = [candidate[2] for candidate in candidates]
    demangled_symbols = _demangle(raw_symbols)
    records: dict[tuple[str, str], TypeIdSymbol] = {}

    for candidate, demangled in zip(candidates, demangled_symbols, strict=True):
        preferred_va, symbol_type, raw_symbol = candidate
        prefix = "ls::TypeId<"
        context = next(
            (
                requested
                for requested in requested_contexts
                if demangled.endswith(f", {requested}>::m_TypeIndex")
            ),
            None,
        )
        if context is None:
            continue
        suffix = f", {context}>::m_TypeIndex"
        if not demangled.startswith(prefix) or not demangled.endswith(suffix):
            raise ExtractionError(
                f"unexpected demangled TypeId symbol: {raw_symbol} -> {demangled}"
            )
        component_name = demangled[len(prefix) : -len(suffix)]
        record = TypeIdSymbol(
            component_name=component_name,
            context=context,
            preferred_va=preferred_va,
            raw_mangled_symbol=raw_symbol,
            build_id=build_id,
            symbol_type=symbol_type,
        )
        key = (context, component_name)
        previous = records.get(key)
        if previous is not None and previous != record:
            raise ExtractionError(
                f"ambiguous TypeId symbol for {component_name} ({context}): "
                f"0x{previous.preferred_va:x} and 0x{preferred_va:x}"
            )
        records[key] = record

    return sorted(records.values())


def extract_component_contexts(
    binary_path: str | Path, build_id: str
) -> list[TypeIdSymbol]:
    """Extract exported ordinary and one-frame Component TypeId contexts."""

    return parse_nm_typeids(
        _run_nm(binary_path, exported_only=True),
        build_id,
        (COMPONENT_CONTEXT, ONE_FRAME_CONTEXT),
    )


def extract_replication_contexts(
    binary_path: str | Path, build_id: str
) -> list[TypeIdSymbol]:
    """Extract ReplicatedTypeContext globals using plain nm (locals included)."""

    return parse_nm_typeids(
        _run_nm(binary_path, exported_only=False),
        build_id,
        (REPLICATED_CONTEXT,),
    )


def extract_system_contexts(
    binary_path: str | Path, build_id: str
) -> list[TypeIdSymbol]:
    """Extract SystemsContext globals using plain nm (locals included)."""

    return parse_nm_typeids(
        _run_nm(binary_path, exported_only=False),
        build_id,
        (SYSTEM_CONTEXT,),
    )


def component_surface(records: Iterable[TypeIdSymbol]) -> list[TypeIdSymbol]:
    """Return the public ordinary-component surface measured by this project."""

    return sorted(
        record
        for record in records
        if record.context == COMPONENT_CONTEXT
        and "Component" in record.component_name
    )


def _require_named_records(
    records: Iterable[TypeIdSymbol],
    names: Iterable[str],
    context: str,
) -> list[TypeIdSymbol]:
    by_name = {
        record.component_name: record
        for record in records
        if record.context == context
    }
    missing = sorted(set(names) - set(by_name))
    if missing:
        raise ExtractionError(
            f"missing {context} symbols: {', '.join(missing)}"
        )
    return [by_name[name] for name in names]


def curated_authority(records: Iterable[TypeIdSymbol]) -> list[TypeIdSymbol]:
    return _require_named_records(
        records, CURATED_NON_SURFACE_COMPONENTS, COMPONENT_CONTEXT
    )


def one_frame_authority(records: Iterable[TypeIdSymbol]) -> list[TypeIdSymbol]:
    return _require_named_records(
        records, CURATED_ONE_FRAME_COMPONENTS, ONE_FRAME_CONTEXT
    )


def supported_replication_contexts(
    records: Iterable[TypeIdSymbol],
) -> list[tuple[str, TypeIdSymbol]]:
    required_names = [component for _, component in SUPPORTED_REPLICATION_COMPONENTS]
    selected = _require_named_records(records, required_names, REPLICATED_CONTEXT)
    by_name = {record.component_name: record for record in selected}
    return [
        (api_name, by_name[component_name])
        for api_name, component_name in SUPPORTED_REPLICATION_COMPONENTS
    ]


def supported_system_contexts(
    records: Iterable[TypeIdSymbol],
) -> list[tuple[str, TypeIdSymbol]]:
    required_names = [engine_class for _, engine_class in SUPPORTED_SYSTEM_TYPES]
    selected = _require_named_records(records, required_names, SYSTEM_CONTEXT)
    by_name = {record.component_name: record for record in selected}
    return [
        (public_name, by_name[engine_class])
        for public_name, engine_class in SUPPORTED_SYSTEM_TYPES
    ]


def extract_typeids(binary_path: str) -> dict[str, int]:
    """Compatibility wrapper returning the 2,004-style component surface."""

    build_id = infer_build_id(binary_path) or "unknown"
    return {
        record.component_name: record.preferred_va
        for record in component_surface(
            extract_component_contexts(binary_path, build_id)
        )
    }


def migration_report(
    baseline: Iterable[TypeIdSymbol],
    current: Iterable[TypeIdSymbol],
    baseline_build_id: str,
    current_build_id: str,
) -> MigrationReport:
    baseline_by_name = {
        record.component_name: record for record in component_surface(baseline)
    }
    current_by_name = {
        record.component_name: record for record in component_surface(current)
    }
    shared = tuple(sorted(set(baseline_by_name) & set(current_by_name)))
    deltas = Counter(
        current_by_name[name].preferred_va - baseline_by_name[name].preferred_va
        for name in shared
    )
    return MigrationReport(
        baseline_build_id=baseline_build_id,
        current_build_id=current_build_id,
        baseline_count=len(baseline_by_name),
        current_count=len(current_by_name),
        shared=shared,
        added=tuple(sorted(set(current_by_name) - set(baseline_by_name))),
        removed=tuple(sorted(set(baseline_by_name) - set(current_by_name))),
        delta_families=tuple(
            sorted(deltas.items(), key=lambda item: (-item[1], item[0]))
        ),
    )


def _categories(records: Iterable[TypeIdSymbol]) -> dict[str, list[TypeIdSymbol]]:
    categories: dict[str, list[TypeIdSymbol]] = defaultdict(list)
    for record in sorted(records):
        namespace = record.component_name.split("::", 1)[0]
        categories[namespace].append(record)
    return categories


def c_identifier(component_name: str) -> str:
    return re.sub(r"[^A-Za-z0-9_]", "_", component_name.replace("::", "_")).upper()


def _c_string(value: str) -> str:
    return value.replace("\\", "\\\\").replace('"', '\\"')


def _report_comment(report: MigrationReport | None) -> list[str]:
    if report is None:
        return [" * Migration report: no baseline supplied"]
    lines = [
        " * Migration report:",
        f" *   Baseline {report.baseline_build_id}: {report.baseline_count}",
        f" *   Current {report.current_build_id}: {report.current_count}",
        f" *   Shared: {len(report.shared)}",
        f" *   Added ({len(report.added)}):",
    ]
    lines.extend(f" *     + {name}" for name in report.added)
    lines.append(f" *   Removed ({len(report.removed)}):")
    lines.extend(f" *     - {name}" for name in report.removed)
    lines.append(" *   Shared address-delta families:")
    lines.extend(
        f" *     0x{delta:x}: {count}" for delta, count in report.delta_families
    )
    return lines


def generate_header(
    surface: Sequence[TypeIdSymbol] | dict[str, int],
    replication: Sequence[tuple[str, TypeIdSymbol]] = (),
    system_types: Sequence[tuple[str, TypeIdSymbol]] = (),
    report: MigrationReport | None = None,
    build_id: str | None = None,
) -> str:
    """Generate deterministic preferred-VA macros and replication records."""

    # Preserve the old public helper shape for repository-local analysis tools.
    if isinstance(surface, dict):
        resolved_build = build_id or "unknown"
        surface = [
            TypeIdSymbol(name, COMPONENT_CONTEXT, va, "", resolved_build, "D")
            for name, va in sorted(surface.items())
        ]
    if not surface:
        raise ExtractionError("cannot generate an empty component header")
    resolved_build = build_id or surface[0].build_id
    categories = _categories(surface)

    lines = [
        "/**",
        " * generated_typeids.h - Auto-generated per-symbol TypeId authority",
        " *",
        " * Generated by tools/extract_typeids.py; do not edit manually.",
        f" * Build identity: {resolved_build}",
        f" * Total components: {len(surface)}",
        " * Every VA below was resolved from its own exact mangled symbol.",
    ]
    lines.extend(_report_comment(report))
    lines.extend(
        [
            " */",
            "",
            "#ifndef GENERATED_TYPEIDS_H",
            "#define GENERATED_TYPEIDS_H",
            "",
            "#include <stdint.h>",
            "",
            f'#define GENERATED_TYPEIDS_BUILD_ID "{_c_string(resolved_build)}"',
            f"#define GENERATED_TYPEIDS_COMPONENT_COUNT {len(surface)}",
            f"#define GENERATED_REPLICATED_TYPE_CONTEXT_COUNT {len(replication)}",
            f"#define GENERATED_SYSTEM_TYPEID_COUNT {len(system_types)}",
            "",
            "// === Statistics ===",
            f"// Total components: {len(surface)}",
        ]
    )
    for namespace, records in sorted(categories.items()):
        lines.append(f"// {namespace}:: namespace: {len(records)} components")
    lines.append("")

    category_order = sorted(categories.items(), key=lambda item: (-len(item[1]), item[0]))
    for namespace, records in category_order:
        lines.extend(
            [
                "// ============================================================================",
                f"// {namespace.upper()}:: NAMESPACE ({len(records)} components)",
                "// ============================================================================",
                "",
            ]
        )
        groups: dict[str, list[TypeIdSymbol]] = defaultdict(list)
        for record in records:
            parts = record.component_name.split("::")
            group = "::".join(parts[:2]) if len(parts) > 2 else namespace
            groups[group].append(record)
        for group, group_records in sorted(groups.items()):
            if group != namespace:
                lines.append(f"// --- {group} ({len(group_records)} components) ---")
            for record in sorted(group_records):
                lines.append(
                    f"#define TYPEID_{c_identifier(record.component_name)} "
                    f"0x{record.preferred_va:x}ULL"
                )
            lines.append("")

    lines.append("/* Generated ReplicatedTypeContext table rows. */")
    if replication:
        lines.append("#define GENERATED_REPLICATED_TYPE_CONTEXT_ENTRIES(X) \\")
        for index, (api_name, record) in enumerate(replication):
            continuation = " \\" if index + 1 < len(replication) else ""
            lines.append(
                f'    X("{_c_string(api_name)}", '
                f'"{_c_string(record.component_name)}", '
                f'"{_c_string(record.raw_mangled_symbol)}", '
                f'"{_c_string(record.context)}", '
                f'"{_c_string(record.build_id)}", '
                f"0x{record.preferred_va:x}ULL){continuation}"
            )
    else:
        lines.append("#define GENERATED_REPLICATED_TYPE_CONTEXT_ENTRIES(X)")
    lines.append("")
    lines.append("/* Generated SystemsContext table rows for ecs_system_update.c. */")
    if system_types:
        lines.append("#define GENERATED_SYSTEM_TYPEID_ENTRIES(X) \\")
        for index, (public_name, record) in enumerate(system_types):
            continuation = " \\" if index + 1 < len(system_types) else ""
            lines.append(
                f'    X("{_c_string(public_name)}", '
                f'"{_c_string(record.component_name)}", '
                f"0x{record.preferred_va:x}ULL){continuation}"
            )
    else:
        lines.append("#define GENERATED_SYSTEM_TYPEID_ENTRIES(X)")
    lines.extend(["", "#endif // GENERATED_TYPEIDS_H", ""])
    return "\n".join(lines)


def _entry_initializer(record: TypeIdSymbol) -> str:
    return (
        f'    {{ "{_c_string(record.component_name)}", '
        f'"{_c_string(record.raw_mangled_symbol)}", '
        f'"{_c_string(record.context)}", '
        f'"{_c_string(record.build_id)}", '
        f"0x{record.preferred_va:x}ULL }},"
    )


def generate_registration_code(
    surface: Sequence[TypeIdSymbol] | dict[str, int],
    curated_only: Sequence[TypeIdSymbol] = (),
    one_frame: Sequence[TypeIdSymbol] = (),
    report: MigrationReport | None = None,
    build_id: str | None = None,
) -> str:
    """Generate the runtime component table with full symbol provenance."""

    if isinstance(surface, dict):
        resolved_build = build_id or "unknown"
        surface = [
            TypeIdSymbol(name, COMPONENT_CONTEXT, va, "", resolved_build, "D")
            for name, va in sorted(surface.items())
        ]
    if not surface:
        raise ExtractionError("cannot generate an empty component registry")
    resolved_build = build_id or surface[0].build_id

    lines = [
        "/**",
        " * generated_component_registry.c - Auto-generated TypeId records",
        " *",
        " * Generated by tools/extract_typeids.py; do not edit manually.",
        f" * Build identity: {resolved_build}",
        f" * Total components: {len(surface)}",
    ]
    lines.extend(_report_comment(report))
    lines.extend(
        [
            " */",
            "",
            '#include "component_registry.h"',
            '#include "component_typeid.h"',
            '#include "generated_typeids.h"',
            '#include "../core/logging.h"',
            "",
            "#include <stddef.h>",
            "#include <string.h>",
            "",
            "typedef struct {",
            "    const char *name;",
            "    const char *mangled_symbol;",
            "    const char *context;",
            "    const char *build_id;",
            "    uint64_t preferred_va;",
            "} GeneratedComponentEntry;",
            "",
            "static const GeneratedComponentEntry g_GeneratedComponents[] = {",
        ]
    )
    lines.extend(_entry_initializer(record) for record in sorted(surface))
    lines.extend(["};", ""])

    lines.append("static const GeneratedComponentEntry g_CuratedOnlyAuthority[] = {")
    lines.extend(_entry_initializer(record) for record in curated_only)
    lines.extend(["};", ""])

    lines.append("static const GeneratedComponentEntry g_OneFrameAuthority[] = {")
    lines.extend(_entry_initializer(record) for record in one_frame)
    lines.extend(
        [
            "};",
            "",
            "static const GeneratedComponentEntry *find_generated_entry(",
            "    const GeneratedComponentEntry *entries, size_t count,",
            "    const char *name, const char *context) {",
            "    for (size_t i = 0; i < count; i++) {",
            "        if (strcmp(entries[i].name, name) == 0 &&",
            "            strcmp(entries[i].context, context) == 0) {",
            "            return &entries[i];",
            "        }",
            "    }",
            "    return NULL;",
            "}",
            "",
            "bool component_typeid_generated_lookup(const char *name,",
            "                                       const char *context,",
            "                                       uint64_t *out_va) {",
            "    if (!name || !context || !out_va) return false;",
            "",
            "    const GeneratedComponentEntry *entry = find_generated_entry(",
            "        g_GeneratedComponents, GENERATED_TYPEIDS_COMPONENT_COUNT,",
            "        name, context);",
            "    if (!entry) {",
            "        entry = find_generated_entry(",
            "            g_CuratedOnlyAuthority,",
            "            sizeof(g_CuratedOnlyAuthority) / sizeof(g_CuratedOnlyAuthority[0]),",
            "            name, context);",
            "    }",
            "    if (!entry) {",
            "        entry = find_generated_entry(",
            "            g_OneFrameAuthority,",
            "            sizeof(g_OneFrameAuthority) / sizeof(g_OneFrameAuthority[0]),",
            "            name, context);",
            "    }",
            "    if (!entry || strcmp(entry->build_id, GENERATED_TYPEIDS_BUILD_ID) != 0) {",
            "        return false;",
            "    }",
            "    *out_va = entry->preferred_va;",
            "    return true;",
            "}",
            "",
            "void component_registry_register_all_generated(void) {",
            "    LOG_ENTITY_INFO(\"Registering %d generated components...\",",
            "                    GENERATED_TYPEIDS_COMPONENT_COUNT);",
            "    for (size_t i = 0; i < GENERATED_TYPEIDS_COMPONENT_COUNT; i++) {",
            "        const GeneratedComponentEntry *entry = &g_GeneratedComponents[i];",
            "        if (component_typeid_is_curated(entry->name)) continue;",
            "        component_registry_register(entry->name, COMPONENT_INDEX_UNDEFINED,",
            "                                    0, false);",
            "    }",
            "    LOG_ENTITY_INFO(\"Registered generated TypeId surface\");",
            "}",
            "",
            "int component_registry_generated_count(void) {",
            "    return GENERATED_TYPEIDS_COMPONENT_COUNT;",
            "}",
            "",
            "int component_typeid_discover_all_generated(void) {",
            "    int discovered = 0;",
            "    for (size_t i = 0; i < GENERATED_TYPEIDS_COMPONENT_COUNT; i++) {",
            "        const GeneratedComponentEntry *entry = &g_GeneratedComponents[i];",
            "        if (component_typeid_is_curated(entry->name)) continue;",
            "        uint16_t type_index = 0;",
            "        if (component_typeid_read(entry->preferred_va, &type_index) &&",
            "            component_registry_register(entry->name, type_index, 0, false)) {",
            "            discovered++;",
            "        }",
            "    }",
            "    LOG_ENTITY_INFO(\"Discovered %d generated component TypeIds\", discovered);",
            "    return discovered;",
            "}",
            "",
        ]
    )
    return "\n".join(lines)


def format_report(report: MigrationReport) -> str:
    lines = [
        f"TypeId migration {report.baseline_build_id} -> {report.current_build_id}",
        f"baseline={report.baseline_count} current={report.current_count} "
        f"shared={len(report.shared)} added={len(report.added)} removed={len(report.removed)}",
        "added:",
    ]
    lines.extend(f"  + {name}" for name in report.added)
    lines.append("removed:")
    lines.extend(f"  - {name}" for name in report.removed)
    lines.append("delta families:")
    lines.extend(
        f"  0x{delta:x}: {count}" for delta, count in report.delta_families
    )
    return "\n".join(lines)


def _write_output(path: str | None, content: str) -> None:
    if path is None:
        return
    Path(path).write_text(content, encoding="utf-8")


def main(argv: Sequence[str] | None = None) -> int:
    parser = argparse.ArgumentParser(
        description="Extract per-symbol component TypeId addresses from BG3"
    )
    parser.add_argument("binary", nargs="?", default=DEFAULT_BINARY)
    parser.add_argument("--build-id", help="Build identity embedded in every record")
    parser.add_argument("--baseline-binary")
    parser.add_argument("--baseline-build-id")
    parser.add_argument("--registry", action="store_true")
    parser.add_argument("--header-out")
    parser.add_argument("--registry-out")
    args = parser.parse_args(argv)

    binary = Path(args.binary)
    if not binary.exists():
        parser.error(f"binary not found: {binary}")
    build_id = args.build_id or infer_build_id(binary)
    if not build_id:
        parser.error("--build-id is required when it cannot be inferred from the path")

    try:
        component_records = extract_component_contexts(binary, build_id)
        surface = component_surface(component_records)
        curated_only = curated_authority(component_records)
        one_frame = one_frame_authority(component_records)
        replication = supported_replication_contexts(
            extract_replication_contexts(binary, build_id)
        )
        system_types = supported_system_contexts(
            extract_system_contexts(binary, build_id)
        )

        report = None
        if args.baseline_binary:
            baseline_binary = Path(args.baseline_binary)
            if not baseline_binary.exists():
                parser.error(f"baseline binary not found: {baseline_binary}")
            baseline_build_id = (
                args.baseline_build_id or infer_build_id(baseline_binary)
            )
            if not baseline_build_id:
                parser.error(
                    "--baseline-build-id is required when it cannot be inferred"
                )
            baseline_records = extract_component_contexts(
                baseline_binary, baseline_build_id
            )
            report = migration_report(
                baseline_records, component_records, baseline_build_id, build_id
            )
            print(format_report(report), file=sys.stderr)

        header = generate_header(surface, replication, system_types, report, build_id)
        registry = generate_registration_code(
            surface, curated_only, one_frame, report, build_id
        )
        _write_output(args.header_out, header)
        _write_output(args.registry_out, registry)

        if not args.header_out and not args.registry_out:
            print(registry if args.registry else header, end="")
        elif args.registry and not args.registry_out:
            print(registry, end="")
    except ExtractionError as error:
        print(f"error: {error}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
