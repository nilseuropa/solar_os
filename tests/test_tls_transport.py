from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]


class TlsTransportTest(unittest.TestCase):
    def test_verified_connect_partial_transfer_timeout_and_cancel(self):
        with tempfile.TemporaryDirectory() as directory:
            stage = Path(directory)
            (stage / 'esp_tls.h').write_text('''
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <sys/types.h>
#include "esp_err.h"
typedef struct esp_tls esp_tls_t;
typedef struct {
    esp_err_t (*crt_bundle_attach)(void *config);
    bool non_block;
    int timeout_ms;
} esp_tls_cfg_t;
#define ESP_TLS_ERR_SSL_WANT_READ (-10)
#define ESP_TLS_ERR_SSL_WANT_WRITE (-11)
esp_tls_t *esp_tls_init(void);
int esp_tls_conn_destroy(esp_tls_t *);
void *esp_tls_get_ssl_context(esp_tls_t *);
int esp_tls_conn_new_async(const char *, int, int, const esp_tls_cfg_t *, esp_tls_t *);
ssize_t esp_tls_conn_read(esp_tls_t *, void *, size_t);
ssize_t esp_tls_conn_write(esp_tls_t *, const void *, size_t);
''')
            (stage / 'esp_crt_bundle.h').write_text(
                '#include "esp_err.h"\nesp_err_t esp_crt_bundle_attach(void *);\n')
            (stage / 'mbedtls').mkdir()
            (stage / 'mbedtls/ssl.h').write_text('''
#pragma once
typedef struct esp_tls mbedtls_ssl_context;
int mbedtls_ssl_get_max_out_record_payload(const mbedtls_ssl_context *);
''')
            (stage / 'esp_timer.h').write_text('#include <stdint.h>\nint64_t esp_timer_get_time(void);\n')
            binary = stage / 'tls'
            host = ROOT / 'tests/host'
            services = ROOT / 'src/services'
            subprocess.run(['cc', '-std=c11', '-D_GNU_SOURCE', '-Wall', '-Wextra', '-Werror',
                '-fsanitize=address,undefined', '-fno-pie', '-no-pie',
                '-I', directory, '-I', str(host),
                '-I', str(ROOT / 'include'), '-I', str(ROOT / 'src'), '-I', str(services),
                str(host / 'tls_transport_test.c'), str(services / 'solar_os_tls.c'),
                '-o', str(binary)], check=True)
            subprocess.run([str(binary)], check=True, timeout=20)
