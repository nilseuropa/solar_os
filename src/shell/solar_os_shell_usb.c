#include "solar_os_shell_commands.h"
#include "solar_os_shell_common.h"
#include "solar_os_cdc.h"
#include "solar_os_usb.h"

#include <stdio.h>
#include <string.h>

void solar_os_shell_cmd_usb(solar_os_context_t *ctx, int argc, char **argv)
{
    solar_os_shell_io_t *io = solar_os_shell_command_io(ctx);
    const char *usage = "usb [status|devices|mode <device|host>]";
    if (argc == 3 && strcmp(argv[1], "mode") == 0 &&
        (strcmp(argv[2], "device") == 0 || strcmp(argv[2], "host") == 0)) {
        const esp_err_t ret = solar_os_usb_set_mode(strcmp(argv[2], "host") == 0 ?
            SOLAR_OS_USB_MODE_HOST : SOLAR_OS_USB_MODE_DEVICE);
        if (ret != ESP_OK) {
            solar_os_cdc_status_t cdc;
            solar_os_cdc_get_status(&cdc);
            if (cdc.port_claimed) solar_os_shell_io_printf(io,
                "usb: %s owns cdc0; release it before switching modes\n", cdc.port_owner);
            else solar_os_shell_io_printf(io, "usb mode: %s\n", solar_os_shell_error_text(ret));
        }
        solar_os_usb_status_t status;
        solar_os_usb_get_status(&status);
        solar_os_shell_io_printf(io, "USB mode: %s\n", solar_os_usb_mode_name(status.mode));
        if (status.rollback_error != ESP_OK) solar_os_shell_io_printf(io,
            "Restore error: %s; retry usb mode device\n", solar_os_shell_error_text(status.rollback_error));
        return;
    } else if (!(argc == 1 || (argc == 2 &&
        (strcmp(argv[1], "status") == 0 || strcmp(argv[1], "devices") == 0)))) {
        solar_os_shell_io_printf(io, "usage: %s\n", usage);
        return;
    }
    solar_os_usb_status_t status;
    solar_os_usb_get_status(&status);
    solar_os_shell_io_printf(io, "USB mode: %s%s\n",
        solar_os_usb_mode_name(status.mode), status.sleep_prepared ? " (sleep prepared)" : "");
    if (status.last_error != ESP_OK) solar_os_shell_io_printf(io,
        "Last error: %s\n", solar_os_shell_error_text(status.last_error));
    if (status.rollback_error != ESP_OK) solar_os_shell_io_printf(io,
        "Restore error: %s; retry usb mode device\n", solar_os_shell_error_text(status.rollback_error));
    if (status.mode == SOLAR_OS_USB_MODE_DEVICE) {
        solar_os_shell_io_writeln(io, "Native USB: CDC / Serial-JTAG");
        return;
    }
    solar_os_shell_io_writeln(io, "Host power: external 5 V required; no software VBUS switch");
    if (status.interface_count == 0) {
        solar_os_shell_io_writeln(io, "No supported HID interfaces");
        return;
    }
    solar_os_shell_io_table_writeln(io, "ADDR IF SOURCE     STATE   REPORTS ERRORS");
    for (size_t i = 0; i < status.interface_count; i++) {
        const usb_host_port_interface_t *interface = &status.interfaces[i];
        char source[16];
        snprintf(source, sizeof(source), "usb-%s%u",
            interface->protocol == SOLAR_OS_USB_HID_KEYBOARD ? "kbd" :
                interface->protocol == SOLAR_OS_USB_HID_MOUSE ? "mouse" :
                interface->protocol == SOLAR_OS_USB_HID_JOYSTICK ? "joy" : "hid", interface->slot);
        solar_os_shell_io_table_printf(io, "%4u %2u %-10s %-7s %7lu %6lu\n",
            interface->address, interface->interface, source,
            interface->ready ? "ready" : "error", (unsigned long)interface->reports,
            (unsigned long)interface->errors);
        if (interface->last_error != ESP_OK) solar_os_shell_io_printf(io,
            "%s: %s\n", source, solar_os_shell_error_text(interface->last_error));
        if (argc == 2 && strcmp(argv[1], "devices") == 0) {
            const char *protocol = interface->report_protocol == 0 ? "boot" :
                interface->report_protocol == 1 ? "report" : "unverified";
            solar_os_shell_io_printf(io,
                "%s: VID:PID=%04x:%04x protocol=%s\n", source,
                interface->vendor_id, interface->product_id, protocol);
            if (interface->protocol_error != ESP_OK) solar_os_shell_io_printf(io,
                "  Protocol check: %s\n", solar_os_shell_error_text(interface->protocol_error));
            if (interface->idle_error != ESP_OK) solar_os_shell_io_printf(io,
                "  Idle setup: %s\n", solar_os_shell_error_text(interface->idle_error));
        }
    }
}
