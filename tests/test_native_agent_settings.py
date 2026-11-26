from pathlib import Path
import subprocess
import tempfile
import unittest


ROOT = Path(__file__).resolve().parents[1]
HOST = ROOT / "tests/host"


class NativeAgentSettingsTest(unittest.TestCase):
    def test_shared_agent_services(self):
        with tempfile.TemporaryDirectory() as directory:
            binary = Path(directory) / "agent"
            subprocess.run([
                "cc", "-std=c11", "-D_GNU_SOURCE", "-Wall", "-Wextra", "-Werror", "-pthread",
                "-fsanitize=address,undefined", "-fno-pie", "-no-pie",
                "-ffunction-sections", "-fdata-sections", "-Wl,--gc-sections",
                "-DSOLAR_OS_PACKAGE_SERVICE_AGENT=1",
                "-DESP_ERR_NOT_ALLOWED=0x10d",
                "-include", str(HOST / "compat.h"),
                "-I", str(HOST / "native_abi_stubs"),
                "-I", str(HOST / "http_server_stubs"), "-I", str(HOST),
                "-I", str(ROOT / "include"), "-I", str(ROOT / "src"),
                "-I", str(ROOT / "src/services"),
                str(HOST / "native_agent_settings_test.c"),
                str(ROOT / "src/services/solar_os_native_agent.c"),
                str(ROOT / "src/services/solar_os_agent_conversation.c"),
                str(ROOT / "src/services/solar_os_native_agent_run.c"),
                str(ROOT / "src/services/solar_os_script_agent.c"),
                str(ROOT / "src/services/solar_os_native_handles.c"),
                str(ROOT / "src/services/solar_os_agent_images.c"),
                str(ROOT / "src/services/solar_os_agent_tool_policy.c"),
                str(ROOT / "src/services/solar_os_media_sample.c"),
                "-o", str(binary),
            ], check=True)
            subprocess.run([str(binary)], check=True, timeout=15)
