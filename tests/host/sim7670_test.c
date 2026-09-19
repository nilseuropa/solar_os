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
    char requests[32][SIM7670_COMMAND_MAX + 3U];
    size_t request_count;
    bool sim_missing;
    bool pdp_active;
    bool packet_attached;
    bool reject_last_pdn_deactivation;
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
    assert(transport->request_count < 32U);
    memcpy(transport->requests[transport->request_count],
           transport->request,
           len + 1U);
    transport->request_count++;
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
    } else if (strcmp(transport->request, "AT+CGACT?\r\n") == 0) {
        transport->response = transport->pdp_active
            ? "\r\n+CGACT: 1,1\r\n\r\nOK\r\n"
            : "\r\n+CGACT: 1,0\r\n\r\nOK\r\n";
    } else if (strcmp(transport->request, "AT+CGNSSINFO\r\n") == 0) {
        transport->response =
            "\r\n+CGNSSINFO: 2,09,05,00,00,3113.330650,N,"
            "12121.262554,E,131117,091918.00,32.9,0.0,255.0,"
            "1.1,0.8,0.7,14\r\n\r\nOK\r\n";
    } else if (strncmp(transport->request, "AT+CGDCONT=", 11U) == 0 ||
               strncmp(transport->request, "AT+CGAUTH=", 10U) == 0 ||
               strncmp(transport->request, "AT+CPIN=", 8U) == 0) {
        transport->response = "\r\nOK\r\n";
    } else if (strcmp(transport->request, "AT+CGATT=1\r\n") == 0) {
        transport->packet_attached = true;
        transport->response = "\r\nOK\r\n";
    } else if (strcmp(transport->request, "AT+CGATT=0\r\n") == 0) {
        transport->packet_attached = false;
        transport->pdp_active = false;
        transport->response = "\r\nOK\r\n";
    } else if (strcmp(transport->request, "AT+CGACT=1,1\r\n") == 0) {
        transport->pdp_active = true;
        transport->response = "\r\nOK\r\n";
    } else if (strcmp(transport->request, "AT+CGACT=0,1\r\n") == 0) {
        if (transport->reject_last_pdn_deactivation) {
            transport->response =
                "\r\n+CME ERROR: Last PDN disconnection not allowed\r\n";
        } else {
            transport->pdp_active = false;
            transport->response = "\r\nOK\r\n";
        }
    } else if (strcmp(transport->request, "ATD*99#\r\n") == 0) {
        transport->response = "\r\nCONNECT\r\n";
    } else if (strcmp(transport->request, "ATD*98#\r\n") == 0) {
        transport->response = "\r\nNO CARRIER\r\n";
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
    assert(status.data_status_valid);
    assert(!status.data_active);

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
    assert(status.data_status_valid);

    sim7670_registration_t registration;
    assert(sim7670_parse_cereg("\r\n+CEREG: 1\r\nOK\r\n", &registration));
    assert(registration == SIM7670_REGISTRATION_HOME);
    bool active = false;
    assert(sim7670_parse_cgact(
        "\r\n+CGACT: 1,0\r\n+CGACT: 2,1\r\nOK\r\n", 2U, &active));
    assert(active);
    assert(!sim7670_parse_cgact("+CGACT: 1,0\r\nOK\r\n", 2U, &active));

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

    assert(sim7670_parse_cgnssinfo(
        "\r\n+CGNSSINFO: 2,09,05,00,00,3113.330650,N,12121.262554,E,"
        "131117,091918.00,32.9,0.0,255.0,1.1,0.8,0.7,14\r\nOK\r\n",
        &fix));
    assert(fix.valid && fix.fix_type == 2U && fix.satellites == 14U);
    assert(fix.latitude_deg_e7 == 312221775);
    assert(fix.longitude_deg_e7 == 1213543759);
    assert(fix.time_valid);
    assert(fix.year == 2017U && fix.month == 11U && fix.day == 13U);
    assert(fix.hour == 9U && fix.minute == 19U && fix.second == 18U);
    assert(fix.height_msl_mm == 32900);
    assert(fix.heading_deg_e5 == 25500000);
    assert(fix.position_dop_e2 == 110U);
    assert(sim7670_parse_cgnssinfo(
        "+CGNSSINFO: ,,,,,,,,,,,,,,,,,\r\nOK\r\n",
        &fix));
    assert(!fix.valid && fix.fix_type == 0U && fix.satellites == 0U);

    size_t request = transport.request_count;
    assert(sim7670_read_gnss_fix(&modem, 10000U, &fix) == ESP_OK);
    assert(strcmp(transport.requests[request++], "AT+CGNSSINFO\r\n") == 0);
    assert(fix.valid && fix.satellites == 14U);

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
    assert(sim7670_command(&modem,
                           "ATD*98#",
                           1000U,
                           response,
                           sizeof(response)) == ESP_FAIL);

    request = transport.request_count;
    assert(sim7670_configure_pdp(&modem,
                                 "5g.vodafone.iot",
                                 SIM7670_PDP_IPV4V6,
                                 SIM7670_AUTH_PAP,
                                 "device-user",
                                 "device-pass") == ESP_OK);
    assert(strcmp(transport.requests[request++],
                  "AT+CGDCONT=1,\"IPV4V6\",\"5g.vodafone.iot\"\r\n") == 0);
    assert(strcmp(transport.requests[request++],
                  "AT+CGAUTH=1,1,\"device-pass\",\"device-user\"\r\n") == 0);
    assert(sim7670_configure_pdp(&modem,
                                 "bad\"apn",
                                 SIM7670_PDP_IPV4,
                                 SIM7670_AUTH_NONE,
                                 "",
                                 "") == ESP_ERR_INVALID_ARG);
    assert(sim7670_set_packet_attached(&modem, true) == ESP_OK);
    assert(transport.packet_attached);
    assert(sim7670_set_pdp_active(&modem, true) == ESP_OK);
    assert(transport.pdp_active);
    assert(transport.packet_attached);
    assert(sim7670_read_status(&modem, &status) == ESP_OK);
    assert(status.data_status_valid && status.data_active);
    assert(sim7670_set_pdp_active(&modem, false) == ESP_OK);
    assert(!transport.pdp_active);
    transport.pdp_active = true;
    transport.reject_last_pdn_deactivation = true;
    request = transport.request_count;
    assert(sim7670_set_pdp_active(&modem, false) == ESP_OK);
    assert(strcmp(transport.requests[request++], "AT+CGACT=0,1\r\n") == 0);
    assert(strcmp(transport.requests[request++], "AT+CGATT=0\r\n") == 0);
    assert(!transport.packet_attached);
    assert(!transport.pdp_active);
    transport.reject_last_pdn_deactivation = false;
    assert(sim7670_enter_data_mode(&modem) == ESP_OK);
    assert(strcmp(transport.requests[transport.request_count - 1U],
                  "ATD*99#\r\n") == 0);
    assert(sim7670_unlock_sim(&modem, "1234") == ESP_OK);
    assert(sim7670_unlock_sim(&modem, "12x4") == ESP_ERR_INVALID_ARG);
    assert(sim7670_clear_pdp(&modem) == ESP_OK);

    puts("SIM7670 tests: ok");
    return 0;
}
