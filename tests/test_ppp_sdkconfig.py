from pathlib import Path
import unittest


ROOT = Path(__file__).resolve().parents[1]
REQUIRED = (
    "CONFIG_LWIP_PPP_SUPPORT=y",
    "CONFIG_LWIP_PPP_ENABLE_IPV4=y",
    "CONFIG_LWIP_PPP_NOTIFY_PHASE_SUPPORT=y",
    "CONFIG_LWIP_PPP_PAP_SUPPORT=y",
    "CONFIG_LWIP_PPP_CHAP_SUPPORT=y",
    "CONFIG_LWIP_PPP_SERVER_SUPPORT=y",
    "# CONFIG_LWIP_PPP_VJ_HEADER_COMPRESSION is not set",
    "CONFIG_LWIP_ENABLE_LCP_ECHO=y",
    "CONFIG_ESP_NETIF_SET_DNS_PER_DEFAULT_NETIF=y",
)


class PppSdkconfigTest(unittest.TestCase):
    def test_every_board_default_can_host_runtime_modem_ppp(self):
        defaults = sorted(ROOT.glob("sdkconfig.defaults*"))
        self.assertTrue(defaults)
        for path in defaults:
            contents = path.read_text(encoding="utf-8")
            with self.subTest(defaults=path.name):
                for setting in REQUIRED:
                    self.assertIn(setting, contents)

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
