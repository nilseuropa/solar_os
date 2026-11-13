import configparser
from pathlib import Path
import shlex
import unittest


ROOT = Path(__file__).resolve().parents[1]
REQUIRED = {
    "CONFIG_LWIP_PPP_SUPPORT": "y",
    "CONFIG_LWIP_PPP_ENABLE_IPV4": "y",
    "CONFIG_LWIP_PPP_NOTIFY_PHASE_SUPPORT": "y",
    "CONFIG_LWIP_PPP_PAP_SUPPORT": "y",
    "CONFIG_LWIP_PPP_CHAP_SUPPORT": "y",
    "CONFIG_LWIP_PPP_SERVER_SUPPORT": "y",
    "CONFIG_LWIP_PPP_VJ_HEADER_COMPRESSION": "n",
    "CONFIG_LWIP_ENABLE_LCP_ECHO": "y",
    "CONFIG_ESP_NETIF_SET_DNS_PER_DEFAULT_NETIF": "y",
}


class PppSdkconfigTest(unittest.TestCase):
    def test_every_environment_can_host_runtime_modem_ppp(self):
        config = configparser.ConfigParser(interpolation=None)
        config.read(ROOT / "platformio.ini")
        environments = [name for name in config if name.startswith("env:")]
        self.assertTrue(environments)
        common_args = config.get("env", "board_build.cmake_extra_args", fallback="")
        for environment in environments:
            defaults = ["sdkconfig.defaults"]
            args = config.get(environment, "board_build.cmake_extra_args",
                              fallback=common_args)
            for arg in shlex.split(args):
                if arg.startswith("-DSDKCONFIG_DEFAULTS="):
                    defaults = arg.split("=", 1)[1].split(";")
            with self.subTest(environment=environment, defaults=defaults):
                settings = {}
                # Later defaults override earlier values, including explicit disables.
                for name in defaults:
                    contents = (ROOT / name).read_text(encoding="utf-8")
                    for line in contents.splitlines():
                        line = line.strip()
                        if line.startswith("CONFIG_") and "=" in line:
                            key, value = line.split("=", 1)
                            settings[key] = value
                        elif line.startswith("# CONFIG_") and line.endswith(" is not set"):
                            settings[line[2:-len(" is not set")]] = "n"
                for key, value in REQUIRED.items():
                    self.assertEqual(settings.get(key), value, key)

    def test_sim7670_requires_generic_ppp_service(self):
        catalog = (ROOT / "packages" / "solar_os_packages.toml").read_text(
            encoding="utf-8"
        )
        sim7670 = catalog.split("[packages.sim7670]", 1)[1].split(
            "\n[packages.", 1
        )[0]
        self.assertIn('"service_ppp"', sim7670)

    def test_pppd_uses_generic_ppp_and_network_services(self):
        catalog = (ROOT / "packages" / "solar_os_packages.toml").read_text(
            encoding="utf-8"
        )
        pppd = catalog.split("[packages.job_pppd]", 1)[1].split(
            "\n[packages.", 1
        )[0]
        self.assertIn('"service_ppp"', pppd)
        self.assertIn('"service_network"', pppd)
        self.assertNotIn('"service_uart"', pppd)

        source = (ROOT / "src/jobs/solar_os_pppd_job.c").read_text(
            encoding="utf-8"
        )
        self.assertIn("PPPD_ROLE_DOWNSTREAM", source)
        self.assertIn("PPPD_ROLE_UPLINK", source)
        self.assertIn("PPPD_ROLE_PEER", source)
        self.assertIn("solar_os_jobs_claim_port", source)
        self.assertIn("solar_os_network_path_register", source)
        self.assertIn("solar_os_network_path_set_connecting", source)
        self.assertIn("solar_os_network_interface_publish", source)
        self.assertIn("solar_os_network_interface_remove", source)
        self.assertIn("esp_netif_napt_enable", source)
        self.assertIn("solar_os_ppp_start", source)
        self.assertIn("solar_os_task_create_pinned_external(pppd_task", source)
        self.assertIn("solar_os_task_delete_external(NULL);", source)
        self.assertIn(".worker_stack_external = true,", source)

        cmake = (ROOT / "CMakeLists.txt").read_text(encoding="utf-8")
        self.assertIn(
            'idf_build_set_property(COMPILE_DEFINITIONS "MEMP_NUM_PPP_PCB=2" APPEND)',
            cmake,
        )


if __name__ == "__main__":
    unittest.main()
