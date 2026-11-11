#include "inmp441.h"
#include "solar_os_audio.h"
#include "solar_os_inmp441.h"
#include "driver/gpio.h"
#include "freertos/task.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

struct fake_channel {
    bool enabled, initialized;
} rx, tx;
static i2s_chan_handle_t clock_master;
static unsigned allocated, deleted, reads;
static esp_err_t init_error, read_error;
static bool unaligned;
static int32_t written_pcm[256];
static bool audio_registered, fail_registration;
static solar_os_audio_device_info_t registered_device;
static bool test_power, powered, settled;
static unsigned low_pins;
static esp_err_t power_error, allocation_error;

esp_err_t gpio_config(const gpio_config_t *config) {
    assert(!rx.enabled && !tx.enabled);
    assert(config->pin_bit_mask == ((1ULL << 14) | (1ULL << 15) | (1ULL << 16)));
    assert(config->mode == GPIO_MODE_OUTPUT && low_pins == 7);
    assert(!config->pull_up_en && !config->pull_down_en && !config->intr_type);
    return ESP_OK;
}
esp_err_t gpio_set_direction(int pin, int mode) {
    assert(mode == GPIO_MODE_OUTPUT);
    assert(pin == 14 || pin == 15 || pin == 16 || pin == 21);
    return ESP_OK;
}
esp_err_t gpio_set_level(int pin, unsigned level) {
    if (pin == 21) {
        assert(!rx.enabled && !tx.enabled);
        assert(low_pins == 7);
        if (level && power_error != ESP_OK)
            return power_error;
        powered = level != 0;
        settled = false;
    } else {
        assert(level == 0);
        low_pins |= 1U << (pin - 14);
    }
    return ESP_OK;
}
void vTaskDelay(TickType_t ticks) {
    assert(powered && ticks >= pdMS_TO_TICKS(20));
    settled = true;
}

size_t strlcpy(char *dst, const char *src, size_t size) {
    size_t len = strlen(src), n = len < size - 1 ? len : size - 1;
    if (size != 0) {
        memcpy(dst, src, n);
        dst[n] = 0;
    }
    return len;
}
uint64_t solar_os_time_uptime_ms(void) { return 1234; }
esp_err_t solar_os_time_get_utc_epoch_ms(uint64_t *value) {
    *value = 1234;
    return ESP_OK;
}
esp_err_t
solar_os_audio_register_device_ex(const solar_os_audio_device_info_t *i,
                                  const solar_os_audio_device_ops_t *ops,
                                  void *user) {
    (void)ops;
    (void)user;
    assert(i->capabilities & SOLAR_OS_AUDIO_DEVICE_CAP_INPUT);
    if (fail_registration)
        return ESP_ERR_NO_MEM;
    registered_device = *i;
    audio_registered = true;
    return ESP_OK;
}
esp_err_t solar_os_audio_unregister_device(const char *name) {
    (void)name;
    assert(audio_registered);
    audio_registered = false;
    return ESP_OK;
}

static void test_expansion(void) {
    solar_os_expansion_binding_t bindings[] = {
        {.kind = SOLAR_OS_EXPANSION_BINDING_I2S_PORT, .value = 1},
        {.kind = SOLAR_OS_EXPANSION_BINDING_GPIO, .role = "bck", .value = 14},
        {.kind = SOLAR_OS_EXPANSION_BINDING_GPIO, .role = "ws", .value = 15},
        {.kind = SOLAR_OS_EXPANSION_BINDING_GPIO, .role = "din", .value = 17},
        {.kind = SOLAR_OS_EXPANSION_BINDING_GPIO, .role = "dout", .value = 16},
        {.kind = SOLAR_OS_EXPANSION_BINDING_PARAMETER,
         .role = "slot",
         .value = 1},
    };
    assert(solar_os_stream_init() == ESP_OK);
    fail_registration = true;
    assert(solar_os_inmp441_attach("audio0", bindings, 6) == ESP_ERR_NO_MEM);
    assert(solar_os_stream_count() == 0);
    fail_registration = false;
    assert(solar_os_inmp441_attach("audio0", bindings, 6) == ESP_OK);
    assert(solar_os_stream_count() == 2);
    assert(strcmp(registered_device.name, "INMP441 mic + I2S DAC/amp") == 0);
    solar_os_stream_handle_t input = SOLAR_OS_STREAM_HANDLE_INIT;
    solar_os_stream_handle_t output = SOLAR_OS_STREAM_HANDLE_INIT;
    /* Playback must work without an open capture stream. */
    assert(solar_os_stream_open("audio0.playback", "play", &output) == ESP_OK);
    const int16_t stereo[] = {-1000, 2000};
    size_t written;
    assert(solar_os_stream_write(&output, stereo, sizeof(stereo), 50,
                                  &written) == ESP_OK);
    assert(written == sizeof(stereo) && written_pcm[0] == -500 * 65536 &&
           written_pcm[1] == 1000 * 65536);
    solar_os_stream_close(&output);
    assert(solar_os_stream_open("audio0.capture", "record", &input) == ESP_OK);
    assert(input.audio.channels == 1);
    assert(solar_os_stream_open("audio0.playback", "play", &output) == ESP_OK);
    assert(solar_os_inmp441_detach("audio0") == ESP_ERR_INVALID_STATE);
    solar_os_stream_close(&output);
    assert(solar_os_inmp441_detach("audio0") == ESP_ERR_INVALID_STATE);
    solar_os_stream_info_t info;
    assert(solar_os_stream_get_info("audio0.playback", &info) == ESP_OK);
    int16_t pcm[4];
    size_t count;
    assert(solar_os_stream_read(&input, pcm, sizeof(pcm), 50, &count) ==
           ESP_OK);
    assert(count == sizeof(pcm) && pcm[0] == -456);
    solar_os_stream_close(&input);
    assert(solar_os_inmp441_detach("audio0") == ESP_OK);
    assert(solar_os_stream_count() == 0 && !audio_registered);
    assert(solar_os_inmp441_attach("mic0", bindings, 4) == ESP_OK);
    assert(solar_os_stream_count() == 1);
    assert(strcmp(registered_device.name, "INMP441 microphone") == 0);
    assert(solar_os_inmp441_detach("mic0") == ESP_OK);

    solar_os_expansion_binding_t powered_bindings[7];
    memcpy(powered_bindings, bindings, sizeof(bindings));
    powered_bindings[6] = (solar_os_expansion_binding_t){
        .kind = SOLAR_OS_EXPANSION_BINDING_GPIO, .role = "power", .value = 21};
    test_power = true;
    fail_registration = true;
    assert(solar_os_ics43432_attach("audio0", powered_bindings, 7) == ESP_ERR_NO_MEM);
    assert(solar_os_stream_count() == 0 && !powered);
    fail_registration = false;
    assert(solar_os_ics43432_attach("audio0", powered_bindings, 7) == ESP_OK);
    assert(!powered);
    assert(solar_os_inmp441_attach("other", bindings, 6) == ESP_ERR_INVALID_STATE);
    assert(strcmp(registered_device.name, "ICS-43432 mic + I2S DAC/amp") == 0);
    assert(solar_os_stream_get_info("audio0.capture", &info) == ESP_OK);
    assert(strcmp(info.provider, "ics43432") == 0);
    assert(solar_os_stream_open("audio0.capture", "record", &input) == ESP_OK);
    assert(powered && settled);
    assert(solar_os_stream_read(&input, pcm, sizeof(pcm), 50, &count) == ESP_OK);
    assert(pcm[0] == -456); /* LR tied high selects the right slot. */
    assert(solar_os_stream_open("audio0.playback", "play", &output) == ESP_OK);
    assert(solar_os_inmp441_detach("audio0") == ESP_ERR_INVALID_STATE);
    solar_os_stream_close(&input);
    assert(powered); /* Playback still owns the rail. */
    solar_os_stream_close(&output);
    assert(!powered && !rx.enabled && !tx.enabled);
    allocation_error = ESP_ERR_NO_MEM;
    assert(solar_os_stream_open("audio0.playback", "play", &output) == ESP_ERR_NO_MEM);
    assert(!powered);
    allocation_error = ESP_OK;
    init_error = ESP_FAIL;
    assert(solar_os_stream_open("audio0.playback", "play", &output) == ESP_FAIL);
    assert(!powered);
    init_error = ESP_OK;
    read_error = ESP_ERR_TIMEOUT;
    assert(solar_os_stream_open("audio0.capture", "record", &input) == ESP_ERR_TIMEOUT);
    assert(!powered);
    read_error = ESP_OK;
    power_error = ESP_FAIL;
    assert(solar_os_stream_open("audio0.playback", "play", &output) == ESP_FAIL);
    assert(!powered);
    power_error = ESP_OK;
    assert(solar_os_stream_open("audio0.playback", "play", &output) == ESP_OK);
    assert(solar_os_stream_open("audio0.capture", "record", &input) == ESP_OK);
    solar_os_stream_close(&output);
    assert(powered); /* Capture clocks also reach the DAC and amp. */
    solar_os_stream_close(&input);
    assert(!powered);
    assert(solar_os_inmp441_detach("audio0") == ESP_OK);
    test_power = false;
}

esp_err_t i2s_new_channel(const i2s_chan_config_t *c, i2s_chan_handle_t *t,
                          i2s_chan_handle_t *r) {
    assert(c->id == 1 && c->dma_desc_num == 4 && c->auto_clear_after_cb);
    if (test_power)
        assert(powered && settled);
    if (allocation_error != ESP_OK)
        return allocation_error;
    low_pins = 0;
    ++allocated;
    memset(&rx, 0, sizeof(rx));
    memset(&tx, 0, sizeof(tx));
    clock_master = NULL;
    *r = &rx;
    if (t != NULL)
        *t = &tx;
    return ESP_OK;
}
esp_err_t i2s_channel_init_std_mode(i2s_chan_handle_t h,
                                    const i2s_std_config_t *c) {
    assert(c->clk_cfg.sample_rate_hz == 16000 &&
           c->slot_cfg.data_bit_width == 32);
    assert(c->slot_cfg.slot_mode == 2 && c->gpio_cfg.mclk == -1);
    if (init_error != ESP_OK)
        return init_error;
    assert(!h->initialized);
    h->initialized = true;
    /* Model ESP-IDF duplex clock ownership: first initialized is master. */
    if (clock_master == NULL)
        clock_master = h;
    return ESP_OK;
}
esp_err_t i2s_channel_enable(i2s_chan_handle_t h) {
    assert(h->initialized && !h->enabled);
    h->enabled = true;
    return ESP_OK;
}
esp_err_t i2s_channel_disable(i2s_chan_handle_t h) {
    assert(h->enabled);
    h->enabled = false;
    return ESP_OK;
}
esp_err_t i2s_del_channel(i2s_chan_handle_t h) {
    assert(!h->enabled);
    ++deleted;
    return ESP_OK;
}
esp_err_t i2s_channel_read(i2s_chan_handle_t h, void *data, size_t len,
                           size_t *got, uint32_t timeout) {
    assert(h == &rx && h->enabled && timeout <= 1000);
    ++reads;
    if (clock_master == NULL || !clock_master->enabled) {
        *got = 0;
        return ESP_ERR_TIMEOUT;
    }
    if (read_error != ESP_OK) {
        *got = 0;
        return read_error;
    }
    int32_t *p = data;
    for (size_t i = 0; i < len / sizeof(int32_t); i += 2) {
        p[i] = 123 * 65536;
        p[i + 1] = -456 * 65536;
    }
    *got = unaligned ? len - 1 : len;
    return ESP_OK;
}
esp_err_t i2s_channel_write(i2s_chan_handle_t h, const void *data, size_t len,
                            size_t *got, uint32_t timeout) {
    assert(h == &tx && h->enabled && timeout <= 1000 &&
           len <= sizeof(written_pcm));
    if (clock_master == NULL || !clock_master->enabled) {
        *got = 0;
        return ESP_ERR_TIMEOUT;
    }
    memcpy(written_pcm, data, len);
    *got = len;
    return ESP_OK;
}

int main(void) {
    solar_os_inmp441_config_t c = {
        .port = 1, .bck = 14, .ws = 15, .din = 17, .dout = 16, .slot = 1,
        .power = -1};
    solar_os_inmp441_t d = {0};
    c.ws = 14;
    assert(solar_os_inmp441_init(&d, &c) == ESP_ERR_INVALID_ARG);
    c.ws = 15;
    c.slot = 2;
    assert(solar_os_inmp441_init(&d, &c) == ESP_ERR_INVALID_ARG);
    c.slot = 1;
    c.power = 14;
    assert(solar_os_inmp441_init(&d, &c) == ESP_ERR_INVALID_ARG);
    c.power = -1;
    assert(solar_os_inmp441_init(&d, &c) == ESP_OK);
    init_error = ESP_FAIL;
    assert(solar_os_inmp441_open(&d, false) == ESP_FAIL);
    assert(d.rx == NULL && d.tx == NULL && deleted == 2);
    init_error = ESP_OK;
    assert(solar_os_inmp441_open(&d, false) == ESP_OK);
    const int16_t mono[] = {-32768, 32767};
    size_t count;
    assert(!rx.enabled);
    assert(solar_os_inmp441_write(&d, mono, 2, 1, 100, 50, &count) == ESP_OK);
    assert(solar_os_inmp441_open(&d, true) == ESP_OK);
    assert(reads == 32 && allocated == 2);
    int16_t samples[3];
    assert(solar_os_inmp441_read(&d, samples, 3, 50, &count) == ESP_OK);
    assert(count == 3 && samples[0] == -456 && samples[2] == -456);
    assert(solar_os_inmp441_write(&d, mono, 2, 1, 100, 50, &count) == ESP_OK);
    assert(count == 2 && written_pcm[0] == INT32_MIN &&
           written_pcm[1] == INT32_MIN);
    assert(written_pcm[2] == 32767 * 65536 && written_pcm[3] == written_pcm[2]);
    unaligned = true;
    assert(solar_os_inmp441_read(&d, samples, 3, 50, &count) ==
           ESP_ERR_INVALID_SIZE);
    unaligned = false;
    /* Closing capture must leave playback clocks running. */
    solar_os_inmp441_close(&d, true);
    assert(!rx.enabled && tx.enabled);
    assert(solar_os_inmp441_write(&d, mono, 2, 1, 100, 50, &count) == ESP_OK);
    assert(solar_os_inmp441_open(&d, true) == ESP_OK);
    solar_os_inmp441_close(&d, false);
    assert(rx.enabled && tx.enabled && d.rx != NULL);
    assert(written_pcm[0] == 0);
    solar_os_inmp441_close(&d, true);
    assert(!rx.enabled && !tx.enabled && d.rx == NULL && d.tx == NULL);
    read_error = ESP_ERR_TIMEOUT;
    assert(solar_os_inmp441_open(&d, true) == ESP_ERR_TIMEOUT);
    assert(d.rx == NULL && d.tx == NULL);
    read_error = ESP_OK;
    solar_os_inmp441_deinit(&d);
    c.dout = -1;
    c.slot = 0;
    assert(solar_os_inmp441_init(&d, &c) == ESP_OK);
    assert(solar_os_inmp441_open(&d, false) == ESP_ERR_NOT_SUPPORTED);
    assert(solar_os_inmp441_open(&d, true) == ESP_OK);
    assert(solar_os_inmp441_read(&d, samples, 3, 50, &count) == ESP_OK &&
           samples[0] == 123);
    solar_os_inmp441_close(&d, true);
    solar_os_inmp441_deinit(&d);
    test_expansion();
    puts("INMP441/ICS-43432 transport and power tests: ok");
}
