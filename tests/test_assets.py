import json
import pathlib
import re
import unittest


ROOT = pathlib.Path(__file__).resolve().parents[1]


class AssetTests(unittest.TestCase):
    def test_param_json_matches_deeplink(self):
        param = json.loads((ROOT / "assets/param.json").read_text())
        self.assertEqual(param["titleId"], "AIRP00001")
        self.assertEqual(param["applicationCategoryType"], 65536)
        self.assertEqual(param["deeplinkUri"], "http://127.0.0.1:18186/app/index.html")

    def test_title_id_matches_system_registration_format(self):
        # Every title in the system homescreen database is four letters and
        # five digits; a different shape is accepted by AppInstallAll but never shown.
        header = (ROOT / "include/installer_config.h").read_text()
        title_id = re.search(r'#define AIRPLAY_TITLE_ID "([^"]+)"', header).group(1)
        self.assertRegex(title_id, r"^[A-Z]{4}\d{5}$")
        param = json.loads((ROOT / "assets/param.json").read_text())
        self.assertEqual(param["titleId"], title_id)

    def test_default_receiver_name(self):
        header = (ROOT / "include/airplay_config.h").read_text()
        self.assertIn('#define AIRPLAY_NAME "#NODISCNOBUY"', header)
        page = (ROOT / "assets/app/index.html").read_text()
        self.assertIn("name: '#NODISCNOBUY'", page)

    def test_features_keep_legacy_pairing_bit(self):
        # Bit 27 ("supports legacy pairing") must stay set: clients skip
        # pair-setup/pair-verify, and the audio key hash, when it is clear.
        header = (ROOT / "include/airplay_config.h").read_text()
        features = re.search(r'#define AIRPLAY_FEATURES "0x([0-9A-Fa-f]+),0x0"', header)
        self.assertIsNotNone(features)
        self.assertTrue(int(features.group(1), 16) & (1 << 27))

    def test_appcache_contains_launcher(self):
        manifest = (ROOT / "assets/airplay.appcache").read_text()
        self.assertIn("CACHE MANIFEST", manifest)
        self.assertIn("PS5 AirPlay AppCache v9", manifest)
        self.assertIn("/app/index.html", manifest)

    def test_placeholder_icon_is_present(self):
        data = (ROOT / "assets/icon0.png").read_bytes()
        self.assertEqual(data[:8], b"\x89PNG\r\n\x1a\n")

    def test_app_page_contains_ps5_settings_ui(self):
        page = (ROOT / "assets/app/index.html").read_text()
        self.assertIn("<h1>PS5 AirPlay</h1>", page)
        self.assertIn("AirPlay for Apple devices on your local network", page)
        self.assertIn('id="passcode-row"', page)
        self.assertIn('id="mode-row"', page)
        for mode in ("quality", "balanced", "performance"):
            self.assertIn(f'data-mode="{mode}"', page)
        # Settings that cannot be changed from the page are not shown.
        for removed in ("Allow Access", "Conference Room Display", "Underscan", "Audio Output",
                        "Full Screen"):
            self.assertNotIn(removed, page)
        self.assertIn("http://127.0.0.1:18182", page)
        self.assertIn("/api/status", page)
        self.assertIn("file:/data/ps5-airplay/ps5airplay.elf", page)


if __name__ == "__main__":
    unittest.main()
