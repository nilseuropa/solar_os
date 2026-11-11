#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "usb_ufi.h"

typedef struct {
    uint8_t command[12], key, asc, ascq;
    unsigned phase, data_size, attention, starting, writes, resets, clears, delays;
    bool inserted, protect, capacity_unsupported, invalid_capacity;
    bool command_stall, data_stall, data_short, status_short, status_timeout, clear_failure, reset_failure;
} fake_t;

static void set_sense(fake_t *fake, uint8_t key, uint8_t asc)
{ fake->key = key; fake->asc = asc; fake->ascq = 0; }

static esp_err_t adsc(void *context, const uint8_t command[12])
{
    fake_t *fake = context;
    if (command[0] == 0x1d) {
        assert(command[1] == 4);
        for (unsigned i = 2; i < 12; ++i) assert(command[i] == 0xff);
        fake->resets++; fake->phase = 0;
        return fake->reset_failure ? ESP_FAIL : ESP_OK;
    }
    assert(fake->phase == 0);
    memcpy(fake->command, command, 12);
    if (fake->command_stall) { fake->command_stall = false; set_sense(fake, 5, 0x20); return SOLAR_OS_UFI_STALL; }
    fake->phase = 1;
    switch (command[0]) {
    case 0x12: fake->data_size = 36; break;
    case 0x03: fake->data_size = 18; break;
    case 0:
        fake->data_size = 0;
        if (fake->attention) { fake->attention--; set_sense(fake, 6, 0x28); }
        else if (!fake->inserted) set_sense(fake, 2, 0x3a);
        else if (fake->starting) { fake->starting--; set_sense(fake, 2, 4); }
        else set_sense(fake, 0, 0);
        break;
    case 0x25: fake->data_size = 8; set_sense(fake, fake->capacity_unsupported ? 5 : 0, fake->capacity_unsupported ? 0x20 : 0); break;
    case 0x23: assert(command[7] == 0 && command[8] == 12); fake->data_size = 12; set_sense(fake, 0, 0); break;
    case 0x5a: assert(command[2] == 0x3f && command[7] == 0 && command[8] == 8); fake->data_size = 8; set_sense(fake, 0, 0); break;
    case 0x28: case 0x2a:
        assert(command[1] == 0 && command[6] == 0 && command[9] == 0 && command[10] == 0 && command[11] == 0);
        fake->data_size = ((unsigned)command[7] << 8 | command[8]) * 512;
        set_sense(fake, 0, 0); break;
    default: assert(false);
    }
    return ESP_OK;
}

static esp_err_t data(void *context, bool input, void *buffer, size_t size, size_t *actual)
{
    fake_t *fake = context;
    uint8_t *bytes = buffer;
    assert(fake->phase == 1 && size == fake->data_size);
    assert(input == (fake->command[0] != 0x2a));
    if (fake->data_stall || (fake->command[0] == 0x25 && fake->capacity_unsupported) ||
        (fake->command[0] == 0x2a && fake->protect)) {
        fake->data_stall = false; fake->phase = 0;
        if (fake->command[0] == 0x2a && fake->protect) set_sense(fake, 7, 0x27);
        return SOLAR_OS_UFI_STALL;
    }
    fake->phase = 2; *actual = size;
    if (!input) { fake->writes++; return ESP_OK; }
    memset(bytes, 0, size);
    switch (fake->command[0]) {
    case 0x03:
        bytes[0] = 0x70; bytes[2] = fake->key; bytes[7] = 10; bytes[12] = fake->asc; bytes[13] = fake->ascq; break;
    case 0x12: bytes[0] = 0; bytes[1] = 0x80; break;
    case 0x25:
        bytes[2] = 0x0b; bytes[3] = 0x3f; /* Last LBA 2879. */
        bytes[6] = fake->invalid_capacity ? 3 : 2; break;
    case 0x23: bytes[3] = 8; bytes[6] = 0x0b; bytes[7] = 0x40; bytes[8] = 2; bytes[10] = 2; break;
    case 0x5a: bytes[3] = fake->protect ? 0x80 : 0; break;
    case 0x28: memset(bytes, 0x5a, size); break;
    default: assert(false);
    }
    if (fake->data_short) { fake->data_short = false; *actual = size - 1; }
    return ESP_OK;
}

static esp_err_t status(void *context, uint8_t bytes[2], size_t *actual)
{
    fake_t *fake = context;
    assert(fake->phase == (fake->data_size ? 2U : 1U));
    fake->phase = 0;
    bytes[0] = fake->asc; bytes[1] = fake->ascq;
    *actual = fake->status_short ? 1 : 2; fake->status_short = false;
    if (fake->status_timeout) { fake->status_timeout = false; return ESP_ERR_TIMEOUT; }
    return ESP_OK;
}
static esp_err_t clear(void *context, bool input)
{ fake_t *fake = context; (void)input; fake->clears++; return fake->clear_failure ? ESP_FAIL : ESP_OK; }
static void delay(void *context, unsigned milliseconds)
{ fake_t *fake = context; assert(milliseconds == 100); fake->delays++; }

static void initialize(solar_os_usb_ufi_t *ufi, fake_t *fake)
{
    solar_os_usb_ufi_io_t io = {.context = fake, .command = adsc, .data = data,
        .status = status, .clear = clear, .delay = delay};
    assert(solar_os_usb_ufi_init(ufi, &io) == ESP_OK);
}

static void descriptors(void)
{
    uint8_t descriptor[] = {
        9,2,39,0,1,1,0,0x80,250, 9,4,0,0,3,8,4,0,0,
        7,5,0x83,3,2,0,32, 7,5,0x02,2,64,0,0, 7,5,0x81,2,64,0,0,
    };
    solar_os_usb_ufi_config_t config;
    assert(solar_os_usb_ufi_parse_config(descriptor, sizeof(descriptor), 0, &config) == ESP_OK);
    assert(config.bulk_in == 0x81 && config.bulk_out == 2 && config.interrupt_in == 0x83);
    assert(config.bulk_in_mps == 64 && config.interrupt_mps == 2);
    descriptor[13] = 2;
    assert(solar_os_usb_ufi_parse_config(descriptor, sizeof(descriptor), 0, &config) == ESP_ERR_NOT_SUPPORTED);
    descriptor[13] = 3; descriptor[34] = 0x83;
    assert(solar_os_usb_ufi_parse_config(descriptor, sizeof(descriptor), 0, &config) == ESP_ERR_INVALID_RESPONSE);
    descriptor[34] = 0x81;
    descriptor[15] = 6; descriptor[16] = 0x50;
    assert(solar_os_usb_ufi_parse_config(descriptor, sizeof(descriptor), 0, &config) == ESP_ERR_NOT_SUPPORTED);
    descriptor[15] = 4; descriptor[16] = 0;
    descriptor[18] = 0;
    assert(solar_os_usb_ufi_parse_config(descriptor, sizeof(descriptor), 0, &config) == ESP_ERR_INVALID_RESPONSE);
    descriptor[18] = 7;
    assert(solar_os_usb_ufi_parse_config(descriptor, sizeof(descriptor)-1, 0, &config) == ESP_ERR_INVALID_RESPONSE);
    descriptor[23] = 1;
    assert(solar_os_usb_ufi_parse_config(descriptor, sizeof(descriptor), 0, &config) == ESP_ERR_INVALID_RESPONSE);
}

int main(void)
{
    descriptors();
    solar_os_usb_ufi_t ufi;
    fake_t fake = {.inserted = true, .attention = 1, .starting = 2};
    initialize(&ufi, &fake);
    assert(ufi.media_present && ufi.sector_count == 2880 && ufi.sector_size == 512 && fake.delays == 3);
    uint32_t spinning_revision = ufi.media_revision;
    fake.starting = 2;
    assert(solar_os_usb_ufi_refresh(&ufi) == ESP_OK && ufi.media_revision == spinning_revision);
    uint8_t bytes[1024];
    assert(solar_os_usb_ufi_transfer(&ufi, 3, bytes, sizeof(bytes), false) == ESP_OK && bytes[0] == 0x5a);
    assert(solar_os_usb_ufi_transfer(&ufi, 3, bytes, sizeof(bytes), true) == ESP_OK && fake.writes == 1);
    assert(solar_os_usb_ufi_transfer(&ufi, 2879, bytes, sizeof(bytes), true) == ESP_ERR_INVALID_SIZE && fake.writes == 1);
    uint32_t revision = ufi.media_revision;
    fake.attention = 1;
    assert(solar_os_usb_ufi_transfer(&ufi, 3, bytes, sizeof(bytes), true) == SOLAR_OS_UFI_MEDIA_CHANGED);
    assert(ufi.media_revision != revision && fake.writes == 1); /* No retry writes on replacement disk. */
    fake.protect = true;
    assert(solar_os_usb_ufi_transfer(&ufi, 3, bytes, sizeof(bytes), true) == SOLAR_OS_UFI_WRITE_PROTECTED);
    assert(ufi.write_protected && fake.writes == 1);
    assert(solar_os_usb_ufi_transfer(&ufi, 3, bytes, sizeof(bytes), false) == ESP_OK);
    fake.inserted = false;
    assert(solar_os_usb_ufi_refresh(&ufi) == ESP_OK && !ufi.media_present && !ufi.sector_count);
    fake.inserted = true; fake.protect = false;
    assert(solar_os_usb_ufi_refresh(&ufi) == ESP_OK && ufi.media_present && !ufi.write_protected);
    fake.data_short = true;
    assert(solar_os_usb_ufi_transfer(&ufi, 3, bytes, sizeof(bytes), false) == ESP_ERR_INVALID_RESPONSE);
    fake.status_timeout = true;
    assert(solar_os_usb_ufi_refresh(&ufi) == ESP_ERR_TIMEOUT && fake.resets == 1 && fake.clears >= 2);
    assert(solar_os_usb_ufi_refresh(&ufi) == ESP_OK && ufi.media_present);
    fake.status_short = true;
    assert(solar_os_usb_ufi_refresh(&ufi) == ESP_ERR_INVALID_RESPONSE && fake.resets == 2);
    fake.status_timeout = fake.reset_failure = true;
    assert(solar_os_usb_ufi_refresh(&ufi) == ESP_ERR_TIMEOUT && ufi.faulted);
    assert(solar_os_usb_ufi_refresh(&ufi) == ESP_ERR_INVALID_STATE);
    fake = (fake_t){.inserted = false, .key = 2, .asc = 0x3a}; initialize(&ufi, &fake);
    assert(!ufi.media_present && !ufi.sector_count); /* Empty drive remains usable. */
    fake = (fake_t){.inserted = true, .protect = true}; initialize(&ufi, &fake);
    assert(ufi.write_protected);
    fake = (fake_t){.inserted = true, .capacity_unsupported = true}; initialize(&ufi, &fake);
    assert(ufi.sector_count == 2880 && fake.clears == 1);
    fake = (fake_t){.inserted = true}; initialize(&ufi, &fake);
    fake.command_stall = true;
    assert(solar_os_usb_ufi_refresh(&ufi) == SOLAR_OS_UFI_COMMAND_FAILED);
    assert(fake.phase == 0 && fake.resets == 0); /* No wait for stalled ADSC status. */
    uint8_t bad[16] = {0x28};
    assert(solar_os_usb_ufi_execute(&ufi, bad, 16, true, bytes, 512) == ESP_ERR_INVALID_ARG);
    bad[8] = 1;
    assert(solar_os_usb_ufi_execute(&ufi, bad, 10, true, bytes, 1024) == ESP_ERR_INVALID_SIZE);
    puts("UFI/CBI descriptors, phases, sense, media changes, protection and recovery: ok");
    return 0;
}
