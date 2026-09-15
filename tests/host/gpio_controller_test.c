#include <assert.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include "driver/gpio.h"
#include "solar_os_gpio_controller.h"
#include "solar_os_resources.h"

static gpio_config_t native_config;
static bool native_level[64];

esp_err_t gpio_config(const gpio_config_t *config)
{
    assert(config != NULL);
    native_config = *config;
    return ESP_OK;
}

int gpio_get_level(gpio_num_t pin)
{
    return native_level[pin] ? 1 : 0;
}

esp_err_t gpio_set_level(gpio_num_t pin, uint32_t level)
{
    native_level[pin] = level != 0U;
    return ESP_OK;
}

size_t strlcpy(char *dst, const char *src, size_t size)
{
    const size_t len = strlen(src);
    if (size > 0U) {
        const size_t copy = len < size - 1U ? len : size - 1U;
        memcpy(dst, src, copy);
        dst[copy] = '\0';
    }
    return len;
}

typedef struct {
    solar_os_gpio_line_mode_t mode[4];
    solar_os_gpio_line_pull_t pull[4];
    bool level[4];
} fake_controller_t;

static esp_err_t fake_configure(void *ctx,
                                uint8_t line,
                                solar_os_gpio_line_mode_t mode,
                                solar_os_gpio_line_pull_t pull)
{
    fake_controller_t *fake = ctx;
    fake->mode[line] = mode;
    fake->pull[line] = pull;
    return ESP_OK;
}

static esp_err_t fake_read(void *ctx, uint8_t line, bool *level)
{
    fake_controller_t *fake = ctx;
    *level = fake->level[line];
    return ESP_OK;
}

static esp_err_t fake_write(void *ctx, uint8_t line, bool level)
{
    fake_controller_t *fake = ctx;
    fake->level[line] = level;
    return ESP_OK;
}

int main(void)
{
    static const solar_os_gpio_controller_ops_t ops = {
        .configure = fake_configure,
        .read = fake_read,
        .write = fake_write,
    };
    fake_controller_t fake = {0};
    const solar_os_gpio_controller_registration_t registration = {
        .name = "gpiox0",
        .line_count = 4,
        .ops = &ops,
        .ctx = &fake,
    };

    assert(solar_os_gpio_controller_register(&registration) == ESP_OK);
    assert(solar_os_gpio_controller_register(&registration) == ESP_ERR_INVALID_STATE);
    assert(solar_os_gpio_controller_count() == 1U);

    solar_os_gpio_controller_info_t info;
    assert(solar_os_gpio_controller_get(0U, &info));
    assert(strcmp(info.name, "gpiox0") == 0);
    assert(info.line_count == 4U);

    solar_os_gpio_line_ref_t line;
    assert(solar_os_gpio_line_parse("gpiox0:3", &line));
    assert(strcmp(line.controller, "gpiox0") == 0);
    assert(line.line == 3U);
    assert(!solar_os_gpio_line_parse("gpiox0", &line));
    assert(!solar_os_gpio_line_parse("gpiox0:256", &line));

    assert(solar_os_gpio_line_configure(&line,
                                        SOLAR_OS_GPIO_LINE_MODE_OUTPUT,
                                        SOLAR_OS_GPIO_LINE_PULL_NONE) == ESP_OK);
    assert(fake.mode[3] == SOLAR_OS_GPIO_LINE_MODE_OUTPUT);
    assert(solar_os_gpio_line_write(&line, true) == ESP_OK);
    bool level = false;
    assert(solar_os_gpio_line_read(&line, &level) == ESP_OK);
    assert(level);

    assert(solar_os_gpio_line_parse("gpio12", &line));
    assert(solar_os_gpio_line_is_native(&line));
    assert(line.controller[0] == '\0');
    assert(line.line == 12U);
    assert(solar_os_gpio_line_write(&line, true) == ESP_OK);
    assert(native_level[12]);
    assert(native_config.pin_bit_mask == (1ULL << 12U));
    assert(native_config.mode == GPIO_MODE_OUTPUT);
    level = false;
    assert(solar_os_gpio_line_read(&line, &level) == ESP_OK);
    assert(level);

    assert(solar_os_gpio_line_parse("gpiox0:3", &line));

    assert(solar_os_resource_claim(SOLAR_OS_RESOURCE_GPIO_LINE,
                                   info.id,
                                   line.line,
                                   "consumer0",
                                   "power") == ESP_OK);
    assert(solar_os_gpio_controller_unregister("gpiox0") == ESP_ERR_INVALID_STATE);
    assert(solar_os_resource_release_owner("consumer0") == 1U);
    assert(solar_os_gpio_controller_unregister("gpiox0") == ESP_OK);
    assert(solar_os_gpio_line_write(&line, false) == ESP_ERR_NOT_FOUND);

    puts("GPIO controller tests passed");
    return 0;
}
