#include "can_twai.h"
#include <string.h>
#include "driver/twai.h"

esp_err_t solar_os_can_twai_open(solar_os_can_twai_t *d, int tx, int rx,
                                  unsigned bitrate, bool listen_only)
{
    if (!d || !GPIO_IS_VALID_OUTPUT_GPIO(tx) || !GPIO_IS_VALID_GPIO(rx) || tx == rx)
        return ESP_ERR_INVALID_ARG;
    if (d->handle) return ESP_ERR_INVALID_STATE;
    twai_timing_config_t timing;
    switch (bitrate) {
    case 25: timing=(twai_timing_config_t)TWAI_TIMING_CONFIG_25KBITS(); break;
    case 50: timing=(twai_timing_config_t)TWAI_TIMING_CONFIG_50KBITS(); break;
    case 100: timing=(twai_timing_config_t)TWAI_TIMING_CONFIG_100KBITS(); break;
    case 125: timing=(twai_timing_config_t)TWAI_TIMING_CONFIG_125KBITS(); break;
    case 250: timing=(twai_timing_config_t)TWAI_TIMING_CONFIG_250KBITS(); break;
    case 500: timing=(twai_timing_config_t)TWAI_TIMING_CONFIG_500KBITS(); break;
    case 800: timing=(twai_timing_config_t)TWAI_TIMING_CONFIG_800KBITS(); break;
    case 1000: timing=(twai_timing_config_t)TWAI_TIMING_CONFIG_1MBITS(); break;
    default: return ESP_ERR_INVALID_ARG;
    }
    twai_general_config_t general=TWAI_GENERAL_CONFIG_DEFAULT(tx,rx,
        listen_only ? TWAI_MODE_LISTEN_ONLY : TWAI_MODE_NORMAL);
    const twai_filter_config_t filter=TWAI_FILTER_CONFIG_ACCEPT_ALL();
    twai_handle_t handle=NULL;
    esp_err_t err=twai_driver_install_v2(&general,&timing,&filter,&handle);
    if (err != ESP_OK) return err;
    err=twai_start_v2(handle);
    if (err != ESP_OK) { (void)twai_driver_uninstall_v2(handle); return err; }
    d->handle=handle; d->listen_only=listen_only;
    return ESP_OK;
}

esp_err_t solar_os_can_twai_close(solar_os_can_twai_t *d)
{
    if (!d || !d->handle) return ESP_ERR_INVALID_STATE;
    twai_status_info_t info;
    esp_err_t err=twai_get_status_info_v2(d->handle,&info);
    if (err != ESP_OK) return err;
    if (info.state == TWAI_STATE_RECOVERING) return ESP_ERR_INVALID_STATE;
    if (info.state == TWAI_STATE_RUNNING) {
        err=twai_stop_v2(d->handle);
        if (err != ESP_OK) return err;
    }
    err=twai_driver_uninstall_v2(d->handle);
    if (err == ESP_OK) memset(d,0,sizeof(*d));
    return err;
}
esp_err_t solar_os_can_twai_send(solar_os_can_twai_t *d, const solar_os_can_frame_t *f,uint32_t ms)
{
    if (!d || !d->handle) return ESP_ERR_INVALID_STATE;
    if (!f || f->length > 8 || f->id > (f->extended ? 0x1fffffffU : 0x7ffU) || ms > 1000)
        return ESP_ERR_INVALID_ARG;
    if (d->listen_only) return ESP_ERR_NOT_SUPPORTED;
    twai_message_t message={.identifier=f->id,.data_length_code=f->length,
        .flags=(f->extended ? TWAI_MSG_FLAG_EXTD : 0) | (f->remote ? TWAI_MSG_FLAG_RTR : 0)};
    if (!f->remote) memcpy(message.data,f->data,f->length);
    return twai_transmit_v2(d->handle,&message,pdMS_TO_TICKS(ms));
}
esp_err_t solar_os_can_twai_receive(solar_os_can_twai_t *d,solar_os_can_frame_t *f,uint32_t ms)
{
    if (!d || !d->handle) return ESP_ERR_INVALID_STATE;
    if (!f || ms > 1000) return ESP_ERR_INVALID_ARG;
    twai_message_t message;
    esp_err_t err=twai_receive_v2(d->handle,&message,pdMS_TO_TICKS(ms));
    if (err != ESP_OK) return err;
    const bool extended=(message.flags & TWAI_MSG_FLAG_EXTD)!=0;
    if (message.data_length_code > 8 || message.identifier > (extended ? 0x1fffffffU : 0x7ffU))
        return ESP_ERR_INVALID_RESPONSE;
    *f=(solar_os_can_frame_t){.id=message.identifier,.length=message.data_length_code,
        .extended=extended,.remote=(message.flags & TWAI_MSG_FLAG_RTR)!=0};
    if (!f->remote) memcpy(f->data,message.data,f->length);
    return ESP_OK;
}
esp_err_t solar_os_can_twai_status(solar_os_can_twai_t *d,solar_os_can_status_t *s)
{
    if (!d || !d->handle) return ESP_ERR_INVALID_STATE;
    if (!s) return ESP_ERR_INVALID_ARG;
    twai_status_info_t status;
    const esp_err_t err=twai_get_status_info_v2(d->handle,&status);
    if (err != ESP_OK) return err;
    *s=(solar_os_can_status_t){.state=(solar_os_can_state_t)status.state,
        .pending_tx=status.msgs_to_tx,.pending_rx=status.msgs_to_rx,
        .tx_errors=status.tx_error_counter,.rx_errors=status.rx_error_counter,
        .failed_tx=status.tx_failed_count,.missed_rx=status.rx_missed_count+status.rx_overrun_count,
        .bus_errors=status.bus_error_count};
    return ESP_OK;
}
esp_err_t solar_os_can_twai_recover(solar_os_can_twai_t *d)
{ return d && d->handle ? twai_initiate_recovery_v2(d->handle) : ESP_ERR_INVALID_STATE; }
esp_err_t solar_os_can_twai_start(solar_os_can_twai_t *d)
{ return d && d->handle ? twai_start_v2(d->handle) : ESP_ERR_INVALID_STATE; }
