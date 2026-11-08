#!/usr/bin/env python3
"""Build a copy of usb_host_hid 1.2.1 with lifecycle and transfer fixes.

SolarOS joins its connection worker before uninstalling the HID driver. Once
the driver's event task has also exited, no application open/close waiters can
remain. Version 1.2.1 omits deletion of open_close_mutex on uninstall and on
install failure; repeated USB role switches would otherwise leak it.
Its control path also omits the completion-status check, allowing a stalled
SET_PROTOCOL request to be reported as successful. Preserve that failure.
Report a failed interrupt-transfer resubmission instead of silently leaving
the interface active without any transfer queued.
Defer disconnect teardown while SolarOS configures an interface, so control
buffers and cached descriptors remain alive until setup has finished.
The managed component and its checksums are left unchanged.
"""

from pathlib import Path
import re
import sys


def patched_source(source: str) -> str:
    status_check = """    // Check transfer status
    // Device can return less data than requested, but it must not return more data than requested"""
    resubmit = """        // Relaunch transfer
        usb_host_transfer_submit(in_xfer);"""
    uninstall = """    s_hid_driver = NULL;
    xSemaphoreGive(open_close_mutex); // Unblock any waiting tasks
    return ESP_OK;"""
    install_failure = """    if (driver->all_events_handled) {
        vSemaphoreDelete(driver->all_events_handled);
    }
    free(driver);
    return ret;"""
    if (source.count(uninstall) != 1 or source.count(install_failure) != 1 or
            source.count(status_check) != 1 or source.count(resubmit) != 1):
        raise ValueError("usb_host_hid source changed; review the SolarOS HID patch")
    source = source.replace(status_check, """    // Check transfer status before interpreting the returned byte count.
    HID_RETURN_ON_FALSE(ctrl_xfer->status != USB_TRANSFER_STATUS_STALL,
                        ESP_ERR_NOT_SUPPORTED, "HID control request stalled");
    HID_RETURN_ON_FALSE(ctrl_xfer->status == USB_TRANSFER_STATUS_COMPLETED,
                        ESP_ERR_INVALID_RESPONSE, "HID control transfer failed");
    // Device can return less data than requested, but it must not return more data than requested""")
    source = source.replace(resubmit, """        // Relaunch transfer; a failed submission leaves no polling queued.
        const esp_err_t ret = usb_host_transfer_submit(in_xfer);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "Unable to relaunch input transfer: %s", esp_err_to_name(ret));
            hid_host_user_interface_callback(iface, HID_HOST_INTERFACE_EVENT_TRANSFER_ERROR);
        }""")
    source = source.replace(uninstall, """    s_hid_driver = NULL;
    /* SolarOS has joined all application users before uninstall. */
    vSemaphoreDelete(open_close_mutex);
    return ESP_OK;""")
    source = source.replace(install_failure, """    if (driver->all_events_handled) {
        vSemaphoreDelete(driver->all_events_handled);
    }
    if (driver->open_close_mutex) {
        vSemaphoreDelete(driver->open_close_mutex);
    }
    if (driver->solar_os_setup_mutex) {
        vSemaphoreDelete(driver->solar_os_setup_mutex);
    }
    free(driver);
    return ret;""")
    return patch_setup_lifetime(source)


def patch_setup_lifetime(source: str) -> str:
    replacements = {
        "    uint8_t dev_addr;                           /**< USB device address */":
        "    uint8_t dev_addr;                           /**< USB device address */\n    bool solar_os_disconnect_pending;",
        "    SemaphoreHandle_t open_close_mutex;":
        "    SemaphoreHandle_t solar_os_setup_mutex;\n    SemaphoreHandle_t open_close_mutex;",
        "static StaticSemaphore_t s_open_close_mutex_buffer;": """static StaticSemaphore_t s_open_close_mutex_buffer;
static StaticSemaphore_t s_solar_os_setup_mutex_buffer;

void solar_os_usb_hid_setup_begin(void)
{
    (void)xSemaphoreTake(s_hid_driver->solar_os_setup_mutex, portMAX_DELAY);
}

void solar_os_usb_hid_setup_end(void)
{
    xSemaphoreGive(s_hid_driver->solar_os_setup_mutex);
    (void)usb_host_client_unblock(s_hid_driver->client_handle);
}""",
        "    driver->open_close_mutex = xSemaphoreCreateMutexStatic(&s_open_close_mutex_buffer);":
        "    driver->solar_os_setup_mutex = xSemaphoreCreateMutexStatic(&s_solar_os_setup_mutex_buffer);\n"
        "    driver->open_close_mutex = xSemaphoreCreateMutexStatic(&s_open_close_mutex_buffer);",
        "    vSemaphoreDelete(s_hid_driver->all_events_handled);\n    free(s_hid_driver);":
        "    vSemaphoreDelete(s_hid_driver->all_events_handled);\n"
        "    vSemaphoreDelete(s_hid_driver->solar_os_setup_mutex);\n    free(s_hid_driver);",
        "    esp_err_t ret = usb_host_client_handle_events(s_hid_driver->client_handle, timeout);":
        "    esp_err_t ret = usb_host_client_handle_events(s_hid_driver->client_handle, timeout);\n"
        "    solar_os_hid_finish_disconnects();",
    }
    match = re.search(r"^static esp_err_t hid_host_device_disconnected\([^;]*?\n\{.*?^\}",
                      source, re.M | re.S)
    if match is None or any(source.count(anchor) != 1 for anchor in replacements):
        raise ValueError("usb_host_hid disconnect lifecycle changed; review the SolarOS setup patch")
    original = match.group()
    replacement = original.replace("hid_host_device_disconnected(", "solar_os_hid_disconnect_impl(", 1)
    replacement += """

static esp_err_t hid_host_device_disconnected(usb_device_handle_t dev_hdl)
{
    /* Never block the HID event task on an application control request: it
     * must still deliver that request's completion, including NO_DEVICE. */
    if (xSemaphoreTake(s_hid_driver->solar_os_setup_mutex, 0) != pdTRUE) {
        hid_device_t *device = get_hid_device_by_handle(dev_hdl);
        if (device != NULL) device->solar_os_disconnect_pending = true;
        return ESP_OK;
    }
    esp_err_t ret = solar_os_hid_disconnect_impl(dev_hdl);
    xSemaphoreGive(s_hid_driver->solar_os_setup_mutex);
    return ret;
}

static void solar_os_hid_finish_disconnects(void)
{
    if (xSemaphoreTake(s_hid_driver->solar_os_setup_mutex, 0) != pdTRUE) return;
    for (;;) {
        usb_device_handle_t handle = NULL;
        HID_ENTER_CRITICAL();
        hid_device_t *device;
        STAILQ_FOREACH(device, &s_hid_driver->hid_devices_tailq, tailq_entry) {
            if (device->solar_os_disconnect_pending) {
                device->solar_os_disconnect_pending = false;
                handle = device->dev_hdl;
                break;
            }
        }
        HID_EXIT_CRITICAL();
        if (handle == NULL) break;
        (void)solar_os_hid_disconnect_impl(handle);
    }
    xSemaphoreGive(s_hid_driver->solar_os_setup_mutex);
}"""
    source = source.replace(original, replacement)
    for anchor, replacement in replacements.items():
        source = source.replace(anchor, replacement)
    return source


if __name__ == "__main__":
    src, dest = map(Path, sys.argv[1:])
    result = patched_source(src.read_text())
    if not dest.exists() or dest.read_text() != result:
        dest.write_text(result)
