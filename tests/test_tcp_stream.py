"""Exercise the production TCP/port/stream path over real loopback sockets."""
from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]


def tcp_headers(path, with_semaphores=True):
    headers = {
        "lwip/sockets.h": "#include <sys/socket.h>\n",
        "lwip/inet.h": "#include <arpa/inet.h>\n",
        "lwip/tcp.h": "#include <netinet/tcp.h>\n",
        "lwip/netdb.h": "#include <netdb.h>\n",
        "esp_netif.h": "typedef struct esp_netif esp_netif_t;\n",
        "freertos/semphr.h": """#pragma once
    #include <pthread.h>
    #include <stdlib.h>
    #define portMAX_DELAY 0xffffffffU
    typedef pthread_mutex_t StaticSemaphore_t;
    typedef StaticSemaphore_t *SemaphoreHandle_t;
    static inline SemaphoreHandle_t xSemaphoreCreateMutexStatic(StaticSemaphore_t *s)
    { pthread_mutex_init(s, NULL); return s; }
    static inline SemaphoreHandle_t xSemaphoreCreateMutex(void)
    { SemaphoreHandle_t s=malloc(sizeof(*s)); return s ? xSemaphoreCreateMutexStatic(s) : NULL; }
    static inline int xSemaphoreTake(SemaphoreHandle_t s, unsigned timeout)
    { (void)timeout; return pthread_mutex_lock(s)==0; }
    static inline int xSemaphoreGive(SemaphoreHandle_t s) { return pthread_mutex_unlock(s)==0; }
    """,
    }
    for name, text in headers.items():
        if not with_semaphores and name.startswith("freertos/"): continue
        target = path / name
        target.parent.mkdir(parents=True, exist_ok=True)
        target.write_text(text)


class TcpStreamTest(unittest.TestCase):
    def run_fixture(self, fixture="tcp_stream_test.c", extra_sources=()):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory)
            tcp_headers(path)
            binary = path / "test"
            subprocess.run([
                "cc", "-std=c11", "-D_POSIX_C_SOURCE=200809L", "-Wall", "-Wextra", "-Werror",
                "-fsanitize=address,undefined", "-fno-omit-frame-pointer", "-pthread",
                "-DSOLAR_OS_PACKAGE_SERVICE_TCP_STREAM=1",
                "-include", str(ROOT / "tests/host/compat.h"),
                "-I" + directory, "-I" + str(ROOT / "tests/host"),
                "-I" + str(ROOT / "src"), "-I" + str(ROOT / "src/services"), "-I" + str(ROOT / "include"),
                str(ROOT / "src/services/solar_os_tcp_stream.c"),
                str(ROOT / "src/services/solar_os_port.c"),
                str(ROOT / "src/services/solar_os_stream.c"),
                str(ROOT / "tests/host" / fixture),
                *[str(ROOT / "src/services" / source) for source in extra_sources], "-o", str(binary),
            ], check=True)
            subprocess.run([str(binary)], check=True, timeout=15)

    def test_raw_socket_lifecycle(self):
        self.run_fixture()


if __name__ == "__main__":
    unittest.main()
