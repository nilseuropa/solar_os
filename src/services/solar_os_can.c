#include "solar_os_can.h"
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "solar_os_gpio_controller.h"

static struct {
    solar_os_can_info_t info;
    solar_os_can_twai_t driver;
    bool select_enabled, select_active;
    solar_os_gpio_line_ref_t select;
} device;
static StaticSemaphore_t mutex_storage;
static SemaphoreHandle_t mutex;
static portMUX_TYPE init_lock=portMUX_INITIALIZER_UNLOCKED;
static void take_lock(void)
{
    portENTER_CRITICAL(&init_lock);
    if (!mutex) mutex=xSemaphoreCreateMutexStatic(&mutex_storage);
    portEXIT_CRITICAL(&init_lock);
    xSemaphoreTake(mutex,portMAX_DELAY);
}
static bool matches(const char *name)
{ return name && device.driver.handle && !strcmp(name,device.info.name); }

esp_err_t solar_os_can_attach(const char *name,const solar_os_expansion_binding_t *bindings,size_t count)
{
    if (!name || !name[0] || strlen(name)>=sizeof(device.info.name) || !bindings) return ESP_ERR_INVALID_ARG;
    int tx=-1,rx=-1,bitrate=500,listen=1,active=1;
    bool have_select=false;
    solar_os_gpio_line_ref_t select={0};
    for(size_t i=0;i<count;i++) {
        const solar_os_expansion_binding_t *b=&bindings[i];
        if(b->kind==SOLAR_OS_EXPANSION_BINDING_GPIO && !strcmp(b->role,"tx") && tx<0) tx=b->value;
        else if(b->kind==SOLAR_OS_EXPANSION_BINDING_GPIO && !strcmp(b->role,"rx") && rx<0) rx=b->value;
        else if(b->kind==SOLAR_OS_EXPANSION_BINDING_GPIO_LINE && !strcmp(b->role,"select") && !have_select) {
            if(b->value<0 || b->value>UINT8_MAX) return ESP_ERR_INVALID_ARG;
            strlcpy(select.controller,b->target,sizeof(select.controller));
            select.line=b->value; have_select=true;
        } else if(b->kind==SOLAR_OS_EXPANSION_BINDING_PARAMETER) {
            if(!strcmp(b->role,"bitrate")) bitrate=b->value;
            else if(!strcmp(b->role,"listen") && b->value>=0 && b->value<=1) listen=b->value;
            else if(!strcmp(b->role,"active") && b->value>=0 && b->value<=1) active=b->value;
            else return ESP_ERR_INVALID_ARG;
        } else return ESP_ERR_INVALID_ARG;
    }
    if(tx<0 || rx<0) return ESP_ERR_INVALID_ARG;
    take_lock();
    if(device.info.name[0]) { xSemaphoreGive(mutex); return ESP_ERR_INVALID_STATE; }
    esp_err_t err=have_select ? solar_os_gpio_line_write(&select,active) : ESP_OK;
    if(err==ESP_OK) err=solar_os_can_twai_open(&device.driver,tx,rx,bitrate,listen);
    if(err==ESP_OK) {
        strlcpy(device.info.name,name,sizeof(device.info.name));
        device.info.bitrate_kbit=bitrate; device.info.listen_only=listen;
        device.select_enabled=have_select; device.select_active=active; device.select=select;
    } else if(have_select) (void)solar_os_gpio_line_write(&select,!active);
    xSemaphoreGive(mutex);
    return err;
}
esp_err_t solar_os_can_detach(const char *name)
{
    take_lock();
    esp_err_t err=matches(name) ? solar_os_can_twai_close(&device.driver) : ESP_ERR_NOT_FOUND;
    if(err==ESP_OK) {
        if(device.select_enabled) err=solar_os_gpio_line_write(&device.select,!device.select_active);
        /* The backend is already closed. Keep metadata until selection is
         * released successfully so a retry can finish detaching the device. */
        if(err==ESP_OK) memset(&device,0,sizeof(device));
    } else if(name && !device.driver.handle && !strcmp(name,device.info.name)) {
        err=device.select_enabled ? solar_os_gpio_line_write(&device.select,!device.select_active) : ESP_OK;
        if(err==ESP_OK) memset(&device,0,sizeof(device));
    }
    xSemaphoreGive(mutex);
    return err;
}
bool solar_os_can_get_info(solar_os_can_info_t *info)
{
    if(!info) return false;
    take_lock();
    const bool found=device.driver.handle!=NULL;
    if(found) *info=device.info;
    xSemaphoreGive(mutex);
    return found;
}
esp_err_t solar_os_can_send(const char *name,const solar_os_can_frame_t *f,uint32_t ms)
{
    take_lock();
    esp_err_t err=matches(name) ? solar_os_can_twai_send(&device.driver,f,ms) : ESP_ERR_NOT_FOUND;
    xSemaphoreGive(mutex); return err;
}
esp_err_t solar_os_can_receive(const char *name,solar_os_can_frame_t *f,uint32_t ms)
{
    take_lock();
    esp_err_t err=matches(name) ? solar_os_can_twai_receive(&device.driver,f,ms) : ESP_ERR_NOT_FOUND;
    xSemaphoreGive(mutex); return err;
}
esp_err_t solar_os_can_status(const char *name,solar_os_can_status_t *s)
{
    take_lock();
    esp_err_t err=matches(name) ? solar_os_can_twai_status(&device.driver,s) : ESP_ERR_NOT_FOUND;
    xSemaphoreGive(mutex); return err;
}
esp_err_t solar_os_can_recover(const char *name)
{
    take_lock();
    esp_err_t err=matches(name) ? solar_os_can_twai_recover(&device.driver) : ESP_ERR_NOT_FOUND;
    xSemaphoreGive(mutex); return err;
}
esp_err_t solar_os_can_start(const char *name)
{
    take_lock();
    esp_err_t err=matches(name) ? solar_os_can_twai_start(&device.driver) : ESP_ERR_NOT_FOUND;
    xSemaphoreGive(mutex); return err;
}
