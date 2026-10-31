from pathlib import Path
import subprocess
import tempfile
import unittest
from test_foreground_presenter import production_functions

ROOT = Path(__file__).resolve().parents[1]


class DiscoveryUrlTest(unittest.TestCase):
    def test_maximum_domain_and_small_output(self):
        source = (ROOT / "src/services/solar_os_sc2_discover.c").read_text()
        functions = production_functions(source, ("discovery_url", "solar_os_sc2_discover_domain_valid"))
        size = source.split("#define SC2_DISCOVERY_URL_SIZE", 1)[1].split("\n\n", 1)[0]
        with tempfile.TemporaryDirectory() as directory:
            probe = Path(directory, "url.c")
            probe.write_text('''#include <stdio.h>
#include <stdbool.h>
#include <string.h>
#include <assert.h>
#include "esp_err.h"
#include "sc2_relaykey.h"
#define SC2_DISCOVERY_URL_SIZE ''' + size + "\n" + functions + '''
int main(void) {
    char domain[SC2_DESC_HOST_MAX + 1];
    memset(domain, 'a', sizeof(domain));
    strcpy(domain + SC2_DESC_HOST_MAX - 4, ".com");
    assert(solar_os_sc2_discover_domain_valid(domain));
    char url[SC2_DISCOVERY_URL_SIZE];
    assert(discovery_url(domain, url, sizeof(url)) == ESP_OK);
    assert(strlen(url) == strlen(domain) + strlen("https:///.well-known/solaros-relay"));
    assert(strcmp(url + 8 + strlen(domain), "/.well-known/solaros-relay") == 0);
    assert(discovery_url(domain, url, sizeof(url) - 1) == ESP_ERR_INVALID_SIZE);
    assert(discovery_url("a.com", url, sizeof(url)) == ESP_OK);
    assert(!solar_os_sc2_discover_domain_valid("localhost"));
}
''')
            binary = Path(directory, "url")
            subprocess.run(["cc", "-std=c11", "-Wall", "-Wextra", "-Werror",
                            "-I" + str(ROOT / "tests/host"),
                            "-I" + str(ROOT / "protocol/sc2/include"),
                            str(probe), "-o", str(binary)], check=True)
            subprocess.run([str(binary)], check=True)
