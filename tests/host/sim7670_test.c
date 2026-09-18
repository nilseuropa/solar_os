#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "sim7670.h"

typedef struct {
    const char *response;
    size_t offset;
    char request[SIM7670_COMMAND_MAX + 3U];
    bool sim_missing;
    unsigned at_failures_remaining;
} fake_transport_t;

static esp_err_t fake_write(void *user,
                            const uint8_t *data,
                            size_t len,
                            size_t *written)
{
    fake_transport_t *transport = user;
    assert(len < sizeof(transport->request));
    memcpy(transport->request, data, len);
    transport->request[len] = '\0';
    transport->offset = 0U;
    if (strcmp(transport->request, "AT\r\n") == 0) {
        if (transport->at_failures_remaining > 0U) {
            transport->at_failures_remaining--;
            transport->response = "\r\nERROR\r\n";
        } else {
            transport->response = "\r\nAT\r\n\r\nOK\r\n";
        }
    } else if (strcmp(transport->request, "AT+CPIN?\r\n") == 0) {
        transport->response = transport->sim_missing
            ? "\r\n+CME ERROR: SIM not inserted\r\n"
            : "\r\n+CPIN: READY\r\n\r\nOK\r\n";
    } else if (strcmp(transport->request, "AT+CEREG?\r\n") == 0) {
        transport->response = "\r\n+CEREG: 0,5\r\n\r\nOK\r\n";
    } else if (strcmp(transport->request, "AT+CSQ\r\n") == 0) {
        transport->response = "\r\n+CSQ: 18,3\r\n\r\nOK\r\n";
    } else {
        transport->response = "\r\nERROR\r\n";
    }
    *written = len;
    return ESP_OK;
}

static esp_err_t fake_read(void *user,
                           uint8_t *data,
                           size_t len,
                           uint32_t timeout_ms,
                           size_t *read_len)
{
    (void)timeout_ms;
    fake_transport_t *transport = user;
    if (transport->response == NULL) {
        *read_len = 0U;
        return ESP_OK;
    }
    const size_t remaining = strlen(transport->response) - transport->offset;
    size_t copy = remaining < len ? remaining : len;
    if (copy > 5U) {
        copy = 5U;
    }
    memcpy(data, transport->response + transport->offset, copy);
    transport->offset += copy;
    *read_len = copy;
    if (transport->response[transport->offset] == '\0') {
        transport->response = NULL;
        transport->offset = 0U;
    }
    return ESP_OK;
}

int main(void)
{
    fake_transport_t transport = {0};
    const sim7670_io_t io = {
        .write = fake_write,
        .read = fake_read,
        .user = &transport,
    };
    sim7670_t modem;
    assert(sim7670_init(&modem, &io) == ESP_OK);

    sim7670_status_t status;
    assert(sim7670_read_status(&modem, &status) == ESP_OK);
    assert(status.online);
    assert(status.sim_status_valid);
    assert(status.sim_ready);
    assert(status.registration_status_valid);
    assert(status.registration == SIM7670_REGISTRATION_ROAMING);
    assert(status.signal_status_valid);
    assert(status.rssi_valid);
    assert(status.rssi_dbm == -77);
    assert(status.bit_error_rate == 3U);

    transport.sim_missing = true;
    transport.at_failures_remaining = 1U;
    assert(sim7670_read_status(&modem, &status) == ESP_OK);
    assert(status.online);
    assert(!status.sim_status_valid);
    assert(!status.sim_ready);
    assert(status.registration_status_valid);
    assert(status.registration == SIM7670_REGISTRATION_ROAMING);
    assert(status.signal_status_valid);
    assert(status.rssi_valid);

    sim7670_registration_t registration;
    assert(sim7670_parse_cereg("\r\n+CEREG: 1\r\nOK\r\n", &registration));
    assert(registration == SIM7670_REGISTRATION_HOME);

    sim7670_gnss_fix_t fix;
    assert(sim7670_parse_cgpsinfo(
        "\r\n+CGPSINFO: 3112.3456,N,12130.0000,E,180926,123456.0,12.3,0.0,90.0\r\nOK\r\n",
        &fix));
    assert(fix.valid);
    assert(fix.latitude_deg_e7 == 312057600);
    assert(fix.longitude_deg_e7 == 1215000000);
    assert(fix.time_valid);
    assert(fix.year == 2026U && fix.month == 9U && fix.day == 18U);
    assert(fix.hour == 12U && fix.minute == 34U && fix.second == 56U);
    assert(fix.height_msl_mm == 12300);

    assert(sim7670_parse_cgpsinfo("+CGPSINFO: ,,,,,,,,\r\nOK\r\n", &fix));
    assert(!fix.valid);

    char response[64];
    assert(sim7670_command(&modem,
                           "AT+UNKNOWN",
                           1000U,
                           response,
                           sizeof(response)) == ESP_FAIL);
    assert(sim7670_command(&modem,
                           "BAD",
                           1000U,
                           response,
                           sizeof(response)) == ESP_ERR_INVALID_ARG);

    puts("SIM7670 tests: ok");
    return 0;
}
