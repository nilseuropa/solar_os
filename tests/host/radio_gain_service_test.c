#include <assert.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <freertos/semphr.h>

static SemaphoreHandle_t xSemaphoreCreateMutex(void)
{
    static StaticSemaphore_t mutexes[8];
    static unsigned count;
    assert(count < 8);
    return xSemaphoreCreateMutexStatic(&mutexes[count++]);
}

#include "../../src/services/solar_os_radio.c"
#include "../../src/services/solar_os_radio_scan.c"

static const char *settings_dir;
static unsigned configure_calls;
static TickType_t ticks;
static solar_os_radio_status_t fake_status;
static unsigned fail_configure;
static bool fail_sample;
static unsigned samples;
static unsigned retunes;
static int64_t timer_offset_us;
int64_t esp_timer_get_time(void) { return (int64_t)ticks * 1000 + timer_offset_us; }
TickType_t xTaskGetTickCount(void) { return ticks; }
void vTaskDelay(TickType_t value) { ticks += value; }
size_t strlcpy(char *dst, const char *src, size_t size)
{
    size_t length = strlen(src);
    if (size) { size_t n = length < size - 1 ? length : size - 1; memcpy(dst, src, n); dst[n] = 0; }
    return length;
}
void *solar_os_memory_calloc(size_t n, size_t size, solar_os_memory_class_t cls, const char *tag)
{ (void)cls; (void)tag; return calloc(n, size); }
void solar_os_memory_free(void *ptr) { free(ptr); }
esp_err_t solar_os_storage_default_path(const char *relative, char *path, size_t size)
{ (void)relative; return snprintf(path, size, "%s", settings_dir) < (int)size ? ESP_OK : ESP_FAIL; }
esp_err_t solar_os_storage_mkdir(const char *path)
{ return mkdir(path, 0700) == 0 ? ESP_OK : ESP_FAIL; }
esp_err_t solar_os_storage_replace_file(const char *staged, const char *active, const char *backup)
{ (void)backup; return rename(staged, active) == 0 ? ESP_OK : ESP_FAIL; }
esp_err_t nvs_open(const char *name, nvs_open_mode_t mode, nvs_handle_t *handle)
{ (void)name; (void)mode; (void)handle; return ESP_ERR_NVS_NOT_FOUND; }
esp_err_t nvs_get_blob(nvs_handle_t h, const char *key, void *data, size_t *size)
{ (void)h; (void)key; (void)data; (void)size; return ESP_ERR_NVS_NOT_FOUND; }
esp_err_t nvs_erase_key(nvs_handle_t h, const char *key)
{ (void)h; (void)key; return ESP_OK; }
esp_err_t nvs_commit(nvs_handle_t h) { (void)h; return ESP_OK; }
void nvs_close(nvs_handle_t h) { (void)h; }
static esp_err_t configure(void *ctx, const solar_os_radio_config_t *config)
{
    (void)ctx; configure_calls++;
    if (fail_configure && --fail_configure == 0) return ESP_FAIL;
    fake_status.config = *config; fake_status.state = SOLAR_OS_RADIO_STATE_STANDBY;
    return ESP_OK;
}
static esp_err_t status(void *ctx, solar_os_radio_status_t *out)
{ (void)ctx; *out = fake_status; return ESP_OK; }
static esp_err_t set_state(void *ctx, solar_os_radio_state_t state)
{ (void)ctx; fake_status.state = state; return ESP_OK; }
static esp_err_t read_rssi(void *ctx, int16_t *dbm)
{
    (void)ctx;
    if (fail_sample) return ESP_FAIL;
    if (fake_status.state != SOLAR_OS_RADIO_STATE_RX) return ESP_ERR_INVALID_STATE;
    *dbm = -105 + (samples++ % 10); return ESP_OK;
}

static esp_err_t tune_rx(void *ctx, uint32_t frequency, uint32_t *settle_ms)
{
    (void)ctx; retunes++;
    fake_status.config.frequency_hz = frequency;
    fake_status.state = SOLAR_OS_RADIO_STATE_RX;
    *settle_ms = 2;
    return ESP_OK;
}

static void test_scanner(void)
{
    solar_os_radio_scan_t scan, observer;
    solar_os_radio_scan_init(&scan);
    solar_os_radio_scan_init(&observer);
    solar_os_radio_scan_config_t config = {.mode = SOLAR_OS_RADIO_SCAN_BAND,
        .start_hz = 868000000, .end_hz = 868250000, .step_hz = 125000};
    solar_os_radio_info_t info;
    solar_os_radio_status_t saved = fake_status;
    saved.state = SOLAR_OS_RADIO_STATE_SLEEP;
    fake_status = saved;
    assert(solar_os_radio_scan_start(&scan, "no-gain", &config) == ESP_ERR_NOT_SUPPORTED);
    solar_os_radio_handle_t owner = SOLAR_OS_RADIO_HANDLE_INIT;
    assert(solar_os_radio_claim("radio0", "meshcore:test", &owner) == ESP_OK);
    assert(solar_os_radio_scan_start(&scan, "radio0", &config) == ESP_ERR_INVALID_STATE);
    assert(strcmp(scan.owner, "meshcore:test") == 0);
    assert(fake_status.config.frequency_hz == saved.config.frequency_hz);

    /* Read-only channel observation coexists with the owner's RX, without
     * configuring or releasing that owner's radio. Never reuse packet RSSI. */
    fake_status.state = SOLAR_OS_RADIO_STATE_RX;
    config.mode = SOLAR_OS_RADIO_SCAN_CHANNEL;
    unsigned before = configure_calls;
    assert(solar_os_radio_scan_start(&observer, "radio0", &config) == ESP_OK);
    assert(!observer.restore && configure_calls == before);
    ticks += 100;
    assert(solar_os_radio_scan_poll(&observer) == ESP_OK && observer.history_count == 1);
    fake_status.state = SOLAR_OS_RADIO_STATE_TX;
    ticks += 100;
    assert(solar_os_radio_scan_poll(&observer) == ESP_OK && observer.history_count == 1);
    assert(solar_os_radio_scan_stop(&observer) == ESP_OK);
    assert(solar_os_radio_handle_valid(&owner));
    assert(configure_calls == before);
    /* Channel retuning and filter changes require ownership, even when the
     * owner's receiver is already running. Failed claims leave it untouched. */
    fake_status.state = SOLAR_OS_RADIO_STATE_RX;
    config.frequency_hz = saved.config.frequency_hz + 25000U;
    assert(solar_os_radio_scan_start(&observer, "radio0", &config) == ESP_ERR_INVALID_STATE);
    assert(configure_calls == before && solar_os_radio_handle_valid(&owner));
    assert(fake_status.config.frequency_hz == saved.config.frequency_hz);
    config.frequency_hz = 0;
    config.bandwidth_hz = saved.config.rx_bandwidth_hz + 25000U;
    assert(solar_os_radio_scan_start(&observer, "radio0", &config) == ESP_ERR_INVALID_STATE);
    assert(configure_calls == before && solar_os_radio_handle_valid(&owner));
    assert(solar_os_radio_release(&owner) == ESP_OK);

    fake_status = saved;
    config.frequency_hz = saved.config.frequency_hz + 62500U;
    assert(solar_os_radio_scan_start(&observer, "radio0", &config) == ESP_OK);
    assert(observer.restore && configure_calls == before + 1);
    assert(observer.receiver.frequency_hz == config.frequency_hz);
    assert(observer.receiver.rx_bandwidth_hz == config.bandwidth_hz);
    assert(fake_status.state == SOLAR_OS_RADIO_STATE_RX);
    ticks += 100;
    assert(solar_os_radio_scan_poll(&observer) == ESP_OK && observer.history_count == 1);
    assert(solar_os_radio_scan_stop(&observer) == ESP_OK);
    assert(memcmp(&fake_status, &saved, sizeof(saved)) == 0);
    assert(solar_os_radio_get_info("radio0", &info) == ESP_OK && !info.claimed);
    fail_configure = 1;
    assert(solar_os_radio_scan_start(&observer, "radio0", &config) == ESP_FAIL);
    assert(memcmp(&fake_status, &saved, sizeof(saved)) == 0);
    assert(solar_os_radio_get_info("radio0", &info) == ESP_OK && !info.claimed);
    config.frequency_hz = config.bandwidth_hz = 0;

    fake_status = saved;
    config.mode = SOLAR_OS_RADIO_SCAN_BAND;
    assert(solar_os_radio_scan_start(&scan, "radio0", &config) == ESP_OK);
    assert(scan.restore && scan.bins == 3);
    assert(solar_os_radio_get_info("radio0", &info) == ESP_OK && info.claimed);
    before = samples;
    assert(solar_os_radio_scan_poll(&scan) == ESP_OK && samples == before); /* settling */
    for (unsigned i = 0; i < 6; i++) { ticks += 25; assert(solar_os_radio_scan_poll(&scan) == ESP_OK); }
    assert(scan.sweeps == 1 && scan.rows == 1 && samples == before + 3);
    assert(scan.levels[2] != SOLAR_OS_RADIO_SCAN_EMPTY);
    assert(scan.recent_peak == scan.peak && scan.recent_peak_frequency_hz == config.end_hz);
    assert(solar_os_radio_scan_poll(&scan) == ESP_OK); /* Retune the next sweep before reading the snapshot. */
    assert(scan.receiver.frequency_hz == config.start_hz && scan.recent_peak_frequency_hz == config.end_hz);
    assert(solar_os_radio_scan_stop(&scan) == ESP_OK);
    assert(fake_status.state == saved.state && memcmp(&fake_status.config, &saved.config, sizeof(saved.config)) == 0);
    assert(solar_os_radio_get_info("radio0", &info) == ESP_OK && !info.claimed);

    /* A failure after the first endpoint changed the receiver rolls back. */
    fail_configure = 2;
    assert(solar_os_radio_scan_start(&scan, "radio0", &config) == ESP_FAIL);
    assert(!scan.running && !scan.restore && scan.handle.index == -1);
    assert(fake_status.state == saved.state && fake_status.config.frequency_hz == saved.config.frequency_hz);

    assert(solar_os_radio_scan_start(&scan, "radio0", &config) == ESP_OK);
    fail_sample = true; ticks += 100;
    assert(solar_os_radio_scan_poll(&scan) == ESP_FAIL);
    fail_sample = false;
    assert(!scan.running && fake_status.state == saved.state);
    assert(solar_os_radio_get_info("radio0", &info) == ESP_OK && !info.claimed);

    assert(solar_os_radio_scan_start(&scan, "radio0", &config) == ESP_OK);
    fail_configure = 1;
    assert(solar_os_radio_scan_stop(&scan) == ESP_FAIL);
    assert(fake_status.state == SOLAR_OS_RADIO_STATE_STANDBY);
    assert(solar_os_radio_get_info("radio0", &info) == ESP_OK && !info.claimed);

    config.step_hz = 0;
    assert(solar_os_radio_scan_start(&scan, "radio0", &config) == ESP_ERR_INVALID_ARG);
    config.step_hz = 1;
    assert(solar_os_radio_scan_start(&scan, "radio0", &config) == ESP_ERR_INVALID_ARG);
    puts("radio scan: shared observation, settling, sweeps, ownership and failure restoration: ok");
}

int main(int argc, char **argv)
{
    assert(argc == 2); settings_dir = argv[1];
    solar_os_radio_config_t config = {.frequency_hz = 868000000,
        .modulation = SOLAR_OS_RADIO_MODULATION_LORA};
    fake_status.config = config;
    const solar_os_radio_ops_t ops = {.configure = configure, .get_status = status,
        .set_state = set_state, .read_rssi = read_rssi};
    solar_os_radio_registration_t registration = {.name = "radio0", .driver = "future-radio", .max_packet_len = 255,
        .modulations = SOLAR_OS_RADIO_MODULATION_LORA, .default_config = config,
        .features = SOLAR_OS_RADIO_FEATURE_RX_GAIN | SOLAR_OS_RADIO_FEATURE_LIVE_RSSI,
        .rx_gain_modes = SOLAR_OS_RADIO_RX_GAIN_BOOST_MODES, .ops = &ops};
    assert(solar_os_radio_register(&registration) == ESP_OK);
    solar_os_radio_info_t info;
    assert(solar_os_radio_get_info("radio0", &info) == ESP_OK);
    assert(info.rx_gain_modes == SOLAR_OS_RADIO_RX_GAIN_BOOST_MODES);
    config.rx_gain = SOLAR_OS_RADIO_RX_GAIN_BOOSTED;
    assert(solar_os_radio_configure("radio0", &config) == ESP_OK && configure_calls == 1);
    config.rx_gain = SOLAR_OS_RADIO_RX_GAIN_G1;
    assert(solar_os_radio_configure("radio0", &config) == ESP_ERR_NOT_SUPPORTED && configure_calls == 1);
    config.rx_gain = 255;
    assert(solar_os_radio_configure("radio0", &config) == ESP_ERR_INVALID_ARG && configure_calls == 1);

    solar_os_radio_handle_t handle = SOLAR_OS_RADIO_HANDLE_INIT;
    assert(solar_os_radio_claim("radio0", "test", &handle) == ESP_OK);
    config.rx_gain = SOLAR_OS_RADIO_RX_GAIN_G1;
    assert(solar_os_radio_handle_configure(&handle, &config) == ESP_ERR_NOT_SUPPORTED);
    config.rx_gain = SOLAR_OS_RADIO_RX_GAIN_BOOSTED;
    assert(solar_os_radio_handle_configure(&handle, &config) == ESP_OK);
    assert(solar_os_radio_configure("radio0", &config) == ESP_ERR_INVALID_STATE);
    assert(solar_os_radio_release(&handle) == ESP_OK);

    assert(solar_os_radio_profile_save("radio0", "boost") == ESP_OK);
    solar_os_radio_profile_t profile;
    assert(solar_os_radio_profile_get("boost", &profile) == ESP_OK);
    assert(profile.config.rx_gain == SOLAR_OS_RADIO_RX_GAIN_BOOSTED);
    config.rx_gain = SOLAR_OS_RADIO_RX_GAIN_POWER_SAVING;
    assert(solar_os_radio_configure("radio0", &config) == ESP_OK);
    assert(solar_os_radio_profile_apply("radio0", "boost") == ESP_OK);
    assert(fake_status.config.rx_gain == SOLAR_OS_RADIO_RX_GAIN_BOOSTED);

    radio_profile_store_t legacy;
    radio_profile_store_empty(&legacy);
    legacy.version = 1;
    legacy.profiles[0].active = true;
    strlcpy(legacy.profiles[0].name, "legacy", sizeof(legacy.profiles[0].name));
    legacy.profiles[0].config = config;
    legacy.profiles[0].config.rx_gain = 0xa5; /* Former padding is not a gain setting. */
    assert(radio_profile_store_valid(&legacy, sizeof(legacy)));
    assert(radio_profile_store_save(&legacy) == ESP_OK);
    assert(solar_os_radio_profile_get("legacy", &profile) == ESP_OK);
    assert(profile.config.rx_gain == SOLAR_OS_RADIO_RX_GAIN_DEFAULT);
    legacy.version = RADIO_PROFILE_VERSION;
    assert(!radio_profile_store_valid(&legacy, sizeof(legacy)));
    legacy.profiles[0].config.rx_gain = SOLAR_OS_RADIO_RX_GAIN_G1;
    assert(radio_profile_store_save(&legacy) == ESP_OK);
    assert(solar_os_radio_profile_apply("radio0", "legacy") == ESP_ERR_NOT_SUPPORTED);
    assert(fake_status.config.rx_gain == SOLAR_OS_RADIO_RX_GAIN_BOOSTED);

    /* Older or future drivers without gain support still accept zero-initialized configs. */
    registration.name = "no-gain";
    registration.rx_gain_modes = 0;
    registration.features = 0;
    assert(solar_os_radio_register(&registration) == ESP_OK);
    config.rx_gain = SOLAR_OS_RADIO_RX_GAIN_BOOSTED;
    assert(solar_os_radio_configure("no-gain", &config) == ESP_ERR_NOT_SUPPORTED);
    config.rx_gain = SOLAR_OS_RADIO_RX_GAIN_DEFAULT;
    assert(solar_os_radio_configure("no-gain", &config) == ESP_OK);

    uint8_t gain = 0;
    for (unsigned i = 0; i < SOLAR_OS_RADIO_RX_GAIN_COUNT; i++) {
        assert(solar_os_radio_rx_gain_parse(solar_os_radio_rx_gain_name(i), &gain) && gain == i);
    }
    assert(!solar_os_radio_rx_gain_parse("maximum", &gain));
    char modes[80];
    solar_os_radio_rx_gains_format(info.rx_gain_modes, modes, sizeof(modes));
    assert(strcmp(modes, "power-saving boosted") == 0);
    test_scanner();
    /* Driver-reported fast tuning uses a claim and preserves the fallback for
     * registrations that only provide live RSSI. */
    registration.name = "fast-radio";
    solar_os_radio_ops_t fast_ops = ops;
    fast_ops.tune_rx = tune_rx;
    registration.ops = &fast_ops;
    registration.features = SOLAR_OS_RADIO_FEATURE_LIVE_RSSI | SOLAR_OS_RADIO_FEATURE_FAST_TUNE;
    assert(solar_os_radio_register(&registration) == ESP_OK);
    solar_os_radio_scan_t fast;
    solar_os_radio_scan_init(&fast);
    solar_os_radio_scan_config_t scan_config = {.mode = SOLAR_OS_RADIO_SCAN_BAND,
        .start_hz = 868000000, .end_hz = 868250000, .step_hz = 125000};
    fake_status.state = SOLAR_OS_RADIO_STATE_STANDBY;
    fake_status.config.rx_gain = SOLAR_OS_RADIO_RX_GAIN_DEFAULT;
    unsigned configured = configure_calls;
    timer_offset_us = 900;
    assert(solar_os_radio_scan_start(&fast, "fast-radio", &scan_config) == ESP_OK);
    assert(fast.fast_tune && fast.settle_ms == 2 && retunes == 2);
    unsigned old_samples = samples;
    ticks += 2; timer_offset_us = 0; /* Only 1.1 ms since tuning, despite a 2 ms tick change. */
    assert(solar_os_radio_scan_poll(&fast) == ESP_OK && samples == old_samples);
    timer_offset_us = 900;
    for (unsigned i = 0; i < 6; i++) { ticks += 2; assert(solar_os_radio_scan_poll(&fast) == ESP_OK); }
    assert(fast.sweeps == 1 && fast.last_sweep_ms > 0);
    assert(configure_calls == configured + 1 && retunes == 5);
    assert(solar_os_radio_scan_stop(&fast) == ESP_OK && configure_calls == configured + 2);
    puts("radio RX gain capabilities, ownership, profiles and v1 migration: ok");
    return 0;
}
