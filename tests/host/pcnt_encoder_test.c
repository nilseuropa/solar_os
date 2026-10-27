#define main encoder_service_suite
#include "encoder_service_test.c"
#undef main
#include "driver/gpio.h"
#include "pcnt_encoder.h"
#include "solar_os_pcnt_encoder.h"

struct fake_channel {
    bool used;
    pcnt_chan_config_t config;
    pcnt_channel_edge_action_t rising, falling;
    pcnt_channel_level_action_t high, low;
};
struct fake_unit {
    bool used, enabled, started;
    int count;
    unsigned watches;
    uint32_t filter_ns;
    struct fake_channel channels[2];
};
static struct fake_unit units[4];
static bool levels[49];
static bool inputs[49];
static gpio_pull_mode_t pulls[49];
static unsigned calls, fail_at;
static bool fail_stop, fail_pin_reset;
static esp_err_t step(void) { return ++calls == fail_at ? ESP_FAIL : ESP_OK; }
static unsigned used_units(void)
{ unsigned n = 0; for (unsigned i = 0; i < 4; i++) n += units[i].used; return n; }
esp_err_t pcnt_new_unit(const pcnt_unit_config_t *c, pcnt_unit_handle_t *out)
{
    if (step() != ESP_OK) return ESP_FAIL;
    assert(c->low_limit == -32767 && c->high_limit == 32767 && c->flags.accum_count);
    for (unsigned i = 0; i < 4; i++) if (!units[i].used) {
        units[i] = (struct fake_unit){.used = true}; *out = &units[i]; return ESP_OK;
    }
    return ESP_ERR_NOT_FOUND;
}
esp_err_t pcnt_del_unit(pcnt_unit_handle_t u)
{ assert(u->used && !u->enabled && !u->channels[0].used && !u->channels[1].used); u->used = false; return ESP_OK; }
esp_err_t pcnt_new_channel(pcnt_unit_handle_t u, const pcnt_chan_config_t *c, pcnt_channel_handle_t *out)
{
    if (step() != ESP_OK) return ESP_FAIL;
    assert(u->used && !u->enabled && inputs[c->edge_gpio_num] && inputs[c->level_gpio_num]);
    for (unsigned i = 0; i < 2; i++) if (!u->channels[i].used) {
        u->channels[i] = (struct fake_channel){.used = true, .config = *c}; *out = &u->channels[i]; return ESP_OK;
    }
    return ESP_ERR_NOT_FOUND;
}
esp_err_t pcnt_del_channel(pcnt_channel_handle_t c) { assert(c->used); c->used = false; return ESP_OK; }
esp_err_t pcnt_channel_set_edge_action(pcnt_channel_handle_t c, pcnt_channel_edge_action_t rising, pcnt_channel_edge_action_t falling)
{ if (step() != ESP_OK) return ESP_FAIL; c->rising = rising; c->falling = falling; return ESP_OK; }
esp_err_t pcnt_channel_set_level_action(pcnt_channel_handle_t c, pcnt_channel_level_action_t high, pcnt_channel_level_action_t low)
{ if (step() != ESP_OK) return ESP_FAIL; c->high = high; c->low = low; return ESP_OK; }
esp_err_t pcnt_unit_set_glitch_filter(pcnt_unit_handle_t u, const pcnt_glitch_filter_config_t *c)
{
    if (step() != ESP_OK) return ESP_FAIL;
    if (c && c->max_glitch_ns > 12787) return ESP_ERR_INVALID_ARG;
    u->filter_ns = c ? c->max_glitch_ns : 0; return ESP_OK;
}
esp_err_t pcnt_unit_add_watch_point(pcnt_unit_handle_t u, int point)
{ if (step() != ESP_OK) return ESP_FAIL; assert(point == -32767 || point == 32767); u->watches++; return ESP_OK; }
esp_err_t pcnt_unit_enable(pcnt_unit_handle_t u)
{ if (step() != ESP_OK) return ESP_FAIL; assert(!u->enabled && u->watches == 2); u->enabled = true; return ESP_OK; }
esp_err_t pcnt_unit_disable(pcnt_unit_handle_t u)
{ assert(u->enabled && !u->started); u->enabled = false; return ESP_OK; }
esp_err_t pcnt_unit_start(pcnt_unit_handle_t u)
{ if (step() != ESP_OK) return ESP_FAIL; assert(u->enabled && !u->started); u->started = true; return ESP_OK; }
esp_err_t pcnt_unit_stop(pcnt_unit_handle_t u)
{ if (fail_stop) return ESP_FAIL; assert(u->started); u->started = false; return ESP_OK; }
esp_err_t pcnt_unit_get_count(pcnt_unit_handle_t u, int *count)
{ assert(u->enabled && u->started); *count = u->count; return ESP_OK; }
esp_err_t pcnt_unit_clear_count(pcnt_unit_handle_t u)
{ if (step() != ESP_OK) return ESP_FAIL; assert(!u->started); u->count = 0; return ESP_OK; }
esp_err_t gpio_set_pull_mode(gpio_num_t pin, gpio_pull_mode_t pull)
{ if (step() != ESP_OK) return ESP_FAIL; assert(GPIO_IS_VALID_GPIO(pin)); pulls[pin] = pull; return ESP_OK; }
esp_err_t gpio_set_direction(gpio_num_t pin, gpio_mode_t mode)
{ if (step() != ESP_OK) return ESP_FAIL; assert(GPIO_IS_VALID_GPIO(pin) && mode == GPIO_MODE_INPUT); inputs[pin] = true; return ESP_OK; }
esp_err_t gpio_reset_pin(gpio_num_t pin)
{ if (fail_pin_reset) return ESP_FAIL; assert(GPIO_IS_VALID_GPIO(pin)); pulls[pin] = GPIO_FLOATING; inputs[pin] = false; return ESP_OK; }

static void edge(int pin, bool high)
{
    assert(levels[pin] != high); levels[pin] = high;
    for (unsigned i = 0; i < 4; i++) if (units[i].used && units[i].started) {
        for (unsigned j = 0; j < 2; j++) {
            struct fake_channel *c = &units[i].channels[j];
            if (!c->used || c->config.edge_gpio_num != pin) continue;
            pcnt_channel_edge_action_t a = high ? c->rising : c->falling;
            int delta = a == PCNT_CHANNEL_EDGE_ACTION_INCREASE ? 1 : a == PCNT_CHANNEL_EDGE_ACTION_DECREASE ? -1 : 0;
            if ((levels[c->config.level_gpio_num] ? c->high : c->low) == PCNT_CHANNEL_LEVEL_ACTION_INVERSE) delta = -delta;
            units[i].count += delta;
        }
    }
}
static void cycle(int a, int b, bool forward)
{
    if (forward) { edge(a, true); edge(b, true); edge(a, false); edge(b, false); }
    else { edge(b, true); edge(a, true); edge(b, false); edge(a, false); }
}
static void attach_bindings(solar_os_expansion_binding_t *b, int mode)
{
    b[0] = (solar_os_expansion_binding_t){.kind = SOLAR_OS_EXPANSION_BINDING_GPIO, .role = "a", .value = 1};
    b[1] = (solar_os_expansion_binding_t){.kind = SOLAR_OS_EXPANSION_BINDING_GPIO, .role = "b", .value = 2};
    b[2] = (solar_os_expansion_binding_t){.kind = SOLAR_OS_EXPANSION_BINDING_PARAMETER, .role = "mode", .value = mode};
}

int main(void)
{
    assert(encoder_service_suite() == 0);
    solar_os_expansion_binding_t b[6];
    const int modes[] = {1, 2, 4};
    for (unsigned m = 0; m < 3; m++) for (unsigned invert = 0; invert < 2; invert++) {
        attach_bindings(b, modes[m]);
        b[3] = (solar_os_expansion_binding_t){.kind = SOLAR_OS_EXPANSION_BINDING_PARAMETER, .role = "invert", .value = invert};
        b[4] = (solar_os_expansion_binding_t){.kind = SOLAR_OS_EXPANSION_BINDING_PARAMETER, .role = "pull", .value = 1};
        b[5] = (solar_os_expansion_binding_t){.kind = SOLAR_OS_EXPANSION_BINDING_PARAMETER, .role = "filter_ns", .value = 0};
        assert(solar_os_pcnt_encoder_attach("wheel", b, 6) == ESP_OK);
        assert(pulls[1] == GPIO_PULLUP_ONLY && units[0].filter_ns == 0);
        cycle(1, 2, true);
        int32_t count;
        assert(solar_os_encoder_read("wheel", &count) == ESP_OK && count == (invert ? -modes[m] : modes[m]));
        cycle(1, 2, false);
        assert(solar_os_encoder_read("wheel", &count) == ESP_OK && count == 0);
        /* Both-edge modes cancel an edge followed immediately by its reversal. */
        edge(1, true); edge(1, false);
        if (modes[m] != 1) assert(solar_os_encoder_read("wheel", &count) == ESP_OK && count == 0);
        assert(solar_os_encoder_reset("wheel") == ESP_OK);
        for (unsigned j = 0; j < 40000; j++) cycle(1, 2, true);
        assert(solar_os_encoder_read("wheel", &count) == ESP_OK && count == (invert ? -1 : 1) * modes[m] * 40000);
        assert(solar_os_pcnt_encoder_detach("wheel") == ESP_OK && !used_units() && !allocations);
    }
    attach_bindings(b, 4);
    assert(solar_os_pcnt_encoder_attach("bad", b, 2) == ESP_ERR_INVALID_ARG);
    b[2].value = 3; assert(solar_os_pcnt_encoder_attach("bad", b, 3) == ESP_ERR_INVALID_ARG);
    b[2].value = 4; b[1].value = 1;
    assert(solar_os_pcnt_encoder_validate(b, 3, NULL) == ESP_ERR_INVALID_ARG);
    b[1].value = 2; b[3] = b[2];
    assert(solar_os_pcnt_encoder_attach("bad", b, 4) == ESP_ERR_INVALID_ARG && !used_units());
    b[3] = (solar_os_expansion_binding_t){.kind = SOLAR_OS_EXPANSION_BINDING_PARAMETER, .role = "filter_ns", .value = 999999};
    assert(solar_os_pcnt_encoder_attach("bad", b, 4) == ESP_ERR_INVALID_ARG && !used_units() && !allocations);
    calls = 0; assert(solar_os_pcnt_encoder_attach("wheel", b, 3) == ESP_OK);
    const unsigned init_calls = calls;
    assert(solar_os_pcnt_encoder_detach("wheel") == ESP_OK);
    for (unsigned i = 1; i <= init_calls; i++) {
        calls = 0; fail_at = i;
        assert(solar_os_pcnt_encoder_attach("wheel", b, 3) == ESP_FAIL);
        assert(!used_units() && !allocations && !solar_os_encoder_count());
    }
    fail_at = 0;
    const char *tags[] = {"encoder.pcnt", "encoder.device", "encoder.lock"};
    for (unsigned i = 0; i < 3; i++) {
        fail_tag = tags[i];
        assert(solar_os_pcnt_encoder_attach("wheel", b, 3) == ESP_ERR_NO_MEM && !used_units() && !allocations);
    }
    fail_tag = NULL;
    const char *names[] = {"one", "two", "three", "four"};
    for (unsigned i = 0; i < 4; i++) { b[0].value = 3 + 2*i; b[1].value = 4 + 2*i;
        assert(solar_os_pcnt_encoder_attach(names[i], b, 3) == ESP_OK); }
    b[0].value = 11; b[1].value = 12;
    assert(solar_os_pcnt_encoder_attach("five", b, 3) == ESP_ERR_NOT_FOUND && used_units() == 4 && solar_os_encoder_count() == 4);
    assert(solar_os_pcnt_encoder_attach("one", b, 3) == ESP_ERR_INVALID_STATE);
    cycle(3, 4, true); cycle(5, 6, false);
    int32_t count;
    assert(solar_os_encoder_read("one", &count) == ESP_OK && count == 4);
    assert(solar_os_encoder_read("two", &count) == ESP_OK && count == -4);
    fail_stop = true;
    assert(solar_os_pcnt_encoder_detach("one") == ESP_FAIL && used_units() == 4);
    assert(solar_os_encoder_read("one", &count) == ESP_OK && count == 4);
    fail_stop = false; fail_pin_reset = true;
    assert(solar_os_pcnt_encoder_detach("one") == ESP_FAIL && solar_os_encoder_count() == 4);
    assert(solar_os_encoder_read("one", &count) == ESP_ERR_INVALID_STATE);
    fail_pin_reset = false;
    for (unsigned i = 0; i < 4; i++) assert(solar_os_pcnt_encoder_detach(names[i]) == ESP_OK);
    assert(!used_units() && !allocations && !solar_os_encoder_count());
    puts("PCNT encoder: x1/x2/x4 edge simulation, inversion, resource exhaustion, setup failures and detach retry PASS");
    return 0;
}
