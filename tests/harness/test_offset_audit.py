"""Audit hardcoded code-patch offsets against the installed game binary.

Dobby code patches aimed at the main BG3 binary use hardcoded addresses
derived from one specific game build. The version-detect sentinels only
validate three *data* addresses, so a game update can silently invalidate
*code* offsets — a stale ``OFFSET_GET_CLASS`` once landed in the middle of
``gui::HotbarSystem::Update`` and crashed every session ~30s after save
load (docs/bugs/codex-debugger-nohooks-2026-07-28.md).

This test parses the offsets out of the C sources and asserts that ``nm``
resolves each one to exactly the symbol it claims to target. It skips when
the game binary is not installed (CI).

COVERAGE LIMIT: the eleven functor-hook addresses in
``src/stats/functor_types.h`` target STRIPPED LOCAL functions with no nm
symbols, so this audit cannot validate them. That family is instead gated
at install time on ``FUNCTOR_ADDRS_VERIFIED_BUILD`` — an exact match
against the build its addresses were derived from, independent of
``BG3_KNOWN_VERSION`` (see test_functor_gate_is_independent below).
"""

import re
import shutil
import subprocess
from pathlib import Path

import pytest

from bg3se_harness.config import BG3_EXEC

REPO_ROOT = Path(__file__).resolve().parents[2]

# (source file, #define name, expected demangled symbol substring,
#  base-relative? — staticdata offsets are relative to 0x100000000)
AUDITED_OFFSETS = [
    ("src/staticdata/staticdata_manager.c", "OFFSET_FEAT_GETFEATS",
     "eoc::FeatManager::GetFeats() const", True),
    ("src/staticdata/staticdata_manager.c", "OFFSET_GETALLFEATS",
     "eoc::character_creation::GetAllFeats", True),
    ("src/staticdata/staticdata_manager.c", "OFFSET_MSTATE_PTR",
     "ls::TypeContext<ls::ImmutableDataHeadmaster>::m_State", True),
    ("src/staticdata/staticdata_manager.c", "OFFSET_GET_BACKGROUND",
     "ImmutableDataHeadmaster::Get<eoc::BackgroundManager>", True),
    ("src/staticdata/staticdata_manager.c", "OFFSET_GET_ORIGIN",
     "ImmutableDataHeadmaster::Get<eoc::OriginManager>", True),
    ("src/staticdata/staticdata_manager.c", "OFFSET_GET_CLASS",
     "ImmutableDataHeadmaster::Get<eoc::ClassDescriptions>", True),
    ("src/staticdata/staticdata_manager.c", "OFFSET_GET_PROGRESSION",
     "ImmutableDataHeadmaster::Get<eoc::ProgressionManager>", True),
    ("src/staticdata/staticdata_manager.c", "OFFSET_GET_ACTIONRESOURCE",
     "ImmutableDataHeadmaster::Get<eoc::ActionResourceTypes>", True),
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
]

IMAGE_BASE = 0x100000000


def _parse_define(source: Path, name: str) -> int:
    text = source.read_text()
    m = re.search(rf"#define\s+{name}\s+(0x[0-9a-fA-F]+)", text)
    assert m, f"{name} not found in {source}"
    return int(m.group(1), 16)


def _expected_vas() -> dict[int, tuple[str, str]]:
    """Map virtual address -> (define name, expected symbol substring)."""
    vas = {}
    for rel_path, name, symbol, base_relative in AUDITED_OFFSETS:
        value = _parse_define(REPO_ROOT / rel_path, name)
        va = value + IMAGE_BASE if base_relative else value
        vas[va] = (name, symbol)
    return vas


@pytest.fixture(scope="module")
def symbols_at_audited_vas():
    if not BG3_EXEC.exists():
        pytest.skip("BG3 binary not installed")
    if not (shutil.which("nm") and shutil.which("c++filt")):
        pytest.skip("nm/c++filt unavailable")

    vas = _expected_vas()
    wanted = {f"{va:016x}" for va in vas}
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
    return vas, found


@pytest.mark.parametrize(
    "rel_path,name,symbol,base_relative",
    AUDITED_OFFSETS,
    ids=[entry[1] for entry in AUDITED_OFFSETS],
)
def test_offset_matches_symbol(
    symbols_at_audited_vas, rel_path, name, symbol, base_relative
):
    vas, found = symbols_at_audited_vas
    value = _parse_define(REPO_ROOT / rel_path, name)
    va = value + IMAGE_BASE if base_relative else value

    symbols_here = found.get(va, [])
    assert symbols_here, (
        f"{name} = {value:#x}: no symbol at VA {va:#x} in the installed "
        f"binary — the game likely updated and this offset is STALE. "
        f"Re-derive it via: nm <BG3 binary> | c++filt | grep '{symbol}'"
    )
    assert any(symbol in s for s in symbols_here), (
        f"{name} = {value:#x}: VA {va:#x} resolves to {symbols_here}, "
        f"expected a symbol containing '{symbol}' — offset is aimed at the "
        f"WRONG function. Patching it would corrupt game code."
    )


def test_functor_gate_is_independent():
    """The functor Dobby family is un-auditable by nm (stripped locals), so
    its install gate must compare against FUNCTOR_ADDRS_VERIFIED_BUILD — the
    build its addresses came from — never the global version match. This
    guards against a refactor quietly re-coupling it to BG3_KNOWN_VERSION,
    which would enable eleven unverified code patches on every version bump.
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
        "against FUNCTOR_ADDRS_VERIFIED_BUILD — eleven stripped-local code "
        "patches would install on unverified builds"
    )
    assert "version_detect_matches()) {\n                        if (functor_hooks_init" \
        not in main_c, "functor install re-coupled to global version match"
