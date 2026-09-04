import importlib.util
import re
import unittest
from pathlib import Path


REPOSITORY = Path(__file__).resolve().parents[1]
GENERATOR_PATH = REPOSITORY / "scripts/generate_manual.py"
SPEC = importlib.util.spec_from_file_location("generate_manual", GENERATOR_PATH)
generate_manual = importlib.util.module_from_spec(SPEC)
assert SPEC.loader is not None
SPEC.loader.exec_module(generate_manual)


class ManualReleaseLimitTest(unittest.TestCase):
    def test_release_limit_matches_firmware(self):
        docs_header = (
            REPOSITORY / "src/services/solar_os_docs.h"
        ).read_text(encoding="utf-8")
        match = re.search(
            r"^#define SOLAR_OS_DOCS_PAGE_MAX \((\d+)U \* 1024U\)$",
            docs_header,
            re.MULTILINE,
        )
        self.assertIsNotNone(match)
        assert match is not None
        self.assertEqual(
            generate_manual.RELEASE_PAGE_MAX,
            int(match.group(1)) * 1024,
        )

        docs_source = (
            REPOSITORY / "src/services/solar_os_docs.c"
        ).read_text(encoding="utf-8")
        loader = docs_source[docs_source.index("solar_os_docs_load_page"):]
        loader = loader[:loader.index("\ntypedef struct {")]
        self.assertIn("SOLAR_OS_DOCS_PAGE_MAX", loader)
        self.assertNotIn("MANUAL_EXTERNAL_MAX", docs_source)

    def test_current_manual_pages_fit_release_limit(self):
        pages = generate_manual.load_pages(
            REPOSITORY / "doc/manual",
            REPOSITORY / "packages/solar_os_packages.toml",
        )

        generate_manual.validate_release_pages(pages)

    def test_command_names_resolve_to_command_pages(self):
        pages = generate_manual.load_pages(
            REPOSITORY / "doc/manual",
            REPOSITORY / "packages/solar_os_packages.toml",
        )
        aliases = {
            str(alias): str(page["id"])
            for page in pages
            for alias in page["aliases"]
        }

        for command in ("battery", "rtc", "schedule", "time"):
            self.assertEqual(aliases[command], f"command.{command}")

    def test_derived_pages_use_runtime_registry_gates(self):
        pages = generate_manual.load_pages(
            REPOSITORY / "doc/manual",
            REPOSITORY / "packages/solar_os_packages.toml",
        )
        by_id = {str(page["id"]): page for page in pages}

        self.assertEqual(
            by_id["command.mqtt"]["packages_any"], ["service_mqtt"]
        )
        self.assertEqual(
            by_id["command.spi"]["condition"],
            "(SOLAR_OS_PACKAGE_SERVICE_RESOURCES && "
            "SOLAR_OS_PACKAGE_SERVICE_SPI)",
        )
        self.assertEqual(
            by_id["command.led"]["condition"],
            "(SOLAR_OS_PACKAGE_SERVICE_GPIO && "
            "SOLAR_OS_BOARD_HAS_STATUS_LED)",
        )
        self.assertEqual(by_id["command.help"]["condition"], "")
        self.assertEqual(by_id["app.hexedit"]["packages_any"], ["app_edit"])
        self.assertEqual(by_id["app.help"]["packages_any"], ["app_docs"])

    def test_runtime_index_allows_package_gated_embedded_pages(self):
        pages = generate_manual.load_pages(
            REPOSITORY / "doc/manual",
            REPOSITORY / "packages/solar_os_packages.toml",
        )
        by_id = {str(page["id"]): page for page in pages}
        self.assertEqual(
            by_id["command.dpad"]["packages_any"], ["service_adc_dpad"]
        )

        docs_source = (
            REPOSITORY / "src/services/solar_os_docs.c"
        ).read_text(encoding="utf-8")
        builder = docs_source[docs_source.index("static esp_err_t docs_build_manual_index("):]
        builder = builder[:builder.index("\nstatic esp_err_t docs_verify_data(")]
        self.assertEqual(builder.count("solar_os_manual_embedded_count()"), 1)
        self.assertIn("topic != count", builder)

    def test_cached_manual_survives_firmware_version_change(self):
        docs_header = (
            REPOSITORY / "src/services/solar_os_docs.h"
        ).read_text(encoding="utf-8")
        self.assertIn("char manual_version[32];", docs_header)
        self.assertNotIn("char version[32];", docs_header)

        docs_source = (
            REPOSITORY / "src/services/solar_os_docs.c"
        ).read_text(encoding="utf-8")
        parser = docs_source[docs_source.index("static esp_err_t docs_parse_catalog("):]
        parser = parser[:parser.index("\nstatic esp_err_t docs_page_metadata(")]
        self.assertIn("bool require_current_version", parser)
        self.assertIn(
            "require_current_version &&\n"
            "         strcmp(info->firmware_version, SOLAR_OS_VERSION) != 0",
            parser,
        )
        self.assertIn(
            "require_current_version &&\n"
            "         info->page_count < solar_os_manual_embedded_count()",
            parser,
        )

        initializer = docs_source[docs_source.index("solar_os_docs_init"):]
        initializer = initializer[:initializer.index("\nesp_err_t solar_os_docs_get_status")]
        self.assertRegex(
            initializer,
            r"docs_verify_revision\(revision,\s*&info,\s*false,\s*false,\s*"
            r"&manual_index\)",
        )

        loader = docs_source[docs_source.index("solar_os_docs_load_page"):]
        loader = loader[:loader.index("\ntypedef struct {")]
        self.assertRegex(
            loader,
            r"docs_parse_catalog\(catalog,\s*catalog_len,\s*&document,\s*"
            r"&info,\s*false\)",
        )

        updater = docs_source[docs_source.index("solar_os_docs_update"):]
        updater = updater[:updater.index("\nesp_err_t solar_os_docs_reset")]
        self.assertRegex(
            updater,
            r"docs_parse_catalog\(catalog,\s*catalog_len,\s*&document,\s*"
            r"&info,\s*true\)",
        )

        shell_source = (
            REPOSITORY / "src/shell/solar_os_shell_manual.c"
        ).read_text(encoding="utf-8")
        self.assertIn("It may be outdated. Run 'help update'.", shell_source)

        app_source = (
            REPOSITORY / "src/apps/solar_os_docs_app.c"
        ).read_text(encoding="utf-8")
        self.assertIn('"SolarOS manual  %u topics  outdated %s"', app_source)

    def test_help_status_topic_has_an_explicit_escape(self):
        pages = generate_manual.load_pages(
            REPOSITORY / "doc/manual",
            REPOSITORY / "packages/solar_os_packages.toml",
        )
        by_id = {str(page["id"]): page for page in pages}
        self.assertIn("status", by_id["command.status"]["aliases"])
        self.assertIn("help command.status", by_id["help"]["markdown"])

    def test_derived_alias_collisions_are_explicit(self):
        self.assertEqual(
            generate_manual.DERIVED_ALIAS_OWNERS[("command.mqtt", "mqtt")],
            "network",
        )
        self.assertNotIn(
            ("command.schedule", "schedule"),
            generate_manual.DERIVED_ALIAS_OWNERS,
        )
        generate_manual.load_pages(
            REPOSITORY / "doc/manual",
            REPOSITORY / "packages/solar_os_packages.toml",
        )

    def test_release_page_at_limit_is_accepted(self):
        page = {
            "id": "at-limit",
            "release_markdown": "x" * generate_manual.RELEASE_PAGE_MAX,
        }

        generate_manual.validate_release_pages([page])

    def test_oversized_release_page_is_rejected(self):
        page = {
            "id": "oversized",
            "release_markdown": "x" * (generate_manual.RELEASE_PAGE_MAX + 1),
        }

        with self.assertRaisesRegex(
            ValueError,
            rf"manual page 'oversized' is {generate_manual.RELEASE_PAGE_MAX + 1} "
            rf"bytes; release pages must be 1\.\.{generate_manual.RELEASE_PAGE_MAX} "
            "bytes",
        ):
            generate_manual.validate_release_pages([page])


if __name__ == "__main__":
    unittest.main()
