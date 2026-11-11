#include "usb_msc_port.h"

#include <stdatomic.h>
#include "usb/msc_host.h"
#include "esp_private/msc_scsi_bot.h"
#include "usb_ufi_msc.h"

static msc_host_device_handle_t device;
static solar_os_usb_msc_callback_t event_callback;
static solar_os_usb_msc_info_t geometry;
static atomic_bool present;
static bool installed;

static void event(const msc_host_event_t *event, void *arg)
{
    (void)arg;
    if (event->event == MSC_DEVICE_CONNECTED) {
        event_callback(true, event->device.address);
    } else if (event->event == MSC_DEVICE_DISCONNECTED) {
        atomic_store(&present, false);
        event_callback(false, 0);
    }
}

esp_err_t solar_os_usb_msc_start(solar_os_usb_msc_callback_t callback)
{
    if (!callback) return ESP_ERR_INVALID_ARG;
    if (installed) return ESP_ERR_INVALID_STATE;
    event_callback = callback;
    atomic_store(&present, false);
    const msc_host_driver_config_t config = {
        .create_backround_task = true, .task_priority = 3,
        .stack_size = 4096, .core_id = 0, .callback = event,
    };
    esp_err_t err = msc_host_install(&config);
    installed = err == ESP_OK;
    return err;
}

esp_err_t solar_os_usb_msc_stop(void)
{
    if (!installed) return ESP_OK;
    if (device) return ESP_ERR_INVALID_STATE;
    esp_err_t err = msc_host_uninstall();
    if (err == ESP_OK) { installed = false; event_callback = NULL; }
    return err;
}

esp_err_t solar_os_usb_msc_open(uint8_t address, solar_os_usb_msc_info_t *info)
{
    if (!info || !address) return ESP_ERR_INVALID_ARG;
    if (!installed || device) return ESP_ERR_INVALID_STATE;
    /* Set before opening so a disconnect during setup cannot be overwritten. */
    atomic_store(&present, true);
    esp_err_t err = msc_host_install_device(address, &device);
    if (err != ESP_OK) { atomic_store(&present, false); return err; }
    msc_host_device_info_t details;
    err = msc_host_get_device_info(device, &details);
    if (err == ESP_OK && (details.sector_size < 512 || details.sector_size > 4096 ||
        (details.sector_size & (details.sector_size - 1)))) err = ESP_ERR_NOT_SUPPORTED;
    if (err == ESP_OK) {
        geometry = (solar_os_usb_msc_info_t){
            .sector_count = details.sector_count, .sector_size = details.sector_size,
            .vendor_id = details.idVendor, .product_id = details.idProduct,
            .media_present = details.sector_count != 0,
        };
        solar_os_usb_ufi_t *ufi = solar_os_usb_ufi_msc_state(device);
        if (ufi) {
            geometry.ufi_cbi = true; geometry.media_revision = ufi->media_revision;
            geometry.media_present = ufi->media_present; geometry.write_protected = ufi->write_protected;
        }
        *info = geometry;
        if (!atomic_load(&present)) err = ESP_ERR_INVALID_STATE;
    }
    return err; /* The caller closes even partially initialized devices. */
}

esp_err_t solar_os_usb_msc_close(void)
{
    if (!device) return ESP_OK;
    esp_err_t err = msc_host_uninstall_device(device);
    if (err == ESP_OK) { device = NULL; atomic_store(&present, false); }
    return err;
}

bool solar_os_usb_msc_present(void) { return atomic_load(&present); }

bool solar_os_usb_msc_media_current(const solar_os_usb_msc_info_t *info)
{
    if (!info || !device || !atomic_load(&present)) return false;
    solar_os_usb_ufi_t *ufi = solar_os_usb_ufi_msc_state(device);
    return !ufi || (ufi->media_present && info->media_revision == ufi->media_revision);
}

esp_err_t solar_os_usb_msc_refresh(solar_os_usb_msc_info_t *info)
{
    if (!info) return ESP_ERR_INVALID_ARG;
    if (!device || !atomic_load(&present)) return ESP_ERR_INVALID_STATE;
    esp_err_t err = ESP_OK;
    solar_os_usb_ufi_t *ufi = solar_os_usb_ufi_msc_state(device);
    if (ufi) {
        err = solar_os_usb_ufi_refresh(ufi);
        geometry.sector_size = ufi->sector_size; geometry.sector_count = ufi->sector_count;
        geometry.media_revision = ufi->media_revision; geometry.media_present = ufi->media_present;
        geometry.write_protected = ufi->write_protected;
    }
    *info = geometry;
    return err;
}

static esp_err_t transfer(uint32_t sector, void *data, size_t bytes, bool write_data)
{
    if (!solar_os_usb_msc_media_current(&geometry)) return ESP_ERR_INVALID_STATE;
    if (!data || !geometry.sector_size || bytes % geometry.sector_size ||
        sector >= geometry.sector_count ||
        bytes / geometry.sector_size > geometry.sector_count - sector) return ESP_ERR_INVALID_SIZE;
    uint8_t *cursor = data;
    while (bytes) {
        const size_t chunk = bytes < 16384U ? bytes : 16384U;
        if (!atomic_load(&present)) return ESP_ERR_INVALID_STATE;
        esp_err_t err = write_data ? scsi_cmd_write10(device, cursor, sector,
            chunk / geometry.sector_size, geometry.sector_size) :
            scsi_cmd_read10(device, cursor, sector, chunk / geometry.sector_size, geometry.sector_size);
        if (err != ESP_OK) return err;
        sector += chunk / geometry.sector_size; cursor += chunk; bytes -= chunk;
    }
    return ESP_OK;
}

esp_err_t solar_os_usb_msc_read(uint32_t sector, void *data, size_t bytes)
{ return transfer(sector, data, bytes, false); }
esp_err_t solar_os_usb_msc_write(uint32_t sector, const void *data, size_t bytes)
{ return transfer(sector, (void *)data, bytes, true); }
