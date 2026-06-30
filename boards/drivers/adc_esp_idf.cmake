set(SOLAR_OS_BOARD_ADC_DRIVER "esp_idf")
list(APPEND SOLAR_OS_BOARD_SRCS
    "drivers/adc_port.c"
)
list(APPEND SOLAR_OS_BOARD_REQUIRES
    esp_adc
    esp_driver_gpio
)
