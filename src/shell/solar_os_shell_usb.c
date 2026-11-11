#include "solar_os_shell_commands.h"
#include "solar_os_shell_common.h"
#include "solar_os_board_caps.h"
#include "solar_os_cdc.h"
#include "solar_os_usb.h"
#if SOLAR_OS_PACKAGE_SERVICE_USB_STORAGE
#include "solar_os_usb_storage.h"
#endif

#if SOLAR_OS_PACKAGE_SERVICE_USB_SERIAL
#include "solar_os_usb_serial.h"
#endif

#include <errno.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>

#if SOLAR_OS_PACKAGE_SERVICE_USB_SERIAL
static void usb_serial_list(solar_os_shell_io_t *io)
{
    solar_os_usb_serial_info_t ports[SOLAR_OS_USB_SERIAL_MAX];
    const size_t count = solar_os_usb_serial_list(ports, SOLAR_OS_USB_SERIAL_MAX);
    solar_os_shell_io_table_writeln(io, "PORT        ADDR IF TYPE        VID:PID   STATE      BAUD");
    for (size_t i = 0; i < count && i < SOLAR_OS_USB_SERIAL_MAX; i++) {
        const solar_os_usb_serial_info_t *port = &ports[i];
        solar_os_shell_io_table_printf(io, "%s %4u %2u %-11s %04x:%04x %-10s %lu\n",
            port->port, port->device.address, port->device.interface,
            usb_serial_port_kind_name(port->device.kind), port->device.vid, port->device.pid,
            port->connected ? "connected" : "removed", (unsigned long)port->config.baud_rate);
        solar_os_shell_io_printf(io, "  RX=%llu TX=%llu dropped=%llu\n",
            (unsigned long long)port->received, (unsigned long long)port->transmitted,
            (unsigned long long)port->dropped);
        if (port->last_error != ESP_OK) solar_os_shell_io_printf(io, "  %s\n", solar_os_shell_error_text(port->last_error));
    }
    if (!count) solar_os_shell_io_writeln(io, "No USB serial ports");
    const esp_err_t error = solar_os_usb_serial_last_error();
    if (error != ESP_OK) solar_os_shell_io_printf(io, "USB serial: %s\n", solar_os_shell_error_text(error));
}

static void usb_serial_command(solar_os_shell_io_t *io, int argc, char **argv)
{
    if (argc == 2) { usb_serial_list(io); return; }
    solar_os_port_serial_config_t config = {0};
    uint32_t mask = 0;
    esp_err_t err = ESP_OK;
    if (argc == 4 && !strncmp(argv[3], "break=", 6)) {
        char *end;
        errno = 0;
        unsigned long duration = strtoul(argv[3] + 6, &end, 10);
        err = errno || argv[3][6] < '0' || argv[3][6] > '9' || *end || duration > UINT16_MAX ?
            ESP_ERR_INVALID_ARG : solar_os_usb_serial_send_break(argv[2], duration);
        if (err == ESP_OK) return;
    } else {
        for (int i = 3; i < argc && err == ESP_OK; i++) {
            const char *equal = strchr(argv[i], '=');
            char key[16];
            if (!equal || equal == argv[i] || (size_t)(equal - argv[i]) >= sizeof(key)) { err = ESP_ERR_INVALID_ARG; break; }
            memcpy(key, argv[i], equal - argv[i]); key[equal - argv[i]] = '\0';
            err = solar_os_usb_serial_parse_option(key, equal + 1, &config, &mask);
        }
        if (err == ESP_OK) err = solar_os_usb_serial_config(argv[2], &config, mask);
    }
    if (err != ESP_OK) {
        solar_os_shell_io_printf(io, "usb serial: %s\n", solar_os_shell_error_text(err));
        solar_os_shell_io_writeln(io, "usage: usb serial [PORT [baud=N bits=5..8 parity=none|odd|even|mark|space stop=1|1.5|2 dtr=0|1 rts=0|1 | break=MS]]");
        return;
    }
    static const char *parity[] = {"none", "odd", "even", "mark", "space"};
    static const char *stop[] = {"1", "1.5", "2"};
    solar_os_shell_io_printf(io, "%s: baud=%lu bits=%u parity=%s stop=%s dtr=%u rts=%u\n",
        argv[2], (unsigned long)config.baud_rate, config.data_bits, parity[config.parity], stop[config.stop_bits], config.dtr, config.rts);
}
#endif

void solar_os_shell_cmd_usb(solar_os_context_t *ctx, int argc, char **argv)
{
    solar_os_shell_io_t *io = solar_os_shell_command_io(ctx);
    const char *usage = "usb [status|devices|mode <device|host>"
#if SOLAR_OS_PACKAGE_SERVICE_USB_SERIAL
        "|serial [PORT [OPTIONS]]"
#endif
        "]";
#if SOLAR_OS_PACKAGE_SERVICE_USB_SERIAL
    if (argc >= 2 && strcmp(argv[1], "serial") == 0) { usb_serial_command(io, argc, argv); return; }
#endif
    if (argc == 3 && strcmp(argv[1], "mode") == 0 &&
        (strcmp(argv[2], "device") == 0 || strcmp(argv[2], "host") == 0)) {
        const esp_err_t ret = solar_os_usb_set_mode(strcmp(argv[2], "host") == 0 ?
            SOLAR_OS_USB_MODE_HOST : SOLAR_OS_USB_MODE_DEVICE);
        if (ret != ESP_OK) {
            solar_os_cdc_status_t cdc;
            solar_os_cdc_get_status(&cdc);
#if SOLAR_OS_PACKAGE_SERVICE_USB_STORAGE
            solar_os_usb_storage_status_t storage;
            solar_os_usb_storage_get_status(&storage);
#endif
            if (cdc.port_claimed) solar_os_shell_io_printf(io,
                "usb: %s owns cdc0; release it before switching modes\n", cdc.port_owner);
#if SOLAR_OS_PACKAGE_SERVICE_USB_STORAGE
            else if (ret == ESP_ERR_INVALID_STATE && storage.mounted && storage.open_handles)
                solar_os_shell_io_writeln(io, "usb: usb0 is busy; close files and unmount disk images first");
#endif
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
#ifdef SOLAR_OS_BOARD_PIN_USB_VBUS_EN
    solar_os_shell_io_printf(io, "Host power: onboard VBUS switch %s (GPIO%d)\n",
        status.vbus_enabled ? "enabled" : "disabled", (int)SOLAR_OS_BOARD_PIN_USB_VBUS_EN);
#endif
    if (status.mode == SOLAR_OS_USB_MODE_DEVICE) {
#if SOLAR_OS_BOARD_HAS_CDC
        solar_os_shell_io_writeln(io, "Native USB: CDC / Serial-JTAG");
#else
        solar_os_shell_io_writeln(io, "Native USB: idle; no CDC port in this board profile");
#endif
        return;
    }
#ifndef SOLAR_OS_BOARD_PIN_USB_VBUS_EN
    solar_os_shell_io_writeln(io, "Host power: external 5 V required; no software VBUS switch");
#endif
#if SOLAR_OS_PACKAGE_SERVICE_USB_STORAGE
    solar_os_usb_storage_status_t storage;
    solar_os_usb_storage_get_status(&storage);
    if (storage.connected) {
        solar_os_shell_io_printf(io, "usb0: %s; %llu bytes; %u open handles; VID:PID=%04x:%04x%s%s\n",
            storage.mounted ? "mounted at /usb0" : storage.media_present ? "unmounted" : "no disk",
            (unsigned long long)storage.sector_count * storage.sector_size,
            (unsigned)storage.open_handles, storage.vendor_id, storage.product_id,
            storage.ufi_cbi ? "; UFI/CBI" : "; SCSI/BOT",
            storage.write_protected ? "; write protected" : "");
    } else solar_os_shell_io_writeln(io, "USB storage: no drive connected");
    if (storage.last_error != ESP_OK) solar_os_shell_io_printf(io,
        "USB storage: %s\n", solar_os_shell_error_text(storage.last_error));
#endif
#if SOLAR_OS_PACKAGE_SERVICE_USB_SERIAL
    usb_serial_list(io);
#endif
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
