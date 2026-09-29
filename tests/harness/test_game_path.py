"""Game-path discovery: libraryfolders.vdf parsing + BG3SE_GAME_PATH override.

Covers #90 (game on external drive) and #86 (alternate source directory).
"""

from bg3se_harness import config

VDF_TWO_LIBRARIES = """\
"libraryfolders"
{
\t"0"
\t{
\t\t"path"\t\t"/Users/someone/Library/Application Support/Steam"
\t\t"label"\t\t""
\t\t"apps"
\t\t{
\t\t\t"1086940"\t\t"142842156"
\t\t}
\t}
\t"1"
\t{
\t\t"path"\t\t"/Volumes/GameDrive/SteamLibrary"
\t\t"label"\t\t"external"
\t\t"apps"
\t\t{
\t\t}
\t}
}
"""


class TestParseLibraryfoldersVdf:
    def test_extracts_all_roots_in_order(self):
        roots = config.parse_libraryfolders_vdf(VDF_TWO_LIBRARIES)
        assert roots == [
            "/Users/someone/Library/Application Support/Steam",
            "/Volumes/GameDrive/SteamLibrary",
        ]

    def test_empty_text(self):
        assert config.parse_libraryfolders_vdf("") == []

    def test_ignores_other_keys(self):
        text = '"label" "path-ish" "path" "/Volumes/X" "apps" "1"'
        assert config.parse_libraryfolders_vdf(text) == ["/Volumes/X"]


class TestResolveBg3AppBundle:
    def test_env_override_bundle_path(self, monkeypatch, tmp_path):
        bundle = tmp_path / "Baldur's Gate 3.app"
        bundle.mkdir()
        monkeypatch.setenv("BG3SE_GAME_PATH", str(bundle))
        assert config.resolve_bg3_app_bundle() == bundle

    def test_env_override_invalid_falls_back(self, monkeypatch, tmp_path):
        home = tmp_path / "home"
        home.mkdir()
        monkeypatch.setenv("BG3SE_GAME_PATH", str(tmp_path / "nowhere"))
        monkeypatch.setattr(config.Path, "home", staticmethod(lambda: home))
        resolved = config.resolve_bg3_app_bundle()
        assert resolved == (
            home / "Library/Application Support/Steam/steamapps/common/"
            "Baldurs Gate 3/Baldur's Gate 3.app"
        )

    def test_env_override_parent_dir(self, monkeypatch, tmp_path):
        bundle = tmp_path / "Baldur's Gate 3.app"
        bundle.mkdir()
        monkeypatch.setenv("BG3SE_GAME_PATH", str(tmp_path))
        assert config.resolve_bg3_app_bundle() == bundle

    def test_env_override_parent_dir_no_apostrophe(self, monkeypatch, tmp_path):
        bundle = tmp_path / "Baldurs Gate 3.app"
        bundle.mkdir()
        monkeypatch.setenv("BG3SE_GAME_PATH", str(tmp_path))
        assert config.resolve_bg3_app_bundle() == bundle

    def test_vdf_library_scanned(self, monkeypatch, tmp_path):
        home = tmp_path / "home"
        library = tmp_path / "external"
        bundle = library / "steamapps/common/Baldurs Gate 3/Baldur's Gate 3.app"
        bundle.mkdir(parents=True)

        steam = home / "Library/Application Support/Steam"
        (steam / "steamapps").mkdir(parents=True)
        (steam / "steamapps/libraryfolders.vdf").write_text(
            f'"libraryfolders" {{ "0" {{ "path" "{library}" }} }}'
        )

        monkeypatch.delenv("BG3SE_GAME_PATH", raising=False)
        monkeypatch.setattr(config.Path, "home", staticmethod(lambda: home))
        assert config.resolve_bg3_app_bundle() == bundle

    def test_default_when_nothing_found(self, monkeypatch, tmp_path):
        home = tmp_path / "empty-home"
        home.mkdir()
        monkeypatch.delenv("BG3SE_GAME_PATH", raising=False)
        monkeypatch.setattr(config.Path, "home", staticmethod(lambda: home))
        resolved = config.resolve_bg3_app_bundle()
        assert resolved == (
            home / "Library/Application Support/Steam/steamapps/common/"
            "Baldurs Gate 3/Baldur's Gate 3.app"
        )


class TestNonSteamInstalls:
    """GOG installs live outside every Steam library, so the vdf scan cannot
    reach them. Covers /Applications and ~/Applications."""

    def _empty_home(self, monkeypatch, tmp_path):
        home = tmp_path / "home"
        home.mkdir()
        monkeypatch.delenv("BG3SE_GAME_PATH", raising=False)
        monkeypatch.setattr(config.Path, "home", staticmethod(lambda: home))
        return home

    def test_user_applications_found(self, monkeypatch, tmp_path):
        home = self._empty_home(monkeypatch, tmp_path)
        bundle = home / "Applications/Baldur's Gate 3.app"
        bundle.mkdir(parents=True)
        assert config.resolve_bg3_app_bundle() == bundle

    def test_steam_library_wins_over_applications(self, monkeypatch, tmp_path):
        """A Steam install must keep resolving first, even with a second
        bundle in ~/Applications."""
        home = self._empty_home(monkeypatch, tmp_path)
        steam_bundle = (home / "Library/Application Support/Steam" /
                        "steamapps/common/Baldurs Gate 3/Baldur's Gate 3.app")
        steam_bundle.mkdir(parents=True)
        (home / "Applications/Baldur's Gate 3.app").mkdir(parents=True)
        assert config.resolve_bg3_app_bundle() == steam_bundle


class TestResolveBg3Executable:
    """GOG's CFBundleExecutable is an arch-selector stub; the game is the
    suffixed binary beside it, so this must prefer the suffix."""

    def _bundle(self, tmp_path, exec_name="Baldur's Gate 3", extra=None):
        bundle = tmp_path / "Baldur's Gate 3.app"
        macos = bundle / "Contents/MacOS"
        macos.mkdir(parents=True)
        (bundle / "Contents/Info.plist").write_bytes(
            b'<?xml version="1.0" encoding="UTF-8"?>'
            b'<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" '
            b'"http://www.apple.com/DTDs/PropertyList-1.0.dtd">'
            b'<plist version="1.0"><dict><key>CFBundleExecutable</key>'
            + b"<string>" + exec_name.encode() + b"</string></dict></plist>"
        )
        (macos / exec_name).write_bytes(b"stub")
        if extra:
            (macos / extra).write_bytes(b"game")
        return bundle, macos

    def test_prefers_gog_suffixed_binary(self, tmp_path):
        bundle, macos = self._bundle(tmp_path, extra="Baldur's Gate 3 GOG")
        assert config.resolve_bg3_executable(bundle) == macos / "Baldur's Gate 3 GOG"

    def test_steam_layout_uses_bundle_executable(self, tmp_path):
        bundle, macos = self._bundle(tmp_path)
        assert config.resolve_bg3_executable(bundle) == macos / "Baldur's Gate 3"

    def test_honours_cfbundleexecutable_name(self, tmp_path):
        bundle, macos = self._bundle(tmp_path, exec_name="BG3", extra="BG3 GOG")
        assert config.resolve_bg3_executable(bundle) == macos / "BG3 GOG"

    def test_missing_plist_falls_back_to_bundle_stem(self, tmp_path):
        """Game not installed: return a usable path rather than raising."""
        bundle = tmp_path / "Baldur's Gate 3.app"
        bundle.mkdir()
        assert config.resolve_bg3_executable(bundle) == (
            bundle / "Contents/MacOS/Baldur's Gate 3"
        )

    def test_directory_named_like_the_suffix_is_ignored(self, tmp_path):
        """Only a regular file counts; a stray directory must not be picked."""
        bundle, macos = self._bundle(tmp_path)
        (macos / "Baldur's Gate 3 GOG").mkdir()
        assert config.resolve_bg3_executable(bundle) == macos / "Baldur's Gate 3"


def test_module_constants_derive_from_bundle():
    assert config.BG3_EXEC == config.resolve_bg3_executable(config.BG3_APP_BUNDLE)
    assert config.BG3_EXEC.parent == config.BG3_APP_BUNDLE / "Contents/MacOS"
    assert config.DEPLOYED_DYLIB == config.BG3_APP_BUNDLE / "Contents/MacOS/libbg3se.dylib"
