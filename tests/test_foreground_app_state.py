import importlib.util
from pathlib import Path
import sys
import tempfile
import unittest


REPOSITORY = Path(__file__).resolve().parents[1]
CHECKER_PATH = REPOSITORY / "scripts" / "check_foreground_app_state.py"
SPEC = importlib.util.spec_from_file_location("check_foreground_app_state", CHECKER_PATH)
assert SPEC is not None and SPEC.loader is not None
check_foreground_app_state = importlib.util.module_from_spec(SPEC)
sys.modules[SPEC.name] = check_foreground_app_state
SPEC.loader.exec_module(check_foreground_app_state)


class ForegroundAppStatePolicyTest(unittest.TestCase):
    def test_cpp_foreground_apps_follow_the_same_state_policy(self):
        with tempfile.TemporaryDirectory() as directory:
            repository = Path(directory)
            source = repository / "src"
            source.mkdir()
            app = source / "app.cpp"
            app.write_text('static unsigned eager[16];\n'
                           'extern "C" const solar_os_app_t app = {};\n', encoding="utf-8")
            self.assertEqual(check_foreground_app_state.foreground_sources(repository), [app])
            self.assertEqual(len(check_foreground_app_state.check(repository)), 1)

    def test_foreground_apps_have_no_unreviewed_eager_mutable_state(self):
        self.assertEqual(check_foreground_app_state.check(REPOSITORY), [])

    def test_checker_rejects_eager_state_and_accepts_cold_pointer(self):
        fixture = [
            "static unsigned eager[16];",
            "static void *cold_state;",
        ]
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "fixture.c"
            path.write_text("\n".join(fixture), encoding="utf-8")
            self.assertEqual(
                check_foreground_app_state.violations(path),
                [(1, "static unsigned eager[16];")],
            )

    def test_foreground_launches_use_the_shared_lifecycle(self):
        self.assertEqual(
            check_foreground_app_state.lifecycle_bypasses(REPOSITORY), []
        )

    def test_backend_operation_hooks_are_not_app_lifecycle_bypasses(self):
        with tempfile.TemporaryDirectory() as directory:
            repository = Path(directory)
            source = repository / "src" / "services"
            source.mkdir(parents=True)
            (source / "backend.c").write_text(
                "backend.ops->start(backend.ctx);\n"
                "const solar_os_test_backend_t *ops = backend.ops;\n"
                "ops->stop();\n",
                encoding="utf-8",
            )
            self.assertEqual(
                check_foreground_app_state.lifecycle_bypasses(repository), []
            )

    def test_cpp_app_callbacks_cannot_bypass_the_shared_lifecycle(self):
        with tempfile.TemporaryDirectory() as directory:
            repository = Path(directory)
            source = repository / "src"
            source.mkdir()
            (source / "app.cpp").write_text(
                'const solar_os_app_t *app = lookup();\napp->start(ctx);\n',
                encoding="utf-8",
            )
            self.assertEqual(len(check_foreground_app_state.lifecycle_bypasses(repository)), 1)


if __name__ == "__main__":
    unittest.main()
