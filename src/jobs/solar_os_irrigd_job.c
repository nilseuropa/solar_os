#include "solar_os_irrigd_job.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "solar_os_config.h"
#include "solar_os_irrig.h"
#if SOLAR_OS_PACKAGE_IRRIG_WEB
#include "solar_os_irrig_web.h"
#endif
#include "solar_os_jobs.h"
#include "solar_os_log.h"
#include "solar_os_time.h"

/*
 * Irrigation daemon: re-evaluates the schedule engine once a second
 * against the local clock. It's a job (not part of the UI app) on
 * purpose -- watering must continue no matter which app is on screen,
 * or with no UI running at all. Configure and inspect it with the
 * 'irrig' shell command; the graphical app is a front-end on top of
 * the same engine.
 *
 * On builds with the web editor package, the job also serves the
 * schedule editor page: job start irrigd [http-port|off]. A web
 * failure never blocks the job -- watering matters more than the
 * page.
 */
#define IRRIGD_EVAL_INTERVAL_MS 1000U

static const char *TAG = "solar_os_irrigd";

typedef struct {
    bool running;
    uint32_t next_eval_ms;
} irrigd_state_t;

static irrigd_state_t irrigd;

static esp_err_t irrigd_start(solar_os_context_t *ctx, int argc, char **argv)
{
    (void)ctx;

#if SOLAR_OS_PACKAGE_IRRIG_WEB
    uint16_t web_port = SOLAR_OS_IRRIG_WEB_DEFAULT_PORT;
    if (argc == 2 && argv != NULL && argv[1] != NULL) {
        if (strcmp(argv[1], "off") == 0) {
            web_port = 0;
        } else {
            char *end = NULL;
            errno = 0;
            const unsigned long parsed = strtoul(argv[1], &end, 10);
            if (errno != 0 || end == argv[1] || *end != '\0' ||
                parsed == 0 || parsed > 65535) {
                return ESP_ERR_INVALID_ARG;
            }
            web_port = (uint16_t)parsed;
        }
    } else if (argc > 2) {
        return ESP_ERR_INVALID_ARG;
    }
#else
    (void)argv;
    if (argc > 1) {
        return ESP_ERR_INVALID_ARG;
    }
#endif

    const esp_err_t ret = solar_os_irrig_init();
    if (ret != ESP_OK) {
        return ret;
    }

#if SOLAR_OS_PACKAGE_IRRIG_WEB
    if (web_port != 0) {
        const esp_err_t web_ret = solar_os_irrig_web_start(web_port);
        if (web_ret == ESP_OK) {
            char net[16];
            snprintf(net, sizeof(net), "tcp:%u", (unsigned)web_port);
            (void)solar_os_jobs_note_resource(solar_os_irrigd_job.name,
                                              SOLAR_OS_JOB_RESOURCE_NET,
                                              net,
                                              "listen");
        } else {
            SOLAR_OS_LOGW(TAG, "web editor failed on port %u: %s",
                          (unsigned)web_port, esp_err_to_name(web_ret));
        }
    }
#endif

    irrigd.running = true;
    irrigd.next_eval_ms = 0;
    SOLAR_OS_LOGI(TAG, "started: %u zones", (unsigned)solar_os_irrig_zone_count());
    return ESP_OK;
}

static void irrigd_stop(solar_os_context_t *ctx)
{
    (void)ctx;

    irrigd.running = false;
#if SOLAR_OS_PACKAGE_IRRIG_WEB
    solar_os_irrig_web_stop();
#endif
    /* Never leave a valve open with nobody watching it. */
    solar_os_irrig_all_off();
    SOLAR_OS_LOGI(TAG, "stopped, all zones off");
}

static bool irrigd_event(solar_os_context_t *ctx, const solar_os_event_t *event)
{
    (void)ctx;

    if (event == NULL || event->type != SOLAR_OS_EVENT_TICK || !irrigd.running) {
        return false;
    }

    const uint32_t now_ms = event->data.tick_ms;
    if (irrigd.next_eval_ms != 0 && (int32_t)(now_ms - irrigd.next_eval_ms) < 0) {
        return false;
    }
    irrigd.next_eval_ms = now_ms + IRRIGD_EVAL_INTERVAL_MS;

#if SOLAR_OS_PACKAGE_IRRIG_WEB
    /* Apply any browser save before this evaluation pass. */
    solar_os_irrig_web_tick();
#endif

    solar_os_datetime_t now;
    if (solar_os_time_get_datetime(&now) == ESP_OK) {
        solar_os_irrig_update(&now);
    } else {
        solar_os_irrig_update(NULL);
    }
    return true;
}

const solar_os_job_t solar_os_irrigd_job = {
    .name = "irrigd",
    .summary = "irrigation schedule engine (configure with 'irrig')",
    .start = irrigd_start,
    .stop = irrigd_stop,
    .event = irrigd_event,
};
