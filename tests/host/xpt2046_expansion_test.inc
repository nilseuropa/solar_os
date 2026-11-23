#include <assert.h>
#include <stdio.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>
#include "driver/gpio.h"
#include "driver/spi_master.h"
#include "solar_os_bus_types.h"
#include "solar_os_display.h"
#include "solar_os_expansion.h"
#include "solar_os_input.h"
#include "xpt2046.h"
#define GPIO_IS_VALID_OUTPUT_GPIO(pin) GPIO_IS_VALID_GPIO(pin)
#define pdMS_TO_TICKS(ms) (ms)
#define pdTRUE 1
#define pdPASS 1
#define tskIDLE_PRIORITY 0
#define tskNO_AFFINITY 0
#define SOLAR_OS_TASK_ROLE_BACKGROUND 1
#define SOLAR_OS_TASK_STOP_WAIT_MS 1000
#define SOLAR_OS_MEMORY_TRANSIENT 1
typedef void *TaskHandle_t;
static unsigned allocations, spi_devices, sources, task_count, writes, reads, removed;
static bool present = true, miso = true, irq_high, fail_task, fail_source, fail_default, fail_spi, fail_irq, fail_idle, fail_join, fail_write, fail_remove;
static esp_err_t read_error;
static solar_os_input_pointer_event_t event;
static solar_os_input_pointer_calibration_t defaults;
static spi_device_interface_config_t spi_config;
static solar_os_xpt2046_sample_t raw = {.valid = true, .touched = true, .x = 1000, .y = 2000};
static void *solar_os_memory_calloc(size_t count, size_t size, int cls, const char *tag)
{ (void)cls; (void)tag; void *p = calloc(count,size); if(p) ++allocations; return p; }
static void solar_os_memory_free(void *p) { assert(p && allocations); --allocations; free(p); }
bool solar_os_bus_find(const char *name, solar_os_bus_protocol_t protocol, solar_os_bus_info_t *info)
{ assert(protocol == SOLAR_OS_BUS_PROTOCOL_SPI); if(strcmp(name,"spi0"))return false; memset(info,0,sizeof(*info));info->config.spi.miso_pin=miso?13:-1;return true; }
bool solar_os_display_find_target(const char *name, solar_os_display_target_t *target)
{ if(!present || (strcmp(name,"lcd0") && strcmp(name,"lcd1")))return false; memset(target,0,sizeof(*target));target->ready=true;target->native_width=480;target->native_height=320;return true; }
esp_err_t solar_os_bus_spi_add_device(const char *bus, const spi_device_interface_config_t *config, spi_device_handle_t *out)
{ assert(!strcmp(bus,"spi0"));if(fail_spi)return ESP_FAIL;spi_config=*config;++spi_devices;*out=(void*)(uintptr_t)spi_devices;return ESP_OK; }
esp_err_t spi_bus_remove_device(spi_device_handle_t handle)
{ assert(handle && spi_devices);if(fail_remove)return ESP_FAIL;--spi_devices;++removed;return ESP_OK; }
esp_err_t spi_device_polling_transmit(spi_device_handle_t handle, spi_transaction_t *t)
{ assert(handle && t->length==24 && t->flags==(SPI_TRANS_USE_TXDATA|SPI_TRANS_USE_RXDATA));return fail_idle?ESP_FAIL:ESP_OK; }
esp_err_t spi_device_acquire_bus(spi_device_handle_t handle, unsigned wait) { assert(handle && wait<=101); return ESP_OK; }
void spi_device_release_bus(spi_device_handle_t handle) { assert(handle); }
esp_err_t gpio_config(const gpio_config_t *config) { assert(config->mode==GPIO_MODE_INPUT);return fail_irq?ESP_FAIL:ESP_OK; }
int gpio_get_level(gpio_num_t pin) { assert(pin==15); return irq_high; }
esp_err_t solar_os_xpt2046_idle(const solar_os_xpt2046_io_t *io)
{ uint8_t tx[3]={0x90,0,0},rx[3];return io->transfer(io->context,tx,rx); }
esp_err_t solar_os_xpt2046_read(const solar_os_xpt2046_io_t *io, uint16_t threshold, solar_os_xpt2046_sample_t *sample)
{ assert(io->context && threshold);++reads;*sample=raw;return read_error; }
esp_err_t solar_os_input_touch_source_open(const char *name, solar_os_input_source_t *out)
{ assert(name && *name);if(fail_source)return ESP_FAIL;*out=++sources;return ESP_OK; }
void solar_os_input_source_close(solar_os_input_source_t source) { assert(source && sources);--sources; }
esp_err_t solar_os_input_pointer_calibration_default(solar_os_input_source_t source, const solar_os_input_pointer_calibration_t *c)
{ assert(source);defaults=*c;return fail_default?ESP_FAIL:ESP_OK; }
esp_err_t solar_os_input_write_pointer(solar_os_input_source_t source, const solar_os_input_pointer_event_t *value)
{ assert(source);if(fail_write)return ESP_ERR_NO_MEM;event=*value;++writes;return ESP_OK; }
static int solar_os_task_create_pinned_internal(void (*fn)(void *), const char *name, unsigned stack, void *context,
    unsigned priority, TaskHandle_t *out, unsigned core, unsigned role)
{ (void)fn;(void)priority;(void)core;assert(name && stack==3072 && role==1);if(fail_task)return 0;*out=context;++task_count;return pdPASS; }
static bool solar_os_task_wait_done(TaskHandle_t task, volatile bool *done, unsigned wait)
{ assert(task && wait==1000);if(fail_join)return false;if(!*done)--task_count;*done=true;return true; }
static unsigned ulTaskNotifyTake(int clear, unsigned wait) { (void)clear;(void)wait;return 0; }
static void solar_os_task_delete_internal(void *unused) { (void)unused; }
/* SERVICE */
static solar_os_expansion_binding_t bindings[] = {
    {.kind=SOLAR_OS_EXPANSION_BINDING_SPI_BUS,.target="spi0"},
    {.kind=SOLAR_OS_EXPANSION_BINDING_SPI_CS,.target="spi0",.value=7},
    {.kind=SOLAR_OS_EXPANSION_BINDING_DISPLAY_TARGET,.role="display",.target="lcd0"},
    {.kind=SOLAR_OS_EXPANSION_BINDING_GPIO,.role="irq",.value=15},
    {.kind=SOLAR_OS_EXPANSION_BINDING_PARAMETER,.role="swap_xy",.value=1},
    {.kind=SOLAR_OS_EXPANSION_BINDING_PARAMETER,.role="invert_x",.value=1},
};
static void empty(void) { assert(!devices && !allocations && !spi_devices && !sources && !task_count); }
int main(void)
{
    const solar_os_expansion_driver_t *driver=&solar_os_xpt2046_expansion_driver;
    assert(!driver->probe_supported);
    assert(!atomic_flag_test_and_set(&xpt_operation_running));
    assert(driver->attach("touch0",bindings,3)==ESP_ERR_INVALID_STATE);
    assert(driver->detach("touch0")==ESP_ERR_INVALID_STATE);
    atomic_flag_clear(&xpt_operation_running);empty();
    assert(driver->attach("touch0",bindings,2)==ESP_ERR_INVALID_ARG);empty();
    miso=false;assert(driver->attach("touch0",bindings,3)==ESP_ERR_INVALID_ARG);empty();miso=true;
    present=false;assert(driver->attach("touch0",bindings,3)==ESP_ERR_NOT_FOUND);empty();present=true;
    bool *failures[]={&fail_spi,&fail_idle,&fail_source,&fail_default,&fail_task,&fail_irq};
    for(unsigned i=0;i<sizeof(failures)/sizeof(failures[0]);++i) {
        *failures[i]=true;assert(driver->attach("touch0",bindings,4)!=ESP_OK);empty();*failures[i]=false;
    }
    assert(driver->attach("touch0",bindings,6)==ESP_OK);
    assert(defaults.max_x==4095 && defaults.width==480 && defaults.height==320);
    assert(spi_config.clock_speed_hz==2000000 && spi_config.mode==0 && spi_config.flags==0 && spi_config.spics_io_num==7);
    assert(driver->attach("touch0",bindings,3)==ESP_ERR_INVALID_STATE);
    xpt_poll(devices);assert(writes==1 && event.action==SOLAR_OS_INPUT_POINTER_PRESS && event.x==2095 && event.y==1000 && !strcmp(event.target,"lcd0"));
    xpt_poll(devices);assert(writes==1);
    raw.x=1100;xpt_poll(devices);assert(writes==2 && event.action==SOLAR_OS_INPUT_POINTER_MOVE);
    irq_high=true;unsigned before=reads;xpt_poll(devices);assert(reads==before && writes==3 && event.action==SOLAR_OS_INPUT_POINTER_RELEASE);
    irq_high=false;fail_write=true;xpt_poll(devices);assert(!devices->pressed);fail_write=false;xpt_poll(devices);assert(devices->pressed);
    read_error=ESP_FAIL;xpt_poll(devices);xpt_poll(devices);assert(devices->pressed);
    xpt_poll(devices);assert(!devices->pressed && event.action==SOLAR_OS_INPUT_POINTER_RELEASE);
    irq_high=true;read_error=ESP_OK;xpt_poll(devices);assert(devices->failures==0 && devices->pressed); /* Recover ADC/PENIRQ despite high IRQ. */
    present=false;xpt_poll(devices);assert(!devices->pressed);present=true;irq_high=false;
    xpt_poll(devices);assert(devices->pressed);fail_join=true;
    assert(driver->detach("touch0")==ESP_ERR_TIMEOUT && devices && spi_devices==1 && sources==1);
    fail_join=false;fail_write=true;assert(driver->detach("touch0")==ESP_ERR_NO_MEM && devices && devices->pressed);
    fail_write=false;fail_remove=true;assert(driver->detach("touch0")==ESP_FAIL && devices && !devices->pressed);
    fail_remove=false;assert(driver->detach("touch0")==ESP_OK);empty();
    assert(driver->attach("touch0",bindings,3)==ESP_OK);xpt_device_t *first=devices;
    bindings[1].value=6;strcpy(bindings[2].target,"lcd1");
    assert(driver->attach("touch1",bindings,3)==ESP_OK && devices->next==first);
    xpt_poll(devices);assert(!strcmp(event.target,"lcd1"));
    assert(driver->detach("touch0")==ESP_OK && devices && !devices->next);
    assert(driver->detach("touch1")==ESP_OK);empty();
    puts("XPT2046 expansion: independent routing, axes, IRQ, calibration, failures, recovery, queue pressure and joined cleanup passed");
}
