#include "solar_os_usb_serial.h"

#include <stdatomic.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "esp_attr.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "solar_os_memory.h"
#include "solar_os_queue.h"
#include "solar_os_sync_dynamic.h"
#include "solar_os_task.h"

#define RX_SIZE 2048U
#define POLL_MS 10U

typedef struct {
    bool reserved, registered;
    atomic_bool present;
    atomic_int error;
    SemaphoreHandle_t io, rx_lock;
    uint8_t *rx;
    size_t head, used;
    usb_serial_port_device_t backend;
    solar_os_usb_serial_info_t info;
} serial_slot_t;

static EXT_RAM_BSS_ATTR serial_slot_t slots[SOLAR_OS_USB_SERIAL_MAX];
static SemaphoreHandle_t guard;
static QueueHandle_t discoveries;
static bool installed;
static atomic_bool accepting, stop_worker, worker_done = true;
static atomic_int last_error;

static void lock(void) { xSemaphoreTake(guard, portMAX_DELAY); }
static void unlock(void) { xSemaphoreGive(guard); }

static void receive(const uint8_t *data, size_t length, void *user)
{
    serial_slot_t *slot = user;
    if (!atomic_load(&slot->present)) return;
    xSemaphoreTake(slot->rx_lock, portMAX_DELAY);
    const size_t copied = length < RX_SIZE - slot->used ? length : RX_SIZE - slot->used;
    const size_t tail = (slot->head + slot->used) % RX_SIZE;
    const size_t first = copied < RX_SIZE - tail ? copied : RX_SIZE - tail;
    memcpy(slot->rx + tail, data, first);
    memcpy(slot->rx, data + first, copied - first);
    slot->used += copied;
    slot->info.received += length;
    slot->info.dropped += length - copied;
    xSemaphoreGive(slot->rx_lock);
}

static void event(esp_err_t error, bool disconnected, void *user)
{
    serial_slot_t *slot = user;
    if (error != ESP_OK) atomic_store(&slot->error, error);
    if (disconnected) atomic_store(&slot->present, false);
}

static void discovered(const usb_serial_port_candidate_t *candidate)
{
    if (!atomic_load(&stop_worker) && xQueueSend(discoveries, candidate, 0) != pdTRUE)
        atomic_store(&last_error, ESP_ERR_NO_MEM);
}

static esp_err_t open_port(void *user)
{
    serial_slot_t *slot = user;
    return atomic_load(&accepting) && atomic_load(&slot->present) ? ESP_OK : ESP_ERR_INVALID_STATE;
}

static esp_err_t read_port(void *user, uint8_t *data, size_t length, uint32_t timeout_ms, size_t *read)
{
    serial_slot_t *slot = user;
    if (read) *read = 0;
    const int64_t deadline = esp_timer_get_time() + (int64_t)timeout_ms * 1000;
    for (;;) {
        if (!atomic_load(&slot->present)) return ESP_ERR_INVALID_STATE;
        xSemaphoreTake(slot->rx_lock, portMAX_DELAY);
        const size_t count = length < slot->used ? length : slot->used;
        const size_t first = count < RX_SIZE - slot->head ? count : RX_SIZE - slot->head;
        if (count) {
            memcpy(data, slot->rx + slot->head, first);
            memcpy(data + first, slot->rx, count - first);
            slot->head = (slot->head + count) % RX_SIZE;
            slot->used -= count;
        }
        xSemaphoreGive(slot->rx_lock);
        if (read) *read = count;
        if (count || !length) return ESP_OK;
        if (esp_timer_get_time() >= deadline) return ESP_ERR_TIMEOUT;
        vTaskDelay(pdMS_TO_TICKS(POLL_MS));
    }
}

static esp_err_t write_port(void *user, const uint8_t *data, size_t length, size_t *written)
{
    serial_slot_t *slot = user;
    if (written) *written = 0;
    xSemaphoreTake(slot->io, portMAX_DELAY);
    esp_err_t err = atomic_load(&slot->present) ? usb_serial_port_write(&slot->backend, data, length) : ESP_ERR_INVALID_STATE;
    if (err == ESP_OK) {
        slot->info.transmitted += length;
        if (written) *written = length;
    } else atomic_store(&slot->error, err);
    xSemaphoreGive(slot->io);
    return err;
}

static esp_err_t control_port(void *user, solar_os_port_control_t request, void *data)
{
    serial_slot_t *slot = user;
    xSemaphoreTake(slot->io, portMAX_DELAY);
    esp_err_t err = ESP_OK;
    if (!atomic_load(&slot->present)) err = ESP_ERR_INVALID_STATE;
    else if (request == SOLAR_OS_PORT_SERIAL_GET) *(solar_os_port_serial_config_t *)data = slot->info.config;
    else if (request == SOLAR_OS_PORT_SERIAL_BREAK)
        err = usb_serial_port_break(&slot->backend, *(uint16_t *)data);
    else if (request == SOLAR_OS_PORT_SERIAL_UPDATE) {
        const solar_os_port_serial_update_t *update = data;
        solar_os_port_serial_config_t next = slot->info.config;
        const uint32_t mask = update->mask;
        if (mask & SOLAR_OS_SERIAL_BAUD) next.baud_rate = update->config.baud_rate;
        if (mask & SOLAR_OS_SERIAL_BITS) next.data_bits = update->config.data_bits;
        if (mask & SOLAR_OS_SERIAL_PARITY) next.parity = update->config.parity;
        if (mask & SOLAR_OS_SERIAL_STOP) next.stop_bits = update->config.stop_bits;
        if (mask & SOLAR_OS_SERIAL_DTR) next.dtr = update->config.dtr;
        if (mask & SOLAR_OS_SERIAL_RTS) next.rts = update->config.rts;
        if (!next.baud_rate || next.baud_rate > 4000000 || next.data_bits < 5 || next.data_bits > 8 ||
            next.parity > 4 || next.stop_bits > 2) err = ESP_ERR_INVALID_ARG;
        const uint32_t coding = SOLAR_OS_SERIAL_BAUD | SOLAR_OS_SERIAL_BITS | SOLAR_OS_SERIAL_PARITY | SOLAR_OS_SERIAL_STOP;
        if (err == ESP_OK && (mask & coding)) {
            err = usb_serial_port_line(&slot->backend, &next);
            if (err == ESP_OK) {
                slot->info.config.baud_rate = next.baud_rate;
                slot->info.config.data_bits = next.data_bits;
                slot->info.config.parity = next.parity;
                slot->info.config.stop_bits = next.stop_bits;
            }
        }
        if (err == ESP_OK && (mask & (SOLAR_OS_SERIAL_DTR | SOLAR_OS_SERIAL_RTS))) {
            err = usb_serial_port_lines(&slot->backend, next.dtr, next.rts);
            if (err == ESP_OK) { slot->info.config.dtr = next.dtr; slot->info.config.rts = next.rts; }
        }
    } else err = ESP_ERR_NOT_SUPPORTED;
    if (err != ESP_OK) atomic_store(&slot->error, err);
    xSemaphoreGive(slot->io);
    return err;
}

/* Caller owns guard. The port registry keeps callback contexts alive until
 * ownership and all in-flight I/O have ended, including after hot removal. */
static esp_err_t close_slot(serial_slot_t *slot)
{
    atomic_store(&slot->present, false);
    if (slot->backend.handle) {
        xSemaphoreTake(slot->io, portMAX_DELAY);
        esp_err_t err = usb_serial_port_close(&slot->backend);
        xSemaphoreGive(slot->io);
        if (err != ESP_OK) return err;
    }
    if (slot->registered) {
        esp_err_t err = solar_os_port_unregister(slot->info.port);
        if (err != ESP_OK) return err;
        slot->registered = false;
    }
    solar_os_memory_free(slot->rx);
    if (slot->io) vSemaphoreDelete(slot->io);
    if (slot->rx_lock) vSemaphoreDelete(slot->rx_lock);
    memset(slot, 0, sizeof(*slot));
    return ESP_OK;
}

static void open_candidate(const usb_serial_port_candidate_t *candidate)
{
    serial_slot_t *slot = NULL;
    for (size_t i = 0; i < SOLAR_OS_USB_SERIAL_MAX; i++) {
        if (slots[i].reserved && atomic_load(&slots[i].present) &&
            slots[i].info.device.address == candidate->address &&
            slots[i].info.device.interface == candidate->interface) return;
        if (!slots[i].reserved && !slot) slot = &slots[i];
    }
    if (!slot) { atomic_store(&last_error, ESP_ERR_NO_MEM); return; }
    slot->reserved = true;
    slot->info.device = *candidate;
    snprintf(slot->info.port, sizeof(slot->info.port), "usb-serial%u", (unsigned)(slot - slots));
    slot->io = xSemaphoreCreateMutex();
    slot->rx_lock = xSemaphoreCreateMutex();
    slot->rx = solar_os_memory_alloc(RX_SIZE, SOLAR_OS_MEMORY_EXTERNAL_PREFERRED, "usb.serial.rx");
    esp_err_t err = slot->io && slot->rx_lock && slot->rx ? ESP_OK : ESP_ERR_NO_MEM;
    atomic_store(&slot->present, err == ESP_OK);
    if (err == ESP_OK) err = usb_serial_port_open(candidate, &slot->backend, receive, event, slot);
    const solar_os_port_serial_config_t initial = {.baud_rate = 115200, .data_bits = 8, .dtr = true};
    if (err == ESP_OK) err = control_port(slot, SOLAR_OS_PORT_SERIAL_UPDATE,
        &(solar_os_port_serial_update_t){.config = initial, .mask = SOLAR_OS_SERIAL_ALL});
    if (err == ESP_OK && !atomic_load(&slot->present)) err = ESP_ERR_INVALID_STATE;
    if (err == ESP_OK) {
        char label[SOLAR_OS_PORT_LABEL_MAX];
        snprintf(label, sizeof(label), "%s %04x:%04x", usb_serial_port_kind_name(candidate->kind), candidate->vid, candidate->pid);
        const solar_os_port_driver_t driver = {.name = slot->info.port, .label = label,
            .capabilities = SOLAR_OS_PORT_CAP_READ | SOLAR_OS_PORT_CAP_WRITE | SOLAR_OS_PORT_CAP_CONFIG | SOLAR_OS_PORT_CAP_SERIAL |
                (candidate->kind == SOLAR_OS_USB_SERIAL_JTAG ? SOLAR_OS_PORT_CAP_USB_JTAG : 0),
            .read = read_port, .write = write_port, .open = open_port, .control = control_port, .user = slot};
        err = solar_os_port_register(&driver);
        slot->registered = err == ESP_OK;
    }
    if (err != ESP_OK) { atomic_store(&last_error, err); (void)close_slot(slot); }
}

static void serial_task(void *user)
{
    (void)user;
    while (!atomic_load(&stop_worker)) {
        usb_serial_port_candidate_t candidate;
        const bool found = xQueueReceive(discoveries, &candidate, pdMS_TO_TICKS(POLL_MS)) == pdTRUE;
        lock();
        if (!atomic_load(&stop_worker)) {
            for (size_t i = 0; i < SOLAR_OS_USB_SERIAL_MAX; i++)
                if (slots[i].reserved && !atomic_load(&slots[i].present)) (void)close_slot(&slots[i]);
            if (found && atomic_load(&accepting)) open_candidate(&candidate);
        }
        unlock();
    }
    atomic_store(&worker_done, true);
    solar_os_task_delete(NULL);
}

esp_err_t solar_os_usb_serial_start(void)
{
    if (!solar_os_mutex_get_dynamic(&guard)) return ESP_ERR_NO_MEM;
    lock();
    if (installed) { unlock(); return ESP_ERR_INVALID_STATE; }
    discoveries = solar_os_queue_create(16, sizeof(usb_serial_port_candidate_t));
    if (!discoveries) { unlock(); return ESP_ERR_NO_MEM; }
    atomic_store(&stop_worker, false); atomic_store(&worker_done, true);
    atomic_store(&accepting, true); atomic_store(&last_error, ESP_OK);
    esp_err_t err = usb_serial_port_start(discovered);
    installed = err == ESP_OK;
    if (err == ESP_OK) {
        atomic_store(&worker_done, false);
        if (solar_os_task_create_pinned(serial_task, "usb-serial", 4096, NULL, 3, NULL, 0,
            SOLAR_OS_TASK_ROLE_SYSTEM) != pdPASS) {
            atomic_store(&worker_done, true); atomic_store(&stop_worker, true);
            if (usb_serial_port_stop() == ESP_OK) installed = false;
            err = ESP_ERR_NO_MEM;
        }
    }
    if (!installed) { solar_os_queue_delete(discoveries); discoveries = NULL; atomic_store(&accepting, false); }
    unlock(); return err;
}

esp_err_t solar_os_usb_serial_prepare_stop(void)
{
    if (!guard) return ESP_OK;
    lock();
    const bool previous = atomic_exchange(&accepting, false);
    for (size_t i = 0; i < SOLAR_OS_USB_SERIAL_MAX; i++) {
        solar_os_port_info_t info;
        if (slots[i].registered && solar_os_port_get_info(slots[i].info.port, &info) == ESP_OK &&
            (info.claimed || info.operations)) {
            atomic_store(&accepting, previous); unlock(); return ESP_ERR_INVALID_STATE;
        }
    }
    unlock(); return ESP_OK;
}

void solar_os_usb_serial_cancel_stop(void)
{
    if (!guard) return;
    lock(); if (installed && !atomic_load(&stop_worker)) atomic_store(&accepting, true); unlock();
}

esp_err_t solar_os_usb_serial_stop(void)
{
    if (!guard) return ESP_OK;
    esp_err_t err = solar_os_usb_serial_prepare_stop();
    if (err != ESP_OK) return err;
    lock();
    if (!installed) { unlock(); return ESP_OK; }
    const char *names[SOLAR_OS_USB_SERIAL_MAX];
    size_t count = 0;
    for (size_t i = 0; i < SOLAR_OS_USB_SERIAL_MAX; i++)
        if (slots[i].registered) names[count++] = slots[i].info.port;
    if (count) err = solar_os_port_unregister_many(names, count);
    if (err == ESP_OK) {
        for (size_t i = 0; i < SOLAR_OS_USB_SERIAL_MAX; i++) slots[i].registered = false;
        for (size_t i = 0; i < SOLAR_OS_USB_SERIAL_MAX; i++) {
            if (slots[i].reserved) {
                esp_err_t close_error = close_slot(&slots[i]);
                if (close_error != ESP_OK) { err = close_error; break; }
            }
        }
    }
    if (err == ESP_OK) atomic_store(&stop_worker, true);
    else atomic_store(&accepting, true);
    unlock();
    if (err != ESP_OK) return err;
    const int64_t deadline = esp_timer_get_time() + 10000000;
    while (!atomic_load(&worker_done)) {
        if (esp_timer_get_time() >= deadline) return ESP_ERR_TIMEOUT;
        vTaskDelay(pdMS_TO_TICKS(POLL_MS));
    }
    lock(); err = usb_serial_port_stop();
    if (err == ESP_OK) { installed = false; solar_os_queue_delete(discoveries); discoveries = NULL; }
    unlock(); return err;
}

size_t solar_os_usb_serial_list(solar_os_usb_serial_info_t *info, size_t capacity)
{
    if (!guard) return 0;
    lock(); size_t count = 0;
    for (size_t i = 0; i < SOLAR_OS_USB_SERIAL_MAX; i++) {
        serial_slot_t *slot = &slots[i];
        if (!slot->registered) continue;
        if (info && count < capacity) {
            xSemaphoreTake(slot->io, portMAX_DELAY); xSemaphoreTake(slot->rx_lock, portMAX_DELAY);
            info[count] = slot->info;
            info[count].connected = atomic_load(&slot->present);
            info[count].last_error = atomic_load(&slot->error);
            xSemaphoreGive(slot->rx_lock); xSemaphoreGive(slot->io);
        }
        count++;
    }
    unlock(); return count;
}

esp_err_t solar_os_usb_serial_last_error(void) { return atomic_load(&last_error); }

esp_err_t solar_os_usb_serial_config(const char *name, solar_os_port_serial_config_t *config, uint32_t mask)
{
    if (!config) return ESP_ERR_INVALID_ARG;
    solar_os_port_handle_t handle = SOLAR_OS_PORT_HANDLE_INIT;
    esp_err_t err = solar_os_port_claim(name, "usb-serial-config", &handle);
    if (err != ESP_OK) return err;
    if (mask) err = solar_os_port_serial_configure(&handle, config, mask);
    if (err == ESP_OK) err = solar_os_port_serial_get(&handle, config);
    esp_err_t release = solar_os_port_release(&handle);
    return err == ESP_OK ? release : err;
}

esp_err_t solar_os_usb_serial_send_break(const char *name, uint16_t duration_ms)
{
    solar_os_port_handle_t handle = SOLAR_OS_PORT_HANDLE_INIT;
    esp_err_t err = solar_os_port_claim(name, "usb-serial-config", &handle);
    if (err != ESP_OK) return err;
    err = solar_os_port_serial_break(&handle, duration_ms);
    esp_err_t release = solar_os_port_release(&handle);
    return err == ESP_OK ? release : err;
}

esp_err_t solar_os_usb_serial_parse_option(const char *key, const char *value,
    solar_os_port_serial_config_t *config, uint32_t *mask)
{
    if (!key || !value || !config || !mask) return ESP_ERR_INVALID_ARG;
    uint32_t bit = 0;
    if (!strcmp(key, "parity")) {
        static const char *names[] = {"none", "odd", "even", "mark", "space"};
        unsigned i = 0;
        while (i < 5 && strcmp(names[i], value)) i++;
        if (i == 5) return ESP_ERR_INVALID_ARG;
        config->parity = i; bit = SOLAR_OS_SERIAL_PARITY;
    } else if (!strcmp(key, "stop")) {
        if (!strcmp(value, "1")) config->stop_bits = 0;
        else if (!strcmp(value, "1.5")) config->stop_bits = 1;
        else if (!strcmp(value, "2")) config->stop_bits = 2;
        else return ESP_ERR_INVALID_ARG;
        bit = SOLAR_OS_SERIAL_STOP;
    } else {
        unsigned long number;
        if (!strcmp(value, "true")) number = 1;
        else if (!strcmp(value, "false")) number = 0;
        else {
            if (value[0] < '0' || value[0] > '9') return ESP_ERR_INVALID_ARG;
            char *end;
            errno = 0; number = strtoul(value, &end, 10);
            if (errno || *end || number > UINT32_MAX) return ESP_ERR_INVALID_ARG;
        }
        if (!strcmp(key, "baud") && number && number <= 4000000) {
            config->baud_rate = number; bit = SOLAR_OS_SERIAL_BAUD;
        } else if (!strcmp(key, "bits") && number >= 5 && number <= 8) {
            config->data_bits = number; bit = SOLAR_OS_SERIAL_BITS;
        } else if (!strcmp(key, "dtr") && number <= 1) {
            config->dtr = number; bit = SOLAR_OS_SERIAL_DTR;
        } else if (!strcmp(key, "rts") && number <= 1) {
            config->rts = number; bit = SOLAR_OS_SERIAL_RTS;
        } else return ESP_ERR_INVALID_ARG;
    }
    if (*mask & bit) return ESP_ERR_INVALID_ARG;
    *mask |= bit;
    return ESP_OK;
}
