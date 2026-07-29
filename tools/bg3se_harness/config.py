import os
import re
import sys
from pathlib import Path

PROJECT_ROOT = Path(__file__).resolve().parent.parent.parent

# The Steam folder omits the apostrophe; the .app bundle usually keeps it
_BG3_APP_NAMES = ("Baldur's Gate 3.app", "Baldurs Gate 3.app")
_STEAM_COMMON_REL = "steamapps/common/Baldurs Gate 3"


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

    for candidate in candidates:
        if candidate.is_dir():
            return candidate
    return candidates[0]


BG3_APP_BUNDLE = resolve_bg3_app_bundle()
BG3_EXEC = BG3_APP_BUNDLE / "Contents/MacOS/Baldur's Gate 3"
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
