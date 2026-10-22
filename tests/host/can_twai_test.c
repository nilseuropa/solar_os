#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "driver/twai.h"
#include "solar_os_can.h"
#include "solar_os_gpio_controller.h"

static twai_state_t state;
static twai_message_t wire;
static unsigned installs,uninstalls;
static esp_err_t start_error,receive_error,select_error;
static bool selected;
esp_err_t twai_driver_install_v2(const twai_general_config_t *g,const twai_timing_config_t *t,
    const twai_filter_config_t *f,twai_handle_t *p)
{ assert(g->tx_io==20 && g->rx_io==19 && t->bitrate==500 && f); installs++; state=TWAI_STATE_STOPPED; *p=&wire; return ESP_OK; }
esp_err_t twai_driver_uninstall_v2(twai_handle_t p)
{ assert(p==&wire && state!=TWAI_STATE_RUNNING && state!=TWAI_STATE_RECOVERING); uninstalls++; return ESP_OK; }
esp_err_t twai_start_v2(twai_handle_t p)
{ assert(p==&wire); if(start_error) return start_error; state=TWAI_STATE_RUNNING; return ESP_OK; }
esp_err_t twai_stop_v2(twai_handle_t p)
{ assert(p==&wire && state==TWAI_STATE_RUNNING); state=TWAI_STATE_STOPPED; return ESP_OK; }
esp_err_t twai_get_status_info_v2(twai_handle_t p,twai_status_info_t *s)
{ assert(p==&wire); *s=(twai_status_info_t){.state=state,.tx_error_counter=12}; return ESP_OK; }
esp_err_t twai_transmit_v2(twai_handle_t p,const twai_message_t *m,TickType_t ticks)
{ assert(p==&wire && ticks<=1000); wire=*m; return ESP_OK; }
esp_err_t twai_receive_v2(twai_handle_t p,twai_message_t *m,TickType_t ticks)
{ assert(p==&wire && ticks<=1000); if(receive_error) return receive_error; *m=wire; return ESP_OK; }
esp_err_t twai_initiate_recovery_v2(twai_handle_t p)
{ assert(p==&wire); if(state!=TWAI_STATE_BUS_OFF) return ESP_ERR_INVALID_STATE; state=TWAI_STATE_RECOVERING; return ESP_OK; }
esp_err_t solar_os_gpio_line_write(const solar_os_gpio_line_ref_t *l,bool value)
{ assert(!strcmp(l->controller,"io0") && l->line==5); if(select_error) return select_error; selected=value; return ESP_OK; }
int main(void)
{
    solar_os_can_twai_t d={0};
    assert(solar_os_can_twai_open(&d,20,19,123,false)==ESP_ERR_INVALID_ARG);
    start_error=ESP_FAIL;
    assert(solar_os_can_twai_open(&d,20,19,500,false)==ESP_FAIL && !d.handle && installs==uninstalls);
    start_error=ESP_OK;
    assert(solar_os_can_twai_open(&d,20,19,500,false)==ESP_OK);
    solar_os_can_frame_t f={.id=0x7ff,.length=8,.data={1,2,3,4,5,6,7,8}};
    assert(solar_os_can_twai_send(&d,&f,100)==ESP_OK && wire.identifier==0x7ff && wire.data[7]==8);
    f.id=0x800;
    assert(solar_os_can_twai_send(&d,&f,0)==ESP_ERR_INVALID_ARG);
    f.extended=true; f.id=0x1fffffff;
    assert(solar_os_can_twai_send(&d,&f,1001)==ESP_ERR_INVALID_ARG);
    assert(solar_os_can_twai_send(&d,&f,0)==ESP_OK && wire.flags==TWAI_MSG_FLAG_EXTD);
    f.remote=true;
    assert(solar_os_can_twai_send(&d,&f,0)==ESP_OK);
    assert(solar_os_can_twai_receive(&d,&f,0)==ESP_OK && f.remote && f.extended && f.length==8 && f.data[0]==0);
    wire.data_length_code=9;
    assert(solar_os_can_twai_receive(&d,&f,0)==ESP_ERR_INVALID_RESPONSE && f.length==8);
    receive_error=ESP_ERR_TIMEOUT;
    assert(solar_os_can_twai_receive(&d,&f,1)==ESP_ERR_TIMEOUT);
    receive_error=ESP_OK;
    state=TWAI_STATE_BUS_OFF;
    assert(solar_os_can_twai_recover(&d)==ESP_OK);
    assert(solar_os_can_twai_close(&d)==ESP_ERR_INVALID_STATE && d.handle);
    state=TWAI_STATE_STOPPED;
    assert(solar_os_can_twai_start(&d)==ESP_OK);
    assert(solar_os_can_twai_close(&d)==ESP_OK && !d.handle);
    assert(solar_os_can_twai_open(&d,20,19,500,true)==ESP_OK);
    assert(solar_os_can_twai_send(&d,&f,0)==ESP_ERR_NOT_SUPPORTED);
    assert(solar_os_can_twai_close(&d)==ESP_OK);
    const solar_os_expansion_binding_t bindings[]={
        {.kind=SOLAR_OS_EXPANSION_BINDING_GPIO,.role="tx",.value=20},
        {.kind=SOLAR_OS_EXPANSION_BINDING_GPIO,.role="rx",.value=19},
        {.kind=SOLAR_OS_EXPANSION_BINDING_PARAMETER,.role="bitrate",.value=500},
        {.kind=SOLAR_OS_EXPANSION_BINDING_GPIO_LINE,.role="select",.target="io0",.value=5},
    };
    assert(solar_os_can_attach("can0",bindings,4)==ESP_OK && selected);
    solar_os_can_info_t info;
    assert(solar_os_can_get_info(&info) && info.listen_only && info.bitrate_kbit==500);
    select_error=ESP_FAIL;
    assert(solar_os_can_detach("can0")==ESP_FAIL);
    assert(solar_os_can_attach("can1",bindings,4)==ESP_ERR_INVALID_STATE);
    select_error=ESP_OK;
    assert(solar_os_can_detach("can0")==ESP_OK && !selected && !solar_os_can_get_info(&info));
    assert(installs==uninstalls);
    puts("TWAI/CAN: frame bounds, remote frames, listen-only, recovery and cleanup retry: ok");
}
