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

    def test_python_and_lua_have_bounded_section_references(self):
        for language in ("python", "lua"):
            page = self.pages_by_id[language]
            references = page["agent_references"]
            self.assertGreater(len(references), 20)
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
        self.assertEqual(len(modules), 36)

        for language in ("python", "lua"):
            page = self.pages_by_id[language]
            body = str(page["body"]).encode("utf-8")
            reference_text = b"".join(
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
            for reference in self.pages_by_id["python"]["agent_references"]
        }
        lua_topics = {
            str(reference["topic"])
            for reference in self.pages_by_id["lua"]["agent_references"]
        }
        self.assertIn("python.files-and-imports", python_topics)
        self.assertIn("python.solaros-http", python_topics)
        self.assertIn("python.solaros-net", python_topics)
        self.assertIn("lua.http-requests", lua_topics)
        self.assertIn("lua.managed-tcp-udp-and-websocket-clients", lua_topics)

    def test_three_largest_excerpts_fit_the_agent_tool_result(self):
        matches = []
        for language in ("python", "lua"):
            page = self.pages_by_id[language]
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
