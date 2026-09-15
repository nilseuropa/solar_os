import importlib.util
import json
import re
import unittest
from pathlib import Path


REPOSITORY = Path(__file__).resolve().parents[1]
GENERATOR_PATH = REPOSITORY / "scripts/generate_manual.py"
SPEC = importlib.util.spec_from_file_location("generate_manual", GENERATOR_PATH)
generate_manual = importlib.util.module_from_spec(SPEC)
assert SPEC.loader is not None
SPEC.loader.exec_module(generate_manual)


class AgentReferenceTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.pages = generate_manual.load_pages(
            REPOSITORY / "doc/manual",
            REPOSITORY / "packages/solar_os_packages.toml",
        )
        cls.pages_by_id = {str(page["id"]): page for page in cls.pages}

    def language_pages(self, language):
        return [
            page for page in self.pages
            if page["id"] == language or str(page["id"]).startswith(language + ".")
        ]

    def test_python_and_lua_have_bounded_section_references(self):
        for language in ("python", "lua"):
            pages = self.language_pages(language)
            self.assertGreater(sum(len(page["agent_references"]) for page in pages), 20)
            for page in pages:
                self.assertTrue(page["agent_references"], page["id"])
                self.check_bounded_references(page)

    def check_bounded_references(self, page):
        references = page["agent_references"]
        body = str(page["body"]).encode("utf-8")
        for reference in references:
            offset = int(reference["offset"])
            length = int(reference["length"])
            self.assertGreater(length, 0)
            self.assertLessEqual(
                length,
                generate_manual.AGENT_REFERENCE_CHUNK_MAX,
            )
            body[offset : offset + length].decode("utf-8")

    def test_every_shared_service_is_present_in_each_language_reference(self):
        descriptor = (
            REPOSITORY / "src/apps/solar_os_script_api.inc"
        ).read_text(encoding="utf-8")
        modules = re.findall(
            r"^SOLAR_OS_SCRIPT_API_MODULE_BEGIN\((\w+)\);$",
            descriptor,
            re.MULTILINE,
        )
        self.assertEqual(len(modules), 47)

        for language in ("python", "lua"):
            reference_text = ""
            for page in self.language_pages(language):
                if page["id"] == language:
                    continue
                body = str(page["body"]).encode("utf-8")
                reference_text += b"".join(
                    body[
                        int(reference["offset"]) :
                        int(reference["offset"]) + int(reference["length"])
                    ]
                    for reference in page["agent_references"]
                ).decode("utf-8").casefold()
            for module in modules:
                self.assertIn(f"solaros.{module}", reference_text)

    def test_recent_runtime_features_have_focused_topics(self):
        python_topics = {
            str(reference["topic"])
            for page in self.language_pages("python")
            for reference in page["agent_references"]
        }
        lua_topics = {
            str(reference["topic"])
            for page in self.language_pages("lua")
            for reference in page["agent_references"]
        }
        self.assertIn("python.storage.files-and-imports", python_topics)
        self.assertIn("python.network.solaros-http", python_topics)
        self.assertIn("python.network.solaros-ftp", python_topics)
        self.assertIn("python.network.solaros-net", python_topics)
        self.assertIn("python.input.solaros-input", python_topics)
        self.assertIn("python.time.solaros-rtc", python_topics)
        self.assertIn("python.time.solaros-schedule", python_topics)
        self.assertIn("lua.network.http-requests", lua_topics)
        self.assertIn("lua.network.ftp-operations", lua_topics)
        self.assertIn("lua.network.managed-tcp-udp-and-websocket-clients", lua_topics)
        self.assertIn("lua.input.generic-pointer-and-axis-input", lua_topics)

    def test_three_largest_excerpts_fit_the_agent_tool_result(self):
        matches = []
        for page in self.language_pages("python") + self.language_pages("lua"):
            body = str(page["body"]).encode("utf-8")
            for reference in page["agent_references"]:
                offset = int(reference["offset"])
                length = int(reference["length"])
                matches.append(
                    {
                        "topic": reference["topic"],
                        "section": reference["section"],
                        "part": reference["part"],
                        "parts": reference["parts"],
                        "reference": body[offset : offset + length].decode("utf-8"),
                    }
                )
        largest = sorted(
            matches,
            key=lambda match: len(
                json.dumps(match, ensure_ascii=False).encode("utf-8")
            ),
            reverse=True,
        )[:3]
        worst_case = json.dumps(
            {"guidance": "x" * 700, "count": 3, "matches": largest},
            ensure_ascii=False,
            separators=(",", ":"),
        ).encode("utf-8")
        self.assertLess(len(worst_case), 4096)


if __name__ == "__main__":
    unittest.main()
