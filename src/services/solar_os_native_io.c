#include "solar_os_native_io.h"
#include "solar_os_native_io_abi.h"
#include "solar_os_native_handles.h"
#include "solar_os_native_device_providers.h"
#include "solar_os_native_support.h"
#include "solar_os_memory.h"
#include "solar_os_config.h"
#include <stdio.h>
#include <string.h>
#define COPY_FIELD(out, in, field) do { \
    _Static_assert(sizeof((out)->field) == sizeof((in)->field), "native I/O field capacity"); \
    memcpy((out)->field, (in)->field, sizeof((out)->field)); \
} while (0)
#if SOLAR_OS_PACKAGE_SERVICE_RESOURCES || SOLAR_OS_PACKAGE_SERVICE_GPIO || SOLAR_OS_PACKAGE_SERVICE_PWM
static int copy_text(char *out, size_t capacity, const char *in)
{
    if (!in) in = "";
    size_t size = strlen(in) + 1;
    if (size > capacity) return SOLAR_OS_NATIVE_ERROR_INVALID_SIZE;
    memcpy(out, in, size); return SOLAR_OS_NATIVE_OK;
}
#endif
#if SOLAR_OS_PACKAGE_SERVICE_RESOURCES || SOLAR_OS_PACKAGE_SERVICE_GPIO
#include "solar_os_resources.h"
#define RESOURCE_KINDS(X) \
    X(GPIO_PIN) X(GPIO_LINE) X(ADC_PIN) X(PWM_PIN) X(I2C_PORT) X(SPI_HOST) \
    X(I2C_ADDRESS) X(SPI_CS) X(UART_PORT) X(I2S_PORT) X(CAMERA_PORT) X(USB_PORT)
static int resource_kind_in(uint32_t in, solar_os_resource_kind_t *out)
{
    switch (in) {
#define KIND(name) case SOLAR_OS_NATIVE_RESOURCE_##name: *out = SOLAR_OS_RESOURCE_##name; return SOLAR_OS_NATIVE_OK;
    RESOURCE_KINDS(KIND)
#undef KIND
    default: return SOLAR_OS_NATIVE_ERROR_INVALID_ARGUMENT;
    }
}
static int claim_out(const solar_os_resource_claim_t *in, solar_os_native_resource_claim_v1_t *out)
{
    memset(out, 0, sizeof(*out));
    switch (in->kind) {
#define KIND(name) case SOLAR_OS_RESOURCE_##name: out->kind = SOLAR_OS_NATIVE_RESOURCE_##name; break;
    RESOURCE_KINDS(KIND)
#undef KIND
    default: return SOLAR_OS_NATIVE_ERROR_INVALID_STATE;
    }
    out->active = in->active; out->primary = in->primary; out->secondary = in->secondary;
    COPY_FIELD(out, in, owner); COPY_FIELD(out, in, label); return SOLAR_OS_NATIVE_OK;
}
#undef RESOURCE_KINDS
static int conflict_out(const solar_os_resource_conflict_t *in, solar_os_native_resource_conflict_v1_t *out)
{
    if (!out) return SOLAR_OS_NATIVE_OK;
    memset(out, 0, sizeof(*out)); out->index = in->request_index;
    return in->existing.active ? claim_out(&in->existing, &out->existing) : SOLAR_OS_NATIVE_OK;
}
#endif
#if SOLAR_OS_PACKAGE_SERVICE_RESOURCES
#include "solar_os_pins.h"
typedef struct {
    solar_os_resource_request_t requests[SOLAR_OS_RESOURCE_BUNDLE_MAX];
    char labels[SOLAR_OS_RESOURCE_BUNDLE_MAX][SOLAR_OS_RESOURCE_LABEL_MAX], owner[SOLAR_OS_RESOURCE_OWNER_MAX];
    bool released[SOLAR_OS_RESOURCE_BUNDLE_MAX], acquired;
    size_t count;
} native_resources_t;
static int resources_destroy(void *value)
{
    native_resources_t *state = value;
    if (state->acquired) for (size_t i = 0; i < state->count; ++i) {
        if (state->released[i]) continue;
        const solar_os_resource_request_t *request = &state->requests[i];
        esp_err_t error = solar_os_resource_release(request->kind, request->primary, request->secondary, state->owner);
        if (error && error != ESP_ERR_NOT_FOUND) return solar_os_native_result(error);
        state->released[i] = true;
    }
    solar_os_memory_free(state); return SOLAR_OS_NATIVE_OK;
}
static int resources_info(size_t index, solar_os_native_resource_claim_v1_t *out)
{
    if (!out) return SOLAR_OS_NATIVE_ERROR_INVALID_ARGUMENT;
    memset(out, 0, sizeof(*out)); solar_os_resource_claim_t value = {0};
    return solar_os_resource_get_claim(index, &value) ? claim_out(&value, out) : SOLAR_OS_NATIVE_ERROR_NOT_FOUND;
}
static int resources_find(uint32_t kind, int32_t primary, int32_t secondary, solar_os_native_resource_claim_v1_t *out)
{
    if (!out) return SOLAR_OS_NATIVE_ERROR_INVALID_ARGUMENT;
    memset(out, 0, sizeof(*out)); solar_os_resource_kind_t value; int result = resource_kind_in(kind, &value);
    if (result) return result;
    solar_os_resource_claim_t claim = {0};
    return solar_os_resource_find_claim(value, primary, secondary, &claim) ? claim_out(&claim, out) : SOLAR_OS_NATIVE_ERROR_NOT_FOUND;
}
static int resources_claim(const solar_os_native_resource_request_v1_t *in, size_t count, const char *owner,
    solar_os_native_resource_conflict_v1_t *conflict, solar_os_native_handle_t *out)
{
    if (!out) return SOLAR_OS_NATIVE_ERROR_INVALID_ARGUMENT;
    *out = 0; if (conflict) memset(conflict, 0, sizeof(*conflict));
    if (!in || !count || count > SOLAR_OS_RESOURCE_BUNDLE_MAX || !owner || !*owner || strlen(owner) >= 24) return SOLAR_OS_NATIVE_ERROR_INVALID_ARGUMENT;
    native_resources_t *state = solar_os_memory_calloc(1, sizeof(*state), SOLAR_OS_MEMORY_EXTERNAL_PREFERRED, "native.resources");
    if (!state) return SOLAR_OS_NATIVE_ERROR_NO_MEMORY;
    state->count = count; int result = SOLAR_OS_NATIVE_OK;
    for (size_t i = 0; i < count && !result; ++i) {
        result = resource_kind_in(in[i].kind, &state->requests[i].kind);
        if (!result) result = copy_text(state->labels[i], sizeof(state->labels[i]), in[i].label);
        state->requests[i].primary = in[i].primary; state->requests[i].secondary = in[i].secondary; state->requests[i].label = state->labels[i];
    }
    if (!result) result = solar_os_native_handle_create(SOLAR_OS_NATIVE_HANDLE_RESOURCES, state, resources_destroy, out);
    if (result) { resources_destroy(state); return result; }
    snprintf(state->owner, sizeof(state->owner), "%08lx/%.14s", (unsigned long)*out, owner);
    solar_os_resource_conflict_t failure = {0};
    result = solar_os_native_result(solar_os_resource_claim_bundle(state->requests, count, state->owner, &failure));
    if (!result) state->acquired = true;
    int converted = conflict_out(&failure, conflict); if (!result) result = converted;
    if (result && !solar_os_native_handle_close_kind(*out, SOLAR_OS_NATIVE_HANDLE_RESOURCES)) *out = 0;
    return result;
}
static int resources_close(solar_os_native_handle_t id) { return solar_os_native_handle_close_kind(id, SOLAR_OS_NATIVE_HANDLE_RESOURCES); }
static int policy_out(solar_os_pin_policy_t in, uint32_t *out)
{
    switch (in) {
    case SOLAR_OS_PIN_POLICY_FIXED: *out = SOLAR_OS_NATIVE_PIN_FIXED; break;
    case SOLAR_OS_PIN_POLICY_RELEASABLE: *out = SOLAR_OS_NATIVE_PIN_RELEASABLE; break;
    case SOLAR_OS_PIN_POLICY_FREE: *out = SOLAR_OS_NATIVE_PIN_FREE; break;
    default: return SOLAR_OS_NATIVE_ERROR_INVALID_STATE;
    }
    return SOLAR_OS_NATIVE_OK;
}
static int pin_out(const solar_os_pin_info_t *in, solar_os_native_pin_info_v1_t *out)
{
    memset(out, 0, sizeof(*out)); out->pin = in->pin; out->expansion = in->expansion; out->direct_gpio = in->direct_gpio;
    int result = policy_out(in->policy, &out->policy); return result ? result : copy_text(out->role, sizeof(out->role), in->role);
}
static int pins_info(size_t index, solar_os_native_pin_info_v1_t *out)
{
    if (!out) return SOLAR_OS_NATIVE_ERROR_INVALID_ARGUMENT;
    memset(out, 0, sizeof(*out)); solar_os_pin_info_t value = {0};
    return solar_os_pin_get_info(index, &value) ? pin_out(&value, out) : SOLAR_OS_NATIVE_ERROR_NOT_FOUND;
}
static int pins_find(int32_t pin, solar_os_native_pin_info_v1_t *out)
{
    if (!out) return SOLAR_OS_NATIVE_ERROR_INVALID_ARGUMENT;
    memset(out, 0, sizeof(*out)); solar_os_pin_info_t value = {0};
    return solar_os_pin_get_info_by_pin(pin, &value) ? pin_out(&value, out) : SOLAR_OS_NATIVE_ERROR_NOT_FOUND;
}
static bool pins_expansion(int32_t pin) { return solar_os_pin_is_expansion(pin); }
static bool pins_direct(int32_t pin) { return solar_os_pin_is_direct_gpio(pin); }
static bool pins_routable(int32_t pin) { return solar_os_pin_is_routable(pin); }
static int connector_layout(solar_os_native_connector_layout_v1_t *out)
{
    if (!out) return SOLAR_OS_NATIVE_ERROR_INVALID_ARGUMENT;
    memset(out, 0, sizeof(*out)); solar_os_connector_layout_info_t value = {0};
    if (!solar_os_connector_layout_get_info(&value)) return SOLAR_OS_NATIVE_ERROR_NOT_FOUND;
    out->rows = value.rows; out->columns = value.columns;
    int result = copy_text(out->title, sizeof(out->title), value.title); return result ? result : copy_text(out->view, sizeof(out->view), value.view);
}
static int connector_out(const solar_os_connector_pin_info_t *in, solar_os_native_connector_pin_v1_t *out)
{
    memset(out, 0, sizeof(*out)); out->pin = in->pin; out->position = in->position; out->row = in->row; out->column = in->column;
    switch (in->kind) {
    case SOLAR_OS_CONNECTOR_PIN_GPIO: out->kind = SOLAR_OS_NATIVE_PIN_GPIO; break;
    case SOLAR_OS_CONNECTOR_PIN_POWER: out->kind = SOLAR_OS_NATIVE_PIN_POWER; break;
    case SOLAR_OS_CONNECTOR_PIN_GROUND: out->kind = SOLAR_OS_NATIVE_PIN_GROUND; break;
    case SOLAR_OS_CONNECTOR_PIN_CONTROL: out->kind = SOLAR_OS_NATIVE_PIN_CONTROL; break;
    case SOLAR_OS_CONNECTOR_PIN_NC: out->kind = SOLAR_OS_NATIVE_PIN_NC; break;
    default: return SOLAR_OS_NATIVE_ERROR_INVALID_STATE;
    }
    int result = copy_text(out->connector, sizeof(out->connector), in->connector); return result ? result : copy_text(out->label, sizeof(out->label), in->label);
}
static int connector_info(size_t index, solar_os_native_connector_pin_v1_t *out)
{
    if (!out) return SOLAR_OS_NATIVE_ERROR_INVALID_ARGUMENT;
    memset(out, 0, sizeof(*out)); solar_os_connector_pin_info_t value = {0};
    return solar_os_connector_pin_get_info(index, &value) ? connector_out(&value, out) : SOLAR_OS_NATIVE_ERROR_NOT_FOUND;
}
static int connector_find(size_t row, size_t column, solar_os_native_connector_pin_v1_t *out, size_t *index)
{
    if (!out || !index) return SOLAR_OS_NATIVE_ERROR_INVALID_ARGUMENT;
    memset(out, 0, sizeof(*out)); *index = 0; solar_os_connector_pin_info_t value = {0};
    return solar_os_connector_pin_find(row, column, &value, index) ? connector_out(&value, out) : SOLAR_OS_NATIVE_ERROR_NOT_FOUND;
}
static const char *resource_kind_name(uint32_t kind)
{
    solar_os_resource_kind_t native;
    return resource_kind_in(kind, &native) == SOLAR_OS_NATIVE_OK ?
        solar_os_resource_kind_name(native) : "unknown";
}

static const char *pin_policy_name(uint32_t policy)
{
    switch (policy) {
    case SOLAR_OS_NATIVE_PIN_FIXED: return solar_os_pin_policy_name(SOLAR_OS_PIN_POLICY_FIXED);
    case SOLAR_OS_NATIVE_PIN_RELEASABLE: return solar_os_pin_policy_name(SOLAR_OS_PIN_POLICY_RELEASABLE);
    case SOLAR_OS_NATIVE_PIN_FREE: return solar_os_pin_policy_name(SOLAR_OS_PIN_POLICY_FREE);
    default: return "unknown";
    }
}

static const char *connector_kind_name(uint32_t kind)
{
    switch (kind) {
    case SOLAR_OS_NATIVE_PIN_GPIO: return solar_os_connector_pin_kind_name(SOLAR_OS_CONNECTOR_PIN_GPIO);
    case SOLAR_OS_NATIVE_PIN_POWER: return solar_os_connector_pin_kind_name(SOLAR_OS_CONNECTOR_PIN_POWER);
    case SOLAR_OS_NATIVE_PIN_GROUND: return solar_os_connector_pin_kind_name(SOLAR_OS_CONNECTOR_PIN_GROUND);
    case SOLAR_OS_NATIVE_PIN_CONTROL: return solar_os_connector_pin_kind_name(SOLAR_OS_CONNECTOR_PIN_CONTROL);
    case SOLAR_OS_NATIVE_PIN_NC: return solar_os_connector_pin_kind_name(SOLAR_OS_CONNECTOR_PIN_NC);
    default: return "unknown";
    }
}

static const solar_os_native_resources_api_v1_t resources_api = {
    .abi_version = SOLAR_OS_NATIVE_RESOURCES_ABI, .struct_size = sizeof(resources_api), .count = solar_os_resource_claim_count,
    .info = resources_info, .find = resources_find, .claim = resources_claim, .close = resources_close,
    .kind_name = resource_kind_name,
};
static const solar_os_native_pins_api_v1_t pins_api = {
    .abi_version = SOLAR_OS_NATIVE_PINS_ABI, .struct_size = sizeof(pins_api), .count = solar_os_pin_count, .info = pins_info, .find = pins_find,
    .is_expansion = pins_expansion, .is_direct_gpio = pins_direct, .is_routable = pins_routable, .connector_layout = connector_layout,
    .connector_count = solar_os_connector_pin_count, .connector_info = connector_info, .connector_find = connector_find, .connector_exists = solar_os_connector_exists,
    .policy_name = pin_policy_name, .kind_name = connector_kind_name,
};
#endif
#if SOLAR_OS_PACKAGE_SERVICE_GPIO
#include "solar_os_gpio.h"
#if SOLAR_OS_PACKAGE_SERVICE_EXPANSION
#include "solar_os_gpio_controller.h"
#endif
typedef struct { int pins[24]; bool released[24], acquired; size_t count; char owner[24]; } native_gpio_t;
static int gpio_mode_in(uint32_t value, solar_os_gpio_mode_t *out)
{
    if (value == SOLAR_OS_NATIVE_GPIO_INPUT) *out = SOLAR_OS_GPIO_MODE_INPUT;
    else if (value == SOLAR_OS_NATIVE_GPIO_OUTPUT) *out = SOLAR_OS_GPIO_MODE_OUTPUT;
    else return SOLAR_OS_NATIVE_ERROR_INVALID_ARGUMENT;
    return SOLAR_OS_NATIVE_OK;
}
static int gpio_pull_in(uint32_t value, solar_os_gpio_pull_t *out)
{
    switch (value) {
    case SOLAR_OS_NATIVE_GPIO_PULL_NONE: *out = SOLAR_OS_GPIO_PULL_NONE; break;
    case SOLAR_OS_NATIVE_GPIO_PULL_UP: *out = SOLAR_OS_GPIO_PULL_UP; break;
    case SOLAR_OS_NATIVE_GPIO_PULL_DOWN: *out = SOLAR_OS_GPIO_PULL_DOWN; break;
    default: return SOLAR_OS_NATIVE_ERROR_INVALID_ARGUMENT;
    }
    return SOLAR_OS_NATIVE_OK;
}
static uint32_t gpio_mode_out(solar_os_gpio_mode_t value)
{
    switch (value) {
    case SOLAR_OS_GPIO_MODE_INPUT: return SOLAR_OS_NATIVE_GPIO_INPUT;
    case SOLAR_OS_GPIO_MODE_OUTPUT: return SOLAR_OS_NATIVE_GPIO_OUTPUT;
    default: return UINT32_MAX;
    }
}
static uint32_t gpio_pull_out(solar_os_gpio_pull_t value)
{
    switch (value) {
    case SOLAR_OS_GPIO_PULL_NONE: return SOLAR_OS_NATIVE_GPIO_PULL_NONE;
    case SOLAR_OS_GPIO_PULL_UP: return SOLAR_OS_NATIVE_GPIO_PULL_UP;
    case SOLAR_OS_GPIO_PULL_DOWN: return SOLAR_OS_NATIVE_GPIO_PULL_DOWN;
    default: return UINT32_MAX;
    }
}
static const char *gpio_label(uint32_t field, uint32_t value)
{
    if (field == SOLAR_OS_NATIVE_GPIO_LABEL_MODE) {
        solar_os_gpio_mode_t mode;
        return gpio_mode_in(value, &mode) == SOLAR_OS_NATIVE_OK ? solar_os_gpio_mode_name(mode) : NULL;
    }
    if (field == SOLAR_OS_NATIVE_GPIO_LABEL_PULL) {
        solar_os_gpio_pull_t pull;
        return gpio_pull_in(value, &pull) == SOLAR_OS_NATIVE_OK ? solar_os_gpio_pull_name(pull) : NULL;
    }
    return NULL;
}
static bool gpio_parse(uint32_t field, const char *text, uint32_t *out)
{
    if (!out) return false;
    uint32_t value = UINT32_MAX;
    if (field == SOLAR_OS_NATIVE_GPIO_LABEL_MODE) {
        solar_os_gpio_mode_t mode;
        if (solar_os_gpio_parse_mode(text, &mode)) value = gpio_mode_out(mode);
    } else if (field == SOLAR_OS_NATIVE_GPIO_LABEL_PULL) {
        solar_os_gpio_pull_t pull;
        if (solar_os_gpio_parse_pull(text, &pull)) value = gpio_pull_out(pull);
    }
    if (value == UINT32_MAX) return false;
    *out = value;
    return true;
}
static int gpio_out(const solar_os_gpio_pin_info_t *in, solar_os_native_gpio_info_v1_t *out)
{
    memset(out, 0, sizeof(*out)); out->pin = in->pin; out->expansion = in->expansion; out->runtime_allowed = in->runtime_allowed;
    out->available = in->available; out->claimed = in->claimed; out->configured = in->configured; out->level = in->level; out->level_valid = in->level_valid;
    out->mode = gpio_mode_out(in->mode);
    out->pull = gpio_pull_out(in->pull);
    if (out->mode == UINT32_MAX || out->pull == UINT32_MAX) return SOLAR_OS_NATIVE_ERROR_INVALID_STATE;
    COPY_FIELD(out, in, owner); int result = policy_out(in->policy, &out->policy); return result ? result : copy_text(out->role, sizeof(out->role), in->role);
}
static int gpio_info(size_t index, solar_os_native_gpio_info_v1_t *out)
{
    if (!out) return SOLAR_OS_NATIVE_ERROR_INVALID_ARGUMENT;
    memset(out, 0, sizeof(*out)); solar_os_gpio_pin_info_t value = {0};
    return solar_os_gpio_get_pin_info(index, &value) ? gpio_out(&value, out) : SOLAR_OS_NATIVE_ERROR_NOT_FOUND;
}
static int gpio_find(int32_t pin, solar_os_native_gpio_info_v1_t *out)
{
    if (!out) return SOLAR_OS_NATIVE_ERROR_INVALID_ARGUMENT;
    memset(out, 0, sizeof(*out)); solar_os_gpio_pin_info_t value = {0};
    return solar_os_gpio_get_pin_info_by_pin(pin, &value) ? gpio_out(&value, out) : SOLAR_OS_NATIVE_ERROR_NOT_FOUND;
}
static bool gpio_allowed(int32_t pin) { return solar_os_gpio_is_runtime_allowed(pin); }
static int gpio_configure(int32_t pin, uint32_t mode, uint32_t pull)
{
    solar_os_gpio_mode_t m; solar_os_gpio_pull_t p; int result = gpio_mode_in(mode, &m); if (!result) result = gpio_pull_in(pull, &p);
    return result ? result : solar_os_native_result(solar_os_gpio_configure(pin, m, p));
}
static int gpio_read(int32_t pin, bool *level) { return solar_os_native_result(solar_os_gpio_read(pin, level)); }
static int gpio_write(int32_t pin, bool level) { return solar_os_native_result(solar_os_gpio_write(pin, level)); }
static int gpio_release(int32_t pin) { return solar_os_native_result(solar_os_gpio_release(pin)); }
static int gpio_destroy(void *value)
{
    native_gpio_t *state = value;
    if (state->acquired) for (size_t i = 0; i < state->count; ++i) {
        if (state->released[i]) continue;
        esp_err_t error = solar_os_gpio_release_owned(state->pins[i], state->owner);
        if (error && error != ESP_ERR_NOT_FOUND) return solar_os_native_result(error);
        state->released[i] = true;
    }
    solar_os_memory_free(state); return SOLAR_OS_NATIVE_OK;
}
static int gpio_claim(const int32_t *pins, const char *const *labels, size_t count, const char *owner,
    solar_os_native_resource_conflict_v1_t *conflict, solar_os_native_handle_t *out)
{
    if (!out) return SOLAR_OS_NATIVE_ERROR_INVALID_ARGUMENT;
    *out = 0; if (conflict) memset(conflict, 0, sizeof(*conflict));
    if (!pins || !labels || !count || count > 24 || !owner || !*owner || strlen(owner) >= 24) return SOLAR_OS_NATIVE_ERROR_INVALID_ARGUMENT;
    native_gpio_t *state = solar_os_memory_calloc(1, sizeof(*state), SOLAR_OS_MEMORY_EXTERNAL_PREFERRED, "native.gpio");
    if (!state) return SOLAR_OS_NATIVE_ERROR_NO_MEMORY;
    state->count = count; for (size_t i = 0; i < count; ++i) state->pins[i] = pins[i];
    int result = solar_os_native_handle_create(SOLAR_OS_NATIVE_HANDLE_GPIO, state, gpio_destroy, out);
    if (result) { gpio_destroy(state); return result; }
    snprintf(state->owner, sizeof(state->owner), "%08lx/%.14s", (unsigned long)*out, owner);
    solar_os_resource_conflict_t failure = {0};
    result = solar_os_native_result(solar_os_gpio_claim_pins(state->pins, labels, count, state->owner, &failure));
    if (!result) state->acquired = true;
    int converted = conflict_out(&failure, conflict); if (!result) result = converted;
    if (result && !solar_os_native_handle_close_kind(*out, SOLAR_OS_NATIVE_HANDLE_GPIO)) *out = 0;
    return result;
}
static native_gpio_t *gpio_acquire_pin(solar_os_native_handle_t id, int pin)
{
    native_gpio_t *state = solar_os_native_handle_acquire(id, SOLAR_OS_NATIVE_HANDLE_GPIO); if (!state) return NULL;
    for (size_t i = 0; i < state->count; ++i) if (state->pins[i] == pin && !state->released[i]) return state;
    solar_os_native_handle_release(id); return NULL;
}
static int gpio_configure_owned(solar_os_native_handle_t id, int32_t pin, uint32_t mode, uint32_t pull)
{
    solar_os_gpio_mode_t m; solar_os_gpio_pull_t p; int result = gpio_mode_in(mode, &m); if (!result) result = gpio_pull_in(pull, &p); if (result) return result;
    native_gpio_t *state = gpio_acquire_pin(id, pin); if (!state) return SOLAR_OS_NATIVE_ERROR_NOT_FOUND;
    result = solar_os_native_result(solar_os_gpio_configure_owned(pin, m, p, state->owner)); solar_os_native_handle_release(id); return result;
}
static int gpio_read_owned(solar_os_native_handle_t id, int32_t pin, bool *level)
{
    native_gpio_t *state = gpio_acquire_pin(id, pin); if (!state) return SOLAR_OS_NATIVE_ERROR_NOT_FOUND;
    int result = solar_os_native_result(solar_os_gpio_read_owned(pin, state->owner, level)); solar_os_native_handle_release(id); return result;
}
static int gpio_write_owned(solar_os_native_handle_t id, int32_t pin, bool level)
{
    native_gpio_t *state = gpio_acquire_pin(id, pin); if (!state) return SOLAR_OS_NATIVE_ERROR_NOT_FOUND;
    int result = solar_os_native_result(solar_os_gpio_write_owned(pin, state->owner, level)); solar_os_native_handle_release(id); return result;
}
static int gpio_close(solar_os_native_handle_t id) { return solar_os_native_handle_close_kind(id, SOLAR_OS_NATIVE_HANDLE_GPIO); }
#if SOLAR_OS_PACKAGE_SERVICE_EXPANSION
typedef struct {
    solar_os_gpio_line_ref_t line;
    uint8_t controller_id;
    bool acquired;
    char owner[SOLAR_OS_GPIO_OWNER_MAX];
} native_gpio_line_t;

static int gpio_line_destroy(void *opaque)
{
    native_gpio_line_t *state = opaque;
    if (state->acquired) {
        esp_err_t error = state->line.controller[0] ? solar_os_resource_release(SOLAR_OS_RESOURCE_GPIO_LINE,
            state->controller_id, state->line.line, state->owner) :
            solar_os_gpio_release_owned(state->line.line, state->owner);
        if (error != ESP_OK && error != ESP_ERR_NOT_FOUND) return solar_os_native_result(error);
    }
    solar_os_memory_free(state);
    return SOLAR_OS_NATIVE_OK;
}

static int gpio_line_open(const solar_os_native_gpio_line_v1_t *line, const char *owner,
    solar_os_native_handle_t *id)
{
    if (!id) return SOLAR_OS_NATIVE_ERROR_INVALID_ARGUMENT;
    *id = 0;
    if (!line || !owner || !*owner || strlen(owner) >= SOLAR_OS_GPIO_OWNER_MAX ||
        strnlen(line->controller, sizeof(line->controller)) == sizeof(line->controller))
        return SOLAR_OS_NATIVE_ERROR_INVALID_ARGUMENT;

    native_gpio_line_t *state = solar_os_memory_calloc(1, sizeof(*state),
        SOLAR_OS_MEMORY_EXTERNAL_PREFERRED, "native.gpio.line");
    if (!state) return SOLAR_OS_NATIVE_ERROR_NO_MEMORY;
    state->line = *line;
    int result = solar_os_native_handle_create(SOLAR_OS_NATIVE_HANDLE_GPIO_LINE, state, gpio_line_destroy, id);
    if (result) {
        solar_os_memory_free(state);
        return result;
    }
    snprintf(state->owner, sizeof(state->owner), "%08lx/%.14s", (unsigned long)*id, owner);

    esp_err_t error;
    if (line->controller[0]) {
        error = solar_os_gpio_line_claim_controller(line, state->owner, &state->controller_id);
    } else {
        int pin = line->line;
        const char *label = "gpio line";
        error = solar_os_gpio_claim_pins(&pin, &label, 1, state->owner, NULL);
    }
    if (error == ESP_OK) state->acquired = true;
    else {
        (void)solar_os_native_handle_close_kind(*id, SOLAR_OS_NATIVE_HANDLE_GPIO_LINE);
        *id = 0;
    }
    return solar_os_native_result(error);
}

static int gpio_line_configure(solar_os_native_handle_t id, uint32_t mode, uint32_t pull)
{
    solar_os_gpio_mode_t m;
    solar_os_gpio_pull_t p;
    int result = gpio_mode_in(mode, &m);
    if (!result) result = gpio_pull_in(pull, &p);
    if (result) return result;

    native_gpio_line_t *state = solar_os_native_handle_acquire(id, SOLAR_OS_NATIVE_HANDLE_GPIO_LINE);
    if (!state) return SOLAR_OS_NATIVE_ERROR_NOT_FOUND;
    _Static_assert((int)SOLAR_OS_GPIO_MODE_INPUT == (int)SOLAR_OS_GPIO_LINE_MODE_INPUT &&
        (int)SOLAR_OS_GPIO_MODE_OUTPUT == (int)SOLAR_OS_GPIO_LINE_MODE_OUTPUT, "GPIO line mode mapping");
    _Static_assert((int)SOLAR_OS_GPIO_PULL_NONE == (int)SOLAR_OS_GPIO_LINE_PULL_NONE &&
        (int)SOLAR_OS_GPIO_PULL_UP == (int)SOLAR_OS_GPIO_LINE_PULL_UP &&
        (int)SOLAR_OS_GPIO_PULL_DOWN == (int)SOLAR_OS_GPIO_LINE_PULL_DOWN, "GPIO line pull mapping");
    esp_err_t error = state->line.controller[0] ?
        solar_os_gpio_line_configure(&state->line, (solar_os_gpio_line_mode_t)m, (solar_os_gpio_line_pull_t)p) :
        solar_os_gpio_configure_owned(state->line.line, m, p, state->owner);
    solar_os_native_handle_release(id);
    return solar_os_native_result(error);
}

static int gpio_line_read(solar_os_native_handle_t id, bool *level)
{
    if (!level) return SOLAR_OS_NATIVE_ERROR_INVALID_ARGUMENT;
    native_gpio_line_t *state = solar_os_native_handle_acquire(id, SOLAR_OS_NATIVE_HANDLE_GPIO_LINE);
    if (!state) return SOLAR_OS_NATIVE_ERROR_NOT_FOUND;
    esp_err_t error = state->line.controller[0] ? solar_os_gpio_line_read(&state->line, level) :
        solar_os_gpio_read_owned(state->line.line, state->owner, level);
    solar_os_native_handle_release(id);
    return solar_os_native_result(error);
}

static int gpio_line_write(solar_os_native_handle_t id, bool level)
{
    native_gpio_line_t *state = solar_os_native_handle_acquire(id, SOLAR_OS_NATIVE_HANDLE_GPIO_LINE);
    if (!state) return SOLAR_OS_NATIVE_ERROR_NOT_FOUND;
    esp_err_t error = state->line.controller[0] ? solar_os_gpio_line_write(&state->line, level) :
        solar_os_gpio_write_owned(state->line.line, state->owner, level);
    solar_os_native_handle_release(id);
    return solar_os_native_result(error);
}

static int gpio_line_acknowledge(solar_os_native_handle_t id)
{
    native_gpio_line_t *state = solar_os_native_handle_acquire(id, SOLAR_OS_NATIVE_HANDLE_GPIO_LINE);
    if (!state) return SOLAR_OS_NATIVE_ERROR_NOT_FOUND;
    esp_err_t error = solar_os_gpio_line_acknowledge_interrupt(&state->line);
    solar_os_native_handle_release(id);
    return solar_os_native_result(error);
}

static int gpio_line_close(solar_os_native_handle_t id)
{ return solar_os_native_handle_close_kind(id, SOLAR_OS_NATIVE_HANDLE_GPIO_LINE); }
#endif

static const solar_os_native_gpio_api_v1_t gpio_api = {
    .abi_version = SOLAR_OS_NATIVE_GPIO_ABI, .struct_size = sizeof(gpio_api), .count = solar_os_gpio_pin_count, .info = gpio_info, .find = gpio_find,
    .runtime_allowed = gpio_allowed, .configure = gpio_configure, .read = gpio_read, .write = gpio_write, .release = gpio_release,
    .claim = gpio_claim, .configure_owned = gpio_configure_owned, .read_owned = gpio_read_owned, .write_owned = gpio_write_owned, .close = gpio_close,
    .label = gpio_label, .parse = gpio_parse,
#if SOLAR_OS_PACKAGE_SERVICE_EXPANSION
    .line_parse = solar_os_gpio_line_parse,
    .line_is_native = solar_os_gpio_line_is_native,
    .controller_count = solar_os_gpio_controller_count,
    .controller_info = solar_os_gpio_controller_get,
    .controller_find = solar_os_gpio_controller_find,
    .line_open = gpio_line_open,
    .line_configure = gpio_line_configure,
    .line_read = gpio_line_read,
    .line_write = gpio_line_write,
    .line_acknowledge_interrupt = gpio_line_acknowledge,
    .line_close = gpio_line_close,
    .controller_register = solar_os_native_gpio_controller_register,
    .controller_close = solar_os_native_gpio_controller_close,
#endif
};
#endif
#if SOLAR_OS_PACKAGE_SERVICE_ADC
#include "solar_os_adc.h"
static int adc_info(size_t index, solar_os_native_adc_info_v1_t *out)
{
    if (!out) return SOLAR_OS_NATIVE_ERROR_INVALID_ARGUMENT;
    memset(out, 0, sizeof(*out)); solar_os_adc_pin_info_t value = {0}; if (!solar_os_adc_get_pin_info(index, &value)) return SOLAR_OS_NATIVE_ERROR_NOT_FOUND;
    *out = (solar_os_native_adc_info_v1_t){value.pin, value.unit, value.channel, value.runtime_allowed, value.adc_capable}; return SOLAR_OS_NATIVE_OK;
}
static int adc_read(int32_t pin, solar_os_native_adc_sample_v1_t *out)
{
    if (!out) return SOLAR_OS_NATIVE_ERROR_INVALID_ARGUMENT;
    memset(out, 0, sizeof(*out)); solar_os_adc_sample_t value = {0}; int result = solar_os_native_result(solar_os_adc_read(pin, &value));
    if (!result) *out = (solar_os_native_adc_sample_v1_t){value.pin, value.raw, value.unit, value.channel, value.voltage_mv, value.calibrated};
    return result;
}
static const solar_os_native_adc_api_v1_t adc_api = {.abi_version = SOLAR_OS_NATIVE_ADC_ABI, .struct_size = sizeof(adc_api),
    .count = solar_os_adc_pin_count, .info = adc_info, .read = adc_read};
#endif
#if SOLAR_OS_PACKAGE_SERVICE_PWM
#include "solar_os_pwm.h"
typedef struct { solar_os_pwm_output_t output; char owner[24]; bool acquired; } native_pwm_t;
static int pwm_output_in(const solar_os_native_pwm_output_v1_t *in, solar_os_pwm_output_t *out)
{
    if (!in || !memchr(in->provider, 0, sizeof(in->provider)) || in->output > UINT16_MAX) return SOLAR_OS_NATIVE_ERROR_INVALID_ARGUMENT;
    COPY_FIELD(out, in, provider); out->output = in->output; return SOLAR_OS_NATIVE_OK;
}
static int pwm_provider_info(size_t index, solar_os_native_pwm_provider_v1_t *out)
{
    if (!out) return SOLAR_OS_NATIVE_ERROR_INVALID_ARGUMENT;
    memset(out, 0, sizeof(*out)); solar_os_pwm_provider_info_t value = {0}; if (!solar_os_pwm_get_provider(index, &value)) return SOLAR_OS_NATIVE_ERROR_NOT_FOUND;
    COPY_FIELD(out, &value, name); COPY_FIELD(out, &value, driver); out->output_count = value.output_count; out->detachable = value.detachable; return SOLAR_OS_NATIVE_OK;
}
static int pwm_info(size_t index, solar_os_native_pwm_info_v1_t *out)
{
    if (!out) return SOLAR_OS_NATIVE_ERROR_INVALID_ARGUMENT;
    memset(out, 0, sizeof(*out)); solar_os_pwm_output_info_t value = {0}; if (!solar_os_pwm_get_output_info(index, &value)) return SOLAR_OS_NATIVE_ERROR_NOT_FOUND;
    COPY_FIELD(out, &value, provider); COPY_FIELD(out, &value, owner); out->frequency_hz = value.freq_hz;
#define SCALAR(field) out->field = value.field
    SCALAR(output); SCALAR(frequency_group); SCALAR(runtime_allowed); SCALAR(active); SCALAR(duty_percent); SCALAR(enabled);
    SCALAR(reserved); SCALAR(resolution_bits); SCALAR(pulse_width_us); SCALAR(pin); SCALAR(channel);
#undef SCALAR
    return SOLAR_OS_NATIVE_OK;
}
static int pwm_parse(const char *text, solar_os_native_pwm_output_v1_t *out)
{
    if (!out) return SOLAR_OS_NATIVE_ERROR_INVALID_ARGUMENT;
    memset(out, 0, sizeof(*out)); solar_os_pwm_output_t value = {0}; if (!solar_os_pwm_parse_output(text, &value)) return SOLAR_OS_NATIVE_ERROR_INVALID_ARGUMENT;
    COPY_FIELD(out, &value, provider); out->output = value.output; return SOLAR_OS_NATIVE_OK;
}
static int pwm_format(const solar_os_native_pwm_output_v1_t *in, char *text, size_t capacity)
{
    if (!text || !capacity) return SOLAR_OS_NATIVE_ERROR_INVALID_ARGUMENT;
    *text = 0; solar_os_pwm_output_t value = {0}; int result = pwm_output_in(in, &value); if (result) return result;
    char buffer[32]; solar_os_pwm_format_output(&value, buffer, sizeof(buffer)); return copy_text(text, capacity, buffer);
}
static int pwm_set(const solar_os_native_pwm_output_v1_t *in, uint32_t frequency, uint32_t value, bool pulse)
{ solar_os_pwm_output_t output = {0}; int result = pwm_output_in(in, &output); return result ? result : solar_os_native_result(solar_os_pwm_set_output(&output, frequency, value, pulse)); }
static int pwm_stop(const solar_os_native_pwm_output_v1_t *in)
{ solar_os_pwm_output_t output = {0}; int result = pwm_output_in(in, &output); return result ? result : solar_os_native_result(solar_os_pwm_stop_output(&output)); }
static int pwm_destroy(void *value)
{
    native_pwm_t *state = value;
    if (state->acquired) { esp_err_t error = solar_os_pwm_close_output(&state->output, state->owner); if (error && error != ESP_ERR_NOT_FOUND) return solar_os_native_result(error); }
    solar_os_memory_free(state); return SOLAR_OS_NATIVE_OK;
}
static int pwm_open(const solar_os_native_pwm_output_v1_t *in, uint32_t frequency, const char *owner, solar_os_native_handle_t *out)
{
    if (!out) return SOLAR_OS_NATIVE_ERROR_INVALID_ARGUMENT;
    *out = 0; if (!owner || !*owner || strlen(owner) >= 24) return SOLAR_OS_NATIVE_ERROR_INVALID_ARGUMENT;
    solar_os_pwm_output_t output = {0}; int result = pwm_output_in(in, &output); if (result) return result;
    native_pwm_t *state = solar_os_memory_calloc(1, sizeof(*state), SOLAR_OS_MEMORY_EXTERNAL_PREFERRED, "native.pwm"); if (!state) return SOLAR_OS_NATIVE_ERROR_NO_MEMORY;
    state->output = output; result = solar_os_native_handle_create(SOLAR_OS_NATIVE_HANDLE_PWM, state, pwm_destroy, out);
    if (result) { pwm_destroy(state); return result; }
    snprintf(state->owner, sizeof(state->owner), "%08lx/%.14s", (unsigned long)*out, owner);
    result = solar_os_native_result(solar_os_pwm_open_output(&state->output, frequency, state->owner)); if (!result) state->acquired = true;
    else if (!solar_os_native_handle_close_kind(*out, SOLAR_OS_NATIVE_HANDLE_PWM)) *out = 0;
    return result;
}
static int pwm_write(solar_os_native_handle_t id, uint32_t pulse)
{
    native_pwm_t *state = solar_os_native_handle_acquire(id, SOLAR_OS_NATIVE_HANDLE_PWM); if (!state) return SOLAR_OS_NATIVE_ERROR_NOT_FOUND;
    int result = solar_os_native_result(solar_os_pwm_write_output(&state->output, state->owner, pulse)); solar_os_native_handle_release(id); return result;
}
static int pwm_disable(solar_os_native_handle_t id)
{
    native_pwm_t *state = solar_os_native_handle_acquire(id, SOLAR_OS_NATIVE_HANDLE_PWM); if (!state) return SOLAR_OS_NATIVE_ERROR_NOT_FOUND;
    int result = solar_os_native_result(solar_os_pwm_disable_output(&state->output, state->owner)); solar_os_native_handle_release(id); return result;
}
static int pwm_close(solar_os_native_handle_t id) { return solar_os_native_handle_close_kind(id, SOLAR_OS_NATIVE_HANDLE_PWM); }
static const solar_os_native_pwm_api_v1_t pwm_api = {
    .abi_version = SOLAR_OS_NATIVE_PWM_ABI, .struct_size = sizeof(pwm_api), .provider_count = solar_os_pwm_provider_count, .provider_info = pwm_provider_info,
    .count = solar_os_pwm_output_count, .info = pwm_info, .parse = pwm_parse, .format = pwm_format, .set = pwm_set, .stop = pwm_stop,
    .open = pwm_open, .write = pwm_write, .disable = pwm_disable, .close = pwm_close,
};
#endif
const void *solar_os_native_io_get_service(const char *name, uint32_t abi, uint32_t minimum)
{
#include "generated/native_io_dispatch.inc"
    (void)name; (void)abi; (void)minimum; return NULL;
}
