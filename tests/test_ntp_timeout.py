"""The NTP wait must outlast one lost SNTP or DNS exchange in lwIP."""
from pathlib import Path
import re
import unittest

ROOT = Path(__file__).resolve().parents[1]

# ESP-IDF 5.5 defaults (no sdkconfig.defaults file sets them):
# CONFIG_LWIP_SNTP_STARTUP_DELAY=y, CONFIG_LWIP_SNTP_MAXIMUM_STARTUP_DELAY=5000
# delay the first request after sntp_init by a random 0..5000 ms (lwip sntp.c sntp_init).
SNTP_STARTUP_DELAY_MAX_MS = 5000
# lwIP sntp_opts.h: SNTP_RECV_TIMEOUT 15000, SNTP_RETRY_TIMEOUT = SNTP_RECV_TIMEOUT.
# A lost reply, or a failed lookup, waits this long before lwIP asks again.
SNTP_RETRY_MS = 15000
# lwIP dns.c with DNS_MAX_RETRIES 4 and DNS_TMR_INTERVAL 1000: sends at 0, 1, 2 and 4 s,
# and reports a failed lookup at 7 s when only one DNS server is set.
DNS_FAIL_MS = 7000


def default_timeout_ms() -> int:
    header = (ROOT / "src" / "services" / "solar_os_time.h").read_text(encoding="utf-8")
    match = re.search(r"#define SOLAR_OS_NTP_DEFAULT_TIMEOUT_MS (\d+)U?", header)
    assert match is not None
    return int(match.group(1))


class NtpTimeoutTest(unittest.TestCase):
    def test_default_wait_covers_one_lost_exchange(self) -> None:
        # Worst first try: the full start delay, then a lookup that fails, then
        # lwIP's retry. The second try needs a little time on top.
        self.assertGreater(default_timeout_ms(),
                           SNTP_STARTUP_DELAY_MAX_MS + DNS_FAIL_MS + SNTP_RETRY_MS)


if __name__ == "__main__":
    unittest.main()
