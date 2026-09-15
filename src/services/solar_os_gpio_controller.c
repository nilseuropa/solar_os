#include "solar_os_gpio_controller.h"

#include <stdlib.h>
#include <string.h>

#include "esp_attr.h"
#include "esp_check.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "gpio_port.h"
#include "solar_os_resources.h"

#define SOLAR_OS_GPIO_CONTROLLER_MAX 4U

typedef struct {
    bool active;
    uint8_t id;
    uint8_t line_count;
    size_t in_flight;
    char name[SOLAR_OS_GPIO_CONTROLLER_NAME_MAX];
    const solar_os_gpio_controller_ops_t *ops;
    void *ctx;
} gpio_controller_slot_t;

typedef struct {
    gpio_controller_slot_t *slot;
    const solar_os_gpio_controller_ops_t *ops;
    void *ctx;
} gpio_controller_call_t;

static EXT_RAM_BSS_ATTR gpio_controller_slot_t controllers[SOLAR_OS_GPIO_CONTROLLER_MAX];
static SemaphoreHandle_t controllers_mutex;
static EXT_RAM_BSS_ATTR StaticSemaphore_t controllers_mutex_storage;

static esp_err_t ensure_init(void)
{
    if (controllers_mutex == NULL) {
        controllers_mutex = xSemaphoreCreateMutexStatic(&controllers_mutex_storage);
    }
    return controllers_mutex != NULL ? ESP_OK : ESP_ERR_NO_MEM;
}

static gpio_controller_slot_t *find_locked(const char *name)
{
    if (name == NULL) {
        return NULL;
    }
    for (size_t i = 0; i < SOLAR_OS_GPIO_CONTROLLER_MAX; i++) {
        if (controllers[i].active && strcmp(controllers[i].name, name) == 0) {
            return &controllers[i];
        }
    }
    return NULL;
}

static void copy_info(const gpio_controller_slot_t *slot,
                      solar_os_gpio_controller_info_t *info)
{
    *info = (solar_os_gpio_controller_info_t) {
        .id = slot->id,
        .line_count = slot->line_count,
    };
    strlcpy(info->name, slot->name, sizeof(info->name));
}

esp_err_t solar_os_gpio_controller_register(
    const solar_os_gpio_controller_registration_t *registration)
{
    if (registration == NULL || registration->name == NULL ||
        registration->name[0] == '\0' ||
        strnlen(registration->name, SOLAR_OS_GPIO_CONTROLLER_NAME_MAX) >=
            SOLAR_OS_GPIO_CONTROLLER_NAME_MAX ||
        registration->line_count == 0U || registration->ops == NULL ||
        registration->ops->configure == NULL || registration->ops->read == NULL ||
        registration->ops->write == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    ESP_RETURN_ON_ERROR(ensure_init(), "gpio-controller", "registry init failed");
    xSemaphoreTake(controllers_mutex, portMAX_DELAY);
    if (find_locked(registration->name) != NULL) {
        xSemaphoreGive(controllers_mutex);
        return ESP_ERR_INVALID_STATE;
    }
    gpio_controller_slot_t *free_slot = NULL;
    uint8_t free_id = 0U;
    for (size_t i = 0; i < SOLAR_OS_GPIO_CONTROLLER_MAX; i++) {
        if (!controllers[i].active) {
            free_slot = &controllers[i];
            free_id = (uint8_t)i;
            break;
        }
    }
    if (free_slot == NULL) {
        xSemaphoreGive(controllers_mutex);
        return ESP_ERR_NO_MEM;
    }
    *free_slot = (gpio_controller_slot_t) {
        .active = true,
        .id = free_id,
        .line_count = registration->line_count,
        .ops = registration->ops,
        .ctx = registration->ctx,
    };
    strlcpy(free_slot->name, registration->name, sizeof(free_slot->name));
    xSemaphoreGive(controllers_mutex);
    return ESP_OK;
}

esp_err_t solar_os_gpio_controller_unregister(const char *name)
{
    if (name == NULL || name[0] == '\0') {
        return ESP_ERR_INVALID_ARG;
    }
    ESP_RETURN_ON_ERROR(ensure_init(), "gpio-controller", "registry init failed");
    xSemaphoreTake(controllers_mutex, portMAX_DELAY);
    gpio_controller_slot_t *slot = find_locked(name);
    if (slot == NULL) {
        xSemaphoreGive(controllers_mutex);
        return ESP_ERR_NOT_FOUND;
    }
    if (slot->in_flight > 0U) {
        xSemaphoreGive(controllers_mutex);
        return ESP_ERR_INVALID_STATE;
    }
    for (uint8_t line = 0; line < slot->line_count; line++) {
        solar_os_resource_claim_t claim;
        if (solar_os_resource_find_claim(SOLAR_OS_RESOURCE_GPIO_LINE,
                                         slot->id,
                                         line,
                                         &claim)) {
            xSemaphoreGive(controllers_mutex);
            return ESP_ERR_INVALID_STATE;
        }
    }
    memset(slot, 0, sizeof(*slot));
    xSemaphoreGive(controllers_mutex);
    return ESP_OK;
}

size_t solar_os_gpio_controller_count(void)
{
    if (ensure_init() != ESP_OK) {
        return 0U;
    }
    size_t count = 0U;
    xSemaphoreTake(controllers_mutex, portMAX_DELAY);
    for (size_t i = 0; i < SOLAR_OS_GPIO_CONTROLLER_MAX; i++) {
        count += controllers[i].active ? 1U : 0U;
    }
    xSemaphoreGive(controllers_mutex);
    return count;
}

bool solar_os_gpio_controller_get(size_t index, solar_os_gpio_controller_info_t *info)
{
    if (info == NULL || ensure_init() != ESP_OK) {
        return false;
    }
    size_t current = 0U;
    bool found = false;
    xSemaphoreTake(controllers_mutex, portMAX_DELAY);
    for (size_t i = 0; i < SOLAR_OS_GPIO_CONTROLLER_MAX; i++) {
        if (!controllers[i].active) {
            continue;
        }
        if (current++ == index) {
            copy_info(&controllers[i], info);
            found = true;
            break;
        }
    }
    xSemaphoreGive(controllers_mutex);
    return found;
}

bool solar_os_gpio_controller_find(const char *name, solar_os_gpio_controller_info_t *info)
{
    if (name == NULL || ensure_init() != ESP_OK) {
        return false;
    }
    xSemaphoreTake(controllers_mutex, portMAX_DELAY);
    const gpio_controller_slot_t *slot = find_locked(name);
    if (slot != NULL && info != NULL) {
        copy_info(slot, info);
    }
    xSemaphoreGive(controllers_mutex);
    return slot != NULL;
}

bool solar_os_gpio_line_parse(const char *text, solar_os_gpio_line_ref_t *line)
{
    if (text == NULL || line == NULL) {
        return false;
    }
    const char *native_text = strncmp(text, "gpio", 4U) == 0 ? text + 4U : text;
    char *native_end = NULL;
    const long native = strtol(native_text, &native_end, 0);
    if (native_end != native_text && *native_end == '\0' &&
        native >= 0 && native <= UINT8_MAX) {
        memset(line, 0, sizeof(*line));
        line->line = (uint8_t)native;
        return true;
    }
    const char *separator = strrchr(text, ':');
    if (separator == NULL || separator == text || separator[1] == '\0') {
        return false;
    }
    const size_t name_len = (size_t)(separator - text);
    if (name_len >= sizeof(line->controller)) {
        return false;
    }
    char *end = NULL;
    const long parsed = strtol(separator + 1, &end, 0);
    if (end == separator + 1 || *end != '\0' || parsed < 0 || parsed > UINT8_MAX) {
        return false;
    }
    memset(line, 0, sizeof(*line));
    memcpy(line->controller, text, name_len);
    line->line = (uint8_t)parsed;
    return true;
}

bool solar_os_gpio_line_is_native(const solar_os_gpio_line_ref_t *line)
{
    return line != NULL && line->controller[0] == '\0';
}

static esp_err_t begin_call(const solar_os_gpio_line_ref_t *line,
                            gpio_controller_call_t *call)
{
    if (line == NULL || call == NULL || line->controller[0] == '\0') {
        return ESP_ERR_INVALID_ARG;
    }
    ESP_RETURN_ON_ERROR(ensure_init(), "gpio-controller", "registry init failed");
    xSemaphoreTake(controllers_mutex, portMAX_DELAY);
    gpio_controller_slot_t *slot = find_locked(line->controller);
    if (slot == NULL) {
        xSemaphoreGive(controllers_mutex);
        return ESP_ERR_NOT_FOUND;
    }
    if (line->line >= slot->line_count) {
        xSemaphoreGive(controllers_mutex);
        return ESP_ERR_INVALID_ARG;
    }
    slot->in_flight++;
    *call = (gpio_controller_call_t) {
        .slot = slot,
        .ops = slot->ops,
        .ctx = slot->ctx,
    };
    xSemaphoreGive(controllers_mutex);
    return ESP_OK;
}

static void end_call(gpio_controller_call_t *call)
{
    xSemaphoreTake(controllers_mutex, portMAX_DELAY);
    if (call->slot->in_flight > 0U) {
        call->slot->in_flight--;
    }
    xSemaphoreGive(controllers_mutex);
}

esp_err_t solar_os_gpio_line_configure(const solar_os_gpio_line_ref_t *line,
                                       solar_os_gpio_line_mode_t mode,
                                       solar_os_gpio_line_pull_t pull)
{
    if ((mode != SOLAR_OS_GPIO_LINE_MODE_INPUT &&
         mode != SOLAR_OS_GPIO_LINE_MODE_OUTPUT) ||
        (pull != SOLAR_OS_GPIO_LINE_PULL_NONE &&
         pull != SOLAR_OS_GPIO_LINE_PULL_UP &&
         pull != SOLAR_OS_GPIO_LINE_PULL_DOWN)) {
        return ESP_ERR_INVALID_ARG;
    }
    if (solar_os_gpio_line_is_native(line)) {
        const gpio_port_mode_t native_mode = mode == SOLAR_OS_GPIO_LINE_MODE_OUTPUT
            ? GPIO_PORT_MODE_OUTPUT : GPIO_PORT_MODE_INPUT;
        gpio_port_pull_t native_pull = GPIO_PORT_PULL_NONE;
        if (pull == SOLAR_OS_GPIO_LINE_PULL_UP) {
            native_pull = GPIO_PORT_PULL_UP;
        } else if (pull == SOLAR_OS_GPIO_LINE_PULL_DOWN) {
            native_pull = GPIO_PORT_PULL_DOWN;
        }
        return gpio_port_configure((gpio_num_t)line->line, native_mode, native_pull);
    }
    gpio_controller_call_t call;
    ESP_RETURN_ON_ERROR(begin_call(line, &call), "gpio-controller", "line unavailable");
    const esp_err_t ret = call.ops->configure(call.ctx, line->line, mode, pull);
    end_call(&call);
    return ret;
}

esp_err_t solar_os_gpio_line_read(const solar_os_gpio_line_ref_t *line, bool *level)
{
    if (level == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (solar_os_gpio_line_is_native(line)) {
        return gpio_port_read((gpio_num_t)line->line, level);
    }
    gpio_controller_call_t call;
    ESP_RETURN_ON_ERROR(begin_call(line, &call), "gpio-controller", "line unavailable");
    const esp_err_t ret = call.ops->read(call.ctx, line->line, level);
    end_call(&call);
    return ret;
}

esp_err_t solar_os_gpio_line_write(const solar_os_gpio_line_ref_t *line, bool level)
{
    if (solar_os_gpio_line_is_native(line)) {
        /* Preload the output latch before enabling the output driver. */
        ESP_RETURN_ON_ERROR(gpio_port_write((gpio_num_t)line->line, level),
                            "gpio-controller",
                            "native line preload failed");
        return gpio_port_configure((gpio_num_t)line->line,
                                   GPIO_PORT_MODE_OUTPUT,
                                   GPIO_PORT_PULL_NONE);
    }
    gpio_controller_call_t call;
    ESP_RETURN_ON_ERROR(begin_call(line, &call), "gpio-controller", "line unavailable");
    const esp_err_t ret = call.ops->write(call.ctx, line->line, level);
    end_call(&call);
    return ret;
}
