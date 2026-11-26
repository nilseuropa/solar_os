#include "solar_os_pwm.h"

#include <stdio.h>
#include <string.h>

#include "esp_attr.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "solar_os_config.h"
#if SOLAR_OS_PACKAGE_DRIVER_PWM_NATIVE
#include "solar_os_pwm_native.h"
#endif

typedef struct {
    bool active;
    solar_os_pwm_provider_info_t info;
    solar_os_pwm_ops_t ops;
    void *ctx;
    unsigned refs;
    unsigned bindings;
    SemaphoreHandle_t mutex;
} pwm_provider_t;

typedef struct {
    bool used;
    unsigned provider;
    uint16_t output;
    bool reserved;
    char owner[SOLAR_OS_PWM_OWNER_MAX];
} pwm_binding_t;

/* Provider metadata is used only from tasks; FreeRTOS mutex storage stays
 * internal so its scheduler operations never depend on the PSRAM cache. */
static EXT_RAM_BSS_ATTR pwm_provider_t providers[SOLAR_OS_PWM_PROVIDER_MAX];
static EXT_RAM_BSS_ATTR pwm_binding_t bindings[SOLAR_OS_PWM_OUTPUT_MAX];
static StaticSemaphore_t provider_mutex_storage[SOLAR_OS_PWM_PROVIDER_MAX];
static StaticSemaphore_t registry_storage, init_storage;
static SemaphoreHandle_t registry_mutex, init_mutex;
static portMUX_TYPE init_lock = portMUX_INITIALIZER_UNLOCKED;

static void ensure_mutex(void)
{
    portENTER_CRITICAL(&init_lock);
    if (registry_mutex == NULL) {
        registry_mutex = xSemaphoreCreateMutexStatic(&registry_storage);
        init_mutex = xSemaphoreCreateMutexStatic(&init_storage);
        for (size_t i = 0; i < SOLAR_OS_PWM_PROVIDER_MAX; i++) {
            providers[i].mutex = xSemaphoreCreateMutexStatic(&provider_mutex_storage[i]);
        }
    }
    portEXIT_CRITICAL(&init_lock);
}

static bool valid_name(const char *name)
{
    if (name == NULL || name[0] == '\0' || strnlen(name, SOLAR_OS_PWM_NAME_MAX) >= SOLAR_OS_PWM_NAME_MAX) return false;
    for (const char *p = name; *p; p++) {
        if (!((*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z') ||
              (*p >= '0' && *p <= '9') || *p == '_' || *p == '-')) return false;
    }
    return true;
}

esp_err_t solar_os_pwm_init(void)
{
    ensure_mutex();
    esp_err_t ret = ESP_OK;
#if SOLAR_OS_PACKAGE_DRIVER_PWM_NATIVE
    static bool native_registered;
    xSemaphoreTake(init_mutex, portMAX_DELAY);
    if (!native_registered) {
        ret = solar_os_pwm_native_register();
        native_registered = ret == ESP_OK;
    }
    xSemaphoreGive(init_mutex);
#endif
    return ret;
}

static pwm_provider_t *find_provider_locked(const char *name)
{
    if (name == NULL) return NULL;
    for (size_t i = 0; i < SOLAR_OS_PWM_PROVIDER_MAX; i++) {
        if (providers[i].active && strcmp(providers[i].info.name, name) == 0) return &providers[i];
    }
    return NULL;
}

static esp_err_t register_provider(const solar_os_pwm_registration_t *r)
{
    if (r == NULL || !valid_name(r->name) || r->driver == NULL ||
        r->driver[0] == '\0' || strnlen(r->driver, SOLAR_OS_PWM_DRIVER_MAX) >= SOLAR_OS_PWM_DRIVER_MAX ||
        r->ops == NULL || r->ops->output_count == NULL || r->ops->get_output == NULL ||
        r->ops->set == NULL || r->ops->open == NULL || r->ops->write_pulse == NULL ||
        r->ops->disable == NULL || r->ops->close == NULL || r->ops->stop == NULL) return ESP_ERR_INVALID_ARG;
    ensure_mutex();
    xSemaphoreTake(registry_mutex, portMAX_DELAY);
    esp_err_t ret = ESP_ERR_NO_MEM;
    if (find_provider_locked(r->name) != NULL) ret = ESP_ERR_INVALID_STATE;
    else {
        for (size_t i = 0; i < SOLAR_OS_PWM_PROVIDER_MAX; i++) {
            pwm_provider_t *p = &providers[i];
            if (p->active) continue;
            p->info = (solar_os_pwm_provider_info_t){.detachable = r->detachable};
            memcpy(p->info.name, r->name, strlen(r->name) + 1U);
            memcpy(p->info.driver, r->driver, strlen(r->driver) + 1U);
            p->ops = *r->ops;
            p->ctx = r->ctx;
            p->refs = p->bindings = 0U;
            p->active = true;
            ret = ESP_OK;
            break;
        }
    }
    xSemaphoreGive(registry_mutex);
    return ret;
}

/* Used only by the native adapter during initialization; public attachment
 * initializes built-ins first so expansion providers cannot exhaust its slot. */
#if SOLAR_OS_PACKAGE_DRIVER_PWM_NATIVE
esp_err_t solar_os_pwm_register_native(const solar_os_pwm_registration_t *r)
{
    return register_provider(r);
}
#endif

esp_err_t solar_os_pwm_register(const solar_os_pwm_registration_t *r)
{
    const esp_err_t ret = solar_os_pwm_init();
    return ret == ESP_OK ? register_provider(r) : ret;
}

static esp_err_t unregister_provider(const char *name, const void *ctx)
{
    if (!valid_name(name)) return ESP_ERR_INVALID_ARG;
    ensure_mutex();
    xSemaphoreTake(registry_mutex, portMAX_DELAY);
    pwm_provider_t *p = find_provider_locked(name);
    esp_err_t ret = ESP_ERR_NOT_FOUND;
    if (p != NULL && (ctx == NULL || p->ctx == ctx)) {
        ret = !p->info.detachable ? ESP_ERR_NOT_ALLOWED :
            (p->refs || p->bindings) ? ESP_ERR_INVALID_STATE : ESP_OK;
        if (ret == ESP_OK) p->active = false;
    }
    xSemaphoreGive(registry_mutex);
    return ret;
}

esp_err_t solar_os_pwm_unregister(const char *name)
{
    return unregister_provider(name, NULL);
}

esp_err_t solar_os_pwm_unregister_owned(const char *name, const void *ctx)
{
    return ctx != NULL ? unregister_provider(name, ctx) : ESP_ERR_INVALID_ARG;
}

/* A reference protects driver context before waiting for its operation mutex. */
static pwm_provider_t *begin(const char *name)
{
    if (solar_os_pwm_init() != ESP_OK) return NULL;
    xSemaphoreTake(registry_mutex, portMAX_DELAY);
    pwm_provider_t *p = find_provider_locked(name);
    if (p != NULL) p->refs++;
    xSemaphoreGive(registry_mutex);
    if (p != NULL) xSemaphoreTake(p->mutex, portMAX_DELAY);
    return p;
}

static void end(pwm_provider_t *p)
{
    xSemaphoreGive(p->mutex);
    xSemaphoreTake(registry_mutex, portMAX_DELAY);
    p->refs--;
    xSemaphoreGive(registry_mutex);
}

static pwm_provider_t *begin_index(size_t index)
{
    if (solar_os_pwm_init() != ESP_OK) return NULL;
    xSemaphoreTake(registry_mutex, portMAX_DELAY);
    pwm_provider_t *p = NULL;
    for (size_t i = 0; i < SOLAR_OS_PWM_PROVIDER_MAX; i++) {
        if (providers[i].active && index-- == 0U) { p = &providers[i]; p->refs++; break; }
    }
    xSemaphoreGive(registry_mutex);
    if (p != NULL) xSemaphoreTake(p->mutex, portMAX_DELAY);
    return p;
}

size_t solar_os_pwm_provider_count(void)
{
    if (solar_os_pwm_init() != ESP_OK) return 0;
    xSemaphoreTake(registry_mutex, portMAX_DELAY);
    size_t count = 0;
    for (size_t i = 0; i < SOLAR_OS_PWM_PROVIDER_MAX; i++) count += providers[i].active;
    xSemaphoreGive(registry_mutex);
    return count;
}

bool solar_os_pwm_get_provider(size_t index, solar_os_pwm_provider_info_t *info)
{
    if (info == NULL) return false;
    pwm_provider_t *p = begin_index(index);
    if (p == NULL) return false;
    *info = p->info;
    info->output_count = p->ops.output_count(p->ctx);
    end(p);
    return true;
}

bool solar_os_pwm_parse_output(const char *text, solar_os_pwm_output_t *output)
{
    if (text == NULL || output == NULL) return false;
    const char *colon = strchr(text, ':');
    solar_os_pwm_output_t result = {.provider = "native"};
    const char *digits = text;
    if (colon != NULL) {
        const size_t len = (size_t)(colon - text);
        if (len == 0U || len >= sizeof(result.provider)) return false;
        memcpy(result.provider, text, len);
        result.provider[len] = '\0';
        if (!valid_name(result.provider)) return false;
        digits = colon + 1;
    }
    if (*digits == '\0') return false;
    uint32_t value = 0;
    for (const char *p = digits; *p; p++) {
        if (*p < '0' || *p > '9') return false;
        value = value * 10U + (uint32_t)(*p - '0');
        if (value > UINT16_MAX) return false;
    }
    result.output = (uint16_t)value;
    *output = result;
    return true;
}

void solar_os_pwm_format_output(const solar_os_pwm_output_t *output, char *text, size_t size)
{
    if (output != NULL && text != NULL) snprintf(text, size, "%s:%u", output->provider, (unsigned)output->output);
}

static pwm_binding_t *find_binding_locked(pwm_provider_t *p, uint16_t output)
{
    const unsigned index = (unsigned)(p - providers);
    for (size_t i = 0; i < SOLAR_OS_PWM_OUTPUT_MAX; i++) {
        if (bindings[i].used && bindings[i].provider == index && bindings[i].output == output) return &bindings[i];
    }
    return NULL;
}

static void fill_info(pwm_provider_t *p, solar_os_pwm_output_info_t *info)
{
    memcpy(info->provider, p->info.name, sizeof(info->provider));
    info->owner[0] = '\0';
    xSemaphoreTake(registry_mutex, portMAX_DELAY);
    const pwm_binding_t *b = find_binding_locked(p, info->output);
    if (b != NULL) {
        info->reserved = b->reserved;
        memcpy(info->owner, b->owner, sizeof(info->owner));
    }
    xSemaphoreGive(registry_mutex);
}

size_t solar_os_pwm_output_count(void)
{
    size_t count = 0;
    for (size_t i = 0;; i++) {
        pwm_provider_t *p = begin_index(i);
        if (p == NULL) break;
        count += p->ops.output_count(p->ctx);
        end(p);
    }
    return count;
}

bool solar_os_pwm_get_output_info(size_t index, solar_os_pwm_output_info_t *info)
{
    if (info == NULL) return false;
    for (size_t i = 0;; i++) {
        pwm_provider_t *p = begin_index(i);
        if (p == NULL) return false;
        const size_t count = p->ops.output_count(p->ctx);
        if (index < count) {
            memset(info, 0, sizeof(*info));
            const bool found = p->ops.get_output(p->ctx, index, info);
            if (found) fill_info(p, info);
            end(p);
            return found;
        }
        index -= count;
        end(p);
    }
}

typedef enum { PWM_SET, PWM_OPEN, PWM_WRITE, PWM_DISABLE, PWM_CLOSE, PWM_STOP } pwm_operation_t;

static esp_err_t operate(const solar_os_pwm_output_t *output, pwm_operation_t operation,
                         const char *owner, uint32_t frequency, uint32_t value, bool pulse)
{
    if (output == NULL || !valid_name(output->provider) ||
        (operation != PWM_SET && operation != PWM_STOP &&
         (owner == NULL || owner[0] == '\0' || strnlen(owner, SOLAR_OS_PWM_OWNER_MAX) >= SOLAR_OS_PWM_OWNER_MAX))) return ESP_ERR_INVALID_ARG;
    if ((operation == PWM_SET || operation == PWM_OPEN) &&
        (frequency == 0U ||
         (operation == PWM_SET && (pulse ? (uint64_t)value * frequency >= 1000000U : value > 100U)))) return ESP_ERR_INVALID_ARG;
    pwm_provider_t *p = begin(output->provider);
    if (p == NULL) return ESP_ERR_NOT_FOUND;
    xSemaphoreTake(registry_mutex, portMAX_DELAY);
    pwm_binding_t *b = find_binding_locked(p, output->output);
    esp_err_t ret = ESP_OK;
    bool created = false;
    if (operation == PWM_OPEN && b != NULL) ret = ESP_ERR_INVALID_STATE;
    else if ((operation == PWM_SET || operation == PWM_STOP) && b != NULL && b->reserved) ret = ESP_ERR_INVALID_STATE;
    else if (operation != PWM_SET && operation != PWM_OPEN && b == NULL) ret = ESP_ERR_NOT_FOUND;
    else if (operation != PWM_SET && operation != PWM_OPEN && operation != PWM_STOP &&
             (!b->reserved || strcmp(b->owner, owner) != 0)) ret = ESP_ERR_INVALID_STATE;
    if (ret == ESP_OK && b == NULL) {
        for (size_t i = 0; i < SOLAR_OS_PWM_OUTPUT_MAX; i++) {
            if (!bindings[i].used) { b = &bindings[i]; break; }
        }
        if (b == NULL) ret = ESP_ERR_NO_MEM;
        else {
            *b = (pwm_binding_t){.used = true, .provider = (unsigned)(p - providers),
                .output = output->output, .reserved = operation == PWM_OPEN};
            if (owner != NULL) memcpy(b->owner, owner, strlen(owner) + 1U);
            else memcpy(b->owner, "pwm", 4U);
            p->bindings++;
            created = true;
        }
    }
    xSemaphoreGive(registry_mutex);
    if (ret == ESP_OK) {
        switch (operation) {
        case PWM_SET: ret = p->ops.set(p->ctx, output->output, frequency, value, pulse); break;
        case PWM_OPEN: ret = p->ops.open(p->ctx, output->output, frequency, owner); break;
        case PWM_WRITE: ret = p->ops.write_pulse(p->ctx, output->output, owner, value); break;
        case PWM_DISABLE: ret = p->ops.disable(p->ctx, output->output, owner); break;
        case PWM_CLOSE:
            ret = p->ops.close(p->ctx, output->output, owner);
            /* A failed backend close leaves the registry claim intact. */
            if (ret == ESP_ERR_NOT_FOUND) ret = ESP_ERR_INVALID_STATE;
            break;
        case PWM_STOP: ret = p->ops.stop(p->ctx, output->output); break;
        }
        if ((ret != ESP_OK && created) || (ret == ESP_OK && (operation == PWM_CLOSE || operation == PWM_STOP))) {
            xSemaphoreTake(registry_mutex, portMAX_DELAY);
            memset(b, 0, sizeof(*b));
            p->bindings--;
            xSemaphoreGive(registry_mutex);
        }
    }
    end(p);
    return ret;
}

esp_err_t solar_os_pwm_set_output(const solar_os_pwm_output_t *o, uint32_t f, uint32_t v, bool pulse)
{ return operate(o, PWM_SET, NULL, f, v, pulse); }
esp_err_t solar_os_pwm_open_output(const solar_os_pwm_output_t *o, uint32_t f, const char *owner)
{ return operate(o, PWM_OPEN, owner, f, 0, false); }
esp_err_t solar_os_pwm_write_output(const solar_os_pwm_output_t *o, const char *owner, uint32_t pulse)
{ return operate(o, PWM_WRITE, owner, 0, pulse, true); }
esp_err_t solar_os_pwm_disable_output(const solar_os_pwm_output_t *o, const char *owner)
{ return operate(o, PWM_DISABLE, owner, 0, 0, false); }
esp_err_t solar_os_pwm_close_output(const solar_os_pwm_output_t *o, const char *owner)
{ return operate(o, PWM_CLOSE, owner, 0, 0, false); }
esp_err_t solar_os_pwm_stop_output(const solar_os_pwm_output_t *o)
{ return operate(o, PWM_STOP, NULL, 0, 0, false); }

static solar_os_pwm_output_t native_output(int pin)
{
    return (solar_os_pwm_output_t){.provider = "native", .output = pin < 0 || pin > 48 ? UINT16_MAX : (uint16_t)pin};
}

size_t solar_os_pwm_pin_count(void)
{
    pwm_provider_t *p = begin("native");
    if (p == NULL) return 0;
    const size_t count = p->ops.output_count(p->ctx);
    end(p);
    return count;
}

bool solar_os_pwm_get_pin_info(size_t index, solar_os_pwm_pin_info_t *info)
{
    if (info == NULL) return false;
    pwm_provider_t *p = begin("native");
    if (p == NULL) return false;
    memset(info, 0, sizeof(*info));
    const bool found = p->ops.get_output(p->ctx, index, info);
    if (found) fill_info(p, info);
    end(p);
    return found;
}

esp_err_t solar_os_pwm_set(int pin, uint32_t f, uint8_t duty)
{ const solar_os_pwm_output_t o = native_output(pin); return solar_os_pwm_set_output(&o, f, duty, false); }
esp_err_t solar_os_pwm_set_pulse(int pin, uint32_t f, uint32_t pulse)
{ const solar_os_pwm_output_t o = native_output(pin); return solar_os_pwm_set_output(&o, f, pulse, true); }
esp_err_t solar_os_pwm_open(int pin, uint32_t f, const char *owner)
{ const solar_os_pwm_output_t o = native_output(pin); return solar_os_pwm_open_output(&o, f, owner); }
esp_err_t solar_os_pwm_write_pulse(int pin, const char *owner, uint32_t pulse)
{ const solar_os_pwm_output_t o = native_output(pin); return solar_os_pwm_write_output(&o, owner, pulse); }
esp_err_t solar_os_pwm_disable(int pin, const char *owner)
{ const solar_os_pwm_output_t o = native_output(pin); return solar_os_pwm_disable_output(&o, owner); }
esp_err_t solar_os_pwm_close(int pin, const char *owner)
{ const solar_os_pwm_output_t o = native_output(pin); return solar_os_pwm_close_output(&o, owner); }
esp_err_t solar_os_pwm_stop(int pin)
{ const solar_os_pwm_output_t o = native_output(pin); return solar_os_pwm_stop_output(&o); }
