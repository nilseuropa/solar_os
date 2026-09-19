#include "solar_os_shell_commands.h"
#include "solar_os_shell_common.h"
#include "solar_os_shell_io.h"

#include <errno.h>
#include <inttypes.h>
#include <stdlib.h>
#include <string.h>

#include "esp_attr.h"
#include "solar_os_modem.h"
#include "solar_os_shell.h"

static solar_os_shell_io_t *terminal(solar_os_context_t *ctx)
{
    return solar_os_shell_command_io(ctx);
}

static void secure_zero(void *data, size_t size)
{
    volatile unsigned char *bytes = data;
    while (size-- > 0U) {
        *bytes++ = 0U;
    }
}

static const char *default_name(void)
{
    static EXT_RAM_BSS_ATTR solar_os_modem_info_t info;
    return solar_os_modem_get(0U, &info) ? info.name : NULL;
}

static void print_error(solar_os_shell_io_t *term,
                        const char *operation,
                        esp_err_t error)
{
    solar_os_shell_io_printf(term,
                             "modem %s: %s\r\n",
                             operation,
                             esp_err_to_name(error));
}

static void print_usage(solar_os_shell_io_t *term)
{
    solar_os_shell_io_writeln(term, "usage:");
    solar_os_shell_io_writeln(term, "  modem [list]");
    solar_os_shell_io_writeln(term, "  modem status [name]");
    solar_os_shell_io_writeln(term, "  modem power <on|off> [name]");
    solar_os_shell_io_writeln(term, "  modem reset [name]");
    solar_os_shell_io_writeln(term, "  modem baud [name] [auto|rate]");
    solar_os_shell_io_writeln(term, "  modem profile show [name]");
    solar_os_shell_io_writeln(
        term,
        "  modem profile set [name] --apn <apn> [--dns <ipv4|auto>]");
    solar_os_shell_io_writeln(
        term,
        "      [--ip ipv4|ipv6|ipv4v6]");
    solar_os_shell_io_writeln(
        term,
        "      [--auth none|pap|chap|auto] [--user <user> --password <password>]");
    solar_os_shell_io_writeln(term, "  modem profile clear [name]");
    solar_os_shell_io_writeln(term, "  modem connect [name]");
    solar_os_shell_io_writeln(term, "  modem disconnect [name]");
    solar_os_shell_io_writeln(term, "  modem sim unlock <pin> [name]");
    solar_os_shell_io_writeln(
        term,
        "  modem at <quoted-command> [name] [timeout-ms]");
}

static void list_modems(solar_os_shell_io_t *term)
{
    if (solar_os_modem_count() == 0U) {
        solar_os_shell_io_writeln(term, "no cellular modems registered");
        return;
    }
    solar_os_modem_info_t info;
    for (size_t i = 0; solar_os_modem_get(i, &info); i++) {
        solar_os_shell_io_printf(term,
                                 "%s  driver=%s  transport=%s  power=%s  reset=%s\r\n",
                                 info.name,
                                 info.driver,
                                 info.transport,
                                 info.power_control
                                     ? (info.powered ? "on" : "off")
                                     : "always-on",
                                 info.reset_control ? "yes" : "no");
    }
}

static void show_status(solar_os_shell_io_t *term,
                        int argc,
                        char **argv)
{
    if (argc > 3) {
        solar_os_shell_io_writeln(term, "usage: modem status [name]");
        return;
    }
    const char *name = argc == 3 ? argv[2] : default_name();
    if (name == NULL) {
        solar_os_shell_io_writeln(term, "modem: no device");
        return;
    }
    solar_os_modem_status_t status;
    const esp_err_t ret = solar_os_modem_get_status(name, &status);
    if (ret != ESP_OK) {
        print_error(term, "status", ret);
        return;
    }
    const char *sim_state = status.sim_status_valid
        ? (status.sim_ready ? "ready" : "not-ready")
        : "unknown";
    const char *registration = status.registration_status_valid
        ? solar_os_modem_registration_name(status.registration)
        : "unknown";
    const char *data = status.data_status_valid
        ? (status.data_active ? "active" : "inactive")
        : "unknown";
    solar_os_shell_io_printf(term,
                             "%s power=%s online=%s sim=%s registration=%s data=%s network=%s\r\n",
                             name,
                             status.power_control
                                 ? (status.powered ? "on" : "off")
                                 : "always-on",
                             status.online ? "yes" : "no",
                             sim_state,
                             registration,
                             data,
                             status.network_status_valid
                                 ? solar_os_modem_network_state_name(
                                       status.network_state)
                                 : "unknown");
    if (status.signal_status_valid && status.rssi_valid) {
        solar_os_shell_io_printf(term, "rssi=%d dBm ", status.rssi_dbm);
    } else {
        solar_os_shell_io_write(term, "rssi=unknown ");
    }
    if (status.signal_status_valid && status.bit_error_rate != 99U) {
        solar_os_shell_io_printf(term, "ber=%u\r\n", status.bit_error_rate);
    } else {
        solar_os_shell_io_writeln(term, "ber=unknown");
    }
    if (status.network_status_valid &&
        status.network_state == SOLAR_OS_MODEM_NETWORK_UP) {
        solar_os_shell_io_printf(term,
                                 "interface=%s ipv4=%s gateway=%s dns=%s\r\n",
                                 status.network_interface[0] != '\0'
                                     ? status.network_interface : "unknown",
                                 status.ipv4_address[0] != '\0'
                                     ? status.ipv4_address : "unknown",
                                 status.ipv4_gateway[0] != '\0'
                                     ? status.ipv4_gateway : "unknown",
                                 status.dns_address[0] != '\0'
                                     ? status.dns_address : "unknown");
    }
}

static void set_power(solar_os_shell_io_t *term,
                      int argc,
                      char **argv)
{
    if (argc != 3 && argc != 4) {
        solar_os_shell_io_writeln(term,
                                  "usage: modem power <on|off> [name]");
        return;
    }
    bool enabled;
    if (strcmp(argv[2], "on") == 0) {
        enabled = true;
    } else if (strcmp(argv[2], "off") == 0) {
        enabled = false;
    } else {
        solar_os_shell_io_writeln(term,
                                  "usage: modem power <on|off> [name]");
        return;
    }
    const char *name = argc == 4 ? argv[3] : default_name();
    if (name == NULL) {
        solar_os_shell_io_writeln(term, "modem: no device");
        return;
    }
    const esp_err_t ret = solar_os_modem_set_power(name, enabled);
    if (ret == ESP_OK) {
        solar_os_shell_io_printf(term,
                                 "%s power=%s\r\n",
                                 name,
                                 enabled ? "on" : "off");
    } else {
        print_error(term, "power", ret);
    }
}

static void reset_modem(solar_os_shell_io_t *term,
                        int argc,
                        char **argv)
{
    if (argc > 3) {
        solar_os_shell_io_writeln(term, "usage: modem reset [name]");
        return;
    }
    const char *name = argc == 3 ? argv[2] : default_name();
    if (name == NULL) {
        solar_os_shell_io_writeln(term, "modem: no device");
        return;
    }
    const esp_err_t ret = solar_os_modem_reset(name);
    if (ret == ESP_OK) {
        solar_os_shell_io_printf(term, "%s reset=complete\r\n", name);
    } else {
        print_error(term, "reset", ret);
    }
}

static bool parse_transport_rate(const char *text, uint32_t *rate)
{
    if (strcmp(text, "auto") == 0) {
        *rate = 0U;
        return true;
    }
    errno = 0;
    char *end = NULL;
    const unsigned long parsed = strtoul(text, &end, 10);
    if (errno != 0 || end == text || *end != '\0' || parsed == 0UL ||
        parsed > UINT32_MAX) {
        return false;
    }
    *rate = (uint32_t)parsed;
    return true;
}

static void show_transport_rate(solar_os_shell_io_t *term, const char *name)
{
    solar_os_modem_transport_rate_info_t info;
    const esp_err_t ret = solar_os_modem_transport_rate_get(name, &info);
    if (ret != ESP_OK) {
        print_error(term, "baud", ret);
        return;
    }
    solar_os_shell_io_printf(term,
                             "%s baud=%" PRIu32 " configured=",
                             name,
                             info.active_rate);
    if (info.automatic) {
        solar_os_shell_io_write(term, "auto");
    } else {
        solar_os_shell_io_printf(term, "%" PRIu32, info.configured_rate);
    }
    solar_os_shell_io_write(term, " supported=");
    for (size_t i = 0U; i < info.supported_rate_count; i++) {
        solar_os_shell_io_printf(term,
                                 "%s%" PRIu32,
                                 i == 0U ? "" : ",",
                                 info.supported_rates[i]);
    }
    solar_os_shell_io_write(term, "\r\n");
}

static void handle_transport_rate(solar_os_shell_io_t *term,
                                  int argc,
                                  char **argv)
{
    if (argc < 2 || argc > 4) {
        solar_os_shell_io_writeln(term,
                                  "usage: modem baud [name] [auto|rate]");
        return;
    }
    const char *name = NULL;
    uint32_t rate = 0U;
    bool set = false;
    if (argc == 2) {
        name = default_name();
    } else if (argc == 3 && parse_transport_rate(argv[2], &rate)) {
        name = default_name();
        set = true;
    } else {
        name = argv[2];
        if (argc == 4) {
            set = parse_transport_rate(argv[3], &rate);
            if (!set) {
                solar_os_shell_io_writeln(
                    term,
                    "usage: modem baud [name] [auto|rate]");
                return;
            }
        }
    }
    if (name == NULL) {
        solar_os_shell_io_writeln(term, "modem: no device");
        return;
    }
    if (set) {
        const esp_err_t ret = solar_os_modem_transport_rate_set(name, rate);
        if (ret != ESP_OK) {
            print_error(term, "baud", ret);
            return;
        }
    }
    show_transport_rate(term, name);
}

static bool parse_profile_options(int argc,
                                  char **argv,
                                  size_t first_option,
                                  solar_os_modem_profile_t *profile)
{
    enum {
        SEEN_APN = 1U << 0,
        SEEN_IP = 1U << 1,
        SEEN_AUTH = 1U << 2,
        SEEN_USER = 1U << 3,
        SEEN_PASSWORD = 1U << 4,
        SEEN_DNS = 1U << 5,
    };
    unsigned seen = 0U;
    *profile = (solar_os_modem_profile_t) {
        .ip_type = SOLAR_OS_MODEM_IP_IPV4V6,
        .auth = SOLAR_OS_MODEM_AUTH_NONE,
    };
    for (size_t i = first_option; i < (size_t)argc; i += 2U) {
        if (i + 1U >= (size_t)argc) {
            return false;
        }
        const char *option = argv[i];
        const char *value = argv[i + 1U];
        unsigned flag = 0U;
        if (strcmp(option, "--apn") == 0) {
            flag = SEEN_APN;
            if (strlcpy(profile->apn, value, sizeof(profile->apn)) >=
                sizeof(profile->apn)) {
                return false;
            }
        } else if (strcmp(option, "--dns") == 0) {
            flag = SEEN_DNS;
            if (strcmp(value, "auto") != 0 &&
                strlcpy(profile->dns, value, sizeof(profile->dns)) >=
                    sizeof(profile->dns)) {
                return false;
            }
        } else if (strcmp(option, "--ip") == 0) {
            flag = SEEN_IP;
            if (!solar_os_modem_ip_type_parse(value, &profile->ip_type)) {
                return false;
            }
        } else if (strcmp(option, "--auth") == 0) {
            flag = SEEN_AUTH;
            if (!solar_os_modem_auth_parse(value, &profile->auth)) {
                return false;
            }
        } else if (strcmp(option, "--user") == 0) {
            flag = SEEN_USER;
            if (strlcpy(profile->username,
                        value,
                        sizeof(profile->username)) >=
                sizeof(profile->username)) {
                return false;
            }
        } else if (strcmp(option, "--password") == 0) {
            flag = SEEN_PASSWORD;
            if (strlcpy(profile->password,
                        value,
                        sizeof(profile->password)) >=
                sizeof(profile->password)) {
                return false;
            }
        } else {
            return false;
        }
        if ((seen & flag) != 0U) {
            return false;
        }
        seen |= flag;
    }
    return (seen & SEEN_APN) != 0U &&
        solar_os_modem_profile_validate(profile) == ESP_OK;
}

static void show_profile(solar_os_shell_io_t *term, const char *name)
{
    solar_os_modem_profile_t profile;
    const esp_err_t ret = solar_os_modem_profile_get(name, &profile);
    if (ret == ESP_ERR_NOT_FOUND) {
        solar_os_shell_io_printf(term, "%s profile=unset\r\n", name);
        return;
    }
    if (ret != ESP_OK) {
        print_error(term, "profile show", ret);
        return;
    }
    solar_os_shell_io_printf(term,
                             "%s apn=%s dns=%s ip=%s auth=%s\r\n",
                             name,
                             profile.apn,
                             profile.dns[0] != '\0' ? profile.dns : "auto",
                             solar_os_modem_ip_type_name(profile.ip_type),
                             solar_os_modem_auth_name(profile.auth));
    solar_os_shell_io_printf(term,
                             "username=%s password=%s\r\n",
                             profile.username[0] != '\0'
                                 ? profile.username
                                 : "none",
                             profile.password[0] != '\0' ? "set" : "none");
    secure_zero(&profile, sizeof(profile));
}

static void handle_profile(solar_os_shell_io_t *term,
                           int argc,
                           char **argv)
{
    if (argc < 3) {
        print_usage(term);
        return;
    }
    if (strcmp(argv[2], "show") == 0) {
        if (argc > 4) {
            solar_os_shell_io_writeln(term,
                                      "usage: modem profile show [name]");
            return;
        }
        const char *name = argc == 4 ? argv[3] : default_name();
        if (name == NULL) {
            solar_os_shell_io_writeln(term, "modem: no device");
            return;
        }
        show_profile(term, name);
        return;
    }
    if (strcmp(argv[2], "clear") == 0) {
        if (argc > 4) {
            solar_os_shell_io_writeln(term,
                                      "usage: modem profile clear [name]");
            return;
        }
        const char *name = argc == 4 ? argv[3] : default_name();
        if (name == NULL) {
            solar_os_shell_io_writeln(term, "modem: no device");
            return;
        }
        const esp_err_t ret = solar_os_modem_profile_clear(name);
        if (ret == ESP_OK) {
            solar_os_shell_io_printf(term, "%s profile=cleared\r\n", name);
        } else {
            print_error(term, "profile clear", ret);
        }
        return;
    }
    if (strcmp(argv[2], "set") == 0) {
        if (argc < 5) {
            print_usage(term);
            return;
        }
        size_t first_option = 3U;
        const char *name = NULL;
        if (strncmp(argv[first_option], "--", 2U) != 0) {
            name = argv[first_option++];
        } else {
            name = default_name();
        }
        solar_os_modem_profile_t profile;
        if (name == NULL ||
            !parse_profile_options(argc, argv, first_option, &profile)) {
            secure_zero(&profile, sizeof(profile));
            solar_os_shell_io_writeln(term,
                                      "modem profile set: invalid profile");
            print_usage(term);
            return;
        }
        const esp_err_t ret = solar_os_modem_profile_set(name, &profile);
        secure_zero(&profile, sizeof(profile));
        if (ret == ESP_OK) {
            solar_os_shell_io_printf(term,
                                     "%s profile=saved applied=yes\r\n",
                                     name);
        } else {
            print_error(term, "profile set", ret);
        }
        return;
    }
    print_usage(term);
}

static void set_data_active(solar_os_shell_io_t *term,
                            int argc,
                            char **argv,
                            bool active)
{
    if (argc > 3) {
        solar_os_shell_io_printf(term,
                                 "usage: modem %s [name]\r\n",
                                 active ? "connect" : "disconnect");
        return;
    }
    const char *name = argc == 3 ? argv[2] : default_name();
    if (name == NULL) {
        solar_os_shell_io_writeln(term, "modem: no device");
        return;
    }
    const esp_err_t ret = solar_os_modem_set_data_active(name, active);
    if (ret == ESP_OK) {
        solar_os_modem_status_t status;
        const esp_err_t status_ret = solar_os_modem_get_status(name, &status);
        if (status_ret == ESP_OK && status.network_status_valid) {
            solar_os_shell_io_printf(
                term,
                "%s network=%s%s%s\r\n",
                name,
                solar_os_modem_network_state_name(status.network_state),
                status.ipv4_address[0] != '\0' ? " ipv4=" : "",
                status.ipv4_address[0] != '\0' ? status.ipv4_address : "");
        } else {
            solar_os_shell_io_printf(term,
                                     "%s data=%s\r\n",
                                     name,
                                     active ? "active" : "inactive");
        }
    } else if (ret == ESP_ERR_NOT_FOUND && active) {
        solar_os_shell_io_writeln(
            term,
            "modem connect: no saved profile; run 'modem profile set' first");
    } else {
        print_error(term, active ? "connect" : "disconnect", ret);
    }
}

static void unlock_sim(solar_os_shell_io_t *term,
                       int argc,
                       char **argv)
{
    if (argc != 4 && argc != 5) {
        solar_os_shell_io_writeln(term,
                                  "usage: modem sim unlock <pin> [name]");
        return;
    }
    const char *name = argc == 5 ? argv[4] : default_name();
    if (name == NULL) {
        solar_os_shell_io_writeln(term, "modem: no device");
        return;
    }
    const esp_err_t ret = solar_os_modem_unlock_sim(name, argv[3]);
    if (ret == ESP_OK) {
        solar_os_shell_io_printf(term, "%s sim=unlocked\r\n", name);
    } else {
        print_error(term, "sim unlock", ret);
    }
}

static void send_at(solar_os_shell_io_t *term,
                    int argc,
                    char **argv)
{
    if (argc < 3 || argc > 5) {
        solar_os_shell_io_writeln(
            term,
            "usage: modem at <quoted-command> [name] [timeout-ms]");
        return;
    }
    const char *name = argc >= 4 ? argv[3] : default_name();
    const uint32_t timeout_ms = argc == 5
        ? (uint32_t)strtoul(argv[4], NULL, 0)
        : 5000U;
    if (name == NULL || timeout_ms == 0U) {
        solar_os_shell_io_writeln(term,
                                  "modem: no device or invalid timeout");
        return;
    }
    static EXT_RAM_BSS_ATTR char response[512];
    const esp_err_t ret = solar_os_modem_command(name,
                                                 argv[2],
                                                 timeout_ms,
                                                 response,
                                                 sizeof(response));
    if (response[0] != '\0') {
        solar_os_shell_io_write(term, response);
        const size_t len = strlen(response);
        if (len == 0U || response[len - 1U] != '\n') {
            solar_os_shell_io_write(term, "\r\n");
        }
    }
    if (ret != ESP_OK) {
        print_error(term, "at", ret);
    }
}

void solar_os_shell_cmd_modem(solar_os_context_t *ctx, int argc, char **argv)
{
    solar_os_shell_io_t *term = terminal(ctx);
    if (argc == 1 || (argc == 2 && strcmp(argv[1], "list") == 0)) {
        list_modems(term);
    } else if (strcmp(argv[1], "status") == 0) {
        show_status(term, argc, argv);
    } else if (strcmp(argv[1], "power") == 0) {
        set_power(term, argc, argv);
    } else if (strcmp(argv[1], "reset") == 0) {
        reset_modem(term, argc, argv);
    } else if (strcmp(argv[1], "baud") == 0) {
        handle_transport_rate(term, argc, argv);
    } else if (strcmp(argv[1], "profile") == 0) {
        handle_profile(term, argc, argv);
    } else if (strcmp(argv[1], "connect") == 0) {
        set_data_active(term, argc, argv, true);
    } else if (strcmp(argv[1], "disconnect") == 0) {
        set_data_active(term, argc, argv, false);
    } else if (strcmp(argv[1], "sim") == 0 && argc >= 3 &&
               strcmp(argv[2], "unlock") == 0) {
        unlock_sim(term, argc, argv);
    } else if (strcmp(argv[1], "at") == 0) {
        send_at(term, argc, argv);
    } else {
        print_usage(term);
    }
}
