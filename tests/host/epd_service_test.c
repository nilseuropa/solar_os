#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "services/solar_os_epd.h"
#include "freertos/FreeRTOS.h"

static int64_t now_us = 1;
static unsigned allocations, releases, fail_at;
static void *allocate(size_t bytes, const char *tag)
{
    (void)tag;
    if (++allocations == fail_at) return NULL;
    return malloc(bytes);
}
static void release(void *memory) { if (memory) ++releases; free(memory); }
static void test_storage_rollback(void)
{
    const solar_os_epd_storage_ops_t storage = {allocate,release};
    for (fail_at=1; fail_at<=4; ++fail_at) {
        solar_os_epd_t state = {0}; uint8_t *planes = NULL;
        allocations=releases=0;
        assert(solar_os_epd_allocate_budget(&state,18,&planes,8,&storage)==ESP_ERR_NO_MEM);
        assert(!state.budget.pixels && !planes && allocations==4 && releases==3);
    }
    fail_at=0; allocations=releases=0;
    solar_os_epd_t state = {0}; uint8_t *planes = NULL;
    assert(solar_os_epd_allocate_budget(&state,18,&planes,8,&storage)==ESP_OK);
    assert(state.budget.count==18 && planes && allocations==4);
    assert(solar_os_epd_allocate_budget(&state,18,&planes,8,&storage)==ESP_OK && allocations==4);
    solar_os_epd_free_budget(&state,&planes,&storage);
    assert(!state.budget.pixels && !planes && releases==4);
    assert(solar_os_epd_allocate_budget(&state,SIZE_MAX,&planes,8,&storage)==ESP_ERR_INVALID_ARG);
}
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
    test_storage_rollback();
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
    p.epd.tuning.black_redrives=2;
    image[2] ^= 1; /* Black to white retains the base settling budget only. */
    assert(solar_os_epd_flush(&a,image) == ESP_OK && p.history[16]==3);
    image[2] ^= 1; /* White to black adds exactly two bounded top-ups. */
    assert(solar_os_epd_flush(&a,image) == ESP_OK && p.history[16]==8);
    const unsigned settled=p.drives;
    for (unsigned i=0; i<100; ++i) assert(solar_os_epd_flush(&a,image) == ESP_OK);
    assert(p.drives==settled && !p.epd.budget.pending);
    p.epd.tuning.white_redrives=1;
    image[2] ^= 1;
    assert(solar_os_epd_step(&a,image,true,&pending,&scanned)==ESP_OK && pending);
    image[2] ^= 1; /* Cancel white top-ups on reversal. */
    assert(solar_os_epd_flush(&a,image)==ESP_OK && p.history[16]==14);
    uint8_t newer[4], recovered[4];
    memcpy(newer,image,4); newer[0]^=1;
    epd_scan_worker_t worker = {.task=(void *)1, .has_target=true,
        .latest=newer, .snapshot=recovered};
    worker.lock=xSemaphoreCreateMutexStatic(&worker.lock_storage);
    solar_os_epd_adapter_t queued=a; queued.worker=&worker;
    const unsigned idle_before=p.idle;
    assert(solar_os_epd_step(&queued,image,false,&pending,&scanned)==ESP_OK);
    assert(p.idle==idle_before && !scanned); /* Keep rails for a queued changed image. */
    worker.has_target=false;
    assert(solar_os_epd_step(&queued,image,false,&pending,&scanned)==ESP_OK);
    assert(p.idle==idle_before+1); /* No queued work: quiesce the panel. */
    const unsigned drives_before=p.drives;
    assert(solar_os_epd_recover(&queued)==ESP_OK);
    assert(memcmp(p.shadow,newer,4)==0 && p.drives==drives_before && p.valid);
    puts("Shared EPD: baselines, padding, duplicate targets, reversal and failed drives passed");
}
