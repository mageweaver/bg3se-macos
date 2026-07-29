"""Audit hardcoded offsets and the per-version offset table against the
installed game binary.

Dobby code patches and singleton reads aimed at the main BG3 binary use
hardcoded addresses derived from one specific game build. The version-detect
sentinels only validate three *data* addresses, so a game update can silently
invalidate *code* offsets — a stale ``OFFSET_GET_CLASS`` once landed in the
middle of ``gui::HotbarSystem::Update`` and crashed every session ~30s after
save load (docs/bugs/codex-debugger-nohooks-2026-07-28.md).

Three audit layers, all skipped when the game binary is absent (CI):

1. ``AUDITED_OFFSETS`` — #defines parsed out of C sources, resolved via nm.
2. The 4.1.1.7209685 row of ``src/core/offset_table.c`` — every field except
   the documented exclusions must resolve to its claimed symbol.
   Exclusions: ``staticdata_mstate_ptr`` is a ``__DATA_CONST,__got`` slot
   (validated GOT-aware via ``otool -Iv`` instead — nm cannot see it);
   ``global_switches_ptr`` is an anonymous __common slot with no symbol
   (verified 2026-07-29 by disassembly: 916 refs across 593 functions incl.
   App::CreateGlobalSwitches; runtime read-back guard in global_switches.c);
   ``component_data_shift`` is a delta, not an address.
3. The two-column function remap in ``offset_table.c`` — every nonzero
   7209685 address must have a symbol at that VA, and no address may appear
   in more than one row (either-column matching must be collision-free).

FUNCTOR GATE: the eleven functor-hook addresses in
``src/stats/functor_types.h`` ARE nm-visible local symbols on 7209685 (the
old "stripped locals" premise was wrong), but the 7209685 Interrupt variant
gained a leading ``ecs::EntityWorld&`` parameter — address validity is not
ABI validity. The family therefore stays gated at install time on
``FUNCTOR_ADDRS_VERIFIED_BUILD`` — an exact match against the build its
addresses AND wrapper ABIs were verified on, independent of
``BG3_KNOWN_VERSION`` (see test_functor_gate_is_independent below).
"""

import re
import shutil
import subprocess
from pathlib import Path

import pytest

from bg3se_harness.config import BG3_EXEC

REPO_ROOT = Path(__file__).resolve().parents[2]
OFFSET_TABLE_C = REPO_ROOT / "src/core/offset_table.c"

# (source file, #define name, expected demangled symbol substring,
#  base-relative? — offsets below 0x100000000 are relative to the image base)
AUDITED_OFFSETS = [
    ("src/network/protocol.h", "ADDR_GETMESSAGE",
     "net::MessageFactory::GetFreeMessage(int)", False),
    ("src/game/video_skip.c", "VA_BINK_LOAD_VIDEO",
     "bik::BinkManager::LoadVideo", False),
    ("src/stats/stats_manager.c", "OFFSET_RPGSTATS_M_PTR",
     "RPGStats::m_ptr", False),
    ("src/strings/fixed_string.c", "OFFSET_RPGSTATS",
     "RPGStats::m_ptr", False),
    ("src/strings/fixed_string.c", "GHIDRA_FIXEDSTRING_CREATE",
     "ls::FixedString::Create(char const*, int)", False),
    ("src/entity/entity_system.c", "OFFSET_EOCSERVER_SINGLETON_PTR",
     "esv::EocServer::m_ptr", False),
    ("src/entity/entity_system.c", "OFFSET_EOCCLIENT_SINGLETON_PTR",
     "ecl::EocClient::m_ptr", False),
    ("src/entity/entity_system.c", "OFFSET_LEGACY_IS_IN_COMBAT",
     "eoc::CombatHelpers::LEGACY_IsInCombat", False),
    ("src/entity/entity_system.c", "OFFSET_LEGACY_GET_COMBAT_FROM_GUID",
     "eoc::CombatHelpers::LEGACY_GetCombatFromGuid", False),
    ("src/entity/entity_storage.h", "ADDR_STORAGE_CONTAINER_TRYGET",
     "ecs::EntityStorageContainer::TryGet", False),
    ("src/stats/prototype_managers.c", "OFFSET_BOOST_PROTOTYPE_MANAGER_PTR",
     "eoc::BoostPrototypeManager::m_ptr", False),
    ("src/stats/prototype_managers.c", "OFFSET_INTERRUPT_PROTOTYPE_MANAGER_PTR",
     "eoc::InterruptPrototypeManager::m_ptr", False),
    ("src/stats/prototype_managers.c", "OFFSET_SPELL_PROTOTYPE_MANAGER_PTR",
     "eoc::SpellPrototypeManager::m_ptr", False),
    ("src/stats/prototype_managers.c", "OFFSET_STATUS_PROTOTYPE_MANAGER_PTR",
     "eoc::StatusPrototypeManager::m_ptr", False),
    ("src/stats/prototype_managers.c", "OFFSET_SPELL_PROTOTYPE_INIT",
     "eoc::SpellPrototype::Init", False),
    ("src/game/focus_hack.c", "BASEAPP_S_APPINSTANCE_VA",
     "BaseApp::s_AppInstance", False),
    ("src/resource/resource_manager.c", "OFFSET_RESOURCEMANAGER_PTR",
     "ls::ResourceManager::m_ptr", True),
    ("src/resource/resource_manager.c", "OFFSET_GETRESOURCE_FUNC",
     "ls::ResourceContainer::GetResource", True),
    ("src/localization/localization.c", "LOCA_REPO_OFFSET",
     "ls::TranslatedStringRepository::m_ptr", True),
    ("src/localization/localization.c", "LOCA_TRYGET_OFFSET",
     "ls::TranslatedStringRepository::TryGet", True),
    ("src/localization/localization.c", "LOCA_FIXEDSTRING_CREATE",
     "ls::FixedString::Create(char const*, int)", True),
    ("src/localization/localization.c", "LOCA_ADDTRANSLATEDSTRING",
     "ls::TranslatedStringRepository::AddTranslatedString", True),
    ("src/audio/audio_manager.c", "OFFSET_STDSTRING_CTOR",
     "ls::STDString::STDString(char const*)", True),
]

# Expected symbol per 7209685-row field in offset_table.c.
# None = documented exclusion (see module docstring).
TABLE_FIELD_SYMBOLS = {
    "eocserver_ptr":           "esv::EocServer::m_ptr",
    "eocclient_ptr":           "ecl::EocClient::m_ptr",
    "spell_proto_mgr_ptr":     "eoc::SpellPrototypeManager::m_ptr",
    "rpgstats_ptr":            "RPGStats::m_ptr",
    "resource_mgr_ptr":        "ls::ResourceManager::m_ptr",
    "level_mgr_ptr":           "esv::LevelManager::m_ptr",
    "global_template_mgr_ptr": "ls::GlobalTemplateManager::m_ptr",
    "cache_template_mgr_ptr":  "esv::CacheTemplateManager::m_ptr",
    "level_cache_mgr_ptr":     "esv::Level::s_CacheTemplateManager",
    "staticdata_mstate_ptr":   None,  # __got slot — see test_mstate_got_slot
    "gst_ptr":                 "ls::gst::s_Instance",
    "global_switches_ptr":     None,  # anonymous __common slot, no symbol
    "fn_feat_getfeats":        "eoc::FeatManager::GetFeats",
    "fn_getallfeats":          "eoc::character_creation::GetAllFeats",
    "fn_get_background":       "ImmutableDataHeadmaster::Get<eoc::BackgroundManager>",
    "fn_get_origin":           "ImmutableDataHeadmaster::Get<eoc::OriginManager>",
    "fn_get_class":            "ImmutableDataHeadmaster::Get<eoc::ClassDescriptions>",
    "fn_get_progression":      "ImmutableDataHeadmaster::Get<eoc::ProgressionManager>",
    "fn_get_actionresource":   "ImmutableDataHeadmaster::Get<eoc::ActionResourceTypes>",
    "fn_get_template_raw":     "ls::GlobalTemplateManager::GetTemplateRaw",
    "fn_cache_template":       "ls::CacheTemplateManagerBase::CacheTemplate",
    "fn_try_get_uuid_mapping": "ToHandleMappingComponent",
    "fn_storage_tryget":       "ecs::EntityStorageContainer::TryGet",
    "fn_spell_proto_init":     "eoc::SpellPrototype::Init",
    "component_data_shift":    None,  # delta, not an address
    "osiris_interface_ptr":    None,  # no symbol — disasm-audited, see
                                      # test_osiris_interface_slot_matches_disasm
}

MSTATE_GOT_SYMBOL = "__ZN2ls11TypeContextINS_23ImmutableDataHeadmasterEE7m_StateE"

IMAGE_BASE = 0x100000000


def _parse_define(source: Path, name: str) -> int:
    text = source.read_text()
    m = re.search(rf"#define\s+{name}\s+(0x[0-9a-fA-F]+)", text)
    assert m, f"{name} not found in {source}"
    return int(m.group(1), 16)


def _parse_table_row(version: str) -> dict[str, int]:
    """Parse one VersionOffsets struct literal out of offset_table.c."""
    text = OFFSET_TABLE_C.read_text()
    block = re.search(
        rf'\.version\s*=\s*"{re.escape(version)}",(.*?)\n    \}},',
        text, re.DOTALL,
    )
    assert block, f"no {version} row in offset_table.c"
    fields = {}
    for m in re.finditer(r"\.(\w+)\s*=\s*(-?0x[0-9a-fA-F]+|\d+)", block.group(1)):
        fields[m.group(1)] = int(m.group(2), 16 if "0x" in m.group(2) else 10)
    return fields


def _parse_remap_rows() -> list[tuple[int, int]]:
    """Parse the two-column g_fn_remap table out of offset_table.c."""
    text = OFFSET_TABLE_C.read_text()
    block = re.search(
        r"g_fn_remap\[\]\s*=\s*\{(.*?)\n\};", text, re.DOTALL)
    assert block, "g_fn_remap not found in offset_table.c"
    rows = []
    for m in re.finditer(
            r"\{\s*(0x[0-9a-fA-F]+)\s*,\s*(0x[0-9a-fA-F]+|0)\s*\}",
            block.group(1)):
        rows.append((int(m.group(1), 16), int(m.group(2), 16)))
    assert rows, "no rows parsed from g_fn_remap"
    return rows


def _all_audited_vas() -> set[int]:
    vas = set()
    for rel_path, name, _sym, base_relative in AUDITED_OFFSETS:
        value = _parse_define(REPO_ROOT / rel_path, name)
        vas.add(value + IMAGE_BASE if base_relative else value)
    row = _parse_table_row("4.1.1.7209685")
    for field, sym in TABLE_FIELD_SYMBOLS.items():
        if sym is not None and row.get(field):
            vas.add(row[field] + IMAGE_BASE)
    for _old, new in _parse_remap_rows():
        if new:
            vas.add(new)
    return vas


@pytest.fixture(scope="module")
def nm_symbols():
    """Map VA -> demangled symbols for every audited address (one nm pass)."""
    if not BG3_EXEC.exists():
        pytest.skip("BG3 binary not installed")
    if not (shutil.which("nm") and shutil.which("c++filt")):
        pytest.skip("nm/c++filt unavailable")

    wanted = {f"{va:016x}" for va in _all_audited_vas()}
    nm = subprocess.run(
        ["nm", str(BG3_EXEC)], capture_output=True, text=True, timeout=300
    )
    if nm.returncode != 0:
        pytest.skip(f"nm failed: {nm.stderr[:200]}")

    matched = [line for line in nm.stdout.splitlines() if line[:16] in wanted]
    demangled = subprocess.run(
        ["c++filt"], input="\n".join(matched), capture_output=True,
        text=True, timeout=60,
    ).stdout.splitlines()

    found: dict[int, list[str]] = {}
    for line in demangled:
        parts = line.split(maxsplit=2)
        if len(parts) == 3:
            found.setdefault(int(parts[0], 16), []).append(parts[2])
    return found


def _assert_symbol_at(found, va, name, symbol):
    symbols_here = found.get(va, [])
    assert symbols_here, (
        f"{name}: no symbol at VA {va:#x} in the installed binary — the game "
        f"likely updated and this offset is STALE. Re-derive it via: "
        f"nm <BG3 binary> | c++filt | grep '{symbol}'"
    )
    assert any(symbol in s for s in symbols_here), (
        f"{name}: VA {va:#x} resolves to {symbols_here}, expected a symbol "
        f"containing '{symbol}' — offset is aimed at the WRONG target. "
        f"Patching/reading it would corrupt behavior."
    )


@pytest.mark.parametrize(
    "rel_path,name,symbol,base_relative",
    AUDITED_OFFSETS,
    ids=[entry[1] for entry in AUDITED_OFFSETS],
)
def test_offset_matches_symbol(nm_symbols, rel_path, name, symbol, base_relative):
    value = _parse_define(REPO_ROOT / rel_path, name)
    va = value + IMAGE_BASE if base_relative else value
    _assert_symbol_at(nm_symbols, va, name, symbol)


@pytest.mark.parametrize(
    "field",
    [f for f, s in TABLE_FIELD_SYMBOLS.items() if s is not None],
)
def test_offset_table_7209685_row(nm_symbols, field):
    row = _parse_table_row("4.1.1.7209685")
    assert field in row, f"{field} missing from 7209685 row"
    value = row[field]
    assert value, f"{field} is 0 in the 7209685 row (feature silently disabled)"
    _assert_symbol_at(
        nm_symbols, value + IMAGE_BASE,
        f"offset_table[7209685].{field}", TABLE_FIELD_SYMBOLS[field])


def test_offset_table_row_fields_complete():
    """Every struct field must be classified: either audited or excluded."""
    header = (REPO_ROOT / "src/core/offset_table.h").read_text()
    struct = re.search(r"typedef struct \{(.*?)\} VersionOffsets;", header,
                       re.DOTALL)
    assert struct
    declared = set(re.findall(r"u?intptr_t\s+(\w+);", struct.group(1)))
    declared.discard("version")
    classified = set(TABLE_FIELD_SYMBOLS)
    assert declared == classified, (
        f"offset_table.h fields not classified in TABLE_FIELD_SYMBOLS: "
        f"{declared - classified or classified - declared} — every new field "
        f"needs an audit entry or a documented exclusion"
    )


def test_remap_7209685_column(nm_symbols):
    """Every nonzero 7209685 remap target must be a real symbol."""
    for old, new in _parse_remap_rows():
        if new:
            symbols_here = nm_symbols.get(new, [])
            assert symbols_here, (
                f"remap {old:#x} -> {new:#x}: no symbol at target VA — "
                f"stale remap would call the wrong function"
            )


def test_remap_columns_collision_free():
    """Either-column matching requires globally unique addresses."""
    seen: dict[int, int] = {}
    for i, (old, new) in enumerate(_parse_remap_rows()):
        for addr in (old, new):
            if addr == 0:
                continue
            assert addr not in seen, (
                f"address {addr:#x} appears in remap rows {seen[addr]} and "
                f"{i} — either-column lookup would be ambiguous"
            )
            seen[addr] = i


def test_mstate_got_slot():
    """staticdata_mstate_ptr is a __DATA_CONST,__got slot: nm cannot see it.
    Validate GOT-aware — otool -Iv must map the slot to the
    TypeContext<ImmutableDataHeadmaster>::m_State symbol."""
    if not BG3_EXEC.exists():
        pytest.skip("BG3 binary not installed")
    if not shutil.which("otool"):
        pytest.skip("otool unavailable")

    row = _parse_table_row("4.1.1.7209685")
    va = row["staticdata_mstate_ptr"] + IMAGE_BASE
    out = subprocess.run(
        ["otool", "-arch", "arm64", "-Iv", str(BG3_EXEC)],
        capture_output=True, text=True, timeout=600,
    )
    if out.returncode != 0:
        pytest.skip(f"otool failed: {out.stderr[:200]}")
    line = next((ln for ln in out.stdout.splitlines()
                 if ln.startswith(f"0x{va:016x}")), None)
    assert line, (
        f"staticdata_mstate_ptr {va:#x}: not an indirect-symbol slot in the "
        f"installed binary — the __got layout changed; re-derive via "
        f"otool -Iv | grep m_StateE"
    )
    assert MSTATE_GOT_SYMBOL in line, (
        f"staticdata_mstate_ptr {va:#x} maps to the wrong symbol: {line} — "
        f"expected {MSTATE_GOT_SYMBOL}. The StaticData traversal would walk "
        f"garbage."
    )


OSIRIS_QUERY_MANGLED = "__ZN3osi15OsirisInterface11OsirisQueryEjP16COsiArgumentDesc"


def test_osiris_interface_slot_matches_disasm():
    """osiris_interface_ptr (the osi::OsirisInterface global instance slot,
    used by osi_read_param_defs in main.c) has no nm symbol. Its authoritative
    reference is OsirisQuery's own load of the global — the first
    ``adrp xN, <page>`` / ``ldr xM, [xN, #off]`` pair in the function prologue
    after the stack-guard load. Disassemble it and require the computed target
    to equal the table value (2026-07-29 on 7209685: adrp 0x108a86000 +
    ldr #0x128 -> 0x108a86128)."""
    if not BG3_EXEC.exists():
        pytest.skip("BG3 binary not installed")
    if not shutil.which("otool"):
        pytest.skip("otool unavailable")

    row = _parse_table_row("4.1.1.7209685")
    expected = row["osiris_interface_ptr"] + IMAGE_BASE
    out = subprocess.run(
        ["otool", "-arch", "arm64", "-tV", "-p", OSIRIS_QUERY_MANGLED,
         str(BG3_EXEC)],
        capture_output=True, text=True, timeout=600,
    )
    if out.returncode != 0:
        pytest.skip(f"otool failed: {out.stderr[:200]}")
    lines = out.stdout.splitlines()[:40]
    assert any(OSIRIS_QUERY_MANGLED in ln for ln in lines), (
        "osi::OsirisInterface::OsirisQuery symbol not found — signature "
        "changed in a game update; re-derive the instance slot"
    )
    targets = []
    page = None
    page_reg = None
    for ln in lines:
        m = re.search(r"adrp\s+(x\d+),\s+\S+\s+;\s+0x([0-9a-f]+)", ln)
        if m:
            page_reg, page = m.group(1), int(m.group(2), 16)
            continue
        m = re.search(r"ldr\s+x\d+,\s+\[(x\d+)(?:,\s+#0x([0-9a-f]+))?\]", ln)
        if m and page is not None and m.group(1) == page_reg:
            off = int(m.group(2), 16) if m.group(2) else 0
            targets.append(page + off)
            page = page_reg = None
    assert expected in targets, (
        f"osiris_interface_ptr {expected:#x} not among OsirisQuery's "
        f"adrp+ldr targets {[hex(t) for t in targets]} — the instance slot "
        f"moved in a game update; osi_read_param_defs would read garbage. "
        f"Re-derive from the disasm and update offset_table.c."
    )


def test_functor_gate_is_independent():
    """The functor Dobby family's addresses are nm-auditable, but its ABI is
    not: the 7209685 Interrupt ExecuteStatsFunctors gained a leading
    ecs::EntityWorld& parameter. The install gate must therefore compare
    against FUNCTOR_ADDRS_VERIFIED_BUILD — the build whose addresses AND
    wrapper signatures were verified — never the global version match. This
    guards against a refactor quietly re-coupling it to BG3_KNOWN_VERSION,
    which would enable eleven ABI-unverified code patches on every version
    bump.
    """
    functor_types = (REPO_ROOT / "src/stats/functor_types.h").read_text()
    m = re.search(
        r'#define\s+FUNCTOR_ADDRS_VERIFIED_BUILD\s+"([\d.]+)"', functor_types
    )
    assert m, "FUNCTOR_ADDRS_VERIFIED_BUILD missing from functor_types.h"

    main_c = (REPO_ROOT / "src/injector/main.c").read_text()
    gate = re.search(
        r"if\s*\(!no_hooks[^)]*FUNCTOR_ADDRS_VERIFIED_BUILD[^)]*\)\s*==\s*0\)",
        main_c,
    )
    assert gate, (
        "main.c functor install gate no longer compares the detected version "
        "against FUNCTOR_ADDRS_VERIFIED_BUILD — eleven ABI-unverified code "
        "patches would install on unverified builds"
    )
    assert "version_detect_matches()) {\n                        if (functor_hooks_init" \
        not in main_c, "functor install re-coupled to global version match"


def test_interrupt_remap_stays_disabled_until_wrapper_exists():
    """The 7209685 Interrupt ExecuteStatsFunctors has a different signature
    (leading ecs::EntityWorld&). Its remap column must stay 0 until a
    dedicated wrapper for the new ABI lands in functor_hooks.c."""
    rows = _parse_remap_rows()
    interrupt = [new for old, new in rows if old == 0x1057965E4]
    assert interrupt, "Interrupt row (0x1057965e4) missing from g_fn_remap"
    if interrupt[0] != 0:
        wrappers = (REPO_ROOT / "src/stats/functor_hooks.c").read_text()
        assert "EntityWorld" in wrappers, (
            "Interrupt remap target enabled but functor_hooks.c has no "
            "EntityWorld-aware wrapper — the hook would be ABI-mismatched"
        )
