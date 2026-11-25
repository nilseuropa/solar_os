#include "hid_port.h"

/* USB Serial/JTAG and boards without native USB cannot send HID reports. */
bool hid_port_available(void)
{
    return false;
}

bool hid_port_is_connected(void)
{
    return false;
}

esp_err_t hid_port_send_report(uint8_t report_id, const void *report, size_t report_len)
{
    (void)report_id;
    (void)report;
    (void)report_len;
    return ESP_ERR_NOT_SUPPORTED;
}
