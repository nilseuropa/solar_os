#include <stddef.h>

#include "solar_os_native_abi.h"

static size_t text_length(const char *text)
{
    size_t length = 0U;
    while (text[length] != '\0') {
        length++;
    }
    return length;
}

static int write_text(const solar_os_native_host_api_v1_t *host, const char *text)
{
    return host->write_utf8(text, text_length(text));
}

int main(int argc, char **argv)
{
    const solar_os_native_host_api_v1_t *host = solar_os_native_host_v1();
    if (host == NULL ||
        host->abi_version != SOLAR_OS_NATIVE_ABI_VERSION ||
        host->struct_size < offsetof(solar_os_native_host_api_v1_t, get_service) ||
        host->write_utf8 == NULL) {
        return 1;
    }

    if (write_text(host, "Hello from a SolarOS native module") != 0) {
        return 2;
    }
    if (argc > 1 && argv != NULL && argv[1] != NULL) {
        if (write_text(host, ", ") != 0 || write_text(host, argv[1]) != 0) {
            return 2;
        }
    }
    return write_text(host, "!\n") == 0 ? 0 : 2;
}
