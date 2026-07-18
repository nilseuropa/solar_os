#include "solar_os_clipboard.h"

#include <string.h>

#include "solar_os_memory.h"

static char *clipboard_data;
static size_t clipboard_len;

esp_err_t solar_os_clipboard_set(const char *data, size_t len)
{
    if (len > SOLAR_OS_CLIPBOARD_MAX_BYTES) {
        return ESP_ERR_INVALID_SIZE;
    }
    if (len > 0 && data == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    char *next = NULL;
    if (len > 0) {
        next = solar_os_memory_alloc(len + 1,
                                     SOLAR_OS_MEMORY_EXTERNAL_PREFERRED,
                                     "clipboard");
        if (next == NULL) {
            return ESP_ERR_NO_MEM;
        }
        memcpy(next, data, len);
        next[len] = '\0';
    }

    solar_os_memory_free(clipboard_data);
    clipboard_data = next;
    clipboard_len = len;
    return ESP_OK;
}

const char *solar_os_clipboard_data(size_t *len)
{
    if (len != NULL) {
        *len = clipboard_len;
    }
    return clipboard_data;
}

size_t solar_os_clipboard_size(void)
{
    return clipboard_len;
}

void solar_os_clipboard_clear(void)
{
    solar_os_memory_free(clipboard_data);
    clipboard_data = NULL;
    clipboard_len = 0;
}
