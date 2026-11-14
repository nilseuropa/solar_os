from copy import deepcopy
from pathlib import Path
import sys
import unittest

from test_foreground_presenter import production_functions
from test_shutdown_policy import run_c

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "scripts"))
from solaros_board_manifest import (
    ManifestError, generate_header, load_board_manifest, load_driver_catalog,
    required_packages, validate_board,
)


class Es7243Test(unittest.TestCase):
    def test_clock_order_cleanup_gain_reopen_and_read_timeout(self):
        source = (ROOT / "src/services/solar_os_es7243.c").read_text()
        state = source[source.index("#define ES7243_DEVICE_NAME_MAX"):
                       source.index("static bool es7243_requested_format_supported")]
        functions = production_functions(source, (
            "es7243_requested_format_supported", "es7243_deinit", "es7243_init",
            "es7243_stream_open", "es7243_stream_close", "es7243_stream_read",
            "es7243_set_input_gain", "es7243_get_input_gain"))
        run_c(r'''
#include <assert.h>
#include <limits.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include "es7243_gain.h"
#define SOLAR_OS_AUDIO_DEVICE_ID_MAX 16
#define SOLAR_OS_STREAM_ID_MAX 32
#define SOLAR_OS_STREAM_AUDIO_S16_LE 0
#define I2S_ROLE_MASTER 1
#define I2S_SLOT_MODE_STEREO 2
#define I2S_GPIO_UNUSED -1
#define I2S_MCLK_MULTIPLE_256 256
#define ESP_CODEC_DEV_TYPE_IN 1
#define ESP_CODEC_DEV_OK 0
#define portMAX_DELAY 0
typedef void *i2s_chan_handle_t;
typedef void *i2c_master_bus_handle_t;
typedef void *esp_codec_dev_handle_t;
typedef int audio_codec_data_if_t;
typedef int audio_codec_ctrl_if_t;
typedef int audio_codec_if_t;
typedef int StaticSemaphore_t;
typedef void *SemaphoreHandle_t;
typedef struct { unsigned sample_format, sample_rate, channels, bits_per_sample, frames_per_block; } solar_os_stream_audio_format_t;
typedef struct { solar_os_stream_audio_format_t requested_audio; } solar_os_stream_open_options_t;
typedef struct { void *context; solar_os_stream_audio_format_t audio; } solar_os_stream_handle_t;
typedef struct { int port, role, dma_desc_num, dma_frame_num; } i2s_chan_config_t;
typedef struct { int bits, mode; } slot_t;
typedef struct { int rate, mclk_multiple; } clock_t;
typedef struct { slot_t slot_cfg; clock_t clk_cfg; struct { int mclk, bclk, ws, dout, din; } gpio_cfg; } i2s_std_config_t;
#define I2S_CHANNEL_DEFAULT_CONFIG(p,r) ((i2s_chan_config_t){.port=p,.role=r})
#define I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(b,m) ((slot_t){b,m})
#define I2S_STD_CLK_DEFAULT_CONFIG(r) ((clock_t){.rate=r})
typedef struct { int port; void *rx_handle; } audio_codec_i2s_cfg_t;
typedef struct { int port, addr; void *bus_handle; } audio_codec_i2c_cfg_t;
typedef struct { const void *ctrl_if; } es7243_codec_cfg_t;
typedef es7243_codec_cfg_t es7243e_codec_cfg_t;
typedef struct { int dev_type; const void *codec_if, *data_if; } esp_codec_dev_cfg_t;
typedef struct { int sample_rate, channel, bits_per_sample; } esp_codec_dev_sample_info_t;
static int stage, fail_at, objects, clocks, bus_locked, mutex_locked, variant;
static int token[5], last_address;
static float last_gain;
static uint32_t last_timeout;
static bool failed(void) { return ++stage == fail_at; }
static int xSemaphoreTake(void *m, int wait) { (void)m; (void)wait; assert(!mutex_locked++); return 1; }
static int xSemaphoreGive(void *m) { (void)m; assert(mutex_locked-- == 1); return 1; }
static void i2c_bus_lock(void) { assert(!bus_locked++); }
static void i2c_bus_unlock(void) { assert(bus_locked-- == 1); }
static esp_err_t i2s_new_channel(const i2s_chan_config_t *c, void *tx, void **rx) {
    assert(c->dma_desc_num == 4 && c->dma_frame_num == 128 && !tx);
    if (failed()) return ESP_FAIL;
    *rx = token; objects |= 1; return ESP_OK;
}
static esp_err_t i2s_channel_init_std_mode(void *h, const i2s_std_config_t *c) {
    assert(h == token && c->slot_cfg.bits == 16 && c->slot_cfg.mode == 2);
    assert(c->clk_cfg.rate == 16000 && c->clk_cfg.mclk_multiple == 256);
    assert(c->gpio_cfg.dout == -1); return failed() ? ESP_FAIL : ESP_OK;
}
static esp_err_t i2s_channel_enable(void *h) { assert(h == token); if (failed()) return ESP_FAIL; clocks = 1; return ESP_OK; }
static esp_err_t i2s_channel_disable(void *h) { assert(h == token && clocks); clocks = 0; return ESP_OK; }
static esp_err_t i2s_del_channel(void *h) { assert(h == token && !clocks); objects &= ~1; return ESP_OK; }
static const void *audio_codec_new_i2s_data(const audio_codec_i2s_cfg_t *c) {
    assert(clocks && c->rx_handle == token); if (failed()) return NULL;
    objects |= 2; return token + 1;
}
static const void *audio_codec_new_i2c_ctrl(const audio_codec_i2c_cfg_t *c) {
    assert(clocks && bus_locked && c->bus_handle); last_address = c->addr;
    if (failed()) return NULL;
    objects |= 4; return token + 2;
}
static const void *codec_new(const es7243_codec_cfg_t *c, int e) {
    assert(clocks && bus_locked && c->ctrl_if == token + 2); variant = e;
    if (failed()) return NULL;
    objects |= 8; return token + 3;
}
static const void *es7243_codec_new(const es7243_codec_cfg_t *c) { return codec_new(c, 0); }
static const void *es7243e_codec_new(const es7243e_codec_cfg_t *c) { return codec_new(c, 1); }
static void *esp_codec_dev_new(const esp_codec_dev_cfg_t *c) {
    assert(c->dev_type == 1 && c->codec_if == token + 3 && c->data_if == token + 1);
    if (failed()) return NULL;
    objects |= 16; return token + 4;
}
static int esp_codec_dev_open(void *h, const esp_codec_dev_sample_info_t *c) {
    assert(h == token + 4 && c->sample_rate == 16000 && c->channel == 2 && c->bits_per_sample == 16);
    return failed() ? -1 : 0;
}
static int esp_codec_dev_set_in_gain(void *h, float gain) { assert(h == token + 4); if (failed()) return -1; last_gain = gain; return 0; }
static int esp_codec_dev_close(void *h) { assert(h == token + 4 && clocks); return 0; }
static void esp_codec_dev_delete(void *h) { assert(h == token + 4); objects &= ~16; }
static void audio_codec_delete_codec_if(const void *h) { assert(h == token + 3 && !(objects & 16)); objects &= ~8; }
static void audio_codec_delete_ctrl_if(const void *h) { assert(h == token + 2 && !(objects & 8)); objects &= ~4; }
static void audio_codec_delete_data_if(const void *h) { assert(h == token + 1 && !(objects & 16)); objects &= ~2; }
static esp_err_t i2s_channel_read(void *h, void *data, size_t len, size_t *read_len, uint32_t timeout) {
    assert(h == token && clocks && data && mutex_locked);
    last_timeout = timeout; *read_len = len / 2; return ESP_ERR_TIMEOUT;
}
''' + state + functions + r'''
static void reset(bool e) {
    assert(!objects && !clocks && !bus_locked && !mutex_locked);
    memset(&es7243_audio, 0, sizeof(es7243_audio));
    es7243_audio.attached = true; es7243_audio.variant_e = e;
    es7243_audio.address = e ? 0x14 : 0x13; es7243_audio.i2c_handle = token;
    stage = 0; fail_at = 0;
    (void)mutex_storage;
}
int main(void) {
    float applied;
    assert(solar_os_es7243_gain(false, NAN, &applied) == ESP_ERR_INVALID_ARG);
    assert(solar_os_es7243_gain(true, INFINITY, &applied) == ESP_ERR_INVALID_ARG);
    assert(solar_os_es7243_gain(false, -1, &applied) == ESP_ERR_INVALID_ARG);
    assert(solar_os_es7243_gain(false, 28, &applied) == ESP_ERR_INVALID_ARG);
    assert(solar_os_es7243_gain(true, 38, &applied) == ESP_ERR_INVALID_ARG);
    assert(solar_os_es7243_gain(false, 0, &applied) == ESP_OK && applied == 1);
    assert(solar_os_es7243_gain(false, 4, &applied) == ESP_OK && applied == 3.5f);
    assert(solar_os_es7243_gain(false, 19, &applied) == ESP_OK && applied == 20.5f);
    for (int e = 0; e < 2; e++) {
        for (int failure = 1; failure <= 9; failure++) {
            reset(e); fail_at = failure;
            assert(es7243_init() != ESP_OK);
            assert(!objects && !clocks && !bus_locked && !es7243_audio.initialized);
            fail_at = stage = 0;
            assert(es7243_init() == ESP_OK && stage == 9 && objects == 31);
            assert(last_address == (e ? 0x28 : 0x26) && variant == e);
            es7243_deinit();
        }
    }
    reset(true);
    assert(es7243_set_input_gain(&es7243_audio, 32.5f) == ESP_OK);
    assert(es7243_get_input_gain(&es7243_audio, &applied) == ESP_OK && applied == 33);
    solar_os_stream_handle_t handle = {0};
    solar_os_stream_open_options_t options = {.requested_audio = {.sample_rate = 48000}};
    assert(es7243_stream_open(&es7243_audio, "test", &options, &handle) == ESP_ERR_NOT_SUPPORTED && !objects);
    assert(es7243_stream_open(&es7243_audio, "test", NULL, &handle) == ESP_OK);
    assert(last_gain == 32.5f && handle.audio.channels == 2 && handle.audio.sample_rate == 16000);
    int16_t data[4]; size_t count = 99;
    assert(es7243_stream_read(&es7243_audio, &handle, data, sizeof(data), 17, &count) == ESP_ERR_TIMEOUT);
    assert(last_timeout == 17 && count == sizeof(data) / 2);
    assert(es7243_stream_read(&es7243_audio, &handle, data, 3, 17, &count) == ESP_ERR_INVALID_SIZE && count == 0);
    fail_at = stage + 1;
    assert(es7243_set_input_gain(&es7243_audio, 12) == ESP_FAIL);
    assert(es7243_audio.gain_db == 33 && es7243_audio.requested_gain_db == 32.5f);
    es7243_stream_close(&es7243_audio, &handle);
    fail_at = 0;
    assert(!handle.context && !objects);
    assert(es7243_stream_open(&es7243_audio, "test", NULL, &handle) == ESP_OK);
    assert(last_gain == 32.5f && es7243_audio.gain_db == 33);
    es7243_stream_close(&es7243_audio, &handle);
    return 0;
}
''', "-I", str(ROOT / "src/drivers"), "-fsanitize=address,undefined", "-no-pie")

    def test_shared_port_uses_standard_i2s_and_preserves_legacy_tdm(self):
        source = (ROOT / "src/drivers/audio_codec_board.c").read_text()
        functions = production_functions(source, (
            "audio_codec_is_duplex", "audio_codec_is_es7243", "audio_codec_i2s_start"))
        run_c(r'''
#include <assert.h>
#include <stdbool.h>
#include <stddef.h>
#include "esp_err.h"
enum { AUDIO_CODEC_BOARD_ES8311_ES7210, AUDIO_CODEC_BOARD_ES8311_DUPLEX,
       AUDIO_CODEC_BOARD_ES8311_ES7243, AUDIO_CODEC_BOARD_ES8311_ES7243E };
typedef void *i2s_chan_handle_t;
typedef int i2s_std_config_t;
typedef int i2s_tdm_config_t;
static struct { struct { int mode; } config; } audio_codec;
static int standard, tdm, enabled;
static esp_err_t init_error;
static i2s_std_config_t audio_codec_i2s_std_config(void) { return 16; }
static i2s_tdm_config_t audio_codec_i2s_tdm_config(void) { return 32; }
static esp_err_t i2s_channel_init_std_mode(void *h, const int *c) { assert(!h && *c == 16); standard++; return init_error; }
static esp_err_t i2s_channel_init_tdm_mode(void *h, const int *c) { assert(!h && *c == 32); tdm++; return init_error; }
static esp_err_t i2s_channel_enable(void *h) { assert(!h); enabled++; return ESP_OK; }
''' + functions + r'''
int main(void) {
    for (int mode = 0; mode < 4; mode++) {
        audio_codec.config.mode = mode;
        assert(audio_codec_i2s_start(NULL) == ESP_OK);
    }
    assert(standard == 3 && tdm == 1 && enabled == 4);
    init_error = ESP_FAIL;
    assert(audio_codec_i2s_start(NULL) == ESP_FAIL && enabled == 4);
}
''')

    def test_manifest_exposes_adc_dac_addresses_and_expander_pa(self):
        manifests = ROOT / "boards/manifests"
        drivers = load_driver_catalog(ROOT / "boards/expansion_drivers.toml")
        board = load_board_manifest(manifests / "esp32_s3_devkitc1_n16r8.toml", manifests)
        for pin in board["pins"]:
            if pin["gpio"] in (14, 15, 16, 17, 21):
                pin.update(policy="fixed", user=False, adc=False, pwm=False)
        device = {"driver": "es8311-es7243e", "name": "audio0", "bindings": {
            "i2c": "i2c0", "addr": 0x14, "dac_addr": 0x18, "i2s": 1,
            "mclk": 14, "bck": 15, "ws": 16, "din": 17, "dout": 21,
            "pa": "gpiox0:12",
        }}
        board.setdefault("devices", []).append(device)
        validate_board(board, drivers)
        self.assertIn("driver_audio_es8311_codecs", required_packages(board, drivers))
        header = generate_header(board, drivers)
        self.assertRegex(header, r'\.role = "dac_addr", \.value = 24')
        self.assertIn('.role = "pa", .target = "gpiox0", .value = 12', header)
        for key, value in (("addr", 0x80), ("dac_addr", 0), ("din", 15)):
            invalid = deepcopy(board)
            invalid["devices"][-1]["bindings"][key] = value
            with self.assertRaises(ManifestError):
                validate_board(invalid, drivers)

    def test_pair_prepares_peer_channel_before_codec_sdk_reconfigures_clocks(self):
        function = production_functions(
            (ROOT / "src/drivers/audio_codec_board.c").read_text(),
            ("audio_codec_ensure_common_interfaces",))
        run_c(r'''
#include <assert.h>
#include <stdbool.h>
#include <stddef.h>
#include <string.h>
#include "esp_err.h"
#define ESP_RETURN_ON_ERROR(expr,...) do { int err_ = (expr); if (err_) return err_; } while (0)
typedef struct { int port; void *rx_handle, *tx_handle; } audio_codec_i2s_cfg_t;
static struct {
    struct { int i2s_port; } config;
    void *rx_handle, *tx_handle;
    const void *gpio_if, *data_if;
} audio_codec;
static bool family, tx_ready, rx_ready;
static int tx_calls, rx_calls, constructors, token;
static esp_err_t tx_error, rx_error;
static bool audio_codec_is_es7243(void) { return family; }
static esp_err_t audio_codec_i2s_ensure_channels(void) { return ESP_OK; }
static esp_err_t audio_codec_i2s_ensure_tx(void) { tx_calls++; if (tx_error) return tx_error; tx_ready = true; return ESP_OK; }
static esp_err_t audio_codec_i2s_ensure_rx(void) { rx_calls++; if (rx_error) return rx_error; rx_ready = true; return ESP_OK; }
static const void *audio_codec_new_gpio(void) { return &token; }
static const void *audio_codec_new_i2s_data(const audio_codec_i2s_cfg_t *c) {
    assert(c->port == 0 && c->rx_handle && c->tx_handle);
    if (family) assert(tx_ready && rx_ready);
    constructors++; return &token;
}
''' + function + r'''
static void reset(bool es7243) {
    memset(&audio_codec, 0, sizeof(audio_codec));
    audio_codec.rx_handle = audio_codec.tx_handle = &token;
    family = es7243; tx_ready = rx_ready = false;
    tx_error = rx_error = ESP_OK; tx_calls = rx_calls = constructors = 0;
}
int main(void) {
    for (int capture_first = 0; capture_first < 2; capture_first++) {
        reset(true); rx_ready = capture_first; tx_ready = !capture_first;
        assert(audio_codec_ensure_common_interfaces() == ESP_OK);
        assert(tx_ready && rx_ready && constructors == 1);
        assert(audio_codec_ensure_common_interfaces() == ESP_OK && constructors == 1);
    }
    reset(true); tx_error = ESP_FAIL;
    assert(audio_codec_ensure_common_interfaces() == ESP_FAIL && !constructors);
    reset(true); rx_error = ESP_FAIL;
    assert(audio_codec_ensure_common_interfaces() == ESP_FAIL && !constructors);
    reset(false);
    assert(audio_codec_ensure_common_interfaces() == ESP_OK && constructors == 1);
    assert(tx_calls == 0 && rx_calls == 0); /* Preserve legacy TDM setup. */
}
''')


if __name__ == "__main__":
    unittest.main()
