from pathlib import Path
import unittest


ROOT = Path(__file__).resolve().parents[1]
REQUIRED = (
    "CONFIG_LWIP_PPP_SUPPORT=y",
    "CONFIG_LWIP_PPP_ENABLE_IPV4=y",
    "CONFIG_LWIP_PPP_NOTIFY_PHASE_SUPPORT=y",
    "CONFIG_LWIP_PPP_PAP_SUPPORT=y",
    "CONFIG_LWIP_PPP_CHAP_SUPPORT=y",
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


if __name__ == "__main__":
    unittest.main()
