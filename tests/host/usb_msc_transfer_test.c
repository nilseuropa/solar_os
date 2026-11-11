#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include "usb_ufi_msc.h"

#define ESP_ERR_MSC_STALL 0x1704
#define ESP_ERR_MSC_INTERNAL 0x1703
#define MSC_RETURN_ON_ERROR(expr) do { esp_err_t error = (expr); if (error != ESP_OK) return error; } while (0)
#define USB_SETUP_PACKET_SIZE 8
typedef enum { USB_TRANSFER_STATUS_COMPLETED, USB_TRANSFER_STATUS_STALL,
    USB_TRANSFER_STATUS_TIMED_OUT, USB_TRANSFER_STATUS_ERROR } usb_transfer_status_t;
typedef struct transfer {
    uint8_t *data_buffer;
    size_t data_buffer_size, num_bytes, actual_num_bytes;
    int status, bEndpointAddress, device_handle;
    unsigned timeout_ms;
    void *context;
    void (*callback)(struct transfer *);
} usb_transfer_t;
typedef struct {
    usb_transfer_t *xfer;
    int handle, transfer_done;
    struct { uint8_t iface_num, bulk_in_ep, bulk_out_ep; uint16_t bulk_in_mps; } config;
} msc_device_t;
typedef struct __attribute__((packed)) {
    uint8_t bmRequestType, bRequest;
    uint16_t wValue, wIndex, wLength;
} usb_setup_packet_t;
typedef enum { MSC_EP_OUT, MSC_EP_IN } msc_endpoint_t;
static bool allocation_failure;
static int next_status;
static size_t next_actual;
static unsigned frees, submits;
static uint8_t expected_cdb[12];
typedef int BaseType_t;
#define pdTRUE 1
#define pdMS_TO_TICKS(value) (value)
#define portMAX_DELAY 0xffffffffU
static unsigned semaphore_takes, endpoint_operations, root_stops;
static int xSemaphoreTake(int semaphore, unsigned ticks)
{
    assert(semaphore == 9);
    if (!semaphore_takes++) { assert(ticks == 5000); return 0; }
    assert(ticks == portMAX_DELAY); return pdTRUE;
}
static int usb_host_endpoint_halt(int device, unsigned endpoint)
{ assert(device == 7 && endpoint); endpoint_operations++; return ESP_OK; }
static int usb_host_endpoint_flush(int device, unsigned endpoint)
{ assert(device == 7 && endpoint); endpoint_operations++; return ESP_OK; }
static int usb_host_endpoint_clear(int device, unsigned endpoint)
{ assert(device == 7 && endpoint); endpoint_operations++; return ESP_OK; }
static int usb_host_lib_set_root_port_power(bool enable) { assert(!enable); root_stops++; return ESP_OK; }

static size_t usb_round_up_to_mps(size_t size, uint16_t mps) { return (size + mps - 1) / mps * mps; }
static esp_err_t usb_host_transfer_alloc(size_t size, int flags, usb_transfer_t **out)
{
    (void)flags;
    if (allocation_failure) return ESP_ERR_NO_MEM;
    *out = calloc(1, sizeof(**out)); assert(*out);
    (*out)->data_buffer = calloc(1, size); assert((*out)->data_buffer); (*out)->data_buffer_size = size;
    return ESP_OK;
}
static esp_err_t usb_host_transfer_free(usb_transfer_t *transfer)
{ frees++; free(transfer->data_buffer); free(transfer); return ESP_OK; }
static void transfer_callback(usb_transfer_t *transfer) { (void)transfer; }
static int wait_for_transfer_done(usb_transfer_t *transfer) { return transfer->status; }
static esp_err_t usb_host_transfer_submit(usb_transfer_t *transfer)
{
    submits++; assert(transfer->device_handle == 7 && transfer->context && transfer->callback == transfer_callback);
    assert(transfer->timeout_ms == 5000);
    transfer->status = next_status; transfer->actual_num_bytes = next_actual;
    if (transfer->bEndpointAddress & 0x80) memset(transfer->data_buffer, 0x5a, transfer->data_buffer_size);
    return ESP_OK;
}
static esp_err_t msc_control_transfer(msc_device_t *device, size_t size)
{
    usb_setup_packet_t *setup = (usb_setup_packet_t *)device->xfer->data_buffer;
    assert(size == 20 && setup->bmRequestType == 0x21 && !setup->bRequest && !setup->wValue);
    assert(setup->wIndex == device->config.iface_num && setup->wLength == 12);
    assert(!memcmp(device->xfer->data_buffer + 8, expected_cdb, 12));
    device->xfer->actual_num_bytes = next_actual; device->xfer->status = next_status;
    return next_status == USB_TRANSFER_STATUS_COMPLETED ? ESP_OK : ESP_FAIL;
}

#include "msc_endpoint.inc"
#include "msc_adsc.inc"
#include "msc_wait.inc"

int main(void)
{
    msc_device_t device = {.handle = 7, .config = {.iface_num = 2, .bulk_in_ep = 0x81, .bulk_out_ep = 2, .bulk_in_mps = 64}};
    assert(usb_host_transfer_alloc(64, 0, &device.xfer) == ESP_OK);
    uint8_t data[1024] = {0}; size_t actual;
    next_actual = 2;
    assert(solar_os_usb_ufi_msc_endpoint(&device, 0x83, 2, data, 2, &actual) == ESP_OK);
    assert(device.xfer->num_bytes == 2 && device.xfer->bEndpointAddress == 0x83 && actual == 2 && data[0] == 0x5a);
    next_actual = 13;
    assert(msc_bulk_transfer(&device, data, 13, MSC_EP_IN) == ESP_OK);
    assert(device.xfer->num_bytes == 64 && device.xfer->bEndpointAddress == 0x81);
    next_actual = sizeof(data);
    assert(solar_os_usb_ufi_msc_endpoint(&device, 2, 64, data, sizeof(data), &actual) == ESP_OK);
    assert(device.xfer->num_bytes == sizeof(data) && frees == 1 && actual == sizeof(data));
    next_actual = 3;
    assert(solar_os_usb_ufi_msc_endpoint(&device, 0x83, 2, data, 2, &actual) == ESP_ERR_INVALID_SIZE);
    next_status = USB_TRANSFER_STATUS_STALL;
    assert(solar_os_usb_ufi_msc_endpoint(&device, 0x83, 2, data, 2, &actual) == ESP_ERR_MSC_STALL);
    next_status = USB_TRANSFER_STATUS_TIMED_OUT;
    assert(solar_os_usb_ufi_msc_endpoint(&device, 0x83, 2, data, 2, &actual) == ESP_ERR_TIMEOUT);
    usb_transfer_t *old = device.xfer;
    allocation_failure = true;
    assert(solar_os_usb_ufi_msc_endpoint(&device, 0x81, 64, data, 2048, &actual) == ESP_ERR_NO_MEM);
    assert(device.xfer == old && frees == 1); /* Retain the live transfer on allocation failure. */
    allocation_failure = false;
    expected_cdb[0] = 0x12; expected_cdb[4] = 36;
    next_status = USB_TRANSFER_STATUS_COMPLETED; next_actual = 20;
    assert(adsc(&device, expected_cdb) == ESP_OK);
    next_actual = 19; assert(adsc(&device, expected_cdb) == ESP_ERR_INVALID_RESPONSE);
    next_status = USB_TRANSFER_STATUS_STALL; assert(adsc(&device, expected_cdb) == SOLAR_OS_UFI_STALL);
    device.transfer_done = 9; device.xfer->context = &device; device.xfer->bEndpointAddress = 0;
    assert(test_wait_for_transfer_done(device.xfer) == USB_TRANSFER_STATUS_TIMED_OUT);
    assert(root_stops == 1 && !endpoint_operations && semaphore_takes == 2);
    semaphore_takes = 0; device.xfer->bEndpointAddress = 0x83;
    assert(test_wait_for_transfer_done(device.xfer) == USB_TRANSFER_STATUS_TIMED_OUT);
    assert(root_stops == 1 && endpoint_operations == 3 && semaphore_takes == 2);
    assert(usb_host_transfer_free(device.xfer) == ESP_OK && submits == 6);
    return 0;
}
