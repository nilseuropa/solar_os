import re
from pathlib import Path
import unittest


REPOSITORY = Path(__file__).resolve().parents[1]
PLAYGROUND = (REPOSITORY / "src/services/solar_os_playground.c").read_text(
    encoding="utf-8"
)
PLAYGROUND_HEADER = (
    REPOSITORY / "src/services/solar_os_playground.h"
).read_text(encoding="utf-8")
SHELL = (REPOSITORY / "src/apps/solar_os_shell.c").read_text(encoding="utf-8")


class PlaygroundAliasPolicyTest(unittest.TestCase):
    def test_registry_is_separate_and_service_managed(self):
        self.assertIn('#define PLAYGROUND_ALIAS_DIR ".shell"', PLAYGROUND)
        self.assertIn('#define PLAYGROUND_ALIAS_FILE "playground"', PLAYGROUND)
        self.assertIn("# managed by Playground; do not edit", PLAYGROUND)
        self.assertGreaterEqual(PLAYGROUND.count("playground_sync_aliases();"), 4)

    def test_aliases_launch_installed_manifests_without_catalog(self):
        self.assertIn(
            "solar_os_playground_find_installed_app(", PLAYGROUND_HEADER
        )
        launch = re.search(
            r"static bool shell_launch_playground_script\(.*?\n}\n#endif",
            SHELL,
            re.DOTALL,
        )
        self.assertIsNotNone(launch)
        self.assertIn("solar_os_playground_find_installed_app", launch.group(0))
        self.assertNotIn("solar_os_playground_catalog_available", launch.group(0))
        self.assertIn('"%s playground run %s\\n"', PLAYGROUND)

    def test_user_alias_file_is_read_before_playground_aliases(self):
        iterator = re.search(
            r"static bool shell_for_each_alias\(.*?\n}\n\nstatic bool shell_append_token",
            SHELL,
            re.DOTALL,
        )
        self.assertIsNotNone(iterator)
        body = iterator.group(0)
        self.assertLess(
            body.index("SHELL_ALIAS_FILE"),
            body.index("SHELL_PLAYGROUND_ALIAS_FILE"),
        )


if __name__ == "__main__":
    unittest.main()
