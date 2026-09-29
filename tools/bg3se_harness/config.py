import os
import plistlib
import re
import sys
from pathlib import Path

PROJECT_ROOT = Path(__file__).resolve().parent.parent.parent

# The Steam folder omits the apostrophe; the .app bundle usually keeps it
_BG3_APP_NAMES = ("Baldur's Gate 3.app", "Baldurs Gate 3.app")
_STEAM_COMMON_REL = "steamapps/common/Baldurs Gate 3"

# Store suffixes Larian appends to the real game executable. GOG's
# CFBundleExecutable is a ~200KB arch-selector stub and the game sits beside it
# as "<CFBundleExecutable> GOG"; Steam's CFBundleExecutable is the game.
_STORE_EXEC_SUFFIXES = (" GOG", " Steam")


def parse_libraryfolders_vdf(text: str) -> list[str]:
    """Extract library root paths from Steam's libraryfolders.vdf."""
    return [m.group(1) for m in re.finditer(r'"path"\s*"([^"]*)"', text)]


def resolve_bg3_app_bundle() -> Path:
    """Resolve the BG3 .app bundle (#90, #86).

    Order: BG3SE_GAME_PATH override (the bundle itself or a directory
    containing it), the default Steam library, then every additional
    library listed in steamapps/libraryfolders.vdf (external drives).
    """
    env = os.environ.get("BG3SE_GAME_PATH")
    if env:
        override = Path(env).expanduser()
        if override.suffix == ".app" and override.is_dir():
            return override
        for name in _BG3_APP_NAMES:
            if (override / name).is_dir():
                return override / name
        # Invalid override: warn and fall through to the Steam-library scan,
        # mirroring scripts/find_bg3.sh — never hand back a garbage path
        print(
            f"Warning: BG3SE_GAME_PATH set but no BG3 app bundle found there: {env}",
            file=sys.stderr,
        )

    steam_root = Path.home() / "Library/Application Support/Steam"
    candidates = [steam_root / _STEAM_COMMON_REL / name for name in _BG3_APP_NAMES]

    vdf = steam_root / "steamapps/libraryfolders.vdf"
    if vdf.is_file():
        try:
            library_roots = parse_libraryfolders_vdf(vdf.read_text(errors="replace"))
        except OSError:
            library_roots = []
        for root in library_roots:
            for name in _BG3_APP_NAMES:
                candidates.append(Path(root) / _STEAM_COMMON_REL / name)

    # Non-Steam installs (GOG, or a bundle someone dragged somewhere). GOG's
    # installer offers both of these; neither is under a Steam library, so the
    # scan above can never reach them.
    for root in (Path("/Applications"), Path.home() / "Applications"):
        for name in _BG3_APP_NAMES:
            candidates.append(root / name)

    for candidate in candidates:
        if candidate.is_dir():
            return candidate
    return candidates[0]


def resolve_bg3_executable(bundle: Path) -> Path:
    """Resolve the real game binary inside `bundle`.

    On GOG this is not CFBundleExecutable but the store-suffixed binary beside
    it; patching or launching the stub misses the game. Falls back to plain
    CFBundleExecutable, which is what Steam ships.
    """
    exec_name = None
    try:
        with open(bundle / "Contents/Info.plist", "rb") as f:
            exec_name = plistlib.load(f).get("CFBundleExecutable")
    except (OSError, ValueError):
        pass
    if not exec_name:
        # No readable plist (game not installed). The bundle stem matches in
        # every BG3 build shipped so far.
        exec_name = bundle.stem

    macos = bundle / "Contents/MacOS"
    for suffix in _STORE_EXEC_SUFFIXES:
        candidate = macos / f"{exec_name}{suffix}"
        if candidate.is_file():
            return candidate
    return macos / exec_name


def resolve_bg3_store(executable: Path) -> str:
    """Which store's build `executable` is: "steam" or "gog".

    Mirrors version_detect_store_for_image_path() in src/core/version_detect.c.
    Keyed on the suffix, not the path, so a moved or symlinked install works.
    """
    for suffix in _STORE_EXEC_SUFFIXES:
        if executable.name.endswith(suffix):
            return suffix.strip().lower()
    return "steam"


def resolve_build_key(bundle: Path) -> str:
    """The key identifying this build in src/core/offset_table.c.

    CFBundleShortVersionString alone does not identify a build (see
    src/gen/<store>/build_identity.h), so non-Steam rows carry a
    "<version>-<store>" key. Mirrors offset_table_init().
    """
    try:
        with open(bundle / "Contents/Info.plist", "rb") as f:
            version = plistlib.load(f).get("CFBundleShortVersionString")
    except (OSError, ValueError):
        return ""
    if not version:
        return ""
    store = resolve_bg3_store(resolve_bg3_executable(bundle))
    return version if store == "steam" else f"{version}-{store}"


BG3_APP_BUNDLE = resolve_bg3_app_bundle()
BG3_EXEC = resolve_bg3_executable(BG3_APP_BUNDLE)
BG3_STORE = resolve_bg3_store(BG3_EXEC)

# Generated address tables live per store, and the dylib selects between them
# at runtime. Audits want the installed game's store; offline analysis of the
# checked-in tables can override with BG3SE_GEN_STORE.
GEN_STORE = os.environ.get("BG3SE_GEN_STORE") or BG3_STORE
GEN_DIR = PROJECT_ROOT / "src/gen" / GEN_STORE


def gen_path(name: str, store: str | None = None) -> Path:
    """Path to a generated address table, e.g. gen_path("generated_typeids.h")."""
    return PROJECT_ROOT / "src/gen" / (store or GEN_STORE) / name
DYLIB_OUTPUT = PROJECT_ROOT / "build/lib/libbg3se.dylib"
DEPLOYED_DYLIB = BG3_APP_BUNDLE / "Contents/MacOS/libbg3se.dylib"
SOCKET_PATH = "/tmp/bg3se.sock"
HEALTH_TIMEOUT = 30
HEALTH_TIMEOUT_CONTINUE = 180  # Save loading takes 30-60s+ on top of launch
BACKUP_SUFFIX = ".bg3se-original"
HASH_FILE = BG3_APP_BUNDLE / "Contents/MacOS/.bg3se-patch-hash"
INSERT_DYLIB = PROJECT_ROOT / "tools/vendor/insert_dylib/insert_dylib_bin"
DYLIB_INSTALL_NAME = "@loader_path/libbg3se.dylib"

# Harness process tracking
PID_FILE = Path("/tmp/bg3se_harness.pid")
HEALTH_FILE = Path("/tmp/bg3se_health.json")
MONITOR_LOG = Path("/tmp/bg3se_monitor.log")

# Mod management paths
LARIAN_LOCAL = Path.home() / "Documents/Larian Studios/Baldur's Gate 3"
MODS_DIR = LARIAN_LOCAL / "Mods"
MODSETTINGS_PATH = LARIAN_LOCAL / "PlayerProfiles/Public/modsettings.lsx"
GRAPHIC_SETTINGS_PATH = LARIAN_LOCAL / "graphicSettings.lsx"
SAVES_DIR = LARIAN_LOCAL / "PlayerProfiles/Public/Savegames/Story"
MOD_CRASH_SANITY_CHECK_DIR = LARIAN_LOCAL / "ModCrashSanityCheck"

# Harness data paths
HARNESS_CONFIG_DIR = Path.home() / ".config/bg3se-harness"
MOD_REGISTRY_PATH = HARNESS_CONFIG_DIR / "mod_registry.json"
SAVE_FIXTURES_DIR = HARNESS_CONFIG_DIR / "save_fixtures"
REPORTS_DIR = PROJECT_ROOT / ".reports"

# Catalog paths (shipped with repo)
CATALOG_DIR = Path(__file__).resolve().parent / "catalog"
SCENARIOS_DIR = Path(__file__).resolve().parent / "scenarios"

# GustavX invariant — must always be at position 0 in modsettings.lsx
# Match by name, not UUID — the UUID changes between game versions
GUSTAVX_NAME = "GustavX"
