#include "solar_os_mpu6050.h"
#include "mpu6050.h"

static const int addresses[] = { MPU6050_I2C_ADDRESS_LOW, MPU6050_I2C_ADDRESS_HIGH };
static const solar_os_expansion_binding_spec_t specs[] = {
    {.key="i2c", .value_hint="bus", .kind=SOLAR_OS_EXPANSION_BINDING_I2C_BUS, .required=true},
    {.key="addr", .value_hint="0x68|0x69", .kind=SOLAR_OS_EXPANSION_BINDING_I2C_ADDRESS,
     .required=true, .allowed_values=addresses, .allowed_value_count=2},
    {.key="irq", .value_hint="gpio", .kind=SOLAR_OS_EXPANSION_BINDING_GPIO, .role="irq"},
};
const solar_os_expansion_driver_t solar_os_mpu6050_expansion_driver = {
    .name="mpu6050", .category=SOLAR_OS_EXPANSION_CATEGORY_SENSOR,
    .summary="MPU6050 six-axis IMU", .required_capabilities=SOLAR_OS_BOARD_CAP_I2C,
    .probe_supported=true, .binding_specs=specs, .binding_spec_count=3,
    .attach=solar_os_mpu6050_attach, .detach=solar_os_mpu6050_detach,
};
