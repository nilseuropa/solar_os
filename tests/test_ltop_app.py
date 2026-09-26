import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
SOURCE = (ROOT / "src/apps/solar_os_ltop.c").read_text(encoding="utf-8")
REGISTRY = (ROOT / "src/apps/solar_os_app_registry.c").read_text(
    encoding="utf-8"
)
PACKAGES = (ROOT / "packages/solar_os_packages.toml").read_text(encoding="utf-8")
MANUAL = (ROOT / "doc/manual/apps.md").read_text(encoding="utf-8")


class LiveTopAppTest(unittest.TestCase):
    def test_ltop_is_a_resumable_text_app_in_every_flavor(self):
        self.assertIn(
            'members = ["core_runtime", "core_shell", "app_ltop"',
            PACKAGES,
        )
        self.assertIn("[packages.app_ltop]", PACKAGES)
        self.assertIn('sources = ["apps/solar_os_ltop.c"]', PACKAGES)
        entry = next(
            line for line in REGISTRY.splitlines() if 'APP_ENTRY("ltop"' in line
        )
        for capability in (
            "SOLAR_OS_APP_CAP_TEXT",
            "SOLAR_OS_APP_CAP_DISPLAY",
            "SOLAR_OS_APP_CAP_PORT",
        ):
            self.assertIn(capability, entry)
        self.assertIn(".app_class = SOLAR_OS_APP_CLASS_TUI", SOURCE)
        self.assertIn(".flags = SOLAR_OS_APP_FLAG_RESUMABLE", SOURCE)

    def test_ltop_uses_interval_deltas_and_per_core_idle_load(self):
        self.assertIn("uxTaskGetSystemState", SOURCE)
        self.assertIn("ltop_counter_delta", SOURCE)
        self.assertIn(
            'snprintf(expected, sizeof(expected), "IDLE%d", core)',
            SOURCE,
        )
        self.assertIn("solar_os_tui_progress_bar", SOURCE)
        self.assertIn("solar_os_memory_get_status", SOURCE)
        self.assertIn('"IRAM"', SOURCE)
        self.assertIn('"ERAM"', SOURCE)
        self.assertIn("uxTaskGetSnapshotAll", SOURCE)
        self.assertIn("stack_bytes - minimum_free", SOURCE)
        self.assertIn('"STACK"', SOURCE)
        self.assertIn("show_priority = cols >= 32U", SOURCE)
        self.assertIn('"%u tasks | CPU %u.%u%%"', SOURCE)
        self.assertNotIn("%% capacity", SOURCE)
        self.assertIn("LTOP_SAMPLE_INTERVAL_MS 1000U", SOURCE)
        self.assertIn("qsort(ltop.tasks", SOURCE)

    def test_manual_explains_dual_core_percentage_semantics(self):
        self.assertIn("## ltop", MANUAL)
        self.assertIn("reports the interval deltas", MANUAL)
        self.assertIn("task total can approach 200%", MANUAL)
        self.assertIn("IRAM and ERAM progress bars", MANUAL)
        self.assertIn("peak stack consumption", MANUAL)


if __name__ == "__main__":
    unittest.main()
