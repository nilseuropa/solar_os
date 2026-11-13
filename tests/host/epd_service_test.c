#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "services/solar_os_epd.h"
#include "freertos/FreeRTOS.h"

static int64_t now_us = 1;
int64_t esp_timer_get_time(void) { return now_us; }
void vTaskDelay(TickType_t ticks) { now_us += (int64_t)ticks * 1000; }

typedef struct {
    solar_os_epd_t epd;
    uint8_t pixels[18], shadow[4];
    uint32_t active[18];
    uint16_t history[18];
    bool valid, cleanup, dirty;
    bool fail_baseline, fail_drive;
    unsigned baselines, drives, idle;
} panel_t;

static esp_err_t baseline(void *context, const uint8_t *target)
{
    panel_t *p = context;
    (void)target;
    ++p->baselines;
    return p->fail_baseline ? ESP_FAIL : ESP_OK;
}

static esp_err_t drive(void *context)
{
    panel_t *p = context;
    if (p->fail_drive) return ESP_FAIL;
    ++p->drives;
    now_us += 4000;
    epd_pixel_budget_advance(&p->epd.budget);
    return ESP_OK;
}

static esp_err_t idle(void *context)
{
    ++((panel_t *)context)->idle;
    return ESP_OK;
}

int main(void)
{
    panel_t p = {0};
    p.epd.tuning = (epd_pulse_tuning_t){.drive_cycles=3, .cleanup_every=64};
    assert(epd_pixel_budget_init(&p.epd.budget,p.pixels,18) == ESP_OK);
    assert(epd_pixel_budget_set_active_storage(&p.epd.budget,p.active,18) == ESP_OK);
    assert(epd_pixel_budget_set_drive_history(&p.epd.budget,p.history,18) == ESP_OK);
    const solar_os_epd_adapter_t a = {.state=&p.epd, .context=&p,
        .shadow=p.shadow, .frame_bytes=4, .width=2, .height=9,
        .shadow_valid=&p.valid, .needs_cleanup=&p.cleanup, .mask_dirty=&p.dirty,
        .asynchronous=true, .count_on_accept=true,
        .baseline=baseline, .drive=drive, .idle=idle};
    uint8_t image[] = {255,255,255,255};
    p.fail_baseline=true;
    assert(solar_os_epd_flush(&a,image) == ESP_FAIL && !p.valid);
    p.fail_baseline=false;
    assert(solar_os_epd_flush(&a,image) == ESP_OK && p.valid && p.drives==0);
    image[2] ^= 0xfe; /* Padding below row 8 cannot activate any physical pixel. */
    assert(solar_os_epd_flush(&a,image) == ESP_OK && p.drives==0 && p.epd.updates==0);
    image[2] ^= 1;
    bool pending, scanned;
    p.fail_drive=true;
    assert(solar_os_epd_step(&a,image,true,&pending,&scanned) == ESP_FAIL);
    assert(!scanned && p.epd.budget.pending==1 && p.history[16]==0);
    p.fail_drive=false;
    assert(solar_os_epd_step(&a,image,true,&pending,&scanned) == ESP_OK);
    assert(scanned && pending && p.history[16]==1 && p.epd.updates==1);
    assert(solar_os_epd_flush(&a,image) == ESP_OK && p.drives==3 && !p.epd.budget.pending);
    assert(solar_os_epd_flush(&a,image) == ESP_OK && p.drives==3 && p.epd.updates==1);
    image[2] ^= 1;
    assert(solar_os_epd_step(&a,image,true,&pending,&scanned) == ESP_OK && pending);
    image[2] ^= 1; /* Reversal replaces remaining direction, preserving exposure. */
    assert(solar_os_epd_flush(&a,image) == ESP_OK && p.history[16]==7);
    const unsigned before=p.baselines;
    p.cleanup=true;
    assert(solar_os_epd_flush(&a,image) == ESP_OK && p.baselines==before+1);
    assert(p.history[16]==0 && p.epd.updates==0 && !p.cleanup);
    puts("Shared EPD: baselines, padding, duplicate targets, reversal and failed drives passed");
}
