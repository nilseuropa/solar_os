#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"

#define SOLAR_OS_UFI_COMMAND_FAILED 0x17e1
#define SOLAR_OS_UFI_NO_MEDIA 0x17e2
#define SOLAR_OS_UFI_MEDIA_CHANGED 0x17e3
#define SOLAR_OS_UFI_WRITE_PROTECTED 0x17e4
#define SOLAR_OS_UFI_NOT_READY 0x17e5
#define SOLAR_OS_UFI_STALL 0x17e6

/* All callbacks are synchronous and bounded. The adapter owns cancellation,
 * DMA buffers and endpoint lifetime. The caller serializes this state. */
typedef struct {
    void *context;
    esp_err_t (*command)(void *context, const uint8_t command[12]);
    esp_err_t (*data)(void *context, bool input, void *data, size_t size, size_t *actual);
    esp_err_t (*status)(void *context, uint8_t status[2], size_t *actual);
    esp_err_t (*clear)(void *context, bool input);
    void (*delay)(void *context, unsigned milliseconds);
} solar_os_usb_ufi_io_t;

typedef struct {
    solar_os_usb_ufi_io_t io;
    uint32_t sector_size, sector_count, media_revision;
    uint8_t sense_key, asc, ascq;
    bool media_present, write_protected, faulted;
} solar_os_usb_ufi_t;

typedef struct {
    uint8_t interface, bulk_in, bulk_out, interrupt_in;
    uint16_t bulk_in_mps, interrupt_mps;
} solar_os_usb_ufi_config_t;

esp_err_t solar_os_usb_ufi_parse_config(const uint8_t *data, size_t size,
                                       uint8_t interface, solar_os_usb_ufi_config_t *config);
esp_err_t solar_os_usb_ufi_execute(solar_os_usb_ufi_t *ufi, const uint8_t *cdb, size_t length,
                                  bool input, void *data, size_t size);

/* USB Mass Storage UFI 1.0 and CBI 1.1, subclass 04h / protocol 00h. */
esp_err_t solar_os_usb_ufi_init(solar_os_usb_ufi_t *ufi, const solar_os_usb_ufi_io_t *io);
esp_err_t solar_os_usb_ufi_refresh(solar_os_usb_ufi_t *ufi);
esp_err_t solar_os_usb_ufi_transfer(solar_os_usb_ufi_t *ufi, uint32_t sector,
                                   void *data, size_t size, bool write_data);
esp_err_t solar_os_usb_ufi_reset(solar_os_usb_ufi_t *ufi);
