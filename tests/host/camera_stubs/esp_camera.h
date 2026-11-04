#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <sys/time.h>
#include "esp_err.h"

typedef enum { FRAMESIZE_QVGA, FRAMESIZE_VGA } framesize_t;
enum { PIXFORMAT_JPEG, CAMERA_FB_IN_PSRAM, CAMERA_GRAB_WHEN_EMPTY, CAMERA_GRAB_LATEST };
#define CAMERA_MODEL_MAX 2
#define OV5640_PID 0x5640
typedef struct { int pid; const char *name; } camera_sensor_info_t;
extern const camera_sensor_info_t camera_sensor[CAMERA_MODEL_MAX];
typedef struct sensor_s {
    struct { uint16_t PID; } id;
    struct { int hmirror, vflip; } status;
    int (*set_hmirror)(struct sensor_s *, int);
    int (*set_vflip)(struct sensor_s *, int);
} sensor_t;
typedef struct {
    int pin_pwdn, pin_reset, pin_xclk, pin_sccb_sda, pin_sccb_scl;
    int pin_d7, pin_d6, pin_d5, pin_d4, pin_d3, pin_d2, pin_d1, pin_d0;
    int pin_vsync, pin_href, pin_pclk, xclk_freq_hz, ledc_timer, ledc_channel;
    int pixel_format, frame_size, jpeg_quality, fb_count, fb_location;
    int grab_mode, sccb_i2c_port;
} camera_config_t;
typedef struct {
    uint8_t *buf;
    size_t len, width, height;
    int format;
    struct timeval timestamp;
} camera_fb_t;
esp_err_t esp_camera_init(const camera_config_t *config);
esp_err_t esp_camera_deinit(void);
sensor_t *esp_camera_sensor_get(void);
camera_fb_t *esp_camera_fb_get(void);
void esp_camera_fb_return(camera_fb_t *frame);
bool esp_camera_get_psram_mode(void);
esp_err_t esp_camera_set_psram_mode(bool enable);
esp_err_t esp_camera_reconfigure(const camera_config_t *config);
