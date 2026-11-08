#include "usb_host_port.h"

#include <stdatomic.h>
#include <string.h>
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "solar_os_sync_dynamic.h"
#include "solar_os_queue.h"
#include "solar_os_task.h"
#include "usb/hid_host.h"
#include "usb/usb_host.h"

#define USB_STOP_TIMEOUT_MS 10000U
#define USB_POLL_MS 20U

typedef struct {
    hid_host_device_handle_t handle;
    usb_host_port_interface_t info;
    bool close_pending;
} usb_interface_t;

static SemaphoreHandle_t guard;
static QueueHandle_t connections;
static usb_interface_t interfaces[USB_HOST_PORT_INTERFACE_MAX];
static usb_host_port_callback_t input_callback;
static bool host_installed;
static bool hid_installed;
static bool root_powered;
static atomic_bool stop_worker, worker_done;
static atomic_bool stop_daemon, daemon_done;

static void lock(void) { xSemaphoreTake(guard, portMAX_DELAY); }
static void unlock(void) { xSemaphoreGive(guard); }

static void interface_event(hid_host_device_handle_t handle,
    hid_host_interface_event_t event, void *arg)
{
    usb_interface_t *interface = arg;
    lock();
    if (interface->handle != handle) {
        unlock();
        return;
    }
    if (event == HID_HOST_INTERFACE_EVENT_INPUT_REPORT && interface->info.ready) {
        uint8_t report[64];
        size_t length = 0;
        esp_err_t ret = hid_host_device_get_raw_input_report_data(handle,
            report, sizeof(report), &length);
        if (ret == ESP_OK) ret = input_callback(USB_HOST_PORT_REPORT,
            &interface->info, report, length);
        interface->info.reports++;
        if (ret != ESP_OK) interface->info.errors++;
    } else if (event == HID_HOST_INTERFACE_EVENT_TRANSFER_ERROR) {
        interface->info.errors++;
        interface->info.ready = false;
        (void)input_callback(USB_HOST_PORT_RESET, &interface->info, NULL, 0);
    } else if (event == HID_HOST_INTERFACE_EVENT_DISCONNECTED) {
        interface->info.ready = false;
        (void)input_callback(USB_HOST_PORT_CLOSE, &interface->info, NULL, 0);
        /* The HID driver has already halted/flushed this interface. Its
         * disconnect callback is the documented final-close opportunity. */
        unlock();
        const esp_err_t ret = hid_host_device_close(handle);
        lock();
        if (ret == ESP_OK) memset(interface, 0, sizeof(*interface));
        else {
            interface->info.errors++;
            interface->close_pending = true;
        }
    }
    unlock();
}

static void connected(hid_host_device_handle_t handle,
    hid_host_driver_event_t event, void *arg)
{
    (void)arg;
    if (event != HID_HOST_DRIVER_EVENT_CONNECTED) return;
    /* Unopened interfaces are removed automatically on root-port disconnect.
     * Over-capacity interfaces remain idle until unplug/stop. */
    if (!atomic_load(&stop_worker)) (void)xQueueSend(connections, &handle, 0);
}

static void open_interface(hid_host_device_handle_t handle)
{
    hid_host_dev_params_t params;
    if (hid_host_device_get_params(handle, &params) != ESP_OK) return;
    if (params.sub_class != HID_SUBCLASS_BOOT_INTERFACE ||
        (params.proto != HID_PROTOCOL_KEYBOARD && params.proto != HID_PROTOCOL_MOUSE)) return;

    lock();
    usb_interface_t *interface = NULL;
    for (size_t i = 0; i < USB_HOST_PORT_INTERFACE_MAX; i++) {
        if (interfaces[i].handle == handle) { unlock(); return; }
        if (interfaces[i].handle == NULL && interface == NULL) interface = &interfaces[i];
    }
    if (interface == NULL) { unlock(); return; }
    interface->handle = handle;
    interface->info = (usb_host_port_interface_t){
        .slot = (uint8_t)(interface - interfaces),
        .address = params.addr, .interface = params.iface_num, .protocol = params.proto,
    };
    unlock();

    const hid_host_device_config_t config = {
        .callback = interface_event, .callback_arg = interface,
    };
    esp_err_t ret = hid_host_device_open(handle, &config);
    if (ret != ESP_OK) {
        lock();
        if (interface->handle == handle) memset(interface, 0, sizeof(*interface));
        unlock();
        return;
    }
    if (ret == ESP_OK) ret = hid_class_request_set_protocol(handle, HID_REPORT_PROTOCOL_BOOT);
    /* SET_IDLE is optional on some otherwise valid boot keyboards. */
    if (ret == ESP_OK && params.proto == HID_PROTOCOL_KEYBOARD) {
        (void)hid_class_request_set_idle(handle, 0, 0);
    }
    lock();
    if (interface->handle != handle) { unlock(); return; }
    if (ret == ESP_OK && !atomic_load(&stop_worker)) {
        ret = input_callback(USB_HOST_PORT_OPEN, &interface->info, NULL, 0);
        interface->info.ready = ret == ESP_OK;
    } else if (ret == ESP_OK) ret = ESP_ERR_INVALID_STATE;
    unlock();
    if (ret == ESP_OK) ret = hid_host_device_start(handle);
    if (ret != ESP_OK) {
        lock();
        if (interface->handle == handle) {
            interface->info.ready = false;
            interface->info.errors++;
            (void)input_callback(USB_HOST_PORT_CLOSE, &interface->info, NULL, 0);
        }
        unlock();
        /* Leave the HID handle tracked until physical/root-port disconnect.
         * That keeps control-transfer and disconnect ownership in the driver. */
    }
}

static void connection_task(void *arg)
{
    (void)arg;
    while (!atomic_load(&stop_worker)) {
        hid_host_device_handle_t handle;
        if (xQueueReceive(connections, &handle, pdMS_TO_TICKS(USB_POLL_MS)) == pdTRUE &&
            !atomic_load(&stop_worker)) open_interface(handle);
    }
    atomic_store(&worker_done, true);
    solar_os_task_delete(NULL);
}

static void library_task(void *arg)
{
    (void)arg;
    for (;;) {
        uint32_t flags;
        (void)usb_host_lib_handle_events(pdMS_TO_TICKS(USB_POLL_MS), &flags);
        if (atomic_load(&stop_daemon) && usb_host_device_free_all() == ESP_OK) break;
    }
    atomic_store(&daemon_done, true);
    solar_os_task_delete(NULL);
}

static bool wait_done(atomic_bool *done, int64_t deadline)
{
    while (!atomic_load(done)) {
        if (esp_timer_get_time() >= deadline) return false;
        vTaskDelay(pdMS_TO_TICKS(USB_POLL_MS));
    }
    return true;
}

bool usb_host_port_is_active(void) { return host_installed; }

static void finish_pending_closes(void)
{
    for (size_t i = 0; i < USB_HOST_PORT_INTERFACE_MAX; i++) {
        lock();
        hid_host_device_handle_t handle = interfaces[i].close_pending ? interfaces[i].handle : NULL;
        unlock();
        if (handle == NULL) continue;
        /* Only failed final disconnect closes reach this path. Their
         * interfaces are already halted and await user deletion. */
        const esp_err_t ret = hid_host_device_close(handle);
        lock();
        if (interfaces[i].handle == handle &&
            (ret == ESP_OK || ret == ESP_ERR_INVALID_ARG)) {
            memset(&interfaces[i], 0, sizeof(interfaces[i]));
        }
        unlock();
    }
}

esp_err_t usb_host_port_stop(void)
{
    if (!host_installed) return ESP_OK;
    const int64_t deadline = esp_timer_get_time() + USB_STOP_TIMEOUT_MS * 1000LL;
    /* Finish pending open/protocol operations before inducing disconnect. */
    atomic_store(&stop_worker, true);
    if (!wait_done(&worker_done, deadline)) return ESP_ERR_TIMEOUT;
    if (root_powered) {
        const esp_err_t ret = usb_host_lib_set_root_port_power(false);
        if (ret != ESP_OK) return ret;
        root_powered = false;
    }
    while (hid_installed) {
        finish_pending_closes();
        const esp_err_t ret = hid_host_uninstall();
        if (ret == ESP_OK) { hid_installed = false; break; }
        if (ret != ESP_ERR_INVALID_STATE) return ret;
        if (esp_timer_get_time() >= deadline) return ESP_ERR_TIMEOUT;
        vTaskDelay(pdMS_TO_TICKS(USB_POLL_MS));
    }
    atomic_store(&stop_daemon, true);
    if (!wait_done(&daemon_done, deadline)) return ESP_ERR_TIMEOUT;
    const esp_err_t ret = usb_host_uninstall();
    if (ret != ESP_OK) return ret;
    host_installed = false;
    lock();
    for (size_t i = 0; i < USB_HOST_PORT_INTERFACE_MAX; i++) {
        interfaces[i].info.slot = (uint8_t)i;
        (void)input_callback(USB_HOST_PORT_CLOSE, &interfaces[i].info, NULL, 0);
    }
    memset(interfaces, 0, sizeof(interfaces));
    unlock();
    solar_os_queue_delete(connections);
    connections = NULL;
    input_callback = NULL;
    return ESP_OK;
}

esp_err_t usb_host_port_start(usb_host_port_callback_t callback)
{
    if (callback == NULL) return ESP_ERR_INVALID_ARG;
    if (host_installed) return ESP_ERR_INVALID_STATE;
    if (solar_os_mutex_get_dynamic(&guard) == NULL) return ESP_ERR_NO_MEM;
    connections = solar_os_queue_create(16, sizeof(hid_host_device_handle_t));
    if (connections == NULL) return ESP_ERR_NO_MEM;
    input_callback = callback;
    atomic_store(&stop_worker, false);
    atomic_store(&worker_done, true);
    atomic_store(&stop_daemon, false);
    atomic_store(&daemon_done, true);
    const usb_host_config_t config = {
        .intr_flags = ESP_INTR_FLAG_LEVEL1, .root_port_unpowered = true,
    };
    esp_err_t ret = usb_host_install(&config);
    if (ret != ESP_OK) {
        solar_os_queue_delete(connections);
        connections = NULL;
        input_callback = NULL;
        return ret;
    }
    host_installed = true;
    atomic_store(&daemon_done, false);
    if (solar_os_task_create_pinned(library_task, "usb-lib", 3072, NULL, 3,
        NULL, 0, SOLAR_OS_TASK_ROLE_SYSTEM) != pdPASS) {
        atomic_store(&daemon_done, true);
        ret = ESP_ERR_NO_MEM;
        goto fail;
    }
    const hid_host_driver_config_t hid_config = {
        .create_background_task = true, .task_priority = 3, .stack_size = 4096,
        .core_id = 0, .callback = connected,
    };
    ret = hid_host_install(&hid_config);
    if (ret != ESP_OK) goto fail;
    hid_installed = true;
    atomic_store(&worker_done, false);
    if (solar_os_task_create_pinned(connection_task, "usb-hid", 4096, NULL, 3,
        NULL, 0, SOLAR_OS_TASK_ROLE_SYSTEM) != pdPASS) {
        atomic_store(&worker_done, true);
        ret = ESP_ERR_NO_MEM;
        goto fail;
    }
    ret = usb_host_lib_set_root_port_power(true);
    if (ret != ESP_OK) goto fail;
    root_powered = true;
    return ESP_OK;
fail:
    (void)usb_host_port_stop();
    return ret;
}

size_t usb_host_port_interfaces(usb_host_port_interface_t *out, size_t capacity)
{
    if (guard == NULL || out == NULL) return 0;
    size_t count = 0;
    lock();
    for (size_t i = 0; i < USB_HOST_PORT_INTERFACE_MAX && count < capacity; i++) {
        if (interfaces[i].handle != NULL) out[count++] = interfaces[i].info;
    }
    unlock();
    return count;
}
