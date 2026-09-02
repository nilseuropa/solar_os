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
        docs_source = (
            REPOSITORY / "src/services/solar_os_docs.c"
        ).read_text(encoding="utf-8")
        match = re.search(
            r"^#define DOCS_PAGE_MAX \((\d+)U \* 1024U\)$",
            docs_source,
            re.MULTILINE,
        )
        self.assertIsNotNone(match)
        assert match is not None
        self.assertEqual(
            generate_manual.RELEASE_PAGE_MAX,
            int(match.group(1)) * 1024,
        )

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
