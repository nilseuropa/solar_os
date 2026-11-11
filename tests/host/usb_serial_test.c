#include <assert.h>
#include <pthread.h>
#include <time.h>
#include "../../src/services/solar_os_usb_serial.c"
#include "solar_os_stream.h"

atomic_uint registry_mutex_initializations, registry_dynamic_live;
pthread_barrier_t *registry_dynamic_create_barrier;
static bool class_installed, fail_task, fail_alloc, fail_lines, fail_close, fail_stop, fail_retire;
static unsigned opened, closed, live_buffers;
static usb_serial_port_discovery_fn discovery;
static pthread_barrier_t read_wait, read_resume;
static atomic_bool block_read;
static solar_os_stream_driver_t streams[SOLAR_OS_PORT_MAX];
static bool stream_used[SOLAR_OS_PORT_MAX];

size_t strlcpy(char *dst, const char *src, size_t size)
{ size_t n = strlen(src); if (size) { size_t c = n < size - 1 ? n : size - 1; memcpy(dst, src, c); dst[c] = 0; } return n; }
int64_t esp_timer_get_time(void)
{ struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return (int64_t)t.tv_sec * 1000000 + t.tv_nsec / 1000; }
void vTaskDelay(TickType_t ticks)
{
    if (atomic_exchange(&block_read, false)) { pthread_barrier_wait(&read_wait); pthread_barrier_wait(&read_resume); }
    else { struct timespec t = {.tv_nsec = ticks * 1000000}; nanosleep(&t, NULL); }
}
void *solar_os_memory_alloc(size_t size, solar_os_memory_class_t cls, const char *tag)
{ assert(cls == SOLAR_OS_MEMORY_EXTERNAL_PREFERRED && !strcmp(tag, "usb.serial.rx")); if (fail_alloc) return NULL; live_buffers++; return malloc(size); }
void solar_os_memory_free(void *data) { if (data) live_buffers--; free(data); }
QueueHandle_t solar_os_queue_create(UBaseType_t length, UBaseType_t size)
{ assert(length == 16 && size == sizeof(usb_serial_port_candidate_t)); return malloc(1); }
void solar_os_queue_delete(QueueHandle_t queue) { free(queue); }
int xQueueSend(QueueHandle_t queue, const void *item, TickType_t timeout)
{ (void)queue; (void)item; (void)timeout; return pdTRUE; }
int xQueueReceive(QueueHandle_t queue, void *item, TickType_t timeout)
{ (void)queue; (void)item; (void)timeout; return 0; }
int solar_os_task_create_pinned(void (*task)(void *), const char *name, uint32_t stack,
    void *arg, unsigned priority, void *handle, int core, int role)
{
    (void)task; (void)name; (void)stack; (void)arg; (void)priority; (void)handle; (void)core; (void)role;
    atomic_store(&worker_done, true); /* Drive enumeration deterministically. */
    return fail_task ? 0 : pdPASS;
}
void solar_os_task_delete(void *handle) { (void)handle; }
esp_err_t usb_serial_port_start(usb_serial_port_discovery_fn cb)
{ assert(!class_installed); class_installed = true; discovery = cb; return ESP_OK; }
esp_err_t usb_serial_port_stop(void)
{ if (fail_stop) return ESP_FAIL; assert(class_installed && opened == closed); class_installed = false; discovery = NULL; return ESP_OK; }
esp_err_t usb_serial_port_open(const usb_serial_port_candidate_t *candidate,
    usb_serial_port_device_t *device, usb_serial_port_rx_fn rx, usb_serial_port_event_fn ev, void *user)
{ assert(class_installed && candidate->address); *device = (usb_serial_port_device_t){.kind = candidate->kind, .handle = device, .receive = rx, .event = ev, .user = user}; opened++; return ESP_OK; }
esp_err_t usb_serial_port_close(usb_serial_port_device_t *device)
{ if (fail_close) return ESP_FAIL; assert(device->handle); device->handle = NULL; closed++; return ESP_OK; }
esp_err_t usb_serial_port_write(usb_serial_port_device_t *device, const uint8_t *data, size_t length)
{ assert(device->handle && data && length); return ESP_OK; }
esp_err_t usb_serial_port_line(usb_serial_port_device_t *device, const solar_os_port_serial_config_t *config)
{ assert(device->handle && config->baud_rate); return ESP_OK; }
esp_err_t usb_serial_port_lines(usb_serial_port_device_t *device, bool dtr, bool rts)
{ assert(device->handle); (void)dtr; (void)rts; return fail_lines ? ESP_ERR_NOT_SUPPORTED : ESP_OK; }
esp_err_t usb_serial_port_break(usb_serial_port_device_t *device, uint16_t duration)
{ assert(device->handle && duration == 100); return ESP_OK; }
const char *usb_serial_port_kind_name(usb_serial_port_kind_t kind) { (void)kind; return "test"; }
esp_err_t solar_os_stream_register(const solar_os_stream_driver_t *driver)
{
    for (unsigned i = 0; i < SOLAR_OS_PORT_MAX; i++) if (!stream_used[i]) {
        streams[i] = *driver; stream_used[i] = true; return ESP_OK;
    }
    return ESP_ERR_NO_MEM;
}
esp_err_t solar_os_stream_unregister_many(const char *const *names, size_t count)
{
    if (fail_retire) return ESP_ERR_INVALID_STATE;
    for (size_t i = 0; i < count; i++) {
        bool found = false;
        for (unsigned j = 0; j < SOLAR_OS_PORT_MAX; j++) if (stream_used[j] && !strcmp(streams[j].info.id, names[i])) { stream_used[j] = false; found = true; break; }
        assert(found);
    }
    return ESP_OK;
}
esp_err_t solar_os_stream_unregister(const char *name) { return solar_os_stream_unregister_many(&name, 1); }
static void connect_at(uint8_t address)
{
    usb_serial_port_candidate_t candidate = {.vid = 0x10c4, .pid = 0xea60, .address = address, .kind = SOLAR_OS_USB_SERIAL_CP210X};
    lock(); open_candidate(&candidate); unlock();
}
static void unplug(unsigned index)
{ slots[index].backend.event(ESP_OK, true, slots[index].backend.user); }
static esp_err_t reap(unsigned index) { lock(); esp_err_t err = close_slot(&slots[index]); unlock(); return err; }
static void *pending_read(void *data)
{
    solar_os_port_handle_t handle = *(solar_os_port_handle_t *)data;
    uint8_t byte; size_t count;
    assert(solar_os_port_read(&handle, &byte, 1, 5000, &count) == ESP_ERR_INVALID_STATE && !count);
    return NULL;
}
int main(void)
{
    assert(solar_os_port_init() == ESP_OK);
    fail_task = true; assert(solar_os_usb_serial_start() == ESP_ERR_NO_MEM && !class_installed && !discoveries);
    fail_task = false;
    assert(solar_os_usb_serial_start() == ESP_OK && discovery);
    fail_alloc = true; connect_at(1); assert(!slots[0].reserved && !live_buffers);
    fail_alloc = false; fail_lines = true; connect_at(1); assert(!slots[0].reserved && opened == closed);
    fail_lines = false; connect_at(1); connect_at(1); connect_at(2);
    assert(solar_os_usb_serial_list(NULL, 0) == 2 && live_buffers == 2);
    solar_os_port_handle_t handle = SOLAR_OS_PORT_HANDLE_INIT, other = SOLAR_OS_PORT_HANDLE_INIT;
    assert(solar_os_port_claim("usb-serial0", "test", &handle) == ESP_OK);
    assert(solar_os_port_claim("usb-serial0", "other", &other) == ESP_ERR_INVALID_STATE);
    solar_os_port_serial_config_t config;
    assert(solar_os_port_serial_get(&handle, &config) == ESP_OK && config.baud_rate == 115200 && config.data_bits == 8 && config.dtr && !config.rts);
    assert(solar_os_usb_serial_config("usb-serial0", &config, 0) == ESP_ERR_INVALID_STATE);
    assert(solar_os_usb_serial_prepare_stop() == ESP_ERR_INVALID_STATE && atomic_load(&accepting));
    config.baud_rate = 9600; config.rts = true; fail_lines = true;
    assert(solar_os_port_serial_configure(&handle, &config, SOLAR_OS_SERIAL_BAUD | SOLAR_OS_SERIAL_RTS) == ESP_ERR_NOT_SUPPORTED);
    assert(solar_os_port_serial_get(&handle, &config) == ESP_OK && config.baud_rate == 9600 && !config.rts);
    fail_lines = false;
    uint8_t data[RX_SIZE + 20]; memset(data, 0x51, sizeof(data));
    receive(data, sizeof(data), &slots[0]); size_t count;
    assert(solar_os_port_read(&handle, data, 4, 0, &count) == ESP_OK && count == 4 && data[0] == 0x51);
    assert(solar_os_port_write(&handle, data, 4, &count) == ESP_OK && count == 4);
    solar_os_usb_serial_info_t info[4];
    assert(solar_os_usb_serial_list(info, 4) == 2 && info[0].dropped == 20 && info[0].transmitted == 4);
    unplug(0); assert(reap(0) == ESP_ERR_INVALID_STATE && slots[0].reserved && !slots[0].backend.handle);
    solar_os_port_handle_t stale = handle;
    assert(solar_os_port_read(&handle, data, 1, 0, &count) == ESP_ERR_INVALID_STATE);
    connect_at(3); assert(slots[2].info.device.address == 3); /* Old owner retains its name. */
    assert(solar_os_port_release(&handle) == ESP_OK && reap(0) == ESP_OK);
    connect_at(4);
    assert(solar_os_port_read(&stale, data, 1, 0, &count) == ESP_ERR_INVALID_ARG);
    assert(solar_os_usb_serial_send_break("usb-serial0", 100) == ESP_OK);
    uint32_t mask = 0;
    assert(solar_os_usb_serial_parse_option("baud", "230400", &config, &mask) == ESP_OK);
    assert(solar_os_usb_serial_parse_option("baud", "9600", &config, &mask) == ESP_ERR_INVALID_ARG);
    mask = 0; assert(solar_os_usb_serial_parse_option("stop", "1.5", &config, &mask) == ESP_OK && config.stop_bits == 1);
    const char *bad[] = {"-1", "", "4000001", "9600x", "4294967296"};
    for (unsigned i = 0; i < sizeof(bad) / sizeof(bad[0]); i++) { mask = 0; assert(solar_os_usb_serial_parse_option("baud", bad[i], &config, &mask) == ESP_ERR_INVALID_ARG); }

    /* Releasing a lease cannot recycle its callback context during pending I/O. */
    assert(solar_os_port_claim("usb-serial0", "reader", &handle) == ESP_OK);
    assert(!pthread_barrier_init(&read_wait, NULL, 2) && !pthread_barrier_init(&read_resume, NULL, 2));
    atomic_store(&block_read, true); pthread_t thread;
    assert(!pthread_create(&thread, NULL, pending_read, &handle)); pthread_barrier_wait(&read_wait);
    assert(solar_os_port_release(&handle) == ESP_OK);
    assert(solar_os_port_claim("usb-serial0", "racing", &other) == ESP_ERR_INVALID_STATE);
    unplug(0); assert(reap(0) == ESP_ERR_INVALID_STATE && slots[0].reserved);
    pthread_barrier_wait(&read_resume); assert(!pthread_join(thread, NULL));
    assert(reap(0) == ESP_OK);
    pthread_barrier_destroy(&read_wait); pthread_barrier_destroy(&read_resume);
    fail_retire = true;
    assert(solar_os_usb_serial_stop() == ESP_ERR_INVALID_STATE && class_installed && solar_os_usb_serial_list(NULL, 0) == 2 && atomic_load(&accepting));
    fail_retire = false; fail_close = true;
    assert(solar_os_usb_serial_stop() == ESP_FAIL && class_installed);
    fail_close = false; fail_stop = true;
    assert(solar_os_usb_serial_stop() == ESP_FAIL && class_installed && !live_buffers);
    fail_stop = false;
    assert(solar_os_usb_serial_stop() == ESP_OK && !class_installed && opened == closed);
    for (unsigned i = 0; i < 20; i++) { assert(solar_os_usb_serial_start() == ESP_OK); connect_at(10); assert(solar_os_usb_serial_stop() == ESP_OK && !live_buffers); }
    assert(atomic_load(&registry_dynamic_live) == 2); /* Lazy service/port locks only. */
    puts("USB serial ownership, hotplug, I/O and teardown: ok");
}
