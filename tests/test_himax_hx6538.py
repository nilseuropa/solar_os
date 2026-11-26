from pathlib import Path
import re
import subprocess
import tempfile
import unittest
from tests.test_foreground_presenter import production_functions
from tests.test_shutdown_policy import run_c
from scripts.solaros_board_manifest import generate_header, load_board_manifest, load_driver_catalog

ROOT = Path(__file__).resolve().parents[1]


class HimaxHx6538Test(unittest.TestCase):
    def test_board_bindings_pass_the_runtime_validator(self):
        board = load_board_manifest(ROOT / "boards/manifests/seeed_sensecap_watcher.toml")
        header = generate_header(board, load_driver_catalog(ROOT / "boards/expansion_drivers.toml"))
        line = next(line for line in header.splitlines() if '.name = "camera0"' in line)
        bindings = re.search(r'\.bindings = \(const solar_os_expansion_binding_t\[\]\)(\{\{.*\}\})\}', line).group(1)
        driver_source = (ROOT / "src/services/solar_os_himax_hx6538.c").read_text()
        descriptor = driver_source[driver_source.index('#define LINE(key_)'):]
        validator = production_functions((ROOT / "src/services/solar_os_expansion.c").read_text(),
            ("binding_matches_spec", "binding_key", "set_binding_validation", "validate_driver_bindings"))
        run_c(r'''
#include <assert.h>
#include <string.h>
#include "solar_os_expansion.h"
static size_t strlcpy(char *to, const char *from, size_t size) {
    size_t n = strlen(from); if (size) { size_t copy = n < size - 1 ? n : size - 1;
    memcpy(to, from, copy); to[copy] = 0; } return n;
}
static esp_err_t attach(const char *n, const solar_os_expansion_binding_t *b, size_t c)
{ (void)n; (void)b; (void)c; return ESP_OK; }
static esp_err_t detach(const char *n) { (void)n; return ESP_OK; }
const char *solar_os_expansion_binding_kind_name(solar_os_expansion_binding_kind_t kind)
{ (void)kind; return "binding"; }
''' + descriptor + r'''
static bool binding_valid(const solar_os_expansion_binding_t *binding,
    const solar_os_expansion_binding_t *bindings, size_t count, bool board) {
    (void)binding; (void)bindings; (void)count; assert(board); return true;
}
''' + validator + '\nstatic const solar_os_expansion_binding_t bindings[] = ' + bindings + r''';
int main(void) {
    solar_os_expansion_binding_validation_t reason = {0};
    assert(validate_driver_bindings(&solar_os_himax_hx6538_expansion_driver, bindings,
        sizeof(bindings)/sizeof(bindings[0]), &reason, true) == ESP_OK);
}
''', "-I", str(ROOT / "src/services"), "-I", str(ROOT / "include"))

    def test_spi_framing_bus_sharing_and_error_release(self):
        source = production_functions((ROOT / "src/services/solar_os_himax_hx6538.c").read_text(),
                                      ("transfer", "packet", "write_bytes", "read_bytes"))
        fixture = (ROOT / "tests/host/himax_spi_test.c").read_text()
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "spi.c"
            path.write_text(fixture.replace("/* PRODUCTION_TRANSPORT */", source))
            binary = Path(directory) / "spi"
            subprocess.run(["cc", "-std=c11", "-Wall", "-Wextra", "-Werror",
                            "-fsanitize=address,undefined", "-no-pie",
                            "-I", str(ROOT / "tests/host"), str(path), "-o", str(binary)], check=True)
            subprocess.run([str(binary)], check=True)

    def test_sampling_protocol_and_bounded_buffers(self):
        with tempfile.TemporaryDirectory() as directory:
            binary = Path(directory) / "himax"
            subprocess.run([
                "cc", "-std=c11", "-Wall", "-Wextra", "-Werror",
                "-fsanitize=address,undefined", "-no-pie",
                "-I", str(ROOT / "tests/host/himax_stubs"),
                "-I", str(ROOT / "tests/host"),
                "-I", str(ROOT / "src/drivers"), "-I", str(ROOT / "src/services"),
                str(ROOT / "tests/host/himax_hx6538_test.c"),
                str(ROOT / "src/drivers/himax_hx6538.c"),
                str(ROOT / "src/services/solar_os_json_scan.c"),
                "-lcrypto", "-o", str(binary),
            ], check=True)
            subprocess.run([str(binary)], check=True)
