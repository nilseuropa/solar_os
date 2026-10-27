"""Exercise the production TCP/port/stream path over real loopback sockets."""
from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]


class TcpStreamTest(unittest.TestCase):
    def test_raw_socket_lifecycle(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory)
            for name, text in {
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
            }.items():
                target = path / name
                target.parent.mkdir(parents=True, exist_ok=True)
                target.write_text(text)
            binary = path / "test"
            subprocess.run([
                "cc", "-std=c11", "-D_POSIX_C_SOURCE=200809L", "-Wall", "-Wextra", "-Werror",
                "-fsanitize=address,undefined", "-fno-omit-frame-pointer", "-pthread",
                "-include", str(ROOT / "tests/host/compat.h"),
                "-I" + directory, "-I" + str(ROOT / "tests/host"),
                "-I" + str(ROOT / "src/services"),
                str(ROOT / "src/services/solar_os_tcp_stream.c"),
                str(ROOT / "src/services/solar_os_port.c"),
                str(ROOT / "src/services/solar_os_stream.c"),
                str(ROOT / "tests/host/tcp_stream_test.c"), "-o", str(binary),
            ], check=True)
            subprocess.run([str(binary)], check=True, timeout=15)


if __name__ == "__main__":
    unittest.main()
