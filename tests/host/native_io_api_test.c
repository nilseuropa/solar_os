#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "solar_os_native_client.h"
#include "solar_os_native_io.h"
#include "solar_os_native_handles.h"
#include "solar_os_native_pwm_provider.h"
#include "solar_os_native_runtime.h"
#include "solar_os_resources.h"
#include "solar_os_pins.h"
#include "pin_labels.inc"
#include "solar_os_gpio.h"
#include "solar_os_gpio_controller.h"
#include "gpio_port.h"
#include "solar_os_adc.h"
#include "solar_os_pwm.h"
#include "solar_os_memory.h"
static unsigned allocations, writes, reads, resets, pwm_writes, pwm_closes;
static bool fail_reset, fail_pwm_close;
static unsigned library_pins;
void *solar_os_native_driver_pin(void) { return NULL; }
void solar_os_native_driver_unpin(void *pin) { assert(pin == NULL); }
int solar_os_native_library_retain(uint32_t lease, uint32_t *retained)
{
    if (lease != 10) return SOLAR_OS_NATIVE_ERROR_NOT_FOUND;
    *retained = 11;
    ++library_pins;
    return 0;
}
int solar_os_native_library_release(uint32_t lease)
{
    assert(lease == 11 && library_pins);
    --library_pins;
    return 0;
}
static char role[] = "available pin";
typedef struct { bool configured, output_level; solar_os_gpio_mode_t mode; solar_os_gpio_pull_t pull; } gpio_slot_t;
static gpio_slot_t gpio_slots[2];
size_t strlcpy(char *out, const char *in, size_t capacity)
{ size_t size = strlen(in); if (capacity) { size_t n = size < capacity - 1 ? size : capacity - 1; memcpy(out, in, n); out[n] = 0; } return size; }
void *solar_os_memory_calloc(size_t n, size_t size, solar_os_memory_class_t cls, const char *tag)
{ assert(cls == SOLAR_OS_MEMORY_EXTERNAL_PREFERRED && tag); void *p = calloc(n, size); if (p) ++allocations; return p; }
void solar_os_memory_free(void *p) { if (p) --allocations; free(p); }
size_t solar_os_pin_count(void) { return 1; }
bool solar_os_pin_get_info(size_t index, solar_os_pin_info_t *out)
{ if (index) return false; *out = (solar_os_pin_info_t){7, true, true, SOLAR_OS_PIN_POLICY_FREE, role}; return true; }
bool solar_os_pin_get_info_by_pin(int pin, solar_os_pin_info_t *out) { return pin == 7 && solar_os_pin_get_info(0, out); }
bool solar_os_pin_is_expansion(int pin) { return pin == 7; }
bool solar_os_pin_is_direct_gpio(int pin) { return pin == 7; }
bool solar_os_pin_is_routable(int pin) { return pin == 7; }
bool solar_os_connector_layout_get_info(solar_os_connector_layout_info_t *out)
{ *out = (solar_os_connector_layout_info_t){"Connector", "top", 2, 4}; return true; }
size_t solar_os_connector_pin_count(void) { return 1; }
bool solar_os_connector_pin_get_info(size_t index, solar_os_connector_pin_info_t *out)
{ if (index) return false; *out = (solar_os_connector_pin_info_t){"J1", 3, 1, 2, 7, SOLAR_OS_CONNECTOR_PIN_GPIO, "SDA"}; return true; }
bool solar_os_connector_pin_find(size_t row, size_t column, solar_os_connector_pin_info_t *out, size_t *index)
{ *index = 0; return row == 1 && column == 2 && solar_os_connector_pin_get_info(0, out); }
bool solar_os_connector_exists(const char *name) { return !strcmp(name, "J1"); }
static gpio_slot_t *find_slot(int pin) { return pin == 7 || pin == 8 ? &gpio_slots[pin - 7] : NULL; }
static esp_err_t gpio_check_owned(int pin, const char *owner)
{
    solar_os_resource_claim_t claim;
    return solar_os_resource_find_claim(SOLAR_OS_RESOURCE_GPIO_PIN, pin, -1, &claim) && !strcmp(claim.owner, owner) ? ESP_OK : ESP_ERR_INVALID_STATE;
}
esp_err_t gpio_port_write(gpio_num_t pin, bool level) { assert(find_slot(pin)); (void)level; ++writes; return ESP_OK; }
esp_err_t gpio_port_configure(gpio_num_t pin, gpio_port_mode_t mode, gpio_port_pull_t pull)
{ (void)pin; (void)mode; (void)pull; return ESP_ERR_NOT_SUPPORTED; }
esp_err_t gpio_port_read(gpio_num_t pin, bool *level) { assert(find_slot(pin)); *level = true; ++reads; return ESP_OK; }
#include "gpio_owned_write.inc"
size_t solar_os_gpio_pin_count(void) { return 2; }
bool solar_os_gpio_get_pin_info(size_t index, solar_os_gpio_pin_info_t *out)
{ if (index >= 2) return false; *out = (solar_os_gpio_pin_info_t){.pin = 7 + index, .runtime_allowed = true, .policy = SOLAR_OS_PIN_POLICY_FREE, .role = role}; return true; }
bool solar_os_gpio_get_pin_info_by_pin(int pin, solar_os_gpio_pin_info_t *out) { return pin >= 7 && pin <= 8 && solar_os_gpio_get_pin_info(pin - 7, out); }
bool solar_os_gpio_is_runtime_allowed(int pin) { return pin == 7 || pin == 8; }
esp_err_t solar_os_gpio_configure(int pin, solar_os_gpio_mode_t mode, solar_os_gpio_pull_t pull)
{ (void)pin; (void)mode; (void)pull; return ESP_ERR_NOT_ALLOWED; }
esp_err_t solar_os_gpio_release(int pin) { (void)pin; return ESP_ERR_NOT_ALLOWED; }
esp_err_t solar_os_gpio_claim_pins(const int *pins, const char *const *labels, size_t count, const char *owner, solar_os_resource_conflict_t *conflict)
{
    solar_os_resource_request_t requests[24];
    for (size_t i = 0; i < count; ++i) requests[i] = (solar_os_resource_request_t){SOLAR_OS_RESOURCE_GPIO_PIN, pins[i], -1, labels[i]};
    return solar_os_resource_claim_bundle(requests, count, owner, conflict);
}
esp_err_t solar_os_gpio_configure_owned(int pin, solar_os_gpio_mode_t mode, solar_os_gpio_pull_t pull, const char *owner)
{
    esp_err_t error = gpio_check_owned(pin, owner); if (error) return error;
    gpio_slot_t *slot = find_slot(pin); assert(slot); slot->configured = true; slot->mode = mode; slot->pull = pull; return ESP_OK;
}
esp_err_t solar_os_gpio_read_owned(int pin, const char *owner, bool *level)
{ esp_err_t error = gpio_check_owned(pin, owner); if (error) return error; *level = true; return ESP_OK; }
esp_err_t solar_os_gpio_release_owned(int pin, const char *owner)
{
    if (pin == 8 && fail_reset) return ESP_ERR_TIMEOUT;
    esp_err_t error = gpio_check_owned(pin, owner); if (error) return error;
    ++resets; return solar_os_resource_release(SOLAR_OS_RESOURCE_GPIO_PIN, pin, -1, owner);
}
size_t solar_os_adc_pin_count(void) { return 1; }
bool solar_os_adc_get_pin_info(size_t index, solar_os_adc_pin_info_t *out)
{ if (index) return false; *out = (solar_os_adc_pin_info_t){7, true, true, 2, 9}; return true; }
esp_err_t solar_os_adc_read(int pin, solar_os_adc_sample_t *out)
{ *out = (solar_os_adc_sample_t){pin, 4095, 65535, 2, 9, true}; return ESP_OK; }
static size_t pwm_count(void *user) { (void)user; return 1; }
static bool pwm_get(void *user, size_t index, solar_os_pwm_output_info_t *out)
{ (void)user; if (index) return false; *out = (solar_os_pwm_output_info_t){.output = 65535, .frequency_group = 4, .pin = -1, .channel = -1, .resolution_bits = 16}; return true; }
static esp_err_t pwm_set(void *user, uint16_t output, uint32_t frequency, uint32_t value, bool pulse)
{ (void)user; assert(output == 65535 && frequency == 50 && value == 1500 && pulse); ++pwm_writes; return ESP_OK; }
static esp_err_t pwm_open(void *user, uint16_t output, uint32_t frequency, const char *owner)
{ (void)user; assert(output == 65535 && frequency == 50 && strstr(owner, "/servo")); return ESP_OK; }
static esp_err_t pwm_write(void *user, uint16_t output, const char *owner, uint32_t pulse)
{ (void)user; assert(output == 65535 && strstr(owner, "/servo") && pulse == 1500); ++pwm_writes; return ESP_OK; }
static esp_err_t pwm_disable(void *user, uint16_t output, const char *owner) { (void)user; assert(output == 65535 && owner); return ESP_OK; }
static esp_err_t pwm_close(void *user, uint16_t output, const char *owner)
{ (void)user; assert(output == 65535 && owner); if (fail_pwm_close) return ESP_ERR_TIMEOUT; ++pwm_closes; return ESP_OK; }
static esp_err_t pwm_stop(void *user, uint16_t output) { (void)user; assert(output == 65535); return ESP_OK; }
typedef struct {
    const solar_os_native_pwm_provider_api_v1_t *api;
    solar_os_native_handle_t handle;
    bool close_fails;
    bool invalid_readout;
} pwm_provider_fixture_t;

static bool native_pwm_get(void *user, size_t index, solar_os_native_pwm_info_v1_t *out)
{
    pwm_provider_fixture_t *fixture = user;
    assert(fixture->api->close(fixture->handle) == SOLAR_OS_NATIVE_ERROR_BUSY);
    assert(solar_os_pwm_unregister("portable") == ESP_ERR_INVALID_STATE);
    if (index) return false;
    *out = (solar_os_native_pwm_info_v1_t){
        .provider = "forged", .owner = "forged", .output = 65535, .frequency_group = 4,
        .pin = -1, .channel = -1, .resolution_bits = fixture->invalid_readout ? 256 : 16,
    };
    return true;
}

static int native_pwm_close(void *user, uint16_t output, const char *owner)
{
    pwm_provider_fixture_t *fixture = user;
    assert(output == 65535 && strstr(owner, "/servo"));
    return fixture->close_fails ? SOLAR_OS_NATIVE_ERROR_NOT_FOUND : SOLAR_OS_NATIVE_OK;
}

static void pwm_provider_test(const solar_os_native_pwm_api_v1_t *pwm)
{
    const solar_os_native_pwm_provider_api_v1_t *api =
        solar_os_native_pwm_provider_get_service("pwm.provider", 1, sizeof(*api));
    assert(api);
    pwm_provider_fixture_t fixture = {.api = api};
    char name[] = "portable", driver[] = "test-portable";
    solar_os_native_pwm_provider_callbacks_v1_t descriptor = {
        .name = name, .driver = driver, .library_lease = 10, .user = &fixture,
        .output_count = pwm_count, .get_output = native_pwm_get, .set = pwm_set,
        .open = pwm_open, .write_pulse = pwm_write, .disable = pwm_disable,
        .close = native_pwm_close, .stop = pwm_stop,
    };
    solar_os_native_handle_t duplicate = 1;
    assert(!api->register_provider(&descriptor, &fixture.handle) && library_pins == 1);
    assert(api->register_provider(&descriptor, &duplicate) == SOLAR_OS_NATIVE_ERROR_INVALID_STATE);
    assert(!duplicate && library_pins == 1);
    name[0] = driver[0] = 'x';
    descriptor.get_output = NULL;
    solar_os_native_pwm_provider_v1_t provider;
    assert(!pwm->provider_info(0, &provider) && !strcmp(provider.name, "portable") &&
           !strcmp(provider.driver, "test-portable") && provider.detachable && provider.output_count == 1);
    solar_os_native_pwm_info_v1_t info;
    fixture.invalid_readout = true;
    assert(pwm->info(0, &info) == SOLAR_OS_NATIVE_ERROR_NOT_FOUND);
    fixture.invalid_readout = false;
    assert(!pwm->info(0, &info) && !strcmp(info.provider, "portable") && !info.owner[0] &&
           info.pin == -1 && info.output == 65535 && info.frequency_group == 4 && info.resolution_bits == 16);
    solar_os_native_pwm_output_v1_t output;
    assert(!pwm->parse("portable:65535", &output));
    assert(!pwm->set(&output, 50, 1500, true));
    assert(api->close(fixture.handle) == SOLAR_OS_NATIVE_ERROR_BUSY && library_pins == 1);
    assert(!pwm->stop(&output));
    solar_os_native_handle_t lease;
    assert(!pwm->open(&output, 50, "servo", &lease));
    assert(!pwm->write(lease, 1500) && !pwm->disable(lease));
    assert(!pwm->info(0, &info) && info.reserved && strstr(info.owner, "/servo"));
    fixture.close_fails = true;
    assert(pwm->close(lease) == SOLAR_OS_NATIVE_ERROR_INVALID_STATE);
    assert(api->close(fixture.handle) == SOLAR_OS_NATIVE_ERROR_BUSY && library_pins == 1);
    fixture.close_fails = false;
    assert(!pwm->close(lease));
    assert(!api->close(fixture.handle) && !pwm->provider_count() && !library_pins);

    descriptor.name = "portable";
    descriptor.driver = "test-portable";
    descriptor.get_output = native_pwm_get;
    assert(!api->register_provider(&descriptor, &fixture.handle));
    assert(!solar_os_pwm_unregister("portable"));
    const solar_os_pwm_ops_t ops = {pwm_count, pwm_get, pwm_set, pwm_open, pwm_write, pwm_disable, pwm_close, pwm_stop};
    const solar_os_pwm_registration_t replacement = {"portable", "built-in", true, &ops, NULL};
    assert(!solar_os_pwm_register(&replacement));
    assert(!api->close(fixture.handle) && !library_pins);
    assert(!pwm->provider_info(0, &provider) && !strcmp(provider.driver, "built-in"));
    assert(!solar_os_pwm_unregister("portable"));
}

static bool external_level;
static unsigned acknowledgements;
static esp_err_t fake_line_config(void *ctx, uint8_t line, solar_os_gpio_line_mode_t mode, solar_os_gpio_line_pull_t pull)
{
    assert(ctx == &external_level && line == 3 && mode == SOLAR_OS_GPIO_LINE_MODE_OUTPUT &&
        pull == SOLAR_OS_GPIO_LINE_PULL_DOWN);
    return ESP_OK;
}
static esp_err_t fake_line_read(void *ctx, uint8_t line, bool *level)
{ assert(ctx == &external_level && line == 3); *level = external_level; return ESP_OK; }
static esp_err_t fake_line_write(void *ctx, uint8_t line, bool level)
{ assert(ctx == &external_level && line == 3); external_level = level; return ESP_OK; }
static esp_err_t fake_line_ack(void *ctx)
{ assert(ctx == &external_level); ++acknowledgements; return ESP_OK; }

static int configure_provider(void *ctx, uint8_t line, uint32_t mode, uint32_t pull)
{ return fake_line_config(ctx, line, (solar_os_gpio_line_mode_t)mode, (solar_os_gpio_line_pull_t)pull); }

static void gpio_controller_provider_test(const solar_os_native_gpio_api_v1_t *gpio)
{
    char name[] = "portable";
    solar_os_native_gpio_controller_provider_v1_t descriptor = {.name = name, .line_count = 4,
        .library_lease = 10, .user = &external_level, .configure = configure_provider,
        .read = fake_line_read, .write = fake_line_write, .acknowledge_interrupt = fake_line_ack};
    solar_os_native_handle_t provider, lease, duplicate;
    assert(!gpio->controller_register(&descriptor, &provider) && library_pins == 1);
    assert(gpio->controller_register(&descriptor, &duplicate) == SOLAR_OS_NATIVE_ERROR_INVALID_STATE);
    assert(!duplicate && library_pins == 1);
    descriptor.write = NULL;
    name[0] = 'x';
    solar_os_native_gpio_line_v1_t line;
    assert(gpio->line_parse("portable:3", &line) && !gpio->line_open(&line, "caller", &lease));
    assert(gpio->controller_close(provider) == SOLAR_OS_NATIVE_ERROR_BUSY && library_pins == 1);
    assert(!gpio->line_configure(lease, SOLAR_OS_NATIVE_GPIO_OUTPUT, SOLAR_OS_NATIVE_GPIO_PULL_DOWN));
    assert(!gpio->line_write(lease, true));
    bool level = false;
    assert(!gpio->line_read(lease, &level) && level);
    const unsigned previous = acknowledgements;
    assert(!gpio->line_acknowledge_interrupt(lease) && acknowledgements == previous + 1);
    assert(!gpio->line_close(lease));
    assert(!gpio->controller_close(provider) && !library_pins);
    assert(gpio->controller_close(provider) == SOLAR_OS_NATIVE_ERROR_NOT_FOUND);
    assert(!gpio->controller_find("portable", NULL));

    descriptor.name = "portable";
    descriptor.write = fake_line_write;
    assert(!gpio->controller_register(&descriptor, &provider));
    assert(!solar_os_gpio_controller_unregister("portable"));
    const solar_os_gpio_controller_ops_t replacement_ops = {.configure = fake_line_config,
        .read = fake_line_read, .write = fake_line_write};
    const solar_os_gpio_controller_registration_t replacement = {.name = "portable", .line_count = 4,
        .ops = &replacement_ops, .ctx = &external_level};
    assert(!solar_os_gpio_controller_register(&replacement));
    assert(!gpio->controller_close(provider) && !library_pins);
    assert(gpio->controller_find("portable", NULL));
    assert(!solar_os_gpio_controller_unregister("portable"));
    assert(!allocations && !solar_os_native_handle_count());
}

static void gpio_line_test(const solar_os_native_gpio_api_v1_t *gpio)
{
    const solar_os_gpio_controller_ops_t ops = {.configure = fake_line_config,
        .read = fake_line_read, .write = fake_line_write, .acknowledge_interrupt = fake_line_ack};
    const solar_os_gpio_controller_registration_t external = {.name = "expander", .line_count = 4,
        .ops = &ops, .ctx = &external_level};
    assert(!solar_os_gpio_controller_register(&external));
    solar_os_native_gpio_controller_v1_t info;
    assert(gpio->controller_count() == 1 && gpio->controller_info(0, &info) && info.line_count == 4);
    assert(gpio->controller_find("expander", &info) && !gpio->controller_find("missing", NULL));
    solar_os_native_gpio_line_v1_t line;
    assert(gpio->line_parse("expander:3", &line) && !gpio->line_is_native(&line));
    solar_os_native_handle_t lease, competitor;
    assert(!gpio->line_open(&line, "same", &lease));
    assert(gpio->line_open(&line, "same", &competitor) == SOLAR_OS_NATIVE_ERROR_INVALID_STATE && !competitor);
    assert(solar_os_gpio_controller_unregister("expander") == ESP_ERR_INVALID_STATE);
    assert(!gpio->line_configure(lease, SOLAR_OS_NATIVE_GPIO_OUTPUT, SOLAR_OS_NATIVE_GPIO_PULL_DOWN));
    assert(!gpio->line_write(lease, true) && external_level);
    bool level = false;
    assert(!gpio->line_read(lease, &level) && level);
    assert(!gpio->line_acknowledge_interrupt(lease) && acknowledgements == 1);
    assert(!gpio->line_close(lease) && !solar_os_gpio_controller_unregister("expander"));
    assert(gpio->line_read(lease, &level) == SOLAR_OS_NATIVE_ERROR_NOT_FOUND);
    assert(gpio->line_open(&line, "gone", &lease) == SOLAR_OS_NATIVE_ERROR_NOT_FOUND && !lease);

    assert(gpio->line_parse("gpio8", &line) && gpio->line_is_native(&line));
    assert(!gpio->line_open(&line, "same", &lease));
    assert(!gpio->line_configure(lease, SOLAR_OS_NATIVE_GPIO_OUTPUT, SOLAR_OS_NATIVE_GPIO_PULL_NONE));
    assert(!gpio->line_write(lease, false) && !gpio->line_acknowledge_interrupt(lease));
    fail_reset = true;
    assert(gpio->line_close(lease) == SOLAR_OS_NATIVE_ERROR_TIMEOUT);
    assert(gpio->line_open(&line, "same", &competitor) == SOLAR_OS_NATIVE_ERROR_INVALID_STATE);
    fail_reset = false;
    assert(!gpio->line_close(lease));
    assert(!allocations && !solar_os_native_handle_count());
}

int main(void)
{
    const solar_os_native_resources_api_v1_t *resources = solar_os_native_io_get_service("resources", 1, sizeof(*resources)); assert(resources);
    const solar_os_native_pins_api_v1_t *pins = solar_os_native_io_get_service("pins", 1, sizeof(*pins)); assert(pins);
    assert(!strcmp(resources->kind_name(SOLAR_OS_NATIVE_RESOURCE_SPI_CS), "spi_cs"));
    assert(!strcmp(resources->kind_name(UINT32_MAX), "unknown"));
    assert(!strcmp(pins->policy_name(SOLAR_OS_NATIVE_PIN_FIXED), "fixed"));
    assert(!strcmp(pins->kind_name(SOLAR_OS_NATIVE_PIN_NC), "not connected"));
    const solar_os_native_gpio_api_v1_t *gpio = solar_os_native_io_get_service("gpio", 1, sizeof(*gpio)); assert(gpio);
    assert(!strcmp(gpio->label(SOLAR_OS_NATIVE_GPIO_LABEL_MODE, SOLAR_OS_NATIVE_GPIO_OUTPUT), "output"));
    assert(!strcmp(gpio->label(SOLAR_OS_NATIVE_GPIO_LABEL_PULL, SOLAR_OS_NATIVE_GPIO_PULL_DOWN), "down"));
    assert(!gpio->label(SOLAR_OS_NATIVE_GPIO_LABEL_PULL, UINT32_MAX));
    assert(!gpio->label(UINT32_MAX, SOLAR_OS_NATIVE_GPIO_INPUT));
    uint32_t parsed_gpio;
    assert(gpio->parse(SOLAR_OS_NATIVE_GPIO_LABEL_MODE, "in", &parsed_gpio) && parsed_gpio == SOLAR_OS_NATIVE_GPIO_INPUT);
    assert(gpio->parse(SOLAR_OS_NATIVE_GPIO_LABEL_PULL, "off", &parsed_gpio) && parsed_gpio == SOLAR_OS_NATIVE_GPIO_PULL_NONE);
    assert(!gpio->parse(SOLAR_OS_NATIVE_GPIO_LABEL_MODE, "off", &parsed_gpio));
    assert(parsed_gpio == SOLAR_OS_NATIVE_GPIO_PULL_NONE);
    const solar_os_native_adc_api_v1_t *adc = solar_os_native_io_get_service("adc", 1, sizeof(*adc)); assert(adc);
    const solar_os_native_pwm_api_v1_t *pwm = solar_os_native_io_get_service("pwm", 1, sizeof(*pwm)); assert(pwm);
    solar_os_native_resource_request_v1_t requests[] = {{SOLAR_OS_NATIVE_RESOURCE_GPIO_PIN, 70, -1, "first"}, {SOLAR_OS_NATIVE_RESOURCE_I2C_ADDRESS, 1, 42, "second"}};
    solar_os_native_resource_conflict_v1_t conflict; solar_os_native_handle_t lease, other;
    assert(!resources->claim(requests, 2, "bundle", &conflict, &lease) && resources->count() == 2);
    assert(resources->claim(requests, 2, "bundle", &conflict, &other) == SOLAR_OS_NATIVE_ERROR_INVALID_STATE && !other && conflict.existing.active);
    assert(conflict.index == 0 && !strcmp(conflict.existing.label, "first")); assert(!resources->close(lease));
    solar_os_native_pin_info_v1_t pin; assert(!pins->info(0, &pin) && pin.pin == 7 && pin.policy == SOLAR_OS_NATIVE_PIN_FREE);
    role[0] = '?'; assert(!strcmp(pin.role, "available pin")); role[0] = 'a';
    solar_os_native_connector_pin_v1_t connector; size_t index;
    assert(!pins->connector_find(1, 2, &connector, &index) && index == 0 && connector.kind == SOLAR_OS_NATIVE_PIN_GPIO && !strcmp(connector.label, "SDA"));
    int32_t gpios[] = {7, 8}; const char *labels[] = {"left", "right"};
    assert(!gpio->claim(gpios, labels, 2, "pins", &conflict, &lease));
    assert(gpio->write_owned(lease, 9, true) == SOLAR_OS_NATIVE_ERROR_NOT_FOUND && !writes);
    assert(gpio->write_owned(lease, 7, true) == SOLAR_OS_NATIVE_ERROR_INVALID_STATE && !writes);
    assert(!gpio->configure_owned(lease, 7, SOLAR_OS_NATIVE_GPIO_OUTPUT, SOLAR_OS_NATIVE_GPIO_PULL_NONE));
    assert(!gpio->write_owned(lease, 7, true) && writes == 1 && gpio_slots[0].output_level);
    assert(gpio->write(7, true) == SOLAR_OS_NATIVE_ERROR_INVALID_STATE && writes == 1);
    bool level; assert(gpio->read(7, &level) == SOLAR_OS_NATIVE_ERROR_INVALID_STATE && !reads);
    assert(solar_os_gpio_write_owned(7, "another", true) == ESP_ERR_INVALID_STATE && writes == 1);
    assert(gpio->configure(7, SOLAR_OS_NATIVE_GPIO_OUTPUT, SOLAR_OS_NATIVE_GPIO_PULL_NONE) == SOLAR_OS_NATIVE_ERROR_PERMISSION);
    assert(resources->close(lease) == SOLAR_OS_NATIVE_ERROR_NOT_FOUND);
    fail_reset = true; assert(gpio->close(lease) == SOLAR_OS_NATIVE_ERROR_TIMEOUT && resets == 1);
    assert(gpio->configure_owned(lease, 7, SOLAR_OS_NATIVE_GPIO_OUTPUT, SOLAR_OS_NATIVE_GPIO_PULL_NONE) == SOLAR_OS_NATIVE_ERROR_NOT_FOUND);
    assert(!solar_os_resource_claim(SOLAR_OS_RESOURCE_GPIO_PIN, 7, -1, "replacement", "new"));
    fail_reset = false; assert(!gpio->close(lease) && resets == 2);
    solar_os_resource_claim_t claim; assert(solar_os_resource_find_claim(SOLAR_OS_RESOURCE_GPIO_PIN, 7, -1, &claim) && !strcmp(claim.owner, "replacement"));
    assert(!solar_os_resource_release(SOLAR_OS_RESOURCE_GPIO_PIN, 7, -1, "replacement"));
    solar_os_native_adc_sample_v1_t sample; assert(!adc->read(7, &sample) && sample.voltage_mv == 65535 && sample.calibrated && sample.raw == 4095);
    const solar_os_pwm_ops_t ops = {pwm_count, pwm_get, pwm_set, pwm_open, pwm_write, pwm_disable, pwm_close, pwm_stop};
    const solar_os_pwm_registration_t registration = {"expander", "test", true, &ops, NULL}; assert(!solar_os_pwm_register(&registration));
    solar_os_native_pwm_output_v1_t output; assert(!pwm->parse("expander:65535", &output));
    char formatted[32]; assert(!pwm->format(&output, formatted, sizeof(formatted)) && !strcmp(formatted, "expander:65535"));
    assert(pwm->format(&output, formatted, 2) == SOLAR_OS_NATIVE_ERROR_INVALID_SIZE);
    output.output = 65536; assert(pwm->open(&output, 50, "servo", &lease) == SOLAR_OS_NATIVE_ERROR_INVALID_ARGUMENT && !lease); output.output = 65535;
    assert(!pwm->open(&output, 50, "servo", &lease)); assert(!pwm->write(lease, 1500) && pwm_writes == 1);
    fail_pwm_close = true; assert(pwm->close(lease) == SOLAR_OS_NATIVE_ERROR_TIMEOUT && !pwm_closes);
    assert(pwm->set(&output, 50, 1500, true) == SOLAR_OS_NATIVE_ERROR_INVALID_STATE);
    fail_pwm_close = false; assert(!pwm->close(lease) && pwm_closes == 1);
    gpio_line_test(gpio);
    gpio_controller_provider_test(gpio);
    assert(!solar_os_pwm_unregister("expander"));
    pwm_provider_test(pwm);
    assert(!resources->count() && !allocations && !solar_os_native_handle_count());
    puts("native I/O: real resource/PWM registries, owned GPIO writes and retryable cleanup passed"); return 0;
}
