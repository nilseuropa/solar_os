#include "solar_os_irrig_web.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "esp_heap_caps.h"
#include "esp_http_server.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "solar_os_config.h"
#include "solar_os_irrig.h"
#include "solar_os_log.h"
#if SOLAR_OS_PACKAGE_JOB_REMOTE
#include "solar_os_remote_screen.h"
#endif

/*
 * Web schedule editor: a port of the standalone controller's own web
 * page (tabs per zone, a card per schedule slot with time fields, day
 * toggles and an on/off switch, live screenshot on top, one SAVE for
 * everything).
 *
 * Threading contract: the engine is main-loop-only by design, but
 * esp_http_server handlers run in the server's task. So handlers
 * never touch the engine. Instead a mutex guards two small buffers:
 * a config snapshot (refreshed by solar_os_irrig_web_tick() from the
 * irrigd job, rendered by GET /) and a pending save (written by
 * POST /update, applied by the next tick). A browser save is
 * therefore live within one engine tick.
 */
#define IRRIG_WEB_CTRL_PORT 32781
#define IRRIG_WEB_STACK_SIZE 6144
#define IRRIG_WEB_PENDING_MAX 1024
#define IRRIG_WEB_ENTRY_MAX 32
#if SOLAR_OS_PACKAGE_JOB_REMOTE
#define IRRIG_WEB_BMP_HEADER_SIZE 62U
#endif

static const char *TAG = "solar_os_irrig_web";

typedef struct {
    httpd_handle_t server;
    uint16_t port;
    SemaphoreHandle_t lock; /* created once, never deleted */
    uint8_t snap_zones;
    solar_os_irrig_schedule_t snap[SOLAR_OS_IRRIG_ZONES_MAX]
                                  [SOLAR_OS_IRRIG_SCHEDULES_PER_ZONE];
    bool pending;
    char pending_data[IRRIG_WEB_PENDING_MAX];
    uint32_t page_hits;
    uint32_t saves;
} irrig_web_state_t;

static irrig_web_state_t irrig_web;

static const char irrig_web_day_letters[7] = {'L', 'M', 'M', 'J', 'V', 'S', 'D'};

/*
 * The page, split around the injected `const schedules = [...]`
 * array. Everything else (zone count, tab letters) is derived from it
 * in the browser. Kept as close to the original controller's page as
 * the move from Arduino to SolarOS allows; the screenshot endpoint
 * hides itself when the build has no screen snapshot support.
 */
static const char irrig_web_page_head[] =
    "<!DOCTYPE html>\n"
    "<html lang=\"ro\">\n"
    "<head>\n"
    "<meta charset=\"UTF-8\">\n"
    "<meta name=\"viewport\" content=\"width=device-width, initial-scale=1\">\n"
    "<title>Scheduler</title>\n"
    "<style>\n"
    ":root { --primary: #6e5ce7; --gray: #999; }\n"
    "body { font-family: 'Segoe UI', sans-serif; background: #f0f2f5; margin:0; padding:20px; }\n"
    "h2 { text-align:center; margin: 10px 0 15px 0; }\n"
    ".tabs-wrapper { max-width: 560px; margin: 0 auto; }\n"
    ".tabs { display: flex; gap: 8px; justify-content: center; flex-wrap: wrap; }\n"
    ".tab { padding: 12px 24px; background: white; border: none; border-radius: 12px 12px 0 0; cursor: pointer; font-weight: bold; color: #555; box-shadow: 0 2px 6px rgba(0,0,0,0.1); white-space: nowrap; }\n"
    ".tab.active { background: var(--primary); color: white; }\n"
    ".screenshot { max-width: 320px; width: 100%; border: 3px solid #333; border-radius: 8px; margin: 0 auto 20px; display: block; image-rendering: pixelated; }\n"
    ".container { display: none; flex-direction: column; gap: 15px; align-items: center; }\n"
    ".container.active { display: flex; }\n"
    ".card { background: white; border-radius: 20px; padding: 18px 25px; width: 100%; max-width: 560px; display: flex; justify-content: space-between; align-items: center; box-shadow: 0 4px 15px rgba(0,0,0,0.06); box-sizing: border-box; }\n"
    ".card.disabled { opacity: 0.5; }\n"
    ".time-display { display: flex; align-items: center; font-size: 28px; color: #333; }\n"
    ".time-group { display: flex; align-items: center; }\n"
    ".time-group input { border: none; width: 50px; font-size: 28px; text-align: center; outline: none; background: transparent; font-family: inherit; }\n"
    ".dash { margin: 0 12px; color: var(--gray); font-weight: 300; }\n"
    ".controls { display: flex; align-items: center; gap: 15px; }\n"
    ".days { display: flex; gap: 7px; color: var(--primary); font-weight: bold; font-size: 14px; }\n"
    ".day { cursor: pointer; width: 20px; text-align: center; position: relative; }\n"
    ".day.inactive { color: #ddd; }\n"
    ".day:not(.inactive)::after { content: '\\2022'; position: absolute; top: -12px; left: 50%; transform: translateX(-50%); }\n"
    ".switch { width: 42px; height: 24px; position: relative; display: inline-block; flex-shrink: 0; }\n"
    ".switch input { opacity: 0; width: 0; height: 0; }\n"
    ".slider { position: absolute; cursor: pointer; top: 0; left: 0; right: 0; bottom: 0; background: #e0e0e0; border-radius: 24px; transition: .3s; }\n"
    ".slider:before { position: absolute; content: \"\"; height: 18px; width: 18px; left: 3px; bottom: 3px; background: white; border-radius: 50%; transition: .3s; }\n"
    "input:checked + .slider { background: var(--primary); }\n"
    "input:checked + .slider:before { transform: translateX(18px); }\n"
    "button.save { padding: 16px 40px; background: var(--primary); color: white; border: none; border-radius: 12px; font-size: 17px; cursor: pointer; margin: 25px auto; display: block; }\n"
    "</style>\n"
    "</head>\n"
    "<body>\n"
    "<h2>Scheduler</h2>\n"
    "<img id=\"screenshot\" class=\"screenshot\" src=\"/screenshot\" "
    "onclick=\"this.src='/screenshot?'+Math.random()\" "
    "onerror=\"this.style.display='none'\">\n"
    "<div class=\"tabs-wrapper\"><div class=\"tabs\" id=\"tabs\"></div></div>\n"
    "<div id=\"zones\"></div>\n"
    "<button class=\"save\" onclick=\"saveAll()\">SAVE</button>\n"
    "<script>\n"
    "const schedules = [";

static const char irrig_web_page_tail[] =
    "];\n"
    "\n"
    "function createZoneHTML(idx, dataStr) {\n"
    "    const alarms = dataStr.split('|');\n"
    "    let html = `<div class=\"container\" id=\"zone${idx}\">`;\n"
    "    alarms.forEach((alarm, j) => {\n"
    "        const parts = alarm.split(','); const start = parts[0].split(':'); const end = parts[1].split(':'); const days = parts[2]; const active = parts[3] === 'A';\n"
    "        html += `\n"
    "        <div class=\"card ${!active ? 'disabled' : ''}\" data-idx=\"${idx*4 + j}\">\n"
    "            <div class=\"time-display\">\n"
    "                <div class=\"time-group\"><input type=\"text\" class=\"h1\" value=\"${start[0]}\" maxlength=\"2\">:<input type=\"text\" class=\"m1\" value=\"${start[1]}\" maxlength=\"2\"></div>\n"
    "                <span class=\"dash\">-</span>\n"
    "                <div class=\"time-group\"><input type=\"text\" class=\"h2\" value=\"${end[0]}\" maxlength=\"2\">:<input type=\"text\" class=\"m2\" value=\"${end[1]}\" maxlength=\"2\"></div>\n"
    "            </div>\n"
    "            <div class=\"controls\">\n"
    "                <div class=\"days\">\n"
    "                    ${['L','M','M','J','V','S','D'].map((d, k) => { const inactive = days[k] === '-' ? 'inactive' : ''; return `<span class=\"day ${inactive}\" data-v=\"${d}\">${d}</span>`; }).join('')}\n"
    "                </div>\n"
    "                <label class=\"switch\"><input type=\"checkbox\" class=\"toggle\" ${active ? 'checked' : ''}><span class=\"slider\"></span></label>\n"
    "            </div>\n"
    "        </div>`;\n"
    "    });\n"
    "    html += `</div>`; return html;\n"
    "}\n"
    "\n"
    "let tabsHTML = '';\n"
    "for(let i = 0; i < schedules.length; i++) tabsHTML += `<button class=\"tab ${i===0 ? 'active' : ''}\" data-tab=\"${i}\">Zone ${String.fromCharCode(65+i)}</button>`;\n"
    "document.getElementById('tabs').innerHTML = tabsHTML;\n"
    "\n"
    "let zonesHTML = '';\n"
    "for(let i = 0; i < schedules.length; i++) zonesHTML += createZoneHTML(i, schedules[i]);\n"
    "document.getElementById('zones').innerHTML = zonesHTML;\n"
    "\n"
    "document.querySelectorAll('.tab').forEach(tab => {\n"
    "    tab.addEventListener('click', () => {\n"
    "        document.querySelectorAll('.tab').forEach(t => t.classList.remove('active')); tab.classList.add('active');\n"
    "        document.querySelectorAll('.container').forEach(c => c.classList.remove('active')); document.getElementById('zone' + tab.dataset.tab).classList.add('active');\n"
    "    });\n"
    "});\n"
    "document.getElementById('zone0').classList.add('active');\n"
    "\n"
    "document.querySelectorAll('.day').forEach(day => { day.addEventListener('click', () => { day.classList.toggle('inactive'); }); });\n"
    "document.querySelectorAll('.toggle').forEach(tog => { tog.addEventListener('change', () => { tog.closest('.card').classList.toggle('disabled', !tog.checked); }); });\n"
    "\n"
    "function getAllData() {\n"
    "    let final = [];\n"
    "    for(let z = 0; z < schedules.length; z++) {\n"
    "        const zone = document.getElementById('zone' + z);\n"
    "        zone.querySelectorAll('.card').forEach(card => {\n"
    "            const h1 = card.querySelector('.h1').value.padStart(2, '0'); const m1 = card.querySelector('.m1').value.padStart(2, '0');\n"
    "            const h2 = card.querySelector('.h2').value.padStart(2, '0'); const m2 = card.querySelector('.m2').value.padStart(2, '0');\n"
    "            let dStr = ''; card.querySelectorAll('.day').forEach(d => { dStr += d.classList.contains('inactive') ? '-' : d.dataset.v; });\n"
    "            const active = card.querySelector('.toggle').checked ? 'A' : '-';\n"
    "            final.push(`${h1}:${m1},${h2}:${m2},${dStr},${active}`);\n"
    "        });\n"
    "    }\n"
    "    return final.join('|');\n"
    "}\n"
    "\n"
    "function saveAll() {\n"
    "    fetch('/update', {method:'POST', body:getAllData()}).then(r => r.text()).then(txt => alert(txt === 'OK' ? 'Salvat cu succes!' : 'Eroare: ' + txt)).catch(() => alert('Eroare de conexiune'));\n"
    "}\n"
    "setInterval(() => { const s = document.getElementById('screenshot'); if (s.style.display !== 'none') s.src = '/screenshot?' + Math.random(); }, 4000);\n"
    "</script>\n"
    "</body>\n"
    "</html>\n";

static void irrig_web_lock(void)
{
    (void)xSemaphoreTake(irrig_web.lock, portMAX_DELAY);
}

static void irrig_web_unlock(void)
{
    (void)xSemaphoreGive(irrig_web.lock);
}

static size_t irrig_web_format_entry(char *out,
                                     size_t out_len,
                                     const solar_os_irrig_schedule_t *schedule)
{
    char days[8];
    for (int i = 0; i < 7; i++) {
        days[i] = (schedule->days & (1U << i)) != 0 ? irrig_web_day_letters[i] : '-';
    }
    days[7] = '\0';

    return (size_t)snprintf(out, out_len, "%02u:%02u,%02u:%02u,%s,%c",
                            (unsigned)(schedule->start_minute / 60U),
                            (unsigned)(schedule->start_minute % 60U),
                            (unsigned)(schedule->end_minute / 60U),
                            (unsigned)(schedule->end_minute % 60U),
                            days,
                            schedule->active ? 'A' : '-');
}

static esp_err_t irrig_web_page_handler(httpd_req_t *req)
{
    /* '"' + 4 entries of <=21 chars + 3 '|' + '"' + ',' per zone. */
    char schedules_js[SOLAR_OS_IRRIG_ZONES_MAX * 92];
    size_t used = 0;

    irrig_web_lock();
    const uint8_t zones = irrig_web.snap_zones;
    for (uint8_t zone = 0; zone < zones; zone++) {
        if (zone > 0) {
            schedules_js[used++] = ',';
        }
        schedules_js[used++] = '"';
        for (uint8_t slot = 0; slot < SOLAR_OS_IRRIG_SCHEDULES_PER_ZONE; slot++) {
            if (slot > 0) {
                schedules_js[used++] = '|';
            }
            used += irrig_web_format_entry(&schedules_js[used],
                                           sizeof(schedules_js) - used,
                                           &irrig_web.snap[zone][slot]);
        }
        schedules_js[used++] = '"';
    }
    irrig_web_unlock();
    schedules_js[used] = '\0';

    const size_t page_len = (sizeof(irrig_web_page_head) - 1U) + used +
        (sizeof(irrig_web_page_tail) - 1U);
    char *page = heap_caps_malloc(page_len, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (page == NULL) {
        page = heap_caps_malloc(page_len, MALLOC_CAP_8BIT);
    }
    if (page == NULL) {
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "out of memory");
    }

    char *at = page;
    memcpy(at, irrig_web_page_head, sizeof(irrig_web_page_head) - 1U);
    at += sizeof(irrig_web_page_head) - 1U;
    memcpy(at, schedules_js, used);
    at += used;
    memcpy(at, irrig_web_page_tail, sizeof(irrig_web_page_tail) - 1U);

    (void)httpd_resp_set_type(req, "text/html");
    const esp_err_t ret = httpd_resp_send(req, page, (ssize_t)page_len);
    heap_caps_free(page);

    if (ret == ESP_OK) {
        irrig_web.page_hits++;
    }
    return ret;
}

static esp_err_t irrig_web_update_handler(httpd_req_t *req)
{
    if (req->content_len == 0 || req->content_len >= IRRIG_WEB_PENDING_MAX) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "bad payload size");
    }

    char body[IRRIG_WEB_PENDING_MAX];
    size_t received = 0;
    while (received < req->content_len) {
        const int chunk = httpd_req_recv(req, &body[received], req->content_len - received);
        if (chunk <= 0) {
            return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "recv failed");
        }
        received += (size_t)chunk;
    }
    body[received] = '\0';

    irrig_web_lock();
    memcpy(irrig_web.pending_data, body, received + 1U);
    irrig_web.pending = true;
    irrig_web_unlock();

    /* The save is queued here and applied by the engine's next 1s
     * tick on the main loop; "OK" means "accepted", same as the
     * original controller's immediate response. */
    irrig_web.saves++;
    return httpd_resp_sendstr(req, "OK");
}

#if SOLAR_OS_PACKAGE_JOB_REMOTE
static void irrig_web_bmp_write_u32(uint8_t *out, uint32_t value)
{
    out[0] = (uint8_t)(value & 0xffU);
    out[1] = (uint8_t)((value >> 8) & 0xffU);
    out[2] = (uint8_t)((value >> 16) & 0xffU);
    out[3] = (uint8_t)((value >> 24) & 0xffU);
}

/* Same 1bpp BMP the remote job serves; duplicated because packages
 * cannot depend on each other and this is only ~40 lines. */
static esp_err_t irrig_web_screenshot_handler(httpd_req_t *req)
{
    uint16_t width = 0;
    uint16_t height = 0;
    esp_err_t ret = solar_os_remote_screen_get_size(&width, &height);
    if (ret != ESP_OK || width == 0 || height == 0) {
        return httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "no display");
    }

    const size_t stride = ((size_t)width + 7U) / 8U;
    const size_t row_bytes = (stride + 3U) & ~(size_t)3U;
    const size_t snapshot_size = stride * height;
    const size_t bmp_size = IRRIG_WEB_BMP_HEADER_SIZE + (row_bytes * height);

    uint8_t *bmp = heap_caps_malloc(bmp_size + snapshot_size,
                                    MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (bmp == NULL) {
        bmp = heap_caps_malloc(bmp_size + snapshot_size, MALLOC_CAP_8BIT);
    }
    if (bmp == NULL) {
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "out of memory");
    }
    uint8_t *snapshot = &bmp[bmp_size];

    ret = solar_os_remote_screen_snapshot(snapshot, snapshot_size, &width, &height);
    if (ret != ESP_OK) {
        heap_caps_free(bmp);
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "snapshot failed");
    }

    const uint32_t image_size = (uint32_t)(row_bytes * height);
    memset(bmp, 0, IRRIG_WEB_BMP_HEADER_SIZE);
    bmp[0] = 'B';
    bmp[1] = 'M';
    irrig_web_bmp_write_u32(&bmp[2], IRRIG_WEB_BMP_HEADER_SIZE + image_size);
    irrig_web_bmp_write_u32(&bmp[10], IRRIG_WEB_BMP_HEADER_SIZE);
    irrig_web_bmp_write_u32(&bmp[14], 40);
    irrig_web_bmp_write_u32(&bmp[18], width);
    irrig_web_bmp_write_u32(&bmp[22], height);
    bmp[26] = 1; /* planes */
    bmp[28] = 1; /* bits per pixel */
    irrig_web_bmp_write_u32(&bmp[34], image_size);
    irrig_web_bmp_write_u32(&bmp[46], 2); /* palette colors */
    bmp[58] = 0xff; /* palette[1] = white (palette[0] stays black) */
    bmp[59] = 0xff;
    bmp[60] = 0xff;

    uint8_t *rows = &bmp[IRRIG_WEB_BMP_HEADER_SIZE];
    memset(rows, 0, row_bytes * height);
    for (uint16_t y = 0; y < height; y++) {
        /* BMP stores rows bottom-up */
        memcpy(&rows[(size_t)(height - 1U - y) * row_bytes],
               &snapshot[(size_t)y * stride],
               stride);
    }

    (void)httpd_resp_set_type(req, "image/bmp");
    (void)httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    ret = httpd_resp_send(req, (const char *)bmp, (ssize_t)bmp_size);
    heap_caps_free(bmp);
    return ret;
}
#else
static esp_err_t irrig_web_screenshot_handler(httpd_req_t *req)
{
    return httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "no screen snapshot in this build");
}
#endif

static void irrig_web_refresh_snapshot(void)
{
    solar_os_irrig_schedule_t fresh[SOLAR_OS_IRRIG_ZONES_MAX]
                                   [SOLAR_OS_IRRIG_SCHEDULES_PER_ZONE];
    const uint8_t zones = solar_os_irrig_zone_count();

    memset(fresh, 0, sizeof(fresh));
    for (uint8_t zone = 0; zone < zones; zone++) {
        for (uint8_t slot = 0; slot < SOLAR_OS_IRRIG_SCHEDULES_PER_ZONE; slot++) {
            (void)solar_os_irrig_get_schedule(zone, slot, &fresh[zone][slot]);
        }
    }

    irrig_web_lock();
    irrig_web.snap_zones = zones;
    memcpy(irrig_web.snap, fresh, sizeof(fresh));
    irrig_web_unlock();
}

static bool irrig_web_parse_entry(const char *entry, solar_os_irrig_schedule_t *out)
{
    unsigned h1;
    unsigned m1;
    unsigned h2;
    unsigned m2;
    char days[8];
    char active;

    if (sscanf(entry, "%2u:%2u,%2u:%2u,%7[-A-Z],%c",
               &h1, &m1, &h2, &m2, days, &active) != 6) {
        return false;
    }
    if (h1 > 23 || m1 > 59 || h2 > 24 || m2 > 59 || strlen(days) != 7) {
        return false;
    }

    memset(out, 0, sizeof(*out));
    out->start_minute = (uint16_t)((h1 * 60U) + m1);
    out->end_minute = (uint16_t)((h2 * 60U) + m2);
    for (int i = 0; i < 7; i++) {
        if (days[i] != '-') {
            out->days |= (uint8_t)(1U << i);
        }
    }
    out->active = active == 'A';
    return true;
}

static void irrig_web_apply(const char *data)
{
    const uint8_t zones = solar_os_irrig_zone_count();
    const unsigned max_entries = zones * SOLAR_OS_IRRIG_SCHEDULES_PER_ZONE;
    unsigned index = 0;
    unsigned applied = 0;
    unsigned rejected = 0;
    const char *at = data;

    while (*at != '\0' && index < max_entries) {
        const char *end = strchr(at, '|');
        const size_t len = end != NULL ? (size_t)(end - at) : strlen(at);

        char entry[IRRIG_WEB_ENTRY_MAX];
        if (len < sizeof(entry)) {
            memcpy(entry, at, len);
            entry[len] = '\0';

            const uint8_t zone = (uint8_t)(index / SOLAR_OS_IRRIG_SCHEDULES_PER_ZONE);
            const uint8_t slot = (uint8_t)(index % SOLAR_OS_IRRIG_SCHEDULES_PER_ZONE);

            solar_os_irrig_schedule_t parsed;
            solar_os_irrig_schedule_t current;
            if (irrig_web_parse_entry(entry, &parsed) &&
                solar_os_irrig_get_schedule(zone, slot, &current) == ESP_OK) {
                /* Skip untouched slots: blank ones (start == end == 0)
                 * would fail the engine's start < end validation. */
                if (memcmp(&parsed, &current, sizeof(parsed)) != 0) {
                    if (solar_os_irrig_set_schedule(zone, slot, &parsed) == ESP_OK) {
                        applied++;
                    } else {
                        rejected++;
                        SOLAR_OS_LOGW(TAG, "zone %c slot %u rejected: %s",
                                      'A' + zone, (unsigned)(slot + 1U), entry);
                    }
                }
            } else {
                rejected++;
                SOLAR_OS_LOGW(TAG, "bad entry at index %u: %s", index, entry);
            }
        } else {
            rejected++;
        }

        index++;
        if (end == NULL) {
            break;
        }
        at = end + 1;
    }

    SOLAR_OS_LOGI(TAG, "web save: %u changed, %u rejected", applied, rejected);
}

void solar_os_irrig_web_tick(void)
{
    if (irrig_web.server == NULL) {
        return;
    }

    char data[IRRIG_WEB_PENDING_MAX];
    bool have_pending = false;

    irrig_web_lock();
    if (irrig_web.pending) {
        memcpy(data, irrig_web.pending_data, sizeof(data));
        irrig_web.pending = false;
        have_pending = true;
    }
    irrig_web_unlock();

    if (have_pending) {
        irrig_web_apply(data);
    }
    irrig_web_refresh_snapshot();
}

bool solar_os_irrig_web_running(void)
{
    return irrig_web.server != NULL;
}

esp_err_t solar_os_irrig_web_start(uint16_t port)
{
    if (irrig_web.server != NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    if (irrig_web.lock == NULL) {
        irrig_web.lock = xSemaphoreCreateMutex();
        if (irrig_web.lock == NULL) {
            return ESP_ERR_NO_MEM;
        }
    }

    irrig_web.pending = false;
    irrig_web.page_hits = 0;
    irrig_web.saves = 0;
    irrig_web_refresh_snapshot();

    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.server_port = port;
    config.ctrl_port = IRRIG_WEB_CTRL_PORT;
    config.stack_size = IRRIG_WEB_STACK_SIZE;
    config.max_open_sockets = 4;
    config.lru_purge_enable = true;

    httpd_handle_t server = NULL;
    esp_err_t ret = httpd_start(&server, &config);
    if (ret != ESP_OK) {
        return ret;
    }

    const httpd_uri_t page_uri = {
        .uri = "/",
        .method = HTTP_GET,
        .handler = irrig_web_page_handler,
        .user_ctx = NULL,
    };
    const httpd_uri_t update_uri = {
        .uri = "/update",
        .method = HTTP_POST,
        .handler = irrig_web_update_handler,
        .user_ctx = NULL,
    };
    const httpd_uri_t screenshot_uri = {
        .uri = "/screenshot",
        .method = HTTP_GET,
        .handler = irrig_web_screenshot_handler,
        .user_ctx = NULL,
    };
    ret = httpd_register_uri_handler(server, &page_uri);
    if (ret == ESP_OK) {
        ret = httpd_register_uri_handler(server, &update_uri);
    }
    if (ret == ESP_OK) {
        ret = httpd_register_uri_handler(server, &screenshot_uri);
    }
    if (ret != ESP_OK) {
        (void)httpd_stop(server);
        return ret;
    }

    irrig_web.server = server;
    irrig_web.port = port;
    SOLAR_OS_LOGI(TAG, "schedule editor on port %u", (unsigned)port);
    return ESP_OK;
}

void solar_os_irrig_web_stop(void)
{
    if (irrig_web.server == NULL) {
        return;
    }

    (void)httpd_stop(irrig_web.server);
    SOLAR_OS_LOGI(TAG, "stopped: pages=%u saves=%u port=%u",
                  (unsigned)irrig_web.page_hits,
                  (unsigned)irrig_web.saves,
                  (unsigned)irrig_web.port);
    irrig_web.server = NULL;
    irrig_web.pending = false;
}
