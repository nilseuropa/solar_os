from pathlib import Path
import importlib.util
import re
import subprocess
import tempfile
import unittest


ROOT = Path(__file__).resolve().parents[1]
SPEC = importlib.util.spec_from_file_location("patch_usb_hid", ROOT / "scripts/patch_usb_hid.py")
PATCH = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(PATCH)
FIXTURE = ROOT / "tests/fixtures/usb_host_hid_transfers_1_2_1.c"


def function(source, name):
    match = re.search(r"^(?:static )?(?:esp_err_t|void) " + name
                      + r"\([^;]*?\n\{.*?^\}", source, re.M | re.S)
    assert match, name
    return match.group()


def run_c(source):
    with tempfile.TemporaryDirectory() as directory:
        path = Path(directory)
        (path / "test.c").write_text(source)
        result = subprocess.run(
            ["cc", "-std=c11", "-Wall", "-Wextra", "-Werror",
             "-I", str(ROOT / "tests/host"), "-I", str(ROOT / "src/drivers"),
             str(path / "test.c"), "-o", str(path / "test")],
            capture_output=True, text=True)
        if result.returncode:
            raise AssertionError(result.stderr)
        subprocess.run([str(path / "test")], check=True)


class UsbHostDriverTest(unittest.TestCase):
    def test_hub_storage_connections_keep_first_drive_until_closed(self):
        source = (ROOT / "src/drivers/usb_msc_port.c").read_text()
        run_c(r'''
#include <assert.h>
#include <stdbool.h>
#include <stdatomic.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"
enum { MSC_DEVICE_CONNECTED, MSC_DEVICE_DISCONNECTED };
typedef void *msc_host_device_handle_t;
typedef struct {
    int event;
    union { uint8_t address; msc_host_device_handle_t handle; } device;
} msc_host_event_t;
static unsigned connections, removals;
static uint8_t queued_address;
static esp_err_t close_error;
static void callback(bool connected, uint8_t address) {
    if (connected) { connections++; queued_address = address; }
    else { removals++; assert(!address); }
}
static msc_host_device_handle_t device;
static void (*event_callback)(bool, uint8_t) = callback;
static atomic_bool present;
static atomic_uint selected_address;
static esp_err_t msc_host_uninstall_device(msc_host_device_handle_t handle) {
    assert(handle == device); return close_error;
}
''' + function(source, "event") + function(source, "solar_os_usb_msc_close") + r'''
int main(void) {
    msc_host_event_t first = {.event = MSC_DEVICE_CONNECTED, .device.address = 2};
    msc_host_event_t second = {.event = MSC_DEVICE_CONNECTED, .device.address = 3};
    event(&first, 0);
    event(&second, 0); /* Second drive must not replace pending setup. */
    assert(connections == 1 && queued_address == 2);
    device = &first; atomic_store(&present, true);
    event(&second, 0); /* Nor schedule a stale reconnect while the first is active. */
    assert(connections == 1 && atomic_load(&present));

    msc_host_event_t gone = {.event = MSC_DEVICE_DISCONNECTED, .device.handle = device};
    event(&gone, 0);
    assert(removals == 1 && !atomic_load(&present));
    close_error = ESP_FAIL;
    assert(solar_os_usb_msc_close() == ESP_FAIL);
    event(&second, 0);
    assert(connections == 1); /* Failed cleanup keeps ownership. */
    close_error = ESP_OK;
    assert(solar_os_usb_msc_close() == ESP_OK);
    event(&second, 0);
    assert(connections == 2 && queued_address == 3);

    /* Failed device installation leaves no handle; cleanup releases selection. */
    assert(!device && solar_os_usb_msc_close() == ESP_OK);
    event(&first, 0);
    assert(connections == 3 && queued_address == 2);
}
''')

    def test_usb_command_is_declared_without_camera_support(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory)
            (path / "solar_os.h").write_text(
                "typedef struct solar_os_context solar_os_context_t;\n")
            (path / "solar_os_config.h").write_text(
                "#define SOLAR_OS_PACKAGE_SERVICE_USB_HOST 1\n"
                "#define SOLAR_OS_PACKAGE_SERVICE_CAMERA 0\n")
            (path / "test.c").write_text(
                '#include "solar_os_shell_commands.h"\n'
                "void (*usb_command)(solar_os_context_t *, int, char **) = solar_os_shell_cmd_usb;\n")
            result = subprocess.run(
                ["cc", "-std=c11", "-Wall", "-Wextra", "-Werror", "-fsyntax-only",
                 "-I", str(path), "-I", str(ROOT / "src/shell"), str(path / "test.c")],
                capture_output=True, text=True)
            self.assertEqual(result.returncode, 0, result.stderr)

    def test_control_completion_status_is_preserved(self):
        source = PATCH.patched_source(FIXTURE.read_text())
        run_c(r'''
#include <assert.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"
#define HID_RETURN_ON_ERROR(expr, ...) do { esp_err_t err = (expr); if (err) return err; } while (0)
#define HID_RETURN_ON_FALSE(expr, err, ...) do { if (!(expr)) return (err); } while (0)
#define ESP_LOG_BUFFER_HEXDUMP(...) ((void)0)
#define pdTRUE 1
#define pdMS_TO_TICKS(ms) (ms)
enum { USB_TRANSFER_STATUS_COMPLETED, USB_TRANSFER_STATUS_STALL,
       USB_TRANSFER_STATUS_ERROR, USB_TRANSFER_STATUS_CANCELED, USB_TRANSFER_STATUS_NO_DEVICE };
typedef int BaseType_t;
typedef struct usb_transfer {
    int device_handle, status, bEndpointAddress;
    void (*callback)(struct usb_transfer *);
    void *context;
    uint32_t timeout_ms;
    size_t num_bytes, actual_num_bytes;
} usb_transfer_t;
typedef struct { usb_transfer_t *ctrl_xfer; int dev_hdl, ctrl_xfer_done; } hid_device_t;
static struct { int client_handle; } driver;
static esp_err_t submit_error;
static BaseType_t completed = pdTRUE;
#define s_hid_driver (&driver)
static void ctrl_xfer_done(usb_transfer_t *transfer) { (void)transfer; }
static esp_err_t usb_host_transfer_submit_control(int client, usb_transfer_t *transfer) {
    assert(client == 9 && transfer->device_handle == 7);
    assert(transfer->callback == ctrl_xfer_done && transfer->context);
    assert(transfer->bEndpointAddress == 0 && transfer->num_bytes == 9);
    return submit_error;
}
static BaseType_t xSemaphoreTake(int semaphore, uint32_t ticks) {
    assert(semaphore == 5 && ticks == 100); return completed;
}
''' + function(source, "hid_control_transfer") + r'''
int main(void) {
    driver.client_handle = 9;
    usb_transfer_t transfer = {.actual_num_bytes = 9};
    hid_device_t device = {&transfer, 7, 5};
    assert(hid_control_transfer(&device, 9, 100) == ESP_OK);
    transfer.actual_num_bytes = 8; /* Valid short read remains accepted. */
    assert(hid_control_transfer(&device, 9, 100) == ESP_OK);
    transfer.actual_num_bytes = 10;
    assert(hid_control_transfer(&device, 9, 100) == ESP_ERR_INVALID_RESPONSE);
    transfer.actual_num_bytes = 0;
    transfer.status = USB_TRANSFER_STATUS_STALL;
    assert(hid_control_transfer(&device, 9, 100) == ESP_ERR_NOT_SUPPORTED);
    transfer.actual_num_bytes = 9;
    assert(hid_control_transfer(&device, 9, 100) == ESP_ERR_NOT_SUPPORTED);
    for (int status = USB_TRANSFER_STATUS_ERROR; status <= USB_TRANSFER_STATUS_NO_DEVICE; status++) {
        transfer.status = status;
        assert(hid_control_transfer(&device, 9, 100) == ESP_ERR_INVALID_RESPONSE);
    }
    completed = 0;
    assert(hid_control_transfer(&device, 9, 100) == ESP_ERR_TIMEOUT);
    submit_error = ESP_ERR_NO_MEM;
    assert(hid_control_transfer(&device, 9, 100) == ESP_ERR_NO_MEM);
}
''')

    def test_patch_rejects_changed_upstream(self):
        source = FIXTURE.read_text()
        for anchor in ("// Check transfer status", "// Relaunch transfer", "s_hid_driver = NULL;",
                       "if (driver->all_events_handled)", "static StaticSemaphore_t s_open_close_mutex_buffer;",
                       "static esp_err_t hid_host_device_disconnected("):
            with self.subTest(anchor=anchor), self.assertRaises(ValueError):
                PATCH.patched_source(source.replace(anchor, "changed"))

    def test_disconnect_waits_for_setup_but_control_events_continue(self):
        source = PATCH.patched_source(FIXTURE.read_text())
        run_c(r'''
#include <assert.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"
#define HID_RETURN_ON_FALSE(expr, err, ...) do { if (!(expr)) return (err); } while (0)
#define ESP_LOGD(...) ((void)0)
#define HID_ENTER_CRITICAL() ((void)0)
#define HID_EXIT_CRITICAL() ((void)0)
#define STAILQ_FOREACH(var, head, member) for ((var) = (head)->first; (var); (var) = (var)->next)
#define pdTRUE 1
#define portMAX_DELAY UINT32_MAX
typedef void *usb_device_handle_t;
typedef struct hid_device {
    struct hid_device *next;
    usb_device_handle_t dev_hdl;
    bool solar_os_disconnect_pending, freed;
} hid_device_t;
static int setup_mutex;
static bool locked;
static unsigned completions, disconnects, unblocks;
static struct {
    void *solar_os_setup_mutex;
    int client_handle;
    void *all_events_handled;
    bool event_handling_started, end_client_event_handling;
    struct { hid_device_t *first; } hid_devices_tailq;
} driver = {.solar_os_setup_mutex = &setup_mutex, .client_handle = 9};
#define s_hid_driver (&driver)
static int xSemaphoreTake(void *mutex, uint32_t ticks) {
    assert(mutex == &setup_mutex);
    assert(ticks == 0 || ticks == portMAX_DELAY);
    if (locked) return 0;
    locked = true; return pdTRUE;
}
static void xSemaphoreGive(void *mutex) { assert(mutex == &setup_mutex && locked); locked = false; }
static esp_err_t usb_host_client_unblock(int client) { assert(client == 9); unblocks++; return ESP_OK; }
static esp_err_t usb_host_client_handle_events(int client, uint32_t timeout) {
    assert(client == 9 && timeout == 0); completions++; return ESP_OK;
}
static hid_device_t *get_hid_device_by_handle(void *handle) {
    for (hid_device_t *device = driver.hid_devices_tailq.first; device; device = device->next) {
        if (device->dev_hdl == handle) return device;
    }
    return NULL;
}
static esp_err_t solar_os_hid_disconnect_impl(void *handle) {
    assert(locked); /* Device/cache destruction is mutually exclusive with setup. */
    hid_device_t **link = &driver.hid_devices_tailq.first;
    while (*link && (*link)->dev_hdl != handle) link = &(*link)->next;
    assert(*link);
    hid_device_t *device = *link;
    *link = device->next;
    device->freed = true;
    disconnects++;
    return ESP_OK;
}
''' + '\n'.join(function(source, name) for name in (
            "solar_os_usb_hid_setup_begin", "solar_os_usb_hid_setup_end",
            "hid_host_device_disconnected", "solar_os_hid_finish_disconnects",
            "hid_host_handle_events")) + r'''
int main(void) {
    hid_device_t first = {.dev_hdl = &first}, second = {.dev_hdl = &second};
    driver.hid_devices_tailq.first = &first;
    solar_os_usb_hid_setup_begin();
    assert(hid_host_device_disconnected(&first) == ESP_OK);
    assert(first.solar_os_disconnect_pending && !first.freed && disconnects == 0);
    assert(hid_host_handle_events(0) == ESP_OK);
    assert(completions == 1 && !first.freed); /* Control completion still reaches the worker. */
    solar_os_usb_hid_setup_end();
    assert(unblocks == 1 && !locked);
    assert(hid_host_handle_events(0) == ESP_OK);
    assert(first.freed && disconnects == 1 && driver.hid_devices_tailq.first == NULL && !locked);

    first = (hid_device_t){.dev_hdl = &first, .next = &second};
    driver.hid_devices_tailq.first = &first;
    solar_os_usb_hid_setup_begin();
    assert(hid_host_device_disconnected(&first) == ESP_OK);
    assert(hid_host_device_disconnected(&second) == ESP_OK);
    assert(!first.freed && !second.freed);
    solar_os_usb_hid_setup_end();
    assert(hid_host_handle_events(0) == ESP_OK);
    assert(first.freed && second.freed && disconnects == 3 && !locked);

    first = (hid_device_t){.dev_hdl = &first};
    driver.hid_devices_tailq.first = &first;
    assert(hid_host_device_disconnected(&first) == ESP_OK);
    assert(first.freed && disconnects == 4 && !locked); /* No setup: immediate cleanup. */
}
''')

    def test_interrupt_resubmission_failure_is_reported(self):
        source = PATCH.patched_source(FIXTURE.read_text())
        run_c(r'''
#include <assert.h>
#include <stddef.h>
#include "esp_err.h"
#define ESP_LOGE(...) ((void)0)
enum { USB_TRANSFER_STATUS_COMPLETED, USB_TRANSFER_STATUS_STALL,
       USB_TRANSFER_STATUS_NO_DEVICE, USB_TRANSFER_STATUS_CANCELED };
enum { HID_HOST_INTERFACE_EVENT_INPUT_REPORT, HID_HOST_INTERFACE_EVENT_TRANSFER_ERROR };
typedef struct { int unused; } hid_iface_t;
typedef struct { void *context; int status; } usb_transfer_t;
static hid_iface_t interface;
static esp_err_t submit_error;
static int inputs, errors, submissions;
static void hid_host_user_interface_callback(hid_iface_t *iface, int event) {
    assert(iface == &interface);
    if (event == HID_HOST_INTERFACE_EVENT_INPUT_REPORT) inputs++;
    else { assert(event == HID_HOST_INTERFACE_EVENT_TRANSFER_ERROR); errors++; }
}
static esp_err_t usb_host_transfer_submit(usb_transfer_t *transfer) {
    assert(transfer->context == &interface); submissions++; return submit_error;
}
''' + function(source, "in_xfer_done") + r'''
int main(void) {
    usb_transfer_t transfer = {&interface, USB_TRANSFER_STATUS_COMPLETED};
    in_xfer_done(&transfer);
    in_xfer_done(&transfer);
    assert(inputs == 2 && submissions == 2 && errors == 0);
    submit_error = ESP_ERR_INVALID_STATE;
    in_xfer_done(&transfer);
    assert(inputs == 3 && submissions == 3 && errors == 1);
    transfer.status = USB_TRANSFER_STATUS_STALL;
    in_xfer_done(&transfer);
    assert(inputs == 3 && submissions == 3 && errors == 2);
    transfer.status = USB_TRANSFER_STATUS_CANCELED;
    in_xfer_done(&transfer);
    transfer.status = USB_TRANSFER_STATUS_NO_DEVICE;
    in_xfer_done(&transfer);
    assert(inputs == 3 && submissions == 3 && errors == 2);
}
''')

    def test_keyboard_mouse_and_joystick_setup_and_optional_requests(self):
        source = (ROOT / "src/drivers/usb_host_port.c").read_text()
        run_c(r'''
#include <assert.h>
#include <stdatomic.h>
#include <string.h>
#include "usb_host_port.h"
enum { HID_SUBCLASS_BOOT_INTERFACE = 1, HID_PROTOCOL_KEYBOARD = 1,
       HID_PROTOCOL_MOUSE = 2, HID_REPORT_PROTOCOL_BOOT = 0, HID_REPORT_PROTOCOL_REPORT = 1 };
typedef int hid_report_protocol_t;
typedef void *hid_host_device_handle_t;
typedef struct { uint8_t addr, iface_num, sub_class, proto; } hid_host_dev_params_t;
typedef struct { uint16_t VID, PID; } hid_host_dev_info_t;
typedef struct { void (*callback)(void); void *callback_arg; } hid_host_device_config_t;
typedef struct { hid_host_device_handle_t handle; usb_host_port_interface_t info; solar_os_usb_hid_joystick_layout_t joystick_layout; bool close_pending; } usb_interface_t;
static usb_interface_t interfaces[USB_HOST_PORT_INTERFACE_MAX];
static atomic_bool stop_worker;
static hid_host_dev_params_t params;
static esp_err_t set_error, report_set_error, idle_error, query_error, start_error, close_error;
static hid_report_protocol_t observed_protocol;
static int idle_calls, starts, opened, closed, descriptor_calls, selections[4], selected_count, queries, closes;
static bool descriptor_available, descriptor_supported, joystick_supported, keyboard_supported;
static void lock(void) { }
static void unlock(void) { }
static void interface_event(void) { }
static esp_err_t hid_host_device_get_params(void *h, hid_host_dev_params_t *out) {
    assert(h); *out = params; return ESP_OK;
}
static esp_err_t hid_host_get_device_info(void *h, hid_host_dev_info_t *out) {
    assert(h); *out = (hid_host_dev_info_t){0x1234, 0x5678}; return ESP_OK;
}
static esp_err_t hid_host_device_open(void *h, const hid_host_device_config_t *config) {
    assert(h && config->callback == interface_event && config->callback_arg == interfaces);
    return ESP_OK;
}
static esp_err_t hid_class_request_set_protocol(void *h, hid_report_protocol_t protocol) {
    assert(h && selected_count < 4);
    selections[selected_count++] = protocol;
    return protocol == HID_REPORT_PROTOCOL_BOOT ? set_error : report_set_error;
}
static const uint8_t *hid_host_get_report_descriptor(void *h, size_t *length) {
    static const uint8_t descriptor[] = {0};
    assert(h); descriptor_calls++; *length = 1;
    return descriptor_available ? descriptor : NULL;
}
esp_err_t solar_os_usb_hid_keyboard_parse_descriptor(const uint8_t *data, size_t length,
    solar_os_usb_hid_keyboard_layout_t *layout) {
    assert(data && length == 1);
    if (!keyboard_supported) return ESP_ERR_NOT_SUPPORTED;
    *layout = (solar_os_usb_hid_keyboard_layout_t){.valid = true, .numbered = true, .report_id = 2};
    return ESP_OK;
}
esp_err_t solar_os_usb_hid_mouse_parse_descriptor(const uint8_t *data, size_t length,
    solar_os_usb_hid_mouse_layout_t *layout) {
    assert(data && length == 1);
    if (!descriptor_supported) return ESP_ERR_NOT_SUPPORTED;
    *layout = (solar_os_usb_hid_mouse_layout_t){.valid = true, .numbered = true, .report_id = 1};
    return ESP_OK;
}
esp_err_t solar_os_usb_hid_joystick_parse_descriptor(const uint8_t *data, size_t length,
    solar_os_usb_hid_joystick_layout_t *layout) {
    assert(data && length == 1);
    if (!joystick_supported) return ESP_ERR_NOT_SUPPORTED;
    *layout = (solar_os_usb_hid_joystick_layout_t){.valid = true, .numbered = true, .report_count = 1};
    layout->reports[0].id = 1;
    return ESP_OK;
}
static esp_err_t hid_host_device_close(void *h) {
    assert(h); closes++;
    if (close_error == ESP_OK) memset(interfaces, 0, sizeof(interfaces));
    return close_error;
}
static esp_err_t hid_class_request_set_idle(void *h, uint8_t duration, uint8_t report) {
    assert(h && duration == 0 && report == 0); idle_calls++; return idle_error;
}
static esp_err_t hid_class_request_get_protocol(void *h, hid_report_protocol_t *protocol) {
    assert(h); queries++; *protocol = observed_protocol; return query_error;
}
static esp_err_t hid_host_device_start(void *h) { assert(h); starts++; return start_error; }
static esp_err_t input_callback(usb_host_port_event_t event,
    const usb_host_port_interface_t *info, const uint8_t *report, size_t length) {
    assert(info == &interfaces[0].info && report == NULL && length == 0);
    if (event == USB_HOST_PORT_OPEN) {
        opened++;
        if (info->protocol == SOLAR_OS_USB_HID_JOYSTICK) {
            assert(info->joystick_layout == &interfaces[0].joystick_layout && info->joystick_layout->valid);
        }
    }
    if (event == USB_HOST_PORT_CLOSE) closed++;
    return ESP_OK;
}
''' + function(source, "open_interface") + r'''
static void reset(uint8_t protocol) {
    memset(interfaces, 0, sizeof(interfaces));
    params = (hid_host_dev_params_t){1, 1, HID_SUBCLASS_BOOT_INTERFACE, protocol};
    set_error = report_set_error = idle_error = query_error = start_error = close_error = ESP_OK;
    observed_protocol = HID_REPORT_PROTOCOL_BOOT;
    idle_calls = starts = opened = closed = descriptor_calls = selected_count = queries = closes = 0;
    descriptor_available = false;
    descriptor_supported = true;
    joystick_supported = false;
    keyboard_supported = false;
}
int main(void) {
    for (uint8_t protocol = HID_PROTOCOL_KEYBOARD; protocol <= HID_PROTOCOL_MOUSE; protocol++) {
        reset(protocol); open_interface(&params);
        assert(idle_calls == 1 && starts == 1 && opened == 1 && closed == 0);
        assert(interfaces[0].info.ready && interfaces[0].info.report_protocol == 0);
        assert(interfaces[0].info.vendor_id == 0x1234 && interfaces[0].info.product_id == 0x5678);
        assert(selected_count == 1 && selections[0] == HID_REPORT_PROTOCOL_BOOT);
        assert(descriptor_calls == (protocol == HID_PROTOCOL_MOUSE ? 1 : 0));
    }
    reset(HID_PROTOCOL_MOUSE); idle_error = query_error = ESP_ERR_NOT_SUPPORTED;
    open_interface(&params);
    assert(interfaces[0].info.ready && starts == 1 && interfaces[0].info.report_protocol == 0xff);
    assert(interfaces[0].info.idle_error == ESP_ERR_NOT_SUPPORTED);
    assert(interfaces[0].info.protocol_error == ESP_ERR_NOT_SUPPORTED);
    reset(HID_PROTOCOL_MOUSE); set_error = ESP_ERR_NOT_SUPPORTED;
    open_interface(&params);
    assert(!interfaces[0].info.ready && starts == 0 && opened == 0 && idle_calls == 0);
    assert(interfaces[0].info.last_error == ESP_ERR_NOT_SUPPORTED && interfaces[0].info.errors == 1);
    reset(HID_PROTOCOL_MOUSE); observed_protocol = HID_REPORT_PROTOCOL_REPORT;
    open_interface(&params);
    assert(!interfaces[0].info.ready && starts == 0 && opened == 0);
    assert(interfaces[0].info.report_protocol == 1 && interfaces[0].info.last_error == ESP_ERR_NOT_SUPPORTED);
    reset(HID_PROTOCOL_MOUSE); start_error = ESP_ERR_NO_MEM;
    open_interface(&params);
    assert(!interfaces[0].info.ready && starts == 1 && opened == 1 && closed == 1);
    assert(interfaces[0].info.last_error == ESP_ERR_NO_MEM);

    reset(HID_PROTOCOL_MOUSE); descriptor_available = true;
    observed_protocol = HID_REPORT_PROTOCOL_REPORT;
    open_interface(&params);
    assert(interfaces[0].info.ready && interfaces[0].info.mouse_layout.valid);
    assert(selected_count == 1 && selections[0] == HID_REPORT_PROTOCOL_REPORT);
    assert(interfaces[0].info.report_protocol == 1 && starts == 1);

    reset(HID_PROTOCOL_MOUSE); descriptor_available = true; descriptor_supported = false;
    open_interface(&params);
    assert(interfaces[0].info.ready && !interfaces[0].info.mouse_layout.valid);
    assert(selected_count == 1 && selections[0] == HID_REPORT_PROTOCOL_BOOT);

    reset(HID_PROTOCOL_MOUSE); descriptor_available = true;
    report_set_error = ESP_ERR_NOT_SUPPORTED;
    open_interface(&params);
    assert(interfaces[0].info.ready && !interfaces[0].info.mouse_layout.valid);
    assert(selected_count == 2 && selections[0] == HID_REPORT_PROTOCOL_REPORT && selections[1] == HID_REPORT_PROTOCOL_BOOT);

    reset(HID_PROTOCOL_MOUSE); descriptor_available = true; /* ACK but still reports boot: fall back. */
    open_interface(&params);
    assert(interfaces[0].info.ready && !interfaces[0].info.mouse_layout.valid && selected_count == 2);

    reset(HID_PROTOCOL_MOUSE); descriptor_available = true; query_error = ESP_ERR_NOT_SUPPORTED;
    open_interface(&params);
    assert(interfaces[0].info.ready && interfaces[0].info.mouse_layout.valid);
    assert(interfaces[0].info.report_protocol == 0xff && selected_count == 1);

    reset(0); params.sub_class = 0; descriptor_available = keyboard_supported = true;
    set_error = report_set_error = query_error = idle_error = ESP_ERR_NOT_SUPPORTED;
    open_interface(&params);
    assert(interfaces[0].info.ready && interfaces[0].info.protocol == SOLAR_OS_USB_HID_KEYBOARD);
    assert(interfaces[0].info.keyboard_layout.valid && interfaces[0].info.keyboard_layout.report_id == 2);
    assert(interfaces[0].info.report_protocol == 1 && interfaces[0].info.protocol_error == ESP_OK);
    assert(selected_count == 0 && queries == 0 && idle_calls == 1 && starts == 1 && opened == 1);

    reset(0); params.sub_class = 0; descriptor_available = keyboard_supported = joystick_supported = true;
    open_interface(&params);
    assert(interfaces[0].info.ready && interfaces[0].info.protocol == SOLAR_OS_USB_HID_JOYSTICK);
    assert(!interfaces[0].info.keyboard_layout.valid); /* Preserve existing combined-controller selection. */

    reset(0); params.sub_class = 0; descriptor_available = joystick_supported = true;
    set_error = report_set_error = query_error = idle_error = ESP_ERR_NOT_SUPPORTED;
    open_interface(&params);
    assert(interfaces[0].info.ready && interfaces[0].info.protocol == SOLAR_OS_USB_HID_JOYSTICK);
    assert(interfaces[0].info.report_protocol == 1 && interfaces[0].info.protocol_error == ESP_OK);
    assert(selected_count == 0 && queries == 0 && idle_calls == 1 && starts == 1 && opened == 1);

    reset(0); params.sub_class = 0; descriptor_available = true;
    open_interface(&params);
    assert(closes == 1 && starts == 0 && opened == 0 && interfaces[0].handle == NULL);

    reset(0); params.sub_class = 0; descriptor_available = true; close_error = ESP_FAIL;
    open_interface(&params);
    assert(closes == 1 && starts == 0 && !interfaces[0].info.ready && interfaces[0].info.errors == 1);
    assert(interfaces[0].info.last_error == ESP_FAIL);
}
''')


if __name__ == "__main__":
    unittest.main()
