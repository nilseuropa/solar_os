#!/usr/bin/env python3
"""Generate UFI/CBI hooks for pristine espressif/usb_host_msc 1.3.0."""
import hashlib
from pathlib import Path
import re
import sys

HASHES = {
    "msc_host.c": "20bca65a326e3c0ac13569734ff98d39ad0c7565a718fd28e2eb65f5a5e1f21c",
    "msc_scsi_bot.c": "2444d8083ec38bb4b9529c4248520f1eb5970656ab4b3bbf3c67cbfc2604e8c7",
}


def replace(source, old, new):
    if source.count(old) != 1:
        raise ValueError("MSC 1.3.0 patch anchor changed; review the UFI integration")
    return source.replace(old, new, 1)


def patched_source(name, source):
    if hashlib.sha256(source.encode()).hexdigest() != HASHES[name]:
        raise ValueError("usb_host_msc source changed; review the pinned UFI/CBI patch")
    source = replace(source, '#include "usb/msc_host.h"', '#include "usb/msc_host.h"\n#include "usb_ufi_msc.h"')
    if name == "msc_scsi_bot.c":
        return replace(source,
            "    msc_csw_t csw;",
            """    if (solar_os_usb_ufi_msc_state(device)) {
        return solar_os_usb_ufi_msc_execute(device, (const uint8_t *)(cbw + 1),
            cbw->cbw_length, (cbw->flags & CWB_FLAG_DIRECTION_IN) != 0, data, size);
    }
    msc_csw_t csw;""")
    source = replace(source,
        "ifc_desc->bInterfaceSubClass == SCSI_COMMAND_SET &&\n                ifc_desc->bInterfaceProtocol == BULK_ONLY_TRANSFER",
        """((ifc_desc->bInterfaceSubClass == SCSI_COMMAND_SET &&
                  ifc_desc->bInterfaceProtocol == BULK_ONLY_TRANSFER) ||
                 (ifc_desc->bInterfaceSubClass == 0x04 && ifc_desc->bInterfaceProtocol == 0x00)) &&
                ifc_desc->bAlternateSetting == 0""")
    source = replace(source,
        """    MSC_GOTO_ON_ERROR( scsi_cmd_inquiry(msc_device) );
    MSC_GOTO_ON_ERROR( msc_wait_for_ready_state(msc_device, WAIT_FOR_READY_TIMEOUT_MS) );
    MSC_GOTO_ON_ERROR( scsi_cmd_read_capacity(msc_device, &block_size, &block_count) );""",
        """    esp_err_t cbi = solar_os_usb_ufi_msc_bind(msc_device);
    if (cbi == ESP_OK) {
        MSC_GOTO_ON_ERROR(solar_os_usb_ufi_msc_initialize(msc_device, &block_size, &block_count));
    } else {
        MSC_GOTO_ON_FALSE(cbi == ESP_ERR_NOT_SUPPORTED, cbi);
        MSC_GOTO_ON_ERROR(scsi_cmd_inquiry(msc_device));
        MSC_GOTO_ON_ERROR(msc_wait_for_ready_state(msc_device, WAIT_FOR_READY_TIMEOUT_MS));
        MSC_GOTO_ON_ERROR(scsi_cmd_read_capacity(msc_device, &block_size, &block_count));
    }""")
    source = replace(source, "    free(dev);", "    solar_os_usb_ufi_msc_unbind(dev);\n    free(dev);")
    source = replace(source,
        "esp_err_t msc_host_reset_recovery(msc_host_device_handle_t device)\n{",
        """esp_err_t msc_host_reset_recovery(msc_host_device_handle_t device)
{
    solar_os_usb_ufi_t *ufi = solar_os_usb_ufi_msc_state(device);
    if (ufi) return solar_os_usb_ufi_reset(ufi);""")
    source = replace(source,
        """        usb_host_endpoint_halt(xfer->device_handle, xfer->bEndpointAddress);
        usb_host_endpoint_flush(xfer->device_handle, xfer->bEndpointAddress);
        usb_host_endpoint_clear(xfer->device_handle, xfer->bEndpointAddress);""",
        """        if (xfer->bEndpointAddress == 0) {
            /* IDF does not expose halt/flush for EP0. Disconnect the root
             * port to cancel control URBs before reusing/freeing their DMA
             * buffer. The library and MSC event tasks remain running. */
            (void)usb_host_lib_set_root_port_power(false);
        } else {
            usb_host_endpoint_halt(xfer->device_handle, xfer->bEndpointAddress);
            usb_host_endpoint_flush(xfer->device_handle, xfer->bEndpointAddress);
            usb_host_endpoint_clear(xfer->device_handle, xfer->bEndpointAddress);
        }""")
    source = replace(source,
        "    return wait_for_transfer_done(xfer) == USB_TRANSFER_STATUS_COMPLETED ? ESP_OK : ESP_ERR_MSC_INTERNAL;",
        """    usb_transfer_status_t status = wait_for_transfer_done(xfer);
    return status == USB_TRANSFER_STATUS_COMPLETED ? ESP_OK :
        status == USB_TRANSFER_STATUS_STALL ? ESP_ERR_MSC_STALL :
        status == USB_TRANSFER_STATUS_TIMED_OUT ? ESP_ERR_TIMEOUT : ESP_ERR_MSC_INTERNAL;""")
    match = re.search(r"^esp_err_t msc_bulk_transfer\([^;]*?\n\{.*?^\}", source, re.M | re.S)
    if not match:
        raise ValueError("MSC bulk transfer function missing")
    original = match.group()
    transfer = replace(original,
        "esp_err_t msc_bulk_transfer(msc_device_t *device, uint8_t *data, size_t size, msc_endpoint_t ep)\n{",
        """esp_err_t solar_os_usb_ufi_msc_endpoint(void *context, uint8_t endpoint, uint16_t mps,
    void *buffer, size_t size, size_t *actual)
{
    msc_device_t *device = context;
    uint8_t *data = buffer;
    bool input = (endpoint & 0x80) != 0;
    *actual = 0;""")
    transfer = transfer.replace("(ep == MSC_EP_IN)", "input")
    transfer = transfer.replace("device->config.bulk_in_mps", "mps")
    transfer = replace(transfer,
        """        MSC_RETURN_ON_ERROR( usb_host_transfer_free(xfer) );
        MSC_RETURN_ON_ERROR( usb_host_transfer_alloc(transfer_size, 0, &device->xfer) );
        xfer = device->xfer;""",
        """        usb_transfer_t *replacement = NULL;
        MSC_RETURN_ON_ERROR(usb_host_transfer_alloc(transfer_size, 0, &replacement));
        esp_err_t free_error = usb_host_transfer_free(xfer);
        if (free_error != ESP_OK) {
            (void)usb_host_transfer_free(replacement);
            return free_error;
        }
        device->xfer = xfer = replacement;""")
    transfer = replace(transfer,
        """    if input {
        xfer->bEndpointAddress = device->config.bulk_in_ep;
    } else {
        xfer->bEndpointAddress = device->config.bulk_out_ep;
        memcpy(xfer->data_buffer, data, size);
    }""",
        """    xfer->bEndpointAddress = endpoint;
    if (!input) memcpy(xfer->data_buffer, data, size);""")
    transfer = transfer.replace("if input", "if (input)")
    transfer = replace(transfer, "    case USB_TRANSFER_STATUS_COMPLETED:",
        "    case USB_TRANSFER_STATUS_COMPLETED:\n        *actual = xfer->actual_num_bytes;")
    transfer = replace(transfer, "    case USB_TRANSFER_STATUS_STALL:",
        "    case USB_TRANSFER_STATUS_TIMED_OUT:\n        ret = ESP_ERR_TIMEOUT; break;\n    case USB_TRANSFER_STATUS_STALL:")
    wrapper = """

esp_err_t msc_bulk_transfer(msc_device_t *device, uint8_t *data, size_t size, msc_endpoint_t ep)
{
    size_t actual;
    return solar_os_usb_ufi_msc_endpoint(device,
        ep == MSC_EP_IN ? device->config.bulk_in_ep : device->config.bulk_out_ep,
        device->config.bulk_in_mps, data, size, &actual);
}"""
    return source[:match.start()] + transfer + wrapper + source[match.end():]


def main():
    component, destination = map(Path, sys.argv[1:])
    destination.mkdir(parents=True, exist_ok=True)
    for name in HASHES:
        result = patched_source(name, (component / "src" / name).read_text())
        path = destination / name
        if not path.exists() or path.read_text() != result:
            path.write_text(result)


if __name__ == "__main__":
    main()
