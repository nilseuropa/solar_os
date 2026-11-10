#include "solar_os_usb.h"

#include <stdio.h>
#include <string.h>
#include "esp_attr.h"
#include "esp_pm.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "solar_os_board_caps.h"
#if SOLAR_OS_BOARD_HAS_CDC
#include "solar_os_cdc.h"
#endif
#include "solar_os_resources.h"
#include "solar_os_sync_dynamic.h"
#include "solar_os_usb_hid_input.h"
#include "solar_os_config.h"
#if SOLAR_OS_PACKAGE_SERVICE_USB_STORAGE
#include "solar_os_usb_storage.h"
#endif

#define USB_OWNER "usb-host"

static SemaphoreHandle_t mode_mutex;
static solar_os_usb_mode_t current_mode = SOLAR_OS_USB_MODE_DEVICE;
static bool sleep_prepared;
static bool resources_held;
static esp_err_t last_error, rollback_error;
/* Decoded input state is accessed only from serialized task callbacks. */
static EXT_RAM_BSS_ATTR solar_os_usb_hid_input_t inputs[USB_HOST_PORT_INTERFACE_MAX];
#if CONFIG_PM_ENABLE
static esp_pm_lock_handle_t apb_lock, sleep_lock;
#endif

static esp_err_t input_event(usb_host_port_event_t event,
    const usb_host_port_interface_t *interface, const uint8_t *data, size_t length)
{
    if (interface->slot >= USB_HOST_PORT_INTERFACE_MAX) return ESP_ERR_INVALID_ARG;
    solar_os_usb_hid_input_t *input = &inputs[interface->slot];
    switch (event) {
    case USB_HOST_PORT_OPEN: {
        char name[SOLAR_OS_INPUT_SOURCE_NAME_MAX];
        snprintf(name, sizeof(name), "usb-%s%u",
            interface->protocol == SOLAR_OS_USB_HID_KEYBOARD ? "kbd" :
                interface->protocol == SOLAR_OS_USB_HID_MOUSE ? "mouse" : "joy",
            interface->slot);
        const esp_err_t ret = solar_os_usb_hid_input_open(input, name, interface->protocol);
        if (ret == ESP_OK) {
            input->mouse_layout = interface->mouse_layout;
            if (interface->joystick_layout != NULL) input->joystick_layout = *interface->joystick_layout;
        }
        return ret;
    }
    case USB_HOST_PORT_REPORT:
        return solar_os_usb_hid_input_report(input, data, length);
    case USB_HOST_PORT_RESET:
        solar_os_usb_hid_input_reset(input);
        break;
    case USB_HOST_PORT_CLOSE:
        solar_os_usb_hid_input_close(input);
        break;
    }
    return ESP_OK;
}

static void release_pm(void)
{
#if CONFIG_PM_ENABLE
    if (sleep_lock != NULL) {
        (void)esp_pm_lock_release(sleep_lock);
        (void)esp_pm_lock_delete(sleep_lock);
        sleep_lock = NULL;
    }
    if (apb_lock != NULL) {
        (void)esp_pm_lock_release(apb_lock);
        (void)esp_pm_lock_delete(apb_lock);
        apb_lock = NULL;
    }
#endif
}

static esp_err_t acquire_pm(void)
{
#if CONFIG_PM_ENABLE
    if (apb_lock != NULL && sleep_lock != NULL) return ESP_OK;
    esp_err_t ret = esp_pm_lock_create(ESP_PM_APB_FREQ_MAX, 0, USB_OWNER, &apb_lock);
    if (ret != ESP_OK) return ret;
    ret = esp_pm_lock_acquire(apb_lock);
    if (ret != ESP_OK) {
        (void)esp_pm_lock_delete(apb_lock);
        apb_lock = NULL;
        return ret;
    }
    ret = esp_pm_lock_create(ESP_PM_NO_LIGHT_SLEEP, 0, USB_OWNER, &sleep_lock);
    if (ret == ESP_OK) {
        ret = esp_pm_lock_acquire(sleep_lock);
        if (ret != ESP_OK) {
            (void)esp_pm_lock_delete(sleep_lock);
            sleep_lock = NULL;
        }
    }
    if (ret != ESP_OK) release_pm();
    return ret;
#else
    return ESP_OK;
#endif
}

static void release_resources(void)
{
    release_pm();
    if (resources_held) {
        (void)solar_os_resource_release_owner(USB_OWNER);
        resources_held = false;
    }
}

static esp_err_t acquire_resources(void)
{
    if (!resources_held) {
        const solar_os_resource_request_t requests[] = {
            {SOLAR_OS_RESOURCE_USB_PORT, 0, -1, "internal USB PHY"},
            {SOLAR_OS_RESOURCE_GPIO_PIN, 19, -1, "USB D-"},
            {SOLAR_OS_RESOURCE_GPIO_PIN, 20, -1, "USB D+"},
        };
        const esp_err_t ret = solar_os_resource_claim_bundle(requests,
            sizeof(requests) / sizeof(requests[0]), USB_OWNER, NULL);
        if (ret != ESP_OK) return ret;
        resources_held = true;
    }
    const esp_err_t ret = acquire_pm();
    if (ret != ESP_OK) release_resources();
    return ret;
}

static esp_err_t set_cdc_enabled(bool enabled)
{
#if SOLAR_OS_BOARD_HAS_CDC
    /* Driver installation explicitly selects the internal PHY. */
    return enabled ? solar_os_cdc_init() : solar_os_cdc_deinit();
#else
    /* USB-UART consoles do not use the native PHY. Device mode is idle. */
    (void)enabled;
    return ESP_OK;
#endif
}

/* Caller holds mode_mutex. A failed teardown retains the PHY, claims and PM
 * locks; device mode must never be installed on top of a live host stack. */
static esp_err_t set_mode_locked(solar_os_usb_mode_t mode)
{
    if (mode == current_mode && mode == SOLAR_OS_USB_MODE_HOST) return ESP_OK;
    if (mode == SOLAR_OS_USB_MODE_DEVICE) {
#if SOLAR_OS_PACKAGE_SERVICE_USB_STORAGE
        esp_err_t storage_ret = solar_os_usb_storage_stop();
        if (storage_ret != ESP_OK) return storage_ret;
#endif
        esp_err_t ret = usb_host_port_stop();
        if (ret != ESP_OK) { current_mode = SOLAR_OS_USB_MODE_FAULT; return ret; }
        ret = set_cdc_enabled(true);
        current_mode = ret == ESP_OK ? SOLAR_OS_USB_MODE_DEVICE : SOLAR_OS_USB_MODE_FAULT;
        release_resources();
        return ret;
    }
    if (current_mode == SOLAR_OS_USB_MODE_FAULT) return ESP_ERR_INVALID_STATE;
    esp_err_t ret = acquire_resources();
    if (ret != ESP_OK) {
        if (current_mode == SOLAR_OS_USB_MODE_SUSPENDED) {
            rollback_error = set_cdc_enabled(true);
            current_mode = rollback_error == ESP_OK ? SOLAR_OS_USB_MODE_DEVICE : SOLAR_OS_USB_MODE_FAULT;
            release_resources();
        }
        return ret;
    }
    ret = set_cdc_enabled(false);
    if (ret != ESP_OK) { release_resources(); return ret; }
    ret = usb_host_port_start(input_event);
#if SOLAR_OS_PACKAGE_SERVICE_USB_STORAGE
    if (ret == ESP_OK) ret = solar_os_usb_storage_start();
#endif
    if (ret == ESP_OK) ret = usb_host_port_enable();
    if (ret == ESP_OK) { current_mode = SOLAR_OS_USB_MODE_HOST; return ESP_OK; }
#if SOLAR_OS_PACKAGE_SERVICE_USB_STORAGE
    rollback_error = solar_os_usb_storage_stop();
    if (rollback_error != ESP_OK) { current_mode = SOLAR_OS_USB_MODE_FAULT; return ret; }
#endif
    rollback_error = usb_host_port_stop();
    if (rollback_error == ESP_OK) {
        rollback_error = set_cdc_enabled(true);
        release_resources();
    }
    current_mode = rollback_error == ESP_OK ? SOLAR_OS_USB_MODE_DEVICE : SOLAR_OS_USB_MODE_FAULT;
    return ret;
}

esp_err_t solar_os_usb_set_mode(solar_os_usb_mode_t mode)
{
    if (mode != SOLAR_OS_USB_MODE_DEVICE && mode != SOLAR_OS_USB_MODE_HOST) return ESP_ERR_INVALID_ARG;
    if (solar_os_mutex_get_dynamic(&mode_mutex) == NULL) return ESP_ERR_NO_MEM;
    xSemaphoreTake(mode_mutex, portMAX_DELAY);
    rollback_error = ESP_OK;
    last_error = sleep_prepared ? ESP_ERR_INVALID_STATE : set_mode_locked(mode);
    const esp_err_t ret = last_error;
    xSemaphoreGive(mode_mutex);
    return ret;
}

void solar_os_usb_get_status(solar_os_usb_status_t *status)
{
    if (status == NULL) return;
    memset(status, 0, sizeof(*status));
    if (solar_os_mutex_get_dynamic(&mode_mutex) == NULL) {
        status->mode = SOLAR_OS_USB_MODE_FAULT;
        status->last_error = ESP_ERR_NO_MEM;
        return;
    }
    xSemaphoreTake(mode_mutex, portMAX_DELAY);
    status->mode = current_mode;
    status->sleep_prepared = sleep_prepared;
    status->last_error = last_error;
    status->rollback_error = rollback_error;
    status->interface_count = usb_host_port_interfaces(status->interfaces,
        USB_HOST_PORT_INTERFACE_MAX);
    xSemaphoreGive(mode_mutex);
}

esp_err_t solar_os_usb_prepare_sleep(void)
{
    if (solar_os_mutex_get_dynamic(&mode_mutex) == NULL) return ESP_ERR_NO_MEM;
    xSemaphoreTake(mode_mutex, portMAX_DELAY);
    esp_err_t ret = ESP_OK;
    if (current_mode == SOLAR_OS_USB_MODE_FAULT) ret = ESP_ERR_INVALID_STATE;
    else if (!sleep_prepared && current_mode != SOLAR_OS_USB_MODE_DEVICE) {
#if SOLAR_OS_PACKAGE_SERVICE_USB_STORAGE
        ret = solar_os_usb_storage_stop();
        if (ret != ESP_OK) { last_error = ret; xSemaphoreGive(mode_mutex); return ret; }
#endif
        ret = usb_host_port_stop();
        if (ret == ESP_OK) {
            current_mode = SOLAR_OS_USB_MODE_SUSPENDED;
            release_pm();
        } else current_mode = SOLAR_OS_USB_MODE_FAULT;
    }
    sleep_prepared = ret == ESP_OK;
    last_error = ret;
    xSemaphoreGive(mode_mutex);
    return ret;
}

esp_err_t solar_os_usb_resume(void)
{
    if (mode_mutex == NULL) return ESP_OK;
    xSemaphoreTake(mode_mutex, portMAX_DELAY);
    sleep_prepared = false;
    esp_err_t ret = ESP_OK;
    if (current_mode == SOLAR_OS_USB_MODE_SUSPENDED) ret = set_mode_locked(SOLAR_OS_USB_MODE_HOST);
    last_error = ret;
    xSemaphoreGive(mode_mutex);
    return ret;
}

const char *solar_os_usb_mode_name(solar_os_usb_mode_t mode)
{
    switch (mode) {
    case SOLAR_OS_USB_MODE_DEVICE: return "device";
    case SOLAR_OS_USB_MODE_HOST: return "host";
    case SOLAR_OS_USB_MODE_SUSPENDED: return "suspended";
    case SOLAR_OS_USB_MODE_FAULT: return "fault";
    }
    return "unknown";
}
