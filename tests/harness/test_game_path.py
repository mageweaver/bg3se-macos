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


def test_module_constants_derive_from_bundle():
    assert config.BG3_EXEC == config.BG3_APP_BUNDLE / "Contents/MacOS/Baldur's Gate 3"
    assert config.DEPLOYED_DYLIB == config.BG3_APP_BUNDLE / "Contents/MacOS/libbg3se.dylib"
