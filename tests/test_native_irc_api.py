from pathlib import Path
import subprocess
import tempfile
import unittest

from test_foreground_presenter import production_functions


ROOT = Path(__file__).resolve().parents[1]
HOST = ROOT / "tests/host"
SERVICES = ROOT / "src/services"


class NativeIrcApiTest(unittest.TestCase):
    def test_profile_updates_and_worker_lifecycle(self):
        with tempfile.TemporaryDirectory() as directory:
            tmp = Path(directory)
            source = (SERVICES / "solar_os_irc.c").read_text()
            functions = (
                "irc_wipe", "irc_config_alloc", "irc_config_free", "irc_secret_valid",
                "irc_config_valid", "irc_get_config_locked", "solar_os_irc_get_config",
                "irc_save", "irc_configure_locked", "solar_os_irc_configure", "irc_patch_copy",
                "irc_apply_update", "solar_os_irc_update", "solar_os_irc_start",
                "solar_os_irc_running", "solar_os_irc_stop_checked",
            )
            (tmp / "irc_core.inc").write_text(production_functions(source, functions))
            binary = tmp / "irc"
            subprocess.run([
                "cc", "-std=c11", "-D_GNU_SOURCE", "-Wall", "-Wextra", "-Werror", "-pthread",
                "-fsanitize=address,undefined", "-fno-pie", "-no-pie",
                "-DSOLAR_OS_PACKAGE_SERVICE_IRC=1", "-include", str(HOST / "compat.h"),
                "-I", directory, "-I", str(HOST / "native_abi_stubs"), "-I", str(HOST),
                "-I", str(ROOT / "include"), "-I", str(ROOT / "src"), "-I", str(SERVICES),
                str(HOST / "native_irc_api_test.c"), str(SERVICES / "solar_os_native_irc.c"),
                str(SERVICES / "solar_os_irc_protocol.c"), "-o", str(binary),
            ], check=True)
            subprocess.run([str(binary)], check=True, timeout=20)
