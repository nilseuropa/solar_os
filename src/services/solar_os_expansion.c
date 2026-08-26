#include "solar_os_expansion.h"

#include <string.h>

#include "esp_check.h"
#include "freertos/FreeRTOS.h"
#include "solar_os_board.h"
#include "solar_os_buses.h"
#include "solar_os_config.h"
#include "solar_os_pins.h"
#if SOLAR_OS_PACKAGE_EXPANSION_ANALOG_JOYSTICK
#include "solar_os_analog_joystick.h"
#endif
#if SOLAR_OS_PACKAGE_EXPANSION_PCD8544
#include "solar_os_pcd8544.h"
#endif
#if SOLAR_OS_PACKAGE_EXPANSION_SSD1683
#include "solar_os_ssd1683.h"
#endif
#if SOLAR_OS_PACKAGE_EXPANSION_RFM69
#include "solar_os_rfm69.h"
#endif
#if SOLAR_OS_PACKAGE_EXPANSION_RFM95
#include "solar_os_rfm95.h"
#endif
#if SOLAR_OS_PACKAGE_EXPANSION_SSD1306
#include "solar_os_ssd1306.h"
#endif
#if SOLAR_OS_PACKAGE_EXPANSION_CARDKB
#include "solar_os_cardkb.h"
#endif
#if SOLAR_OS_PACKAGE_EXPANSION_GPIO_KEYS
#include "solar_os_gpio_keys.h"
#endif
#if SOLAR_OS_PACKAGE_EXPANSION_PS2_KEYBOARD
#include "solar_os_ps2_keyboard_device.h"
#endif
#if SOLAR_OS_PACKAGE_EXPANSION_PS2_MOUSE
#include "solar_os_ps2_mouse_device.h"
#endif
#if SOLAR_OS_BOARD_HAS_POINTER
#include "solar_os_ft6336.h"
#endif

#if SOLAR_OS_PACKAGE_EXPANSION_PS2_KEYBOARD || SOLAR_OS_PACKAGE_EXPANSION_PS2_MOUSE
static const solar_os_expansion_binding_spec_t ps2_device_binding_specs[] = {
    {
        .key = "ps2",
        .value_hint = "bus",
        .kind = SOLAR_OS_EXPANSION_BINDING_PS2_BUS,
        .required = true,
    },
};
#endif
#if SOLAR_OS_PACKAGE_EXPANSION_SDSPI && !SOLAR_OS_BOARD_HAS_SD
#include "solar_os_sdspi.h"
#endif
#if SOLAR_OS_PACKAGE_EXPANSION_NEOPIXEL
#include "solar_os_neopixel.h"
#endif
#if SOLAR_OS_PACKAGE_EXPANSION_AUDIO_PWM
#include "solar_os_audio_pwm.h"
#endif
#if SOLAR_OS_PACKAGE_EXPANSION_PCM5102
#include "solar_os_pcm5102.h"
#endif
#include "solar_os_resources.h"
#include "solar_os_stream.h"

#define SOLAR_OS_EXPANSION_DEVICE_MAX 8

#if SOLAR_OS_PACKAGE_EXPANSION_ANALOG_JOYSTICK
static const solar_os_expansion_binding_spec_t analog_joystick_binding_specs[] = {
    {
        .key = "x",
        .value_hint = "scalar-stream",
        .kind = SOLAR_OS_EXPANSION_BINDING_SCALAR_STREAM,
        .role = "x",
        .required = true,
    },
    {
        .key = "y",
        .value_hint = "scalar-stream",
        .kind = SOLAR_OS_EXPANSION_BINDING_SCALAR_STREAM,
        .role = "y",
        .required = true,
    },
    {
        .key = "min",
        .value_hint = "value",
        .kind = SOLAR_OS_EXPANSION_BINDING_PARAMETER,
        .role = "min",
        .required = true,
    },
    {
        .key = "center",
        .value_hint = "value",
        .kind = SOLAR_OS_EXPANSION_BINDING_PARAMETER,
        .role = "center",
        .required = true,
    },
    {
        .key = "max",
        .value_hint = "value",
        .kind = SOLAR_OS_EXPANSION_BINDING_PARAMETER,
        .role = "max",
        .required = true,
    },
    {
        .key = "deadzone",
        .value_hint = "value",
        .kind = SOLAR_OS_EXPANSION_BINDING_PARAMETER,
        .role = "deadzone",
        .has_value_range = true,
        .min_value = 0,
        .max_value = 1000000,
    },
};
#endif

#if SOLAR_OS_PACKAGE_EXPANSION_RFM69
static const solar_os_expansion_binding_spec_t rfm69_binding_specs[] = {
    {.key = "spi", .value_hint = "bus", .kind = SOLAR_OS_EXPANSION_BINDING_SPI_BUS, .required = true},
    {.key = "cs", .value_hint = "gpio", .kind = SOLAR_OS_EXPANSION_BINDING_SPI_CS, .required = true},
    {.key = "irq", .value_hint = "gpio", .kind = SOLAR_OS_EXPANSION_BINDING_GPIO, .role = "irq"},
    {.key = "reset", .value_hint = "gpio", .kind = SOLAR_OS_EXPANSION_BINDING_GPIO, .role = "reset"},
};
#endif

#if SOLAR_OS_PACKAGE_EXPANSION_RFM95
static const solar_os_expansion_binding_spec_t rfm95_binding_specs[] = {
    {.key = "spi", .value_hint = "bus", .kind = SOLAR_OS_EXPANSION_BINDING_SPI_BUS, .required = true},
    {.key = "cs", .value_hint = "gpio", .kind = SOLAR_OS_EXPANSION_BINDING_SPI_CS, .required = true},
    {.key = "irq", .value_hint = "gpio", .kind = SOLAR_OS_EXPANSION_BINDING_GPIO, .role = "irq"},
    {.key = "reset", .value_hint = "gpio", .kind = SOLAR_OS_EXPANSION_BINDING_GPIO, .role = "reset"},
};
#endif

#if SOLAR_OS_PACKAGE_EXPANSION_PCD8544
static const solar_os_expansion_binding_spec_t pcd8544_binding_specs[] = {
    {.key = "spi", .value_hint = "bus", .kind = SOLAR_OS_EXPANSION_BINDING_SPI_BUS, .required = true},
    {.key = "cs", .value_hint = "gpio", .kind = SOLAR_OS_EXPANSION_BINDING_SPI_CS, .required = true},
    {.key = "dc", .value_hint = "gpio", .kind = SOLAR_OS_EXPANSION_BINDING_GPIO, .role = "dc", .required = true},
    {.key = "reset", .value_hint = "gpio", .kind = SOLAR_OS_EXPANSION_BINDING_GPIO, .role = "reset", .required = true},
};
#endif

#if SOLAR_OS_PACKAGE_EXPANSION_SSD1683
static const solar_os_expansion_binding_spec_t ssd1683_binding_specs[] = {
    {.key = "spi", .value_hint = "bus", .kind = SOLAR_OS_EXPANSION_BINDING_SPI_BUS, .required = true},
    {.key = "cs", .value_hint = "gpio", .kind = SOLAR_OS_EXPANSION_BINDING_SPI_CS, .required = true},
    {.key = "dc", .value_hint = "gpio", .kind = SOLAR_OS_EXPANSION_BINDING_GPIO, .role = "dc", .required = true},
    {.key = "reset", .value_hint = "gpio", .kind = SOLAR_OS_EXPANSION_BINDING_GPIO, .role = "reset", .required = true},
    {.key = "busy", .value_hint = "gpio", .kind = SOLAR_OS_EXPANSION_BINDING_GPIO, .role = "busy", .required = true},
};
#endif

#if SOLAR_OS_PACKAGE_EXPANSION_SSD1306
static const int oled_i2c_addresses[] = {0x3c, 0x3d};

static const solar_os_expansion_binding_spec_t oled_binding_specs[] = {
    {.key = "i2c", .value_hint = "bus", .kind = SOLAR_OS_EXPANSION_BINDING_I2C_BUS, .required = true},
    {
        .key = "addr",
        .value_hint = "0x3c|0x3d",
        .kind = SOLAR_OS_EXPANSION_BINDING_I2C_ADDRESS,
        .required = true,
        .allowed_values = oled_i2c_addresses,
        .allowed_value_count = sizeof(oled_i2c_addresses) / sizeof(oled_i2c_addresses[0]),
    },
};
#endif

#if SOLAR_OS_PACKAGE_EXPANSION_CARDKB
static const int cardkb_i2c_addresses[] = {SOLAR_OS_CARDKB_ADDRESS};

static const solar_os_expansion_binding_spec_t cardkb_binding_specs[] = {
    {.key = "i2c", .value_hint = "bus", .kind = SOLAR_OS_EXPANSION_BINDING_I2C_BUS, .required = true},
    {
        .key = "addr",
        .value_hint = "0x5f",
        .kind = SOLAR_OS_EXPANSION_BINDING_I2C_ADDRESS,
        .required = true,
        .allowed_values = cardkb_i2c_addresses,
        .allowed_value_count = sizeof(cardkb_i2c_addresses) /
            sizeof(cardkb_i2c_addresses[0]),
    },
};
#endif
#if SOLAR_OS_BOARD_HAS_POINTER
#define FT6336_ADDRESS 0x38

static const int ft6336_i2c_addresses[] = {FT6336_ADDRESS};

static const solar_os_expansion_binding_spec_t ft6336_binding_specs[] = {
    {.key = "i2c", .value_hint = "bus", .kind = SOLAR_OS_EXPANSION_BINDING_I2C_BUS, .required = true},
    {
        .key = "addr",
        .value_hint = "0x38",
        .kind = SOLAR_OS_EXPANSION_BINDING_I2C_ADDRESS,
        .required = true,
        .allowed_values = ft6336_i2c_addresses,
        .allowed_value_count = sizeof(ft6336_i2c_addresses) /
            sizeof(ft6336_i2c_addresses[0]),
    },
    {
        .key = "reset",
        .value_hint = "gpio",
        .kind = SOLAR_OS_EXPANSION_BINDING_GPIO,
        .role = "reset",
        .required = true,
    },
    {
        .key = "irq",
        .value_hint = "gpio",
        .kind = SOLAR_OS_EXPANSION_BINDING_GPIO,
        .role = "irq",
        .required = true,
    },
};
#endif

#if SOLAR_OS_PACKAGE_EXPANSION_SDSPI && !SOLAR_OS_BOARD_HAS_SD
static const solar_os_expansion_binding_spec_t sdspi_binding_specs[] = {
    {.key = "spi", .value_hint = "bus", .kind = SOLAR_OS_EXPANSION_BINDING_SPI_BUS, .required = true},
    {.key = "cs", .value_hint = "gpio", .kind = SOLAR_OS_EXPANSION_BINDING_SPI_CS, .required = true},
};
#endif

#if SOLAR_OS_PACKAGE_EXPANSION_NEOPIXEL
static const solar_os_expansion_binding_spec_t neopixel_binding_specs[] = {
    {
        .key = "data",
        .value_hint = "gpio",
        .kind = SOLAR_OS_EXPANSION_BINDING_GPIO,
        .role = "data",
        .required = true,
    },
    {
        .key = "count",
        .value_hint = "1..256",
        .kind = SOLAR_OS_EXPANSION_BINDING_PARAMETER,
        .role = "count",
        .required = true,
        .has_value_range = true,
        .min_value = 1,
        .max_value = SOLAR_OS_NEOPIXEL_MAX_PIXELS,
    },
};
#endif

#if SOLAR_OS_PACKAGE_EXPANSION_AUDIO_PWM
static const solar_os_expansion_binding_spec_t audio_pwm_binding_specs[] = {
    {
        .key = "pwm",
        .value_hint = "gpio",
        .kind = SOLAR_OS_EXPANSION_BINDING_PWM,
        .role = "pwm",
        .required = true,
    },
};
#endif

#if SOLAR_OS_PACKAGE_EXPANSION_PCM5102
static const solar_os_expansion_binding_spec_t pcm5102_binding_specs[] = {
    {
        .key = "bck",
        .value_hint = "gpio",
        .kind = SOLAR_OS_EXPANSION_BINDING_GPIO,
        .role = "bck",
        .required = true,
    },
    {
        .key = "din",
        .value_hint = "gpio",
        .kind = SOLAR_OS_EXPANSION_BINDING_GPIO,
        .role = "din",
        .required = true,
    },
    {
        .key = "rck",
        .value_hint = "gpio",
        .kind = SOLAR_OS_EXPANSION_BINDING_GPIO,
        .role = "rck",
        .required = true,
    },
};
#endif

static const solar_os_expansion_driver_t expansion_drivers[] = {
    {
        .name = "manual",
        .summary = "manual resource profile",
        .required_capabilities = 0,
        .probe_supported = false,
        .allow_unlisted_bindings = true,
    },
#if SOLAR_OS_PACKAGE_EXPANSION_ANALOG_JOYSTICK
    {
        .name = "analog-joystick",
        .summary = "two-axis joystick from scalar streams",
        .required_capabilities = 0,
        .probe_supported = false,
        .binding_specs = analog_joystick_binding_specs,
        .binding_spec_count = sizeof(analog_joystick_binding_specs) /
            sizeof(analog_joystick_binding_specs[0]),
        .attach = solar_os_analog_joystick_attach,
        .detach = solar_os_analog_joystick_detach,
    },
#endif
#if SOLAR_OS_PACKAGE_EXPANSION_RFM69
    {
        .name = "rfm69",
        .summary = "HopeRF RFM69W/CW SPI packet radio",
        .required_capabilities = SOLAR_OS_BOARD_CAP_EXPANSION_SPI,
        .probe_supported = true,
        .binding_specs = rfm69_binding_specs,
        .binding_spec_count = sizeof(rfm69_binding_specs) / sizeof(rfm69_binding_specs[0]),
        .attach = solar_os_rfm69_attach,
        .detach = solar_os_rfm69_detach,
    },
    {
        .name = "rfm69h",
        .summary = "HopeRF RFM69HW/HCW high-power SPI packet radio",
        .required_capabilities = SOLAR_OS_BOARD_CAP_EXPANSION_SPI,
        .probe_supported = true,
        .binding_specs = rfm69_binding_specs,
        .binding_spec_count = sizeof(rfm69_binding_specs) / sizeof(rfm69_binding_specs[0]),
        .attach = solar_os_rfm69h_attach,
        .detach = solar_os_rfm69_detach,
    },
#endif
#if SOLAR_OS_PACKAGE_EXPANSION_RFM95
    {
        .name = "rfm95",
        .summary = "HopeRF RFM95W multimode radio",
        .required_capabilities = SOLAR_OS_BOARD_CAP_EXPANSION_SPI,
        .probe_supported = true,
        .binding_specs = rfm95_binding_specs,
        .binding_spec_count = sizeof(rfm95_binding_specs) / sizeof(rfm95_binding_specs[0]),
        .attach = solar_os_rfm95_attach,
        .detach = solar_os_rfm95_detach,
    },
#endif
#if SOLAR_OS_PACKAGE_EXPANSION_PCD8544
    {
        .name = "pcd8544",
        .summary = "PCD8544 84x48 SPI LCD",
        .required_capabilities = SOLAR_OS_BOARD_CAP_EXPANSION_SPI |
            SOLAR_OS_BOARD_CAP_EXPANSION_GPIO,
        .probe_supported = false,
        .binding_specs = pcd8544_binding_specs,
        .binding_spec_count = sizeof(pcd8544_binding_specs) / sizeof(pcd8544_binding_specs[0]),
        .attach = solar_os_pcd8544_attach,
        .detach = solar_os_pcd8544_detach,
    },
#endif
#if SOLAR_OS_PACKAGE_EXPANSION_SSD1683
    {
        .name = "ssd1683",
        .summary = "Waveshare 4.2-inch V2 400x300 e-paper",
        .required_capabilities = SOLAR_OS_BOARD_CAP_EXPANSION_SPI |
            SOLAR_OS_BOARD_CAP_EXPANSION_GPIO,
        .probe_supported = false,
        .binding_specs = ssd1683_binding_specs,
        .binding_spec_count = sizeof(ssd1683_binding_specs) /
            sizeof(ssd1683_binding_specs[0]),
        .attach = solar_os_ssd1683_attach,
        .detach = solar_os_ssd1683_detach,
    },
#endif
#if SOLAR_OS_PACKAGE_EXPANSION_SSD1306
    {
        .name = "ssd1306",
        .summary = "SSD1306 128x64 I2C OLED",
        .required_capabilities = SOLAR_OS_BOARD_CAP_EXPANSION_I2C,
        .probe_supported = true,
        .binding_specs = oled_binding_specs,
        .binding_spec_count = sizeof(oled_binding_specs) / sizeof(oled_binding_specs[0]),
        .attach = solar_os_ssd1306_attach,
        .detach = solar_os_ssd1306_detach,
    },
    {
        .name = "sh1106",
        .summary = "SH1106 128x64 I2C OLED",
        .required_capabilities = SOLAR_OS_BOARD_CAP_EXPANSION_I2C,
        .probe_supported = true,
        .binding_specs = oled_binding_specs,
        .binding_spec_count = sizeof(oled_binding_specs) / sizeof(oled_binding_specs[0]),
        .attach = solar_os_sh1106_attach,
        .detach = solar_os_ssd1306_detach,
    },
#endif
#if SOLAR_OS_PACKAGE_EXPANSION_CARDKB
    {
        .name = "cardkb",
        .summary = "M5Stack Unit CardKB I2C keyboard",
        .required_capabilities = SOLAR_OS_BOARD_CAP_EXPANSION_I2C,
        .probe_supported = true,
        .binding_specs = cardkb_binding_specs,
        .binding_spec_count = sizeof(cardkb_binding_specs) /
            sizeof(cardkb_binding_specs[0]),
        .attach = solar_os_cardkb_attach,
        .detach = solar_os_cardkb_detach,
    },
#endif
#if SOLAR_OS_PACKAGE_EXPANSION_GPIO_KEYS
    {
        .name = "gpio-keys",
        .summary = "pull-up GPIO keyboard buttons",
        .required_capabilities = SOLAR_OS_BOARD_CAP_EXPANSION_GPIO,
        .probe_supported = false,
        .allow_unlisted_bindings = true,
        .attach = solar_os_gpio_keys_attach,
        .detach = solar_os_gpio_keys_detach,
    },
#endif
#if SOLAR_OS_PACKAGE_EXPANSION_PS2_KEYBOARD
    {
        .name = "ps2-keyboard",
        .summary = "PS/2 keyboard",
        .required_capabilities = SOLAR_OS_BOARD_CAP_EXPANSION_GPIO,
        .probe_supported = false,
        .binding_specs = ps2_device_binding_specs,
        .binding_spec_count = sizeof(ps2_device_binding_specs) /
            sizeof(ps2_device_binding_specs[0]),
        .attach = solar_os_ps2_keyboard_attach,
        .detach = solar_os_ps2_keyboard_detach,
    },
#endif
#if SOLAR_OS_PACKAGE_EXPANSION_PS2_MOUSE
    {
        .name = "ps2-mouse",
        .summary = "PS/2 relative mouse",
        .required_capabilities = SOLAR_OS_BOARD_CAP_EXPANSION_GPIO,
        .probe_supported = false,
        .binding_specs = ps2_device_binding_specs,
        .binding_spec_count = sizeof(ps2_device_binding_specs) /
            sizeof(ps2_device_binding_specs[0]),
        .attach = solar_os_ps2_mouse_attach,
        .detach = solar_os_ps2_mouse_detach,
    },
#endif
#if SOLAR_OS_BOARD_HAS_POINTER
    {
        .name = "ft6336",
        .summary = "FocalTech FT6336 capacitive touchscreen",
        .required_capabilities = SOLAR_OS_BOARD_CAP_POINTER |
            SOLAR_OS_BOARD_CAP_EXPANSION_I2C,
        .probe_supported = true,
        .binding_specs = ft6336_binding_specs,
        .binding_spec_count = sizeof(ft6336_binding_specs) /
            sizeof(ft6336_binding_specs[0]),
        .attach = solar_os_ft6336_attach,
        .detach = solar_os_ft6336_detach,
    },
#endif
#if SOLAR_OS_PACKAGE_EXPANSION_SDSPI && !SOLAR_OS_BOARD_HAS_SD
    {
        .name = "sdspi",
        .summary = "SPI microSD card adapter",
        .required_capabilities = SOLAR_OS_BOARD_CAP_EXPANSION_SPI,
        .probe_supported = true,
        .binding_specs = sdspi_binding_specs,
        .binding_spec_count = sizeof(sdspi_binding_specs) /
            sizeof(sdspi_binding_specs[0]),
        .attach = solar_os_sdspi_attach,
        .detach = solar_os_sdspi_detach,
    },
#endif
#if SOLAR_OS_PACKAGE_EXPANSION_NEOPIXEL
    {
        .name = "neopixel",
        .summary = "WS2812/NeoPixel GRB strip",
        .required_capabilities = SOLAR_OS_BOARD_CAP_EXPANSION_GPIO,
        .probe_supported = false,
        .binding_specs = neopixel_binding_specs,
        .binding_spec_count = sizeof(neopixel_binding_specs) /
            sizeof(neopixel_binding_specs[0]),
        .attach = solar_os_neopixel_attach,
        .detach = solar_os_neopixel_detach,
    },
#endif
#if SOLAR_OS_PACKAGE_EXPANSION_AUDIO_PWM
    {
        .name = "audio-pwm",
        .summary = "LEDC PWM mono audio output",
        .required_capabilities = SOLAR_OS_BOARD_CAP_EXPANSION_PWM,
        .probe_supported = false,
        .binding_specs = audio_pwm_binding_specs,
        .binding_spec_count = sizeof(audio_pwm_binding_specs) /
            sizeof(audio_pwm_binding_specs[0]),
        .attach = solar_os_audio_pwm_attach,
        .detach = solar_os_audio_pwm_detach,
    },
#endif
#if SOLAR_OS_PACKAGE_EXPANSION_PCM5102
    {
        .name = "pcm5102",
        .summary = "PCM5102A I2S stereo audio output",
        .required_capabilities = SOLAR_OS_BOARD_CAP_EXPANSION_I2S,
        .probe_supported = false,
        .binding_specs = pcm5102_binding_specs,
        .binding_spec_count = sizeof(pcm5102_binding_specs) /
            sizeof(pcm5102_binding_specs[0]),
        .attach = solar_os_pcm5102_attach,
        .detach = solar_os_pcm5102_detach,
    },
#endif
};

static solar_os_expansion_device_t devices[SOLAR_OS_EXPANSION_DEVICE_MAX];
typedef enum {
    EXPANSION_SLOT_FREE,
    EXPANSION_SLOT_ATTACHING,
    EXPANSION_SLOT_ACTIVE,
    EXPANSION_SLOT_DETACHING,
} expansion_slot_state_t;

static expansion_slot_state_t device_states[SOLAR_OS_EXPANSION_DEVICE_MAX];
static uint32_t device_generations[SOLAR_OS_EXPANSION_DEVICE_MAX];
static portMUX_TYPE devices_lock = portMUX_INITIALIZER_UNLOCKED;

#if SOLAR_OS_BOARD_DEFAULT_EXPANSION_DEVICE_COUNT > 0
static const solar_os_expansion_default_device_t board_default_devices[] =
    SOLAR_OS_BOARD_DEFAULT_EXPANSION_DEVICES;

_Static_assert(sizeof(board_default_devices) / sizeof(board_default_devices[0]) ==
                   SOLAR_OS_BOARD_DEFAULT_EXPANSION_DEVICE_COUNT,
               "board expansion-device count does not match its definitions");
#endif

static esp_err_t expansion_attach(const char *driver,
                                  const char *name,
                                  const solar_os_expansion_binding_t *bindings,
                                  size_t binding_count,
                                  solar_os_expansion_origin_t origin,
                                  bool autostart,
                                  bool detachable);

static bool mask_contains(uint64_t mask, int pin)
{
    return pin >= 0 && pin < 64 && (mask & (1ULL << (uint32_t)pin)) != 0;
}

static bool device_name_valid(const char *name)
{
    if (name == NULL || name[0] == '\0' || strncmp(name, "bus:", 4) == 0) {
        return false;
    }
    return strnlen(name, SOLAR_OS_EXPANSION_DEVICE_NAME_MAX) < SOLAR_OS_EXPANSION_DEVICE_NAME_MAX;
}

static int find_device_locked(const char *name)
{
    if (name == NULL) {
        return -1;
    }
    for (size_t i = 0; i < SOLAR_OS_EXPANSION_DEVICE_MAX; i++) {
        if (device_states[i] != EXPANSION_SLOT_FREE && strcmp(devices[i].name, name) == 0) {
            return (int)i;
        }
    }
    return -1;
}

static int alloc_device_locked(void)
{
    for (size_t i = 0; i < SOLAR_OS_EXPANSION_DEVICE_MAX; i++) {
        if (device_states[i] == EXPANSION_SLOT_FREE) {
            return (int)i;
        }
    }
    return -1;
}

static uint32_t next_device_generation_locked(size_t index)
{
    device_generations[index]++;
    if (device_generations[index] == 0) {
        device_generations[index]++;
    }
    return device_generations[index];
}

static void release_device_reservation(size_t index, uint32_t generation)
{
    portENTER_CRITICAL(&devices_lock);
    if (index < SOLAR_OS_EXPANSION_DEVICE_MAX &&
        device_generations[index] == generation &&
        device_states[index] == EXPANSION_SLOT_ATTACHING) {
        memset(&devices[index], 0, sizeof(devices[index]));
        device_states[index] = EXPANSION_SLOT_FREE;
    }
    portEXIT_CRITICAL(&devices_lock);
}

static const solar_os_expansion_driver_t *find_driver(const char *name)
{
    if (name == NULL) {
        return NULL;
    }
    for (size_t i = 0; i < sizeof(expansion_drivers) / sizeof(expansion_drivers[0]); i++) {
        if (strcmp(expansion_drivers[i].name, name) == 0) {
            return &expansion_drivers[i];
        }
    }
    return NULL;
}

static bool binding_matches_spec(const solar_os_expansion_binding_t *binding,
                                 const solar_os_expansion_binding_spec_t *spec)
{
    if (binding == NULL || spec == NULL || binding->kind != spec->kind) {
        return false;
    }
    return spec->role == NULL || strcmp(binding->role, spec->role) == 0;
}

static const char *binding_key(const solar_os_expansion_binding_t *binding)
{
    if (binding == NULL) {
        return "resource";
    }
    switch (binding->kind) {
    case SOLAR_OS_EXPANSION_BINDING_GPIO:
    case SOLAR_OS_EXPANSION_BINDING_ADC:
    case SOLAR_OS_EXPANSION_BINDING_PWM:
        return binding->role[0] != '\0' ? binding->role :
            solar_os_expansion_binding_kind_name(binding->kind);
    case SOLAR_OS_EXPANSION_BINDING_I2C_BUS:
        return "i2c";
    case SOLAR_OS_EXPANSION_BINDING_I2C_ADDRESS:
        return "addr";
    case SOLAR_OS_EXPANSION_BINDING_SPI_BUS:
        return "spi";
    case SOLAR_OS_EXPANSION_BINDING_SPI_CS:
        return "cs";
    case SOLAR_OS_EXPANSION_BINDING_UART_PORT:
        return "uart";
    case SOLAR_OS_EXPANSION_BINDING_PS2_BUS:
        return "ps2";
    case SOLAR_OS_EXPANSION_BINDING_SCALAR_STREAM:
        return binding->role[0] != '\0' ? binding->role : "stream";
    case SOLAR_OS_EXPANSION_BINDING_PARAMETER:
        return binding->role[0] != '\0' ? binding->role : "parameter";
    default:
        return "resource";
    }
}

static void set_binding_validation(solar_os_expansion_binding_validation_t *validation,
                                   solar_os_expansion_binding_validation_reason_t reason,
                                   const char *key)
{
    if (validation == NULL) {
        return;
    }
    validation->reason = reason;
    strlcpy(validation->key, key != NULL ? key : "resource", sizeof(validation->key));
}

static bool pin_is_expansion_gpio(int pin)
{
    return solar_os_board_has(SOLAR_OS_BOARD_CAP_EXPANSION_GPIO) &&
        solar_os_pin_is_direct_gpio(pin);
}

static bool pin_is_expansion_adc(int pin)
{
    return solar_os_board_has(SOLAR_OS_BOARD_CAP_EXPANSION_ADC) &&
        mask_contains(SOLAR_OS_BOARD_EXPANSION_ADC_MASK, pin);
}

static bool pin_is_expansion_pwm(int pin)
{
    return solar_os_board_has(SOLAR_OS_BOARD_CAP_EXPANSION_PWM) &&
        mask_contains(SOLAR_OS_BOARD_EXPANSION_PWM_MASK, pin);
}

static bool first_i2c_binding(const solar_os_expansion_binding_t *bindings,
                              size_t binding_count,
                              char *target,
                              size_t target_len,
                              size_t *bus_index)
{
    for (size_t i = 0; i < binding_count; i++) {
        if (bindings[i].kind != SOLAR_OS_EXPANSION_BINDING_I2C_BUS) {
            continue;
        }
        size_t found_index = 0;
        if (!solar_os_expansion_find_i2c_bus(bindings[i].target, NULL, &found_index)) {
            return false;
        }
        if (target != NULL && target_len > 0) {
            strlcpy(target, bindings[i].target, target_len);
        }
        if (bus_index != NULL) {
            *bus_index = found_index;
        }
        return true;
    }
    return false;
}

static esp_err_t append_claim(solar_os_resource_request_t *requests,
                              size_t *request_count,
                              solar_os_resource_kind_t kind,
                              int primary,
                              int secondary,
                              const char *label)
{
    if (requests == NULL || request_count == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (*request_count >= SOLAR_OS_RESOURCE_BUNDLE_MAX) {
        return ESP_ERR_NO_MEM;
    }
    requests[(*request_count)++] = (solar_os_resource_request_t) {
        .kind = kind,
        .primary = primary,
        .secondary = secondary,
        .label = label,
    };
    return ESP_OK;
}

static esp_err_t append_binding_claims(const solar_os_expansion_binding_t *binding,
                                       const solar_os_expansion_binding_t *bindings,
                                       size_t binding_count,
                                       solar_os_resource_request_t *requests,
                                       size_t *request_count)
{
    switch (binding->kind) {
    case SOLAR_OS_EXPANSION_BINDING_GPIO:
        return append_claim(requests,
                            request_count,
                            SOLAR_OS_RESOURCE_GPIO_PIN,
                            binding->value,
                            -1,
                            binding->role);
    case SOLAR_OS_EXPANSION_BINDING_ADC:
        ESP_RETURN_ON_ERROR(append_claim(requests,
                                         request_count,
                                         SOLAR_OS_RESOURCE_ADC_PIN,
                                         binding->value,
                                         -1,
                                         binding->role),
                            "expansion",
                            "append adc claim failed");
        return append_claim(requests,
                            request_count,
                            SOLAR_OS_RESOURCE_GPIO_PIN,
                            binding->value,
                            -1,
                            "adc");
    case SOLAR_OS_EXPANSION_BINDING_PWM:
        ESP_RETURN_ON_ERROR(append_claim(requests,
                                         request_count,
                                         SOLAR_OS_RESOURCE_PWM_PIN,
                                         binding->value,
                                         -1,
                                         binding->role),
                            "expansion",
                            "append pwm claim failed");
        return append_claim(requests,
                            request_count,
                            SOLAR_OS_RESOURCE_GPIO_PIN,
                            binding->value,
                            -1,
                            "pwm");
    case SOLAR_OS_EXPANSION_BINDING_SPI_CS:
        ESP_RETURN_ON_ERROR(append_claim(requests,
                                         request_count,
                                         SOLAR_OS_RESOURCE_SPI_CS,
                                         binding->value,
                                         -1,
                                         binding->target),
                            "expansion",
                            "append spi cs claim failed");
        return append_claim(requests,
                            request_count,
                            SOLAR_OS_RESOURCE_GPIO_PIN,
                            binding->value,
                            -1,
                            "spi-cs");
    case SOLAR_OS_EXPANSION_BINDING_I2C_ADDRESS: {
        char target[SOLAR_OS_EXPANSION_TARGET_MAX] = {0};
        size_t bus_index = 0;
        if (binding->target[0] != '\0') {
            if (!solar_os_expansion_find_i2c_bus(binding->target, NULL, &bus_index)) {
                return ESP_ERR_INVALID_ARG;
            }
        } else if (!first_i2c_binding(bindings, binding_count, target, sizeof(target), &bus_index)) {
            return ESP_ERR_INVALID_ARG;
        }
        return append_claim(requests,
                            request_count,
                            SOLAR_OS_RESOURCE_I2C_ADDRESS,
                            (int)bus_index,
                            binding->value,
                            binding->target[0] != '\0' ? binding->target : "i2c");
    }
    case SOLAR_OS_EXPANSION_BINDING_UART_PORT:
        /* The named UART bus owns its controller; the device owns a bus lease. */
        return ESP_OK;
    case SOLAR_OS_EXPANSION_BINDING_PS2_BUS:
        return ESP_OK;
    case SOLAR_OS_EXPANSION_BINDING_SCALAR_STREAM:
        return ESP_OK;
    case SOLAR_OS_EXPANSION_BINDING_I2C_BUS:
    case SOLAR_OS_EXPANSION_BINDING_SPI_BUS:
    case SOLAR_OS_EXPANSION_BINDING_PARAMETER:
    default:
        return ESP_OK;
    }
}

static bool binding_valid(const solar_os_expansion_binding_t *binding,
                          const solar_os_expansion_binding_t *bindings,
                          size_t binding_count,
                          bool allow_board_pins)
{
    switch (binding->kind) {
    case SOLAR_OS_EXPANSION_BINDING_GPIO:
        return pin_is_expansion_gpio(binding->value) ||
            (allow_board_pins &&
             solar_os_pin_get_info_by_pin(binding->value, NULL));
    case SOLAR_OS_EXPANSION_BINDING_ADC:
        return pin_is_expansion_adc(binding->value) ||
            (allow_board_pins &&
             solar_os_pin_get_info_by_pin(binding->value, NULL));
    case SOLAR_OS_EXPANSION_BINDING_PWM:
        return pin_is_expansion_pwm(binding->value) ||
            (allow_board_pins &&
             solar_os_pin_get_info_by_pin(binding->value, NULL));
    case SOLAR_OS_EXPANSION_BINDING_I2C_BUS:
        return solar_os_expansion_find_i2c_bus(binding->target, NULL, NULL);
    case SOLAR_OS_EXPANSION_BINDING_I2C_ADDRESS:
        if (binding->value < 0x03 || binding->value > 0x77) {
            return false;
        }
        if (binding->target[0] != '\0') {
            return solar_os_expansion_find_i2c_bus(binding->target, NULL, NULL);
        }
        return first_i2c_binding(bindings, binding_count, NULL, 0, NULL);
    case SOLAR_OS_EXPANSION_BINDING_SPI_BUS:
        return solar_os_expansion_find_spi_bus(binding->target, NULL, NULL);
    case SOLAR_OS_EXPANSION_BINDING_SPI_CS:
        return (pin_is_expansion_gpio(binding->value) ||
                (allow_board_pins &&
                 solar_os_pin_get_info_by_pin(binding->value, NULL))) &&
            solar_os_expansion_spi_cs_allowed(binding->target[0] != '\0' ? binding->target : NULL,
                                              binding->value);
    case SOLAR_OS_EXPANSION_BINDING_UART_PORT: {
        solar_os_expansion_uart_port_t port;
        return solar_os_expansion_find_uart_port(binding->target, &port, NULL) &&
            port.port == binding->value;
    }
    case SOLAR_OS_EXPANSION_BINDING_PS2_BUS:
        return solar_os_bus_find(binding->target,
                                 SOLAR_OS_BUS_PROTOCOL_PS2,
                                 NULL);
    case SOLAR_OS_EXPANSION_BINDING_SCALAR_STREAM: {
        solar_os_stream_info_t info;
        return solar_os_stream_get_info(binding->target, &info) == ESP_OK &&
            info.type == SOLAR_OS_STREAM_TYPE_SCALAR &&
            info.direction != SOLAR_OS_STREAM_DIRECTION_SINK;
    }
    case SOLAR_OS_EXPANSION_BINDING_PARAMETER:
        return binding->role[0] != '\0';
    default:
        return false;
    }
}

typedef struct {
    solar_os_bus_protocol_t protocol;
    const char *name;
} expansion_bus_ref_t;

static bool binding_bus_ref(const solar_os_expansion_binding_t *binding,
                            expansion_bus_ref_t *ref)
{
    if (binding == NULL || ref == NULL) {
        return false;
    }
    switch (binding->kind) {
    case SOLAR_OS_EXPANSION_BINDING_I2C_BUS:
        *ref = (expansion_bus_ref_t) {
            .protocol = SOLAR_OS_BUS_PROTOCOL_I2C,
            .name = binding->target,
        };
        return true;
    case SOLAR_OS_EXPANSION_BINDING_I2C_ADDRESS:
        if (binding->target[0] == '\0') {
            return false;
        }
        *ref = (expansion_bus_ref_t) {
            .protocol = SOLAR_OS_BUS_PROTOCOL_I2C,
            .name = binding->target,
        };
        return true;
    case SOLAR_OS_EXPANSION_BINDING_SPI_BUS:
        *ref = (expansion_bus_ref_t) {
            .protocol = SOLAR_OS_BUS_PROTOCOL_SPI,
            .name = binding->target,
        };
        return true;
    case SOLAR_OS_EXPANSION_BINDING_UART_PORT:
        *ref = (expansion_bus_ref_t) {
            .protocol = SOLAR_OS_BUS_PROTOCOL_UART,
            .name = binding->target,
        };
        return true;
    case SOLAR_OS_EXPANSION_BINDING_PS2_BUS:
        *ref = (expansion_bus_ref_t) {
            .protocol = SOLAR_OS_BUS_PROTOCOL_PS2,
            .name = binding->target,
        };
        return true;
    default:
        return false;
    }
}

static esp_err_t acquire_binding_buses(const solar_os_expansion_binding_t *bindings,
                                       size_t binding_count,
                                       const char *owner)
{
    expansion_bus_ref_t acquired[SOLAR_OS_EXPANSION_DEVICE_BINDING_MAX];
    size_t acquired_count = 0;

    for (size_t i = 0; i < binding_count; i++) {
        expansion_bus_ref_t ref;
        if (!binding_bus_ref(&bindings[i], &ref)) {
            continue;
        }
        bool duplicate = false;
        for (size_t j = 0; j < acquired_count; j++) {
            if (acquired[j].protocol == ref.protocol &&
                strcmp(acquired[j].name, ref.name) == 0) {
                duplicate = true;
                break;
            }
        }
        if (duplicate) {
            continue;
        }

        const esp_err_t ret = solar_os_bus_acquire(ref.name, ref.protocol, owner);
        if (ret != ESP_OK) {
            (void)solar_os_bus_release_owner(owner);
            return ret;
        }
        acquired[acquired_count++] = ref;
    }
    return ESP_OK;
}

esp_err_t solar_os_expansion_init(void)
{
    ESP_RETURN_ON_ERROR(solar_os_resources_init(), "expansion", "resource init failed");
    ESP_RETURN_ON_ERROR(solar_os_buses_init(), "expansion", "bus init failed");
#if SOLAR_OS_BOARD_DEFAULT_EXPANSION_DEVICE_COUNT > 0
    for (size_t i = 0; i < SOLAR_OS_BOARD_DEFAULT_EXPANSION_DEVICE_COUNT; i++) {
        const solar_os_expansion_default_device_t *device = &board_default_devices[i];
        ESP_RETURN_ON_ERROR(expansion_attach(device->driver,
                                             device->name,
                                             device->bindings,
                                             device->binding_count,
                                             SOLAR_OS_EXPANSION_ORIGIN_BOARD,
                                             true,
                                             false),
                            "expansion",
                            "board device attach failed");
    }
#endif
    return ESP_OK;
}

bool solar_os_expansion_available(void)
{
    return solar_os_board_has(SOLAR_OS_BOARD_CAP_EXPANSION_GPIO) ||
        solar_os_bus_count_protocol(SOLAR_OS_BUS_PROTOCOL_I2C) > 0 ||
        solar_os_bus_count_protocol(SOLAR_OS_BUS_PROTOCOL_SPI) > 0 ||
        solar_os_bus_count_protocol(SOLAR_OS_BUS_PROTOCOL_UART) > 0 ||
        solar_os_bus_count_protocol(SOLAR_OS_BUS_PROTOCOL_MIDI) > 0 ||
        solar_os_bus_count_protocol(SOLAR_OS_BUS_PROTOCOL_ONEWIRE) > 0 ||
        solar_os_bus_count_protocol(SOLAR_OS_BUS_PROTOCOL_PS2) > 0 ||
        solar_os_board_has(SOLAR_OS_BOARD_CAP_EXPANSION_ADC) ||
        solar_os_board_has(SOLAR_OS_BOARD_CAP_EXPANSION_PWM);
}

size_t solar_os_expansion_driver_count(void)
{
    return sizeof(expansion_drivers) / sizeof(expansion_drivers[0]);
}

bool solar_os_expansion_get_driver(size_t index, solar_os_expansion_driver_t *driver)
{
    if (driver == NULL || index >= solar_os_expansion_driver_count()) {
        return false;
    }
    *driver = expansion_drivers[index];
    return true;
}

bool solar_os_expansion_driver_supported(const char *name)
{
    const solar_os_expansion_driver_t *driver = find_driver(name);
    if (driver == NULL) {
        return false;
    }
    solar_os_board_capabilities_t caps = solar_os_board_capabilities();
    if (solar_os_bus_count_protocol(SOLAR_OS_BUS_PROTOCOL_I2C) > 0) {
        caps |= SOLAR_OS_BOARD_CAP_EXPANSION_I2C;
    }
    if (solar_os_bus_count_protocol(SOLAR_OS_BUS_PROTOCOL_SPI) > 0) {
        caps |= SOLAR_OS_BOARD_CAP_EXPANSION_SPI;
    }
    if (solar_os_bus_count_protocol(SOLAR_OS_BUS_PROTOCOL_UART) > 0 ||
        solar_os_bus_count_protocol(SOLAR_OS_BUS_PROTOCOL_MIDI) > 0) {
        caps |= SOLAR_OS_BOARD_CAP_EXPANSION_UART;
    }
    return driver->required_capabilities == 0 ||
        (caps & driver->required_capabilities) == driver->required_capabilities;
}

static esp_err_t validate_bindings(
    const char *driver,
    const solar_os_expansion_binding_t *bindings,
    size_t binding_count,
    solar_os_expansion_binding_validation_t *validation,
    bool allow_board_pins)
{
    if (validation != NULL) {
        memset(validation, 0, sizeof(*validation));
        validation->reason = SOLAR_OS_EXPANSION_BINDINGS_VALID;
    }
    if (driver == NULL || driver[0] == '\0' ||
        binding_count > SOLAR_OS_EXPANSION_DEVICE_BINDING_MAX ||
        (binding_count > 0 && bindings == NULL)) {
        return ESP_ERR_INVALID_ARG;
    }

    const solar_os_expansion_driver_t *driver_def = find_driver(driver);
    if (driver_def == NULL) {
        return ESP_ERR_NOT_FOUND;
    }
    if (driver_def->allow_unlisted_bindings && binding_count == 0) {
        set_binding_validation(validation, SOLAR_OS_EXPANSION_BINDINGS_MISSING, "resource");
        return ESP_ERR_INVALID_ARG;
    }

    for (size_t i = 0; i < binding_count; i++) {
        const solar_os_expansion_binding_spec_t *matched = NULL;
        size_t matches = 0;
        for (size_t s = 0; s < driver_def->binding_spec_count; s++) {
            const solar_os_expansion_binding_spec_t *spec = &driver_def->binding_specs[s];
            if (!binding_matches_spec(&bindings[i], spec)) {
                continue;
            }
            matched = spec;
            for (size_t j = 0; j < i; j++) {
                if (binding_matches_spec(&bindings[j], spec)) {
                    matches++;
                }
            }
            break;
        }
        if (matched == NULL &&
            (!driver_def->allow_unlisted_bindings ||
             bindings[i].kind == SOLAR_OS_EXPANSION_BINDING_PARAMETER)) {
            set_binding_validation(validation,
                                   SOLAR_OS_EXPANSION_BINDINGS_UNEXPECTED,
                                   binding_key(&bindings[i]));
            return ESP_ERR_INVALID_ARG;
        }
        if (matched != NULL && matches > 0) {
            set_binding_validation(validation,
                                   SOLAR_OS_EXPANSION_BINDINGS_DUPLICATE,
                                   matched->key);
            return ESP_ERR_INVALID_ARG;
        }
        if (matched != NULL && matched->allowed_value_count > 0) {
            bool allowed = false;
            for (size_t v = 0; v < matched->allowed_value_count; v++) {
                if (bindings[i].value == matched->allowed_values[v]) {
                    allowed = true;
                    break;
                }
            }
            if (!allowed) {
                set_binding_validation(validation,
                                       SOLAR_OS_EXPANSION_BINDINGS_INVALID_VALUE,
                                       matched->key);
                return ESP_ERR_INVALID_ARG;
            }
        }
        if (matched != NULL && matched->has_value_range &&
            (bindings[i].value < matched->min_value ||
             bindings[i].value > matched->max_value)) {
            set_binding_validation(validation,
                                   SOLAR_OS_EXPANSION_BINDINGS_INVALID_VALUE,
                                   matched->key);
            return ESP_ERR_INVALID_ARG;
        }
    }

    for (size_t s = 0; s < driver_def->binding_spec_count; s++) {
        const solar_os_expansion_binding_spec_t *spec = &driver_def->binding_specs[s];
        if (!spec->required) {
            continue;
        }
        bool found = false;
        for (size_t i = 0; i < binding_count; i++) {
            if (binding_matches_spec(&bindings[i], spec)) {
                found = true;
                break;
            }
        }
        if (!found) {
            set_binding_validation(validation,
                                   SOLAR_OS_EXPANSION_BINDINGS_MISSING,
                                   spec->key);
            return ESP_ERR_INVALID_ARG;
        }
    }

    for (size_t i = 0; i < binding_count; i++) {
        if (!binding_valid(&bindings[i],
                           bindings,
                           binding_count,
                           allow_board_pins)) {
            set_binding_validation(validation,
                                   SOLAR_OS_EXPANSION_BINDINGS_UNAVAILABLE,
                                   binding_key(&bindings[i]));
            return ESP_ERR_INVALID_ARG;
        }
    }
    return ESP_OK;
}

esp_err_t solar_os_expansion_validate_bindings(
    const char *driver,
    const solar_os_expansion_binding_t *bindings,
    size_t binding_count,
    solar_os_expansion_binding_validation_t *validation)
{
    return validate_bindings(driver,
                             bindings,
                             binding_count,
                             validation,
                             false);
}

size_t solar_os_expansion_i2c_bus_count(void)
{
    return solar_os_bus_count_protocol(SOLAR_OS_BUS_PROTOCOL_I2C);
}

bool solar_os_expansion_get_i2c_bus(size_t index, solar_os_expansion_i2c_bus_t *bus)
{
    if (bus == NULL) {
        return false;
    }
    solar_os_bus_info_t info;
    if (!solar_os_bus_get_protocol(SOLAR_OS_BUS_PROTOCOL_I2C, index, &info)) {
        return false;
    }
    *bus = (solar_os_expansion_i2c_bus_t) {
        .port = info.config.i2c.port,
        .sda_pin = info.config.i2c.sda_pin,
        .scl_pin = info.config.i2c.scl_pin,
        .speed_hz = info.config.i2c.speed_hz,
    };
    strlcpy(bus->name, info.name, sizeof(bus->name));
    return true;
}

bool solar_os_expansion_find_i2c_bus(const char *name, solar_os_expansion_i2c_bus_t *bus, size_t *index)
{
    solar_os_bus_info_t info;
    if (!solar_os_bus_find(name, SOLAR_OS_BUS_PROTOCOL_I2C, &info)) {
        return false;
    }
    if (bus != NULL) {
        *bus = (solar_os_expansion_i2c_bus_t) {
            .port = info.config.i2c.port,
            .sda_pin = info.config.i2c.sda_pin,
            .scl_pin = info.config.i2c.scl_pin,
            .speed_hz = info.config.i2c.speed_hz,
        };
        strlcpy(bus->name, info.name, sizeof(bus->name));
    }
    if (index != NULL) {
        *index = info.id;
    }
    return true;
}

size_t solar_os_expansion_spi_bus_count(void)
{
    return solar_os_bus_count_protocol(SOLAR_OS_BUS_PROTOCOL_SPI);
}

bool solar_os_expansion_get_spi_bus(size_t index, solar_os_expansion_spi_bus_t *bus)
{
    if (bus == NULL) {
        return false;
    }
    solar_os_bus_info_t info;
    if (!solar_os_bus_get_protocol(SOLAR_OS_BUS_PROTOCOL_SPI, index, &info)) {
        return false;
    }
    *bus = (solar_os_expansion_spi_bus_t) {
        .host = info.config.spi.host,
        .sclk_pin = info.config.spi.sclk_pin,
        .miso_pin = info.config.spi.miso_pin,
        .mosi_pin = info.config.spi.mosi_pin,
        .max_transfer_size = info.config.spi.max_transfer_size,
        .cs_count = info.config.spi.cs_count,
    };
    strlcpy(bus->name, info.name, sizeof(bus->name));
    for (size_t i = 0; i < info.config.spi.cs_count && i < SOLAR_OS_EXPANSION_SPI_CS_MAX; i++) {
        bus->cs[i] = info.config.spi.cs[i];
    }
    return true;
}

bool solar_os_expansion_find_spi_bus(const char *name, solar_os_expansion_spi_bus_t *bus, size_t *index)
{
    solar_os_bus_info_t info;
    if (!solar_os_bus_find(name, SOLAR_OS_BUS_PROTOCOL_SPI, &info)) {
        return false;
    }
    if (bus != NULL) {
        *bus = (solar_os_expansion_spi_bus_t) {
            .host = info.config.spi.host,
            .sclk_pin = info.config.spi.sclk_pin,
            .miso_pin = info.config.spi.miso_pin,
            .mosi_pin = info.config.spi.mosi_pin,
            .max_transfer_size = info.config.spi.max_transfer_size,
            .cs_count = info.config.spi.cs_count,
        };
        strlcpy(bus->name, info.name, sizeof(bus->name));
        for (size_t i = 0; i < info.config.spi.cs_count && i < SOLAR_OS_EXPANSION_SPI_CS_MAX; i++) {
            bus->cs[i] = info.config.spi.cs[i];
        }
    }
    if (index != NULL) {
        *index = info.id;
    }
    return true;
}

bool solar_os_expansion_spi_cs_allowed(const char *bus_name, int pin)
{
    for (size_t i = 0; i < solar_os_bus_count_protocol(SOLAR_OS_BUS_PROTOCOL_SPI); i++) {
        solar_os_bus_info_t bus;
        if (!solar_os_bus_get_protocol(SOLAR_OS_BUS_PROTOCOL_SPI, i, &bus) ||
            (bus_name != NULL && strcmp(bus.name, bus_name) != 0)) {
            continue;
        }
        for (size_t cs = 0;
             cs < bus.config.spi.cs_count && cs < SOLAR_OS_EXPANSION_SPI_CS_MAX;
             cs++) {
            if (bus.config.spi.cs[cs].pin == pin) {
                return true;
            }
        }
    }
    return false;
}

size_t solar_os_expansion_uart_port_count(void)
{
    return solar_os_bus_count_protocol(SOLAR_OS_BUS_PROTOCOL_UART);
}

bool solar_os_expansion_get_uart_port(size_t index, solar_os_expansion_uart_port_t *port)
{
    if (port == NULL) {
        return false;
    }
    solar_os_bus_info_t info;
    if (!solar_os_bus_get_protocol(SOLAR_OS_BUS_PROTOCOL_UART, index, &info)) {
        return false;
    }
    *port = (solar_os_expansion_uart_port_t) {
        .port = info.config.uart.port,
        .tx_pin = info.config.uart.tx_pin,
        .rx_pin = info.config.uart.rx_pin,
        .baud_rate = info.config.uart.baud_rate,
    };
    strlcpy(port->name, info.name, sizeof(port->name));
    return true;
}

bool solar_os_expansion_find_uart_port(const char *name, solar_os_expansion_uart_port_t *port, size_t *index)
{
    solar_os_bus_info_t info;
    if (!solar_os_bus_find(name, SOLAR_OS_BUS_PROTOCOL_UART, &info)) {
        return false;
    }
    if (port != NULL) {
        *port = (solar_os_expansion_uart_port_t) {
            .port = info.config.uart.port,
            .tx_pin = info.config.uart.tx_pin,
            .rx_pin = info.config.uart.rx_pin,
            .baud_rate = info.config.uart.baud_rate,
        };
        strlcpy(port->name, info.name, sizeof(port->name));
    }
    if (index != NULL) {
        *index = info.id;
    }
    return true;
}

size_t solar_os_expansion_onewire_bus_count(void)
{
    return solar_os_bus_count_protocol(SOLAR_OS_BUS_PROTOCOL_ONEWIRE);
}

bool solar_os_expansion_get_onewire_bus(size_t index, solar_os_expansion_onewire_bus_t *bus)
{
    if (bus == NULL) {
        return false;
    }
    solar_os_bus_info_t info;
    if (!solar_os_bus_get_protocol(SOLAR_OS_BUS_PROTOCOL_ONEWIRE, index, &info)) {
        return false;
    }
    *bus = (solar_os_expansion_onewire_bus_t) {
        .pin = info.config.onewire.pin,
    };
    strlcpy(bus->name, info.name, sizeof(bus->name));
    return true;
}

bool solar_os_expansion_find_onewire_bus(const char *name,
                                         solar_os_expansion_onewire_bus_t *bus,
                                         size_t *index)
{
    solar_os_bus_info_t info;
    if (!solar_os_bus_find(name, SOLAR_OS_BUS_PROTOCOL_ONEWIRE, &info)) {
        return false;
    }
    if (bus != NULL) {
        *bus = (solar_os_expansion_onewire_bus_t) {
            .pin = info.config.onewire.pin,
        };
        strlcpy(bus->name, info.name, sizeof(bus->name));
    }
    if (index != NULL) {
        *index = info.id;
    }
    return true;
}

static esp_err_t expansion_attach(const char *driver,
                                  const char *name,
                                  const solar_os_expansion_binding_t *bindings,
                                  size_t binding_count,
                                  solar_os_expansion_origin_t origin,
                                  bool autostart,
                                  bool detachable)
{
    if (driver == NULL || driver[0] == '\0' ||
        !device_name_valid(name) ||
        binding_count > SOLAR_OS_EXPANSION_DEVICE_BINDING_MAX ||
        (binding_count > 0 && bindings == NULL)) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!solar_os_expansion_available()) {
        return ESP_ERR_NOT_SUPPORTED;
    }
    const solar_os_expansion_driver_t *driver_def = find_driver(driver);
    if (driver_def == NULL || !solar_os_expansion_driver_supported(driver)) {
        return ESP_ERR_NOT_FOUND;
    }
    ESP_RETURN_ON_ERROR(validate_bindings(driver,
                                          bindings,
                                          binding_count,
                                          NULL,
                                          origin == SOLAR_OS_EXPANSION_ORIGIN_BOARD),
                        "expansion",
                        "invalid bindings");
    solar_os_expansion_binding_t normalized[SOLAR_OS_EXPANSION_DEVICE_BINDING_MAX];
    for (size_t i = 0; i < binding_count; i++) {
        normalized[i] = bindings[i];
    }
    solar_os_resource_request_t requests[SOLAR_OS_RESOURCE_BUNDLE_MAX];
    size_t request_count = 0;
    for (size_t i = 0; i < binding_count; i++) {
        const esp_err_t ret = append_binding_claims(&normalized[i],
                                                    normalized,
                                                    binding_count,
                                                    requests,
                                                    &request_count);
        if (ret != ESP_OK) {
            return ret;
        }
    }

    size_t device_index = 0;
    uint32_t generation = 0;
    portENTER_CRITICAL(&devices_lock);
    if (find_device_locked(name) >= 0) {
        portEXIT_CRITICAL(&devices_lock);
        return ESP_ERR_INVALID_STATE;
    }
    const int free_index = alloc_device_locked();
    if (free_index < 0) {
        portEXIT_CRITICAL(&devices_lock);
        return ESP_ERR_NO_MEM;
    }
    device_index = (size_t)free_index;
    generation = next_device_generation_locked(device_index);
    memset(&devices[device_index], 0, sizeof(devices[device_index]));
    strlcpy(devices[device_index].name, name, sizeof(devices[device_index].name));
    strlcpy(devices[device_index].driver, driver, sizeof(devices[device_index].driver));
    devices[device_index].origin = origin;
    devices[device_index].autostart = autostart;
    devices[device_index].detachable = detachable;
    devices[device_index].binding_count = binding_count;
    memcpy(devices[device_index].bindings,
           normalized,
           binding_count * sizeof(normalized[0]));
    device_states[device_index] = EXPANSION_SLOT_ATTACHING;
    portEXIT_CRITICAL(&devices_lock);

    if (request_count > 0) {
        const esp_err_t ret = solar_os_resource_claim_bundle(requests,
                                                             request_count,
                                                             name,
                                                             NULL);
        if (ret != ESP_OK) {
            release_device_reservation(device_index, generation);
            return ret;
        }
    }

    const esp_err_t bus_ret = acquire_binding_buses(normalized, binding_count, name);
    if (bus_ret != ESP_OK) {
        (void)solar_os_resource_release_owner(name);
        release_device_reservation(device_index, generation);
        return bus_ret;
    }

    if (driver_def->attach != NULL) {
        const esp_err_t ret = driver_def->attach(name, normalized, binding_count);
        if (ret != ESP_OK) {
            (void)solar_os_bus_release_owner(name);
            (void)solar_os_resource_release_owner(name);
            release_device_reservation(device_index, generation);
            return ret;
        }
    }

    portENTER_CRITICAL(&devices_lock);
    if (device_generations[device_index] != generation ||
        device_states[device_index] != EXPANSION_SLOT_ATTACHING) {
        portEXIT_CRITICAL(&devices_lock);
        (void)solar_os_bus_release_owner(name);
        (void)solar_os_resource_release_owner(name);
        return ESP_ERR_INVALID_STATE;
    }
    devices[device_index].active = true;
    devices[device_index].ready = true;
    device_states[device_index] = EXPANSION_SLOT_ACTIVE;
    portEXIT_CRITICAL(&devices_lock);
    return ESP_OK;
}

esp_err_t solar_os_expansion_attach(const char *driver,
                                    const char *name,
                                    const solar_os_expansion_binding_t *bindings,
                                    size_t binding_count)
{
    return expansion_attach(driver,
                            name,
                            bindings,
                            binding_count,
                            SOLAR_OS_EXPANSION_ORIGIN_RUNTIME,
                            false,
                            true);
}

esp_err_t solar_os_expansion_detach(const char *name)
{
    solar_os_expansion_device_t device;
    size_t device_index = 0;
    uint32_t generation = 0;

    portENTER_CRITICAL(&devices_lock);
    const int found_index = find_device_locked(name);
    if (found_index < 0) {
        portEXIT_CRITICAL(&devices_lock);
        return ESP_ERR_NOT_FOUND;
    }
    device_index = (size_t)found_index;
    if (device_states[device_index] != EXPANSION_SLOT_ACTIVE) {
        portEXIT_CRITICAL(&devices_lock);
        return ESP_ERR_INVALID_STATE;
    }
    if (!devices[device_index].detachable) {
        portEXIT_CRITICAL(&devices_lock);
        return ESP_ERR_NOT_SUPPORTED;
    }
    generation = device_generations[device_index];
    device = devices[device_index];
    devices[device_index].active = false;
    device_states[device_index] = EXPANSION_SLOT_DETACHING;
    portEXIT_CRITICAL(&devices_lock);

    const solar_os_expansion_driver_t *driver = find_driver(device.driver);
    if (driver != NULL && driver->detach != NULL) {
        const esp_err_t ret = driver->detach(name);
        if (ret != ESP_OK) {
            portENTER_CRITICAL(&devices_lock);
            if (device_generations[device_index] == generation &&
                device_states[device_index] == EXPANSION_SLOT_DETACHING) {
                devices[device_index].active = true;
                device_states[device_index] = EXPANSION_SLOT_ACTIVE;
            }
            portEXIT_CRITICAL(&devices_lock);
            return ret;
        }
    }

    (void)solar_os_bus_release_owner(name);
    (void)solar_os_resource_release_owner(name);
    portENTER_CRITICAL(&devices_lock);
    if (device_generations[device_index] == generation &&
        device_states[device_index] == EXPANSION_SLOT_DETACHING) {
        memset(&devices[device_index], 0, sizeof(devices[device_index]));
        device_states[device_index] = EXPANSION_SLOT_FREE;
    }
    portEXIT_CRITICAL(&devices_lock);
    return ESP_OK;
}

esp_err_t solar_os_expansion_device_set_ready(const char *name, bool ready)
{
    if (!device_name_valid(name)) {
        return ESP_ERR_INVALID_ARG;
    }

    esp_err_t result = ESP_OK;
    portENTER_CRITICAL(&devices_lock);
    const int found_index = find_device_locked(name);
    if (found_index < 0) {
        result = ESP_ERR_NOT_FOUND;
    } else if (device_states[found_index] != EXPANSION_SLOT_ACTIVE) {
        result = ESP_ERR_INVALID_STATE;
    } else {
        devices[found_index].ready = ready;
    }
    portEXIT_CRITICAL(&devices_lock);
    return result;
}

size_t solar_os_expansion_device_count(void)
{
    size_t count = 0;
    portENTER_CRITICAL(&devices_lock);
    for (size_t i = 0; i < SOLAR_OS_EXPANSION_DEVICE_MAX; i++) {
        if (device_states[i] == EXPANSION_SLOT_ACTIVE) {
            count++;
        }
    }
    portEXIT_CRITICAL(&devices_lock);
    return count;
}

bool solar_os_expansion_get_device(size_t index, solar_os_expansion_device_t *device)
{
    size_t current = 0;
    if (device == NULL) {
        return false;
    }
    portENTER_CRITICAL(&devices_lock);
    for (size_t i = 0; i < SOLAR_OS_EXPANSION_DEVICE_MAX; i++) {
        if (device_states[i] != EXPANSION_SLOT_ACTIVE) {
            continue;
        }
        if (current++ == index) {
            *device = devices[i];
            portEXIT_CRITICAL(&devices_lock);
            return true;
        }
    }
    portEXIT_CRITICAL(&devices_lock);
    return false;
}

const char *solar_os_expansion_binding_kind_name(solar_os_expansion_binding_kind_t kind)
{
    switch (kind) {
    case SOLAR_OS_EXPANSION_BINDING_GPIO:
        return "gpio";
    case SOLAR_OS_EXPANSION_BINDING_ADC:
        return "adc";
    case SOLAR_OS_EXPANSION_BINDING_PWM:
        return "pwm";
    case SOLAR_OS_EXPANSION_BINDING_I2C_BUS:
        return "i2c";
    case SOLAR_OS_EXPANSION_BINDING_I2C_ADDRESS:
        return "i2c_addr";
    case SOLAR_OS_EXPANSION_BINDING_SPI_BUS:
        return "spi";
    case SOLAR_OS_EXPANSION_BINDING_SPI_CS:
        return "spi_cs";
    case SOLAR_OS_EXPANSION_BINDING_UART_PORT:
        return "uart";
    case SOLAR_OS_EXPANSION_BINDING_PS2_BUS:
        return "ps2";
    case SOLAR_OS_EXPANSION_BINDING_SCALAR_STREAM:
        return "scalar";
    case SOLAR_OS_EXPANSION_BINDING_PARAMETER:
        return "parameter";
    default:
        return "unknown";
    }
}

const char *solar_os_expansion_origin_name(solar_os_expansion_origin_t origin)
{
    return origin == SOLAR_OS_EXPANSION_ORIGIN_RUNTIME ? "runtime" : "board";
}
