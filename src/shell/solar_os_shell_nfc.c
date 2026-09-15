#include "solar_os_shell_commands.h"
#include "solar_os_shell_common.h"
#include "solar_os_shell_io.h"

#include <stdlib.h>
#include <string.h>

#include "solar_os_nfc.h"
#include "solar_os_shell.h"

static solar_os_shell_io_t *terminal(solar_os_context_t *ctx)
{
    return solar_os_shell_command_io(ctx);
}

static const char *default_name(void)
{
    static solar_os_nfc_info_t info;
    return solar_os_nfc_get(0U, &info) ? info.name : NULL;
}

void solar_os_shell_cmd_nfc(solar_os_context_t *ctx, int argc, char **argv)
{
    solar_os_shell_io_t *term = terminal(ctx);
    if (argc == 1 || (argc == 2 && strcmp(argv[1], "list") == 0)) {
        solar_os_nfc_info_t info;
        for (size_t i = 0; solar_os_nfc_get(i, &info); i++) {
            solar_os_shell_io_printf(term, "%s  %s\r\n", info.name, info.driver);
        }
        return;
    }
    if (argc < 2 || argc > 4 || strcmp(argv[1], "scan") != 0) {
        solar_os_shell_io_writeln(term, "usage: nfc [list] | nfc scan [name] [timeout-ms]");
        return;
    }
    const char *name = argc >= 3 ? argv[2] : default_name();
    const uint32_t timeout_ms = argc == 4 ? (uint32_t)strtoul(argv[3], NULL, 0) : 1000U;
    if (name == NULL || timeout_ms == 0U) {
        solar_os_shell_io_writeln(term, "nfc: no reader or invalid timeout");
        return;
    }
    solar_os_nfc_tag_t tag;
    const esp_err_t ret = solar_os_nfc_scan(name, timeout_ms, &tag);
    if (ret != ESP_OK) {
        solar_os_shell_io_printf(term, "nfc: %s\r\n", esp_err_to_name(ret));
        return;
    }
    solar_os_shell_io_printf(term, "%s NFC-A uid=", name);
    for (size_t i = 0; i < tag.uid_len; i++) {
        solar_os_shell_io_printf(term, "%02X", tag.uid[i]);
    }
    solar_os_shell_io_printf(term,
                             " atqa=%02X%02X sak=%02X\r\n",
                             tag.atqa[0], tag.atqa[1], tag.sak);
}
