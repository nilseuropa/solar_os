/*
 * SPDX-FileCopyrightText: 2022-2026 Espressif Systems (Shanghai) CO LTD
 * SPDX-License-Identifier: Apache-2.0
 *
 * Transfer functions from espressif/usb_host_hid 1.2.1. The additional
 * snippets retain the lifecycle anchors required by the SolarOS source patch.
 */
static void in_xfer_done(usb_transfer_t *in_xfer)
{
    assert(in_xfer);
    assert(in_xfer->context);

    hid_iface_t *iface = (hid_iface_t *) in_xfer->context;

    switch (in_xfer->status) {
    case USB_TRANSFER_STATUS_COMPLETED:
        // Notify user
        hid_host_user_interface_callback(iface, HID_HOST_INTERFACE_EVENT_INPUT_REPORT);
        // Relaunch transfer
        usb_host_transfer_submit(in_xfer);
        return;
    case USB_TRANSFER_STATUS_NO_DEVICE:
    case USB_TRANSFER_STATUS_CANCELED:
        // User is notified about device disconnection from usb_event_cb
        // No need to do anything
        return;
    default:
        // Any other error
        break;
    }

    ESP_LOGE(TAG, "Transfer failed, status %d", in_xfer->status);
    // Notify user about transfer or any other error
    hid_host_user_interface_callback(iface, HID_HOST_INTERFACE_EVENT_TRANSFER_ERROR);
}

static esp_err_t hid_control_transfer(hid_device_t *hid_device,
                                      size_t len,
                                      uint32_t timeout_ms)
{

    usb_transfer_t *ctrl_xfer = hid_device->ctrl_xfer;

    ctrl_xfer->device_handle = hid_device->dev_hdl;
    ctrl_xfer->callback = ctrl_xfer_done;
    ctrl_xfer->context = hid_device;
    ctrl_xfer->bEndpointAddress = 0;
    ctrl_xfer->timeout_ms = timeout_ms;
    ctrl_xfer->num_bytes = len;

    HID_RETURN_ON_ERROR( usb_host_transfer_submit_control(s_hid_driver->client_handle, ctrl_xfer),
                         "Unable to submit control transfer");

    BaseType_t received = xSemaphoreTake(hid_device->ctrl_xfer_done, pdMS_TO_TICKS(ctrl_xfer->timeout_ms));

    // In case transfer was not finished, error in USB LIB. This is EP0, USBH will reset the endpoint.
    HID_RETURN_ON_FALSE(received == pdTRUE, ESP_ERR_TIMEOUT, "Control transfer timeout");
    // Check transfer status
    // Device can return less data than requested, but it must not return more data than requested
    HID_RETURN_ON_FALSE(ctrl_xfer->actual_num_bytes <= ctrl_xfer->num_bytes, ESP_ERR_INVALID_RESPONSE, "Incorrect number of bytes transferred");

    ESP_LOG_BUFFER_HEXDUMP(TAG, ctrl_xfer->data_buffer, ctrl_xfer->actual_num_bytes, ESP_LOG_DEBUG);

    return ESP_OK;
}

/* Uninstall anchor (not compiled by the regression harness). */
    s_hid_driver = NULL;
    xSemaphoreGive(open_close_mutex); // Unblock any waiting tasks
    return ESP_OK;

/* Install-failure anchor (not compiled by the regression harness). */
    if (driver->all_events_handled) {
        vSemaphoreDelete(driver->all_events_handled);
    }
    free(driver);
    return ret;

/* Additional upstream anchors and functions used by setup-lifetime tests. */


    uint8_t dev_addr;                           /**< USB device address */

    SemaphoreHandle_t open_close_mutex;                         /**< Mutex to prevent race conditions during device open/close */

static StaticSemaphore_t s_open_close_mutex_buffer;

    driver->open_close_mutex = xSemaphoreCreateMutexStatic(&s_open_close_mutex_buffer);

    vSemaphoreDelete(s_hid_driver->all_events_handled);
    free(s_hid_driver);

static esp_err_t hid_host_device_disconnected(usb_device_handle_t dev_hdl)
{
    hid_device_t *hid_device = get_hid_device_by_handle(dev_hdl);
    HID_RETURN_ON_INVALID_ARG(hid_device);

    HID_ENTER_CRITICAL();
    hid_iface_t *hid_iface_curr;
    hid_iface_t *hid_iface_next;
    // Go through list
    hid_iface_curr = STAILQ_FIRST(&s_hid_driver->hid_ifaces_tailq);
    while (hid_iface_curr != NULL) {
        hid_iface_next = STAILQ_NEXT(hid_iface_curr, tailq_entry);
        HID_EXIT_CRITICAL();

        if (hid_iface_curr->parent && (hid_iface_curr->parent->dev_addr == hid_device->dev_addr)) {
            // Best-effort cleanup: an individual interface close failure must not
            // prevent the remaining interfaces or the final uninstall_device from
            // being processed. The device is already physically gone from the bus,
            // so we call the disconnect-specific close variant which swallows
            // endpoint errors; the graceful hid_host_device_close() path stays strict.
            esp_err_t close_err = hid_host_device_close_disconnect(hid_iface_curr);
            if (close_err != ESP_OK) {
                ESP_LOGW(TAG, "Close iface %d failed on disconnect (continuing): %s",
                         hid_iface_curr->dev_params.iface_num,
                         esp_err_to_name(close_err));
            }
        }
        HID_ENTER_CRITICAL();
        hid_iface_curr = hid_iface_next;
    }
    HID_EXIT_CRITICAL();

    // Delete HID compliant device — must run even if an individual close above failed,
    // otherwise the device context is leaked and cannot be recovered without a
    // physical re-plug of the USB device. Interfaces with a user callback remain
    // in hid_ifaces_tailq in HID_INTERFACE_STATE_WAIT_USER_DELETION awaiting the
    // user's second hid_host_device_close(); that is a normal state and must not
    // be altered here. The best-effort path inside hid_host_device_close_impl()
    // forces the state forward on disable/release failure, so by the time we reach
    // this point any interface that still references hid_device is already either
    // (a) owned by the user (WAIT_USER_DELETION) or (b) removed from the list.
    esp_err_t uninstall_err = hid_host_uninstall_device(hid_device);
    if (uninstall_err != ESP_OK) {
        ESP_LOGW(TAG, "Uninstall device failed on disconnect (continuing): %s",
                 esp_err_to_name(uninstall_err));
    }

    return ESP_OK;
}

esp_err_t hid_host_handle_events(uint32_t timeout)
{
    HID_RETURN_ON_FALSE(s_hid_driver != NULL,
                        ESP_ERR_INVALID_STATE,
                        "HID Driver is not installed");

    ESP_LOGD(TAG, "USB HID handling");
    s_hid_driver->event_handling_started = true;
    esp_err_t ret = usb_host_client_handle_events(s_hid_driver->client_handle, timeout);
    if (s_hid_driver->end_client_event_handling) {
        xSemaphoreGive(s_hid_driver->all_events_handled);
        return ESP_FAIL;
    }
    return ret;
}
