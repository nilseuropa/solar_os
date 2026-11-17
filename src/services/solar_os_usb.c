#include "solar_os_usb.h"
#include "usb_phy_port.h"

#include <stdio.h>
#include <string.h>
#include "esp_attr.h"
#include "esp_pm.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "solar_os_board_caps.h"
#include "solar_os_board_usb.h"
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
#if SOLAR_OS_PACKAGE_SERVICE_USB_SERIAL
#include "solar_os_usb_serial.h"
#endif

#define USB_OWNER "usb-host"

static SemaphoreHandle_t mode_mutex;
#if SOLAR_OS_BOARD_HAS_CDC && !SOLAR_OS_BOARD_USB_DEFAULT_OFF
#define USB_BOOT_MODE SOLAR_OS_USB_MODE_DEVICE
#else
#define USB_BOOT_MODE SOLAR_OS_USB_MODE_OFF
#endif
static solar_os_usb_mode_t current_mode = SOLAR_OS_USB_MODE_FAULT;
static solar_os_usb_mode_t host_origin_mode = USB_BOOT_MODE;
static bool mode_initialized;
static bool sleep_prepared;
static bool resources_held;
static bool vbus_enabled;
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
            input->keyboard_layout = interface->keyboard_layout;
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
#ifdef SOLAR_OS_BOARD_PIN_USB_VBUS_EN
            {SOLAR_OS_RESOURCE_GPIO_PIN, SOLAR_OS_BOARD_PIN_USB_VBUS_EN, -1, "USB VBUS enable"},
#endif
        };
        const esp_err_t ret = solar_os_resource_claim_bundle(requests,
            sizeof(requests) / sizeof(requests[0]), USB_OWNER, NULL);
        if (ret != ESP_OK) return ret;
        resources_held = true;
    }
    return ESP_OK;
}

static esp_err_t set_cdc_enabled(bool enabled)
{
#if SOLAR_OS_BOARD_HAS_CDC
    /* Driver installation explicitly selects the internal PHY. */
    if (!enabled) {
        solar_os_cdc_status_t status;
        solar_os_cdc_get_status(&status);
        if (!status.initialized && !status.driver_installed) return ESP_OK;
    }
    return enabled ? solar_os_cdc_init() : solar_os_cdc_deinit();
#else
    /* USB-UART consoles do not use the native PHY. Device mode is idle. */
    (void)enabled;
    return ESP_OK;
#endif
}

static esp_err_t set_vbus_enabled(bool enabled)
{
    const esp_err_t ret = solar_os_board_usb_set_vbus(enabled);
#ifdef SOLAR_OS_BOARD_PIN_USB_VBUS_EN
    if (ret == ESP_OK) vbus_enabled = enabled;
#endif
    return ret;
}

static esp_err_t stop_host_port(void)
{
    const esp_err_t ret = usb_host_port_stop();
    const esp_err_t power_ret = resources_held || vbus_enabled ? set_vbus_enabled(false) : ESP_OK;
    return ret == ESP_OK ? power_ret : ret;
}

/* Caller holds mode_mutex. Failed teardown retains ownership until recovery. */
static esp_err_t stop_host_stack(void)
{
#if SOLAR_OS_PACKAGE_SERVICE_USB_SERIAL
    esp_err_t ret = solar_os_usb_serial_prepare_stop();
    if (ret != ESP_OK) return ret;
#endif
#if SOLAR_OS_PACKAGE_SERVICE_USB_STORAGE
    esp_err_t storage_ret = solar_os_usb_storage_stop();
    if (storage_ret != ESP_OK) {
#if SOLAR_OS_PACKAGE_SERVICE_USB_SERIAL
        solar_os_usb_serial_cancel_stop();
#endif
        return storage_ret;
    }
#endif
#if SOLAR_OS_PACKAGE_SERVICE_USB_SERIAL
    ret = solar_os_usb_serial_stop();
    if (ret != ESP_OK) { current_mode = SOLAR_OS_USB_MODE_FAULT; return ret; }
#endif
    const esp_err_t stop_ret = stop_host_port();
    if (stop_ret != ESP_OK) current_mode = SOLAR_OS_USB_MODE_FAULT;
    return stop_ret;
}

static esp_err_t release_host_resources(void)
{
    if (resources_held) {
        const esp_err_t ret = usb_phy_port_release_pins();
        if (ret != ESP_OK) { current_mode = SOLAR_OS_USB_MODE_FAULT; return ret; }
    }
    release_resources();
    return ESP_OK;
}

static esp_err_t restore_mode(solar_os_usb_mode_t mode)
{
    const esp_err_t ret = mode == SOLAR_OS_USB_MODE_DEVICE ? set_cdc_enabled(true) : ESP_OK;
    current_mode = ret == ESP_OK ? mode :
        mode_initialized ? SOLAR_OS_USB_MODE_OFF : SOLAR_OS_USB_MODE_FAULT;
#if SOLAR_OS_BOARD_HAS_CDC
    if (ret != ESP_OK) {
        solar_os_cdc_status_t status;
        solar_os_cdc_get_status(&status);
        if (status.initialized || status.driver_installed) current_mode = SOLAR_OS_USB_MODE_FAULT;
    }
#endif
    mode_initialized = true;
    return ret;
}

static esp_err_t set_mode_locked(solar_os_usb_mode_t mode)
{
    if (mode == current_mode && mode_initialized) return ESP_OK;
    if (mode != SOLAR_OS_USB_MODE_HOST) {
        /* Boot ROM can leave USB pads active without an installed driver. */
        if (mode == SOLAR_OS_USB_MODE_OFF && !resources_held) {
            bool cdc_active = false;
#if SOLAR_OS_BOARD_HAS_CDC
            solar_os_cdc_status_t cdc;
            solar_os_cdc_get_status(&cdc);
            cdc_active = cdc.initialized || cdc.driver_installed;
#endif
            if (!cdc_active) {
                const esp_err_t claim = acquire_resources();
                if (claim != ESP_OK) return claim;
            }
        }
        esp_err_t ret = stop_host_stack();
        if (ret != ESP_OK) return ret;
        if (mode == SOLAR_OS_USB_MODE_OFF) {
            ret = set_cdc_enabled(false);
            if (ret != ESP_OK) {
                if (ret != ESP_ERR_INVALID_STATE) current_mode = SOLAR_OS_USB_MODE_FAULT;
                return ret;
            }
        }
        ret = release_host_resources();
        return ret == ESP_OK ? restore_mode(mode) : ret;
    }
    if (current_mode == SOLAR_OS_USB_MODE_FAULT) return ESP_ERR_INVALID_STATE;
    const solar_os_usb_mode_t previous = current_mode == SOLAR_OS_USB_MODE_SUSPENDED ?
        host_origin_mode : current_mode;
    esp_err_t ret = set_cdc_enabled(false);
    if (ret != ESP_OK) {
        if (ret != ESP_ERR_INVALID_STATE) current_mode = SOLAR_OS_USB_MODE_FAULT;
        return ret;
    }
    ret = acquire_resources();
    if (ret == ESP_OK) ret = acquire_pm();
    if (ret != ESP_OK) {
        rollback_error = release_host_resources();
        if (rollback_error == ESP_OK) rollback_error = restore_mode(previous);
        return ret;
    }
    ret = usb_host_port_start(input_event);
#if SOLAR_OS_PACKAGE_SERVICE_USB_SERIAL
    if (ret == ESP_OK) ret = solar_os_usb_serial_start();
#endif
#if SOLAR_OS_PACKAGE_SERVICE_USB_STORAGE
    if (ret == ESP_OK) ret = solar_os_usb_storage_start();
#endif
    if (ret == ESP_OK) ret = set_vbus_enabled(true);
    if (ret == ESP_OK) ret = usb_host_port_enable();
    if (ret == ESP_OK) {
        current_mode = SOLAR_OS_USB_MODE_HOST;
        host_origin_mode = previous;
        mode_initialized = true;
        return ESP_OK;
    }
    rollback_error = stop_host_stack();
    if (rollback_error == ESP_OK) rollback_error = release_host_resources();
    if (rollback_error == ESP_OK) rollback_error = restore_mode(previous);
    if (rollback_error != ESP_OK) current_mode = SOLAR_OS_USB_MODE_FAULT;
    return ret;
}

esp_err_t solar_os_usb_init(void)
{
    if (solar_os_mutex_get_dynamic(&mode_mutex) == NULL) return ESP_ERR_NO_MEM;
    xSemaphoreTake(mode_mutex, portMAX_DELAY);
    esp_err_t ret = current_mode == SOLAR_OS_USB_MODE_FAULT ? ESP_ERR_INVALID_STATE : ESP_OK;
    if (!mode_initialized) {
        rollback_error = ESP_OK;
        ret = last_error = set_mode_locked(USB_BOOT_MODE);
    }
    xSemaphoreGive(mode_mutex);
    return ret;
}

esp_err_t solar_os_usb_set_mode(solar_os_usb_mode_t mode)
{
    if (mode != SOLAR_OS_USB_MODE_DEVICE && mode != SOLAR_OS_USB_MODE_HOST &&
        mode != SOLAR_OS_USB_MODE_OFF) return ESP_ERR_INVALID_ARG;
    /* Preserve the old UART-only 'device' command as an idle/off alias. */
    if (mode == SOLAR_OS_USB_MODE_DEVICE && !SOLAR_OS_BOARD_HAS_CDC) mode = SOLAR_OS_USB_MODE_OFF;
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
    status->vbus_enabled = vbus_enabled;
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
    else if (!sleep_prepared && current_mode == SOLAR_OS_USB_MODE_HOST) {
#if SOLAR_OS_PACKAGE_SERVICE_USB_SERIAL
        ret = solar_os_usb_serial_prepare_stop();
        if (ret != ESP_OK) { last_error = ret; xSemaphoreGive(mode_mutex); return ret; }
#endif
#if SOLAR_OS_PACKAGE_SERVICE_USB_STORAGE
        ret = solar_os_usb_storage_stop();
        if (ret != ESP_OK) {
#if SOLAR_OS_PACKAGE_SERVICE_USB_SERIAL
            solar_os_usb_serial_cancel_stop();
#endif
            last_error = ret; xSemaphoreGive(mode_mutex); return ret;
        }
#endif
#if SOLAR_OS_PACKAGE_SERVICE_USB_SERIAL
        ret = solar_os_usb_serial_stop();
        if (ret != ESP_OK) { last_error = ret; current_mode = SOLAR_OS_USB_MODE_FAULT; xSemaphoreGive(mode_mutex); return ret; }
#endif
        ret = stop_host_port();
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
    case SOLAR_OS_USB_MODE_OFF: return "off";
    case SOLAR_OS_USB_MODE_DEVICE: return "device";
    case SOLAR_OS_USB_MODE_HOST: return "host";
    case SOLAR_OS_USB_MODE_SUSPENDED: return "suspended";
    case SOLAR_OS_USB_MODE_FAULT: return "fault";
    }
    return "unknown";
}
