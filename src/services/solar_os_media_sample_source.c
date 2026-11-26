#include "solar_os_media_sample_source.h"
#include "solar_os_script_media.h"
#include "solar_os_memory.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#if SOLAR_OS_PACKAGE_SERVICE_PIPELINE
#include "solar_os_pipeline.h"
#endif

static bool cancelled(const solar_os_media_sample_source_options_t *options)
{ return options->cancel && options->cancel(options->user); }

esp_err_t solar_os_media_sample_source_read(const char *source,
    const solar_os_media_sample_source_options_t *options, solar_os_media_sample_t **out)
{
    if (!out) return ESP_ERR_INVALID_ARG;
    *out = NULL;
    if (!source || !*source || !options || !options->owner || !*options->owner || !options->max_bytes ||
        !options->timeout_ms || options->timeout_ms > 60000) return ESP_ERR_INVALID_ARG;
    if (strlen(source)>=SOLAR_OS_MEDIA_SAMPLE_SOURCE_MAX) return ESP_ERR_INVALID_SIZE;
    if (cancelled(options)) return ESP_ERR_INVALID_STATE;
    esp_err_t error;
    if (!strncmp(source,"pipeline:",9)) {
#if SOLAR_OS_PACKAGE_SERVICE_PIPELINE
        char *end; errno=0; unsigned long id=strtoul(source+9,&end,10);
        if (errno || !id || id>UINT32_MAX || *end || source[9]<'0' || source[9]>'9') return ESP_ERR_INVALID_ARG;
        error=solar_os_pipeline_result_sample((uint32_t)id,0,NULL,out);
        if (!error && !*out) error=ESP_ERR_NOT_FOUND;
#else
        return ESP_ERR_NOT_SUPPORTED;
#endif
    } else if (strncmp(source,"stream:",7) && strncmp(source,"rtsp://",7)) {
        error=solar_os_media_sample_open_image_cancellable(source,options->max_bytes,options->cancel,options->user,out);
    } else {
        solar_os_script_media_t *media=NULL;
        error=solar_os_script_media_create(options->owner,options->cancel,options->user,&media);
        if (error) return error;
        uint32_t handle=0,frame=0;
        if (!strncmp(source,"stream:",7)) {
            error=solar_os_script_media_snapshot(media,source+7,0,0,0,&frame);
        } else {
#if SOLAR_OS_PACKAGE_SERVICE_RTSP_CLIENT
            error=solar_os_script_media_rtsp_open(media,source,true,false,&handle);
            const int64_t deadline=esp_timer_get_time()+(int64_t)options->timeout_ms*1000;
            while (!error && !frame && !cancelled(options) && esp_timer_get_time()<deadline) {
                error=solar_os_script_media_rtsp_read(media,handle,50,&frame);
                if (!error && !frame) {
                    solar_os_rtsp_client_status_t status; bool ended=false;
                    error=solar_os_script_media_rtsp_status(media,handle,&status,&ended);
                    if (!error && ended) error=status.error ? status.error : ESP_FAIL;
                }
            }
            if (!error && !frame) error=cancelled(options) ? ESP_ERR_INVALID_STATE : ESP_ERR_TIMEOUT;
#else
            (void)handle; error=ESP_ERR_NOT_SUPPORTED;
#endif
        }
        if (!error && !cancelled(options)) {
            solar_os_script_media_frame_t info;
            error=solar_os_script_media_frame(media,frame,&info);
            if (!error && info.jpeg.length>options->max_bytes) error=ESP_ERR_INVALID_SIZE;
            if (!error) {
                const solar_os_media_sample_metadata_t metadata={.source=source,
                    .timestamp_us=info.jpeg.timestamp_us,.width=info.jpeg.width,.height=info.jpeg.height};
                error=solar_os_media_sample_copy_image(info.jpeg.data,info.jpeg.length,&metadata,out);
            }
        } else if (!error) error=ESP_ERR_INVALID_STATE;
        if (frame) {
            esp_err_t release=solar_os_script_media_release(media,frame);
            if (!error) error=release;
        }
        /* Retain ownership through provider shutdown, including cancelled reads. */
        while (solar_os_script_media_close_all(media)!=ESP_OK) vTaskDelay(1);
        solar_os_script_media_destroy(media);
    }
    if (!error && cancelled(options)) error=ESP_ERR_INVALID_STATE;
    if (!error && solar_os_media_sample_image(*out)->length>options->max_bytes) error=ESP_ERR_INVALID_SIZE;
    if (error) { solar_os_media_sample_release(*out); *out=NULL; }
    return error;
}
