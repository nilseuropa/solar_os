#include "usb_ufi.h"

#include <limits.h>
#include <string.h>

static uint32_t read_be32(const uint8_t *data)
{ return (uint32_t)data[0] << 24 | (uint32_t)data[1] << 16 | (uint32_t)data[2] << 8 | data[3]; }
static void write_be32(uint8_t *data, uint32_t value)
{ data[0] = value >> 24; data[1] = value >> 16; data[2] = value >> 8; data[3] = value; }

esp_err_t solar_os_usb_ufi_parse_config(const uint8_t *data, size_t size,
                                       uint8_t interface, solar_os_usb_ufi_config_t *config)
{
    if (!data || !config || size < 9 || data[0] < 9 || data[1] != 2) return ESP_ERR_INVALID_ARG;
    size_t total = data[2] | (size_t)data[3] << 8;
    if (total < 9 || total > size) return ESP_ERR_INVALID_RESPONSE;
    bool selected = false, found = false;
    memset(config, 0, sizeof(*config));
    for (size_t offset = 0; offset < total;) {
        const uint8_t *desc = data + offset;
        if (total - offset < 2 || desc[0] < 2 || desc[0] > total - offset) return ESP_ERR_INVALID_RESPONSE;
        if (desc[1] == 4) {
            if (desc[0] < 9) return ESP_ERR_INVALID_RESPONSE;
            if (selected) break;
            selected = desc[2] == interface && !desc[3] && desc[4] == 3 && desc[5] == 8 && desc[6] == 4 && !desc[7];
            found = found || selected;
            if (selected) config->interface = interface;
        } else if (selected && desc[1] == 5) {
            if (desc[0] < 7) return ESP_ERR_INVALID_RESPONSE;
            uint8_t endpoint = desc[2], type = desc[3] & 3;
            uint16_t mps = desc[4] | (uint16_t)desc[5] << 8;
            if (!(endpoint & 15) || (endpoint & 0x70) || !mps || mps > 64) return ESP_ERR_INVALID_RESPONSE;
            if (type == 2 && mps != 8 && mps != 16 && mps != 32 && mps != 64) return ESP_ERR_INVALID_RESPONSE;
            if (type == 2 && (endpoint & 0x80)) {
                if (config->bulk_in) return ESP_ERR_INVALID_RESPONSE;
                config->bulk_in = endpoint; config->bulk_in_mps = mps;
            } else if (type == 2) {
                if (config->bulk_out) return ESP_ERR_INVALID_RESPONSE;
                config->bulk_out = endpoint;
            } else if (type == 3 && (endpoint & 0x80)) {
                if (config->interrupt_in || mps != 2 || !desc[6]) return ESP_ERR_INVALID_RESPONSE;
                config->interrupt_in = endpoint; config->interrupt_mps = mps;
            }
        }
        offset += desc[0];
    }
    if (config->bulk_in && config->bulk_in == config->interrupt_in) return ESP_ERR_INVALID_RESPONSE;
    return found && config->bulk_in && config->bulk_out && config->interrupt_in ? ESP_OK : ESP_ERR_NOT_SUPPORTED;
}

static void invalidate(solar_os_usb_ufi_t *ufi, bool attention)
{
    if (ufi->media_present || attention) ++ufi->media_revision;
    ufi->media_present = false; ufi->sector_count = 0; ufi->write_protected = false;
}

esp_err_t solar_os_usb_ufi_reset(solar_os_usb_ufi_t *ufi)
{
    uint8_t command[12]; memset(command, 0xff, sizeof(command));
    command[0] = 0x1d; command[1] = 0x04;
    /* CBI command-block reset is ADSC, then clear both bulk endpoints. */
    esp_err_t err = ufi->io.command(ufi->io.context, command);
    esp_err_t in = ufi->io.clear(ufi->io.context, true);
    esp_err_t out = ufi->io.clear(ufi->io.context, false);
    invalidate(ufi, true);
    ufi->faulted = err != ESP_OK || in != ESP_OK || out != ESP_OK;
    return err != ESP_OK ? err : in != ESP_OK ? in : out;
}

static esp_err_t transport(solar_os_usb_ufi_t *ufi, const uint8_t command[12],
                           bool input, void *data, size_t size)
{
    if (ufi->faulted) return ESP_ERR_INVALID_STATE;
    esp_err_t err = ufi->io.command(ufi->io.context, command);
    /* A stalled ADSC has no completion interrupt. REQUEST SENSE clears the
     * command failure; waiting for an interrupt here would hang the drive. */
    if (err == SOLAR_OS_UFI_STALL) return SOLAR_OS_UFI_COMMAND_FAILED;
    if (err != ESP_OK) { (void)solar_os_usb_ufi_reset(ufi); return err; }
    size_t transferred = 0;
    if (size) {
        err = ufi->io.data(ufi->io.context, input, data, size, &transferred);
        if (err == SOLAR_OS_UFI_STALL) {
            esp_err_t clear = ufi->io.clear(ufi->io.context, input);
            if (clear != ESP_OK) { (void)solar_os_usb_ufi_reset(ufi); return clear; }
            return SOLAR_OS_UFI_COMMAND_FAILED;
        }
        if (err != ESP_OK) { (void)solar_os_usb_ufi_reset(ufi); return err; }
    }
    uint8_t status[2] = {0}; size_t actual = 0;
    err = ufi->io.status(ufi->io.context, status, &actual);
    if (err != ESP_OK || actual != 2) {
        (void)solar_os_usb_ufi_reset(ufi);
        return err != ESP_OK ? err : ESP_ERR_INVALID_RESPONSE;
    }
    /* UFI interrupt bytes are ASC/ASCQ, not BOT CSW or generic CBI status.
     * INQUIRY and REQUEST SENSE preserve prior sense, including its errors. */
    if (command[0] != 0x03 && command[0] != 0x12 && (status[0] || status[1]))
        return SOLAR_OS_UFI_COMMAND_FAILED;
    return transferred == size ? ESP_OK : ESP_ERR_INVALID_RESPONSE;
}

static esp_err_t sense(solar_os_usb_ufi_t *ufi)
{
    uint8_t command[12] = {0x03, 0, 0, 0, 18}, data[18] = {0};
    esp_err_t err = transport(ufi, command, true, data, sizeof(data));
    if (err != ESP_OK) return err;
    if ((data[0] & 0x7f) != 0x70 && (data[0] & 0x7f) != 0x71) return ESP_ERR_INVALID_RESPONSE;
    if (data[7] < 6) return ESP_ERR_INVALID_RESPONSE;
    ufi->sense_key = data[2] & 15; ufi->asc = data[12]; ufi->ascq = data[13];
    if (ufi->sense_key == 6 || ufi->asc == 0x28) {
        invalidate(ufi, true); return SOLAR_OS_UFI_MEDIA_CHANGED;
    }
    if (ufi->asc == 0x3a) { invalidate(ufi, false); return SOLAR_OS_UFI_NO_MEDIA; }
    if (ufi->sense_key == 7 || ufi->asc == 0x27) {
        ufi->write_protected = true; return SOLAR_OS_UFI_WRITE_PROTECTED;
    }
    if (ufi->sense_key == 2) return SOLAR_OS_UFI_NOT_READY;
    if (ufi->sense_key == 0 || ufi->sense_key == 1) return ESP_OK;
    return SOLAR_OS_UFI_COMMAND_FAILED;
}

static esp_err_t command(solar_os_usb_ufi_t *ufi, const uint8_t cdb[12],
                         bool input, void *data, size_t size)
{
    esp_err_t err = transport(ufi, cdb, input, data, size);
    if (err != ESP_OK && err != SOLAR_OS_UFI_COMMAND_FAILED) return err;
    esp_err_t status = sense(ufi);
    if (status != ESP_OK) return status;
    return err; /* A command transport failure never turns into success. */
}

static esp_err_t capacity(solar_os_usb_ufi_t *ufi, uint32_t *count, uint32_t *size)
{
    uint8_t cdb[12] = {0x25}, data[12] = {0};
    esp_err_t err = command(ufi, cdb, true, data, 8);
    if (err == SOLAR_OS_UFI_COMMAND_FAILED && ufi->sense_key == 5) {
        /* Some floppy bridges implement READ FORMAT CAPACITIES only. The
         * first descriptor is current media, never a formatting instruction. */
        memset(cdb, 0, sizeof(cdb)); cdb[0] = 0x23; cdb[8] = sizeof(data);
        err = command(ufi, cdb, true, data, sizeof(data));
        if (err != ESP_OK) return err;
        if (data[3] < 8) return ESP_ERR_INVALID_RESPONSE;
        if ((data[8] & 3) == 3) { invalidate(ufi, false); return SOLAR_OS_UFI_NO_MEDIA; }
        if ((data[8] & 3) != 2) return ESP_ERR_NOT_SUPPORTED;
        *count = read_be32(data + 4);
        *size = (uint32_t)data[9] << 16 | (uint32_t)data[10] << 8 | data[11];
    } else {
        if (err != ESP_OK) return err;
        uint32_t last = read_be32(data);
        if (last == UINT32_MAX) return ESP_ERR_INVALID_SIZE;
        *count = last + 1; *size = read_be32(data + 4);
    }
    return *count && *size >= 512 && *size <= 4096 && !(*size & (*size - 1)) ? ESP_OK : ESP_ERR_INVALID_SIZE;
}

esp_err_t solar_os_usb_ufi_refresh(solar_os_usb_ufi_t *ufi)
{
    if (!ufi) return ESP_ERR_INVALID_ARG;
    if (ufi->faulted) return ESP_ERR_INVALID_STATE;
    uint8_t cdb[12] = {0};
    esp_err_t err = ESP_OK;
    for (unsigned attempt = 0; attempt < 20; ++attempt) {
        err = command(ufi, cdb, true, NULL, 0);
        if (err == SOLAR_OS_UFI_NO_MEDIA) return ESP_OK;
        if (err != SOLAR_OS_UFI_MEDIA_CHANGED && err != SOLAR_OS_UFI_NOT_READY) break;
        ufi->io.delay(ufi->io.context, 100);
    }
    if (err != ESP_OK) {
        if (err == SOLAR_OS_UFI_NOT_READY) invalidate(ufi, false);
        return err;
    }
    if (ufi->media_present) return ESP_OK;
    uint32_t count, size;
    err = capacity(ufi, &count, &size);
    if (err == SOLAR_OS_UFI_NO_MEDIA) return ESP_OK;
    if (err != ESP_OK) return err;
    uint8_t mode[8] = {0};
    memset(cdb, 0, sizeof(cdb)); cdb[0] = 0x5a; cdb[2] = 0x3f; cdb[8] = sizeof(mode);
    err = command(ufi, cdb, true, mode, sizeof(mode));
    if (err != ESP_OK) return err;
    ufi->write_protected = (mode[3] & 0x80) != 0;
    ufi->sector_count = count; ufi->sector_size = size; ufi->media_present = true;
    ++ufi->media_revision;
    return ESP_OK;
}

esp_err_t solar_os_usb_ufi_init(solar_os_usb_ufi_t *ufi, const solar_os_usb_ufi_io_t *io)
{
    if (!ufi || !io || !io->command || !io->data || !io->status || !io->clear || !io->delay)
        return ESP_ERR_INVALID_ARG;
    memset(ufi, 0, sizeof(*ufi)); ufi->io = *io; ufi->sector_size = 512;
    uint8_t cdb[12] = {0x12, 0, 0, 0, 36}, inquiry[36] = {0};
    esp_err_t err = transport(ufi, cdb, true, inquiry, sizeof(inquiry));
    if (err != ESP_OK) return err;
    return solar_os_usb_ufi_refresh(ufi);
}

esp_err_t solar_os_usb_ufi_transfer(solar_os_usb_ufi_t *ufi, uint32_t sector,
                                   void *data, size_t size, bool write_data)
{
    if (!ufi || !data || !size) return ESP_ERR_INVALID_ARG;
    uint32_t revision = ufi->media_revision;
    esp_err_t err = solar_os_usb_ufi_refresh(ufi);
    if (err != ESP_OK) return err;
    /* Never retry a write after a media change. Let storage discard the old
     * filesystem, even when the replacement disk has identical capacity. */
    if (revision != ufi->media_revision) return SOLAR_OS_UFI_MEDIA_CHANGED;
    if (!ufi->media_present) return SOLAR_OS_UFI_NO_MEDIA;
    if (write_data && ufi->write_protected) return SOLAR_OS_UFI_WRITE_PROTECTED;
    if (size % ufi->sector_size || sector >= ufi->sector_count ||
        size / ufi->sector_size > ufi->sector_count - sector ||
        size / ufi->sector_size > UINT16_MAX) return ESP_ERR_INVALID_SIZE;
    uint8_t cdb[12] = {0}; cdb[0] = write_data ? 0x2a : 0x28;
    write_be32(cdb + 2, sector);
    uint32_t sectors = size / ufi->sector_size; cdb[7] = sectors >> 8; cdb[8] = sectors;
    return command(ufi, cdb, !write_data, data, size);
}

esp_err_t solar_os_usb_ufi_execute(solar_os_usb_ufi_t *ufi, const uint8_t *cdb, size_t length,
                                  bool input, void *data, size_t size)
{
    if (!ufi || !cdb || !length || length > 12 || (size && !data)) return ESP_ERR_INVALID_ARG;
    uint8_t padded[12] = {0}; memcpy(padded, cdb, length);
    if (padded[0] == 0x28 || padded[0] == 0x2a) {
        uint32_t sectors = (uint32_t)padded[7] << 8 | padded[8];
        if (length < 10 || input != (padded[0] == 0x28) ||
            (uint64_t)sectors * ufi->sector_size != size) return ESP_ERR_INVALID_SIZE;
        return solar_os_usb_ufi_transfer(ufi, read_be32(padded + 2), data, size, !input);
    }
    if (padded[0] == 0x03 || padded[0] == 0x12) return transport(ufi, padded, input, data, size);
    return command(ufi, padded, input, data, size);
}
