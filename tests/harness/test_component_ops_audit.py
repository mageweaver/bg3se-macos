"""Offline contracts for the build-7209685 ComponentOps unlock.

The native mutation path is intentionally instruction-recipe-specific. These
tests keep later cleanups from regressing to the refuted EntityWorld+0x368
pointer-field estimate, a Windows vtable slot, or generic RemoveComponent.
"""

import re
from pathlib import Path


REPO_ROOT = Path(__file__).resolve().parents[2]
ENTITY_SYSTEM_C = REPO_ROOT / "src/entity/entity_system.c"
PROTOTYPE_MANAGERS_C = REPO_ROOT / "src/stats/prototype_managers.c"
LUA_EXT_C = REPO_ROOT / "src/lua/lua_ext.c"
STATS_MD = REPO_ROOT / "ghidra/offsets/STATS.md"


def _function_body(source: str, name: str) -> str:
    match = re.search(rf"\b{name}\s*\([^)]*\)\s*\{{", source)
    assert match, f"{name} not found"
    start = match.end() - 1
    depth = 0
    for index in range(start, len(source)):
        if source[index] == "{":
            depth += 1
        elif source[index] == "}":
            depth -= 1
            if depth == 0:
                return source[start:index + 1]
    raise AssertionError(f"{name} body is unterminated")


def _define(source: str, name: str) -> str:
    match = re.search(rf"^#define\s+{name}\s+(\S+)", source, re.MULTILINE)
    assert match, f"{name} not found"
    return match.group(1)


def test_create_component_uses_verified_embedded_registry_recipe():
    source = ENTITY_SYSTEM_C.read_text()
    body = _function_body(source, "lua_entity_create_component")

    # 7398727: Wave 2C re-verified registry base +0x390, stride 8, vptr
    # slot 5, and the three-argument ABI instruction-identical, with two
    # concrete vtables corroborating (ghidra/offsets/ABI_REVIEW_7398727.md
    # §2); the matching TypeIds landed in the Wave 1B regeneration. Any
    # future change to this pin needs equivalent per-build evidence.
    assert _define(source, "COMPONENT_OPS_VERIFIED_BUILD") == '"4.1.1.7398727"'
    assert int(_define(
        source, "ENTITYWORLD_COMPONENT_OPS_REGISTRY_OFFSET"), 16) == 0x390
    assert int(_define(
        source, "COMPONENT_OPS_REGISTRY_BUFFER_OFFSET"), 16) == 0
    assert int(_define(
        source, "COMPONENT_OPS_REGISTRY_CAPACITY_OFFSET"), 16) == 0x8
    assert int(_define(
        source, "COMPONENT_OPS_REGISTRY_SIZE_OFFSET"), 16) == 0xC
    assert int(_define(
        source, "COMPONENT_OPS_ADD_IMMEDIATE_VPTR_OFFSET"), 16) == 0x28
    assert "0x368" not in source

    assert "component_registry_lookup(component)" in body
    assert "info->index & 0x7fffU" in body
    assert "safe_memory_read_i32(" in body
    assert body.count("safe_memory_read_pointer(") >= 4
    assert "index >= (uint32_t)size" in body
    assert "((AddImmediateDefaultComponentFn)add_immediate)(" in body
    assert "ops, (uint64_t)ud->handle, 0" in body
    assert "lua_pushboolean(L, 1)" in body


def test_create_component_guards_precede_native_dispatch():
    source = ENTITY_SYSTEM_C.read_text()
    body = _function_body(source, "lua_entity_create_component")
    dispatch = body.index("((AddImmediateDefaultComponentFn)add_immediate)(")

    for guard in (
        "strcmp(offsets->version, COMPONENT_OPS_VERIFIED_BUILD)",
        "component_registry_lookup(component)",
        "info->index == COMPONENT_INDEX_UNDEFINED",
        "index >= (uint32_t)size",
        "ComponentOps registry buffer is unavailable",
        "ComponentOps registry entry is null",
        "ComponentOps AddImmediateDefaultComponent slot is null",
    ):
        assert guard in body
        assert body.index(guard) < dispatch


def test_remove_component_dispatches_only_exact_specializations():
    """RemoveComponent was undeferred on 2026-08-20 via a generated per-build
    dispatch table (src/entity/generated_remove_component.h).

    The original hazard was that macOS emits 734 type-specialized removers with
    no generic runtime-TypeId entry point, so "calling a specialization for a
    different type would remove the wrong component". This test pins the
    property that makes that impossible: dispatch is by exact name match only,
    and anything not in the table fails closed.
    """
    source = ENTITY_SYSTEM_C.read_text()
    body = _function_body(source, "lua_entity_remove_component")
    resolver = _function_body(source, "remove_component_fn_for")

    # Exact-name dispatch, never index- or guess-based.
    assert "strcmp(component_name, (name)) == 0" in resolver
    assert "GENERATED_REMOVE_COMPONENT_ENTRIES" in resolver

    # Gated on the audited build, and slide-corrected rather than raw VAs.
    assert "version_detect_matches()" in resolver
    assert "version_detect_get_binary_base()" in resolver
    assert "0x100000000" in resolver

    # Unknown / uncovered component names must fail closed.
    assert "if (!fn)" in body
    assert "lua_pushboolean(L, 0)" in body

    # The cache pointer comes from the recorded EntityWorld offset, guarded.
    assert "ENTITYWORLD_CACHE_OFFSET" in body
    assert "safe_memory_read_pointer" in body


def test_passive_singleton_and_prototype_sync_claims_match_re_report():
    source = PROTOTYPE_MANAGERS_C.read_text()
    passive = _function_body(source, "sync_passive_prototype")
    interrupt = _function_body(source, "sync_interrupt_prototype")

    # The passives singleton moved from a hardcoded define into the
    # per-version offset table (Wave 2 lead, 2026-08-04). The resolution
    # must go through the table field, and both verified rows must carry
    # the nm-derived slot (7209685: 0x089bc228; 7398727: 0x089ec8c8).
    assert "table_slot_to_runtime(slots->passives_ptr)" in source
    assert "OFFSET_PASSIVE_PROTOTYPE_MANAGER_PTR" not in source
    offset_table = (REPO_ROOT / "src/core/offset_table.c").read_text()
    assert re.search(r"\.passives_ptr\s*=\s*0x089bc228", offset_table)
    assert re.search(r"\.passives_ptr\s*=\s*0x089ec8c8", offset_table)
    assert "74 ADRP+LDR sites" in source
    assert "NO top-level vptr" in passive
    assert "NO" in interrupt
    assert "top-level vptr" in interrupt
    assert "return false;" in passive
    assert "return false;" in interrupt

    for path in (PROTOTYPE_MANAGERS_C, STATS_MD):
        assert "108aeccd8" not in path.read_text(), (
            f"stale passive singleton claim remains in {path}")


def test_tier2_entity_mutation_contracts_cover_guards_and_deferral():
    source = LUA_EXT_C.read_text()

    assert "Parity.Entity.CreateComponent" in source
    assert "e:CreateComponent('__BG3SE_InvalidComponent__')" in source
    assert (
        "invalid CreateComponent type must return false without a native call"
        in source
    )
    assert "Parity.Entity.RemoveComponent" in source
    assert "e:RemoveComponent('__BG3SE_RemoveComponentDeferred__')" in source
    assert "deferred RemoveComponent must return false" in source
