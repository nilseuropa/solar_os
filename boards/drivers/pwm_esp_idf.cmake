set(SOLAR_OS_BOARD_PWM_DRIVER "esp_idf")
list(APPEND SOLAR_OS_BOARD_SRCS
    "drivers/pwm_port.c"
)
list(APPEND SOLAR_OS_BOARD_REQUIRES
    esp_driver_gpio
    esp_driver_ledc
)
