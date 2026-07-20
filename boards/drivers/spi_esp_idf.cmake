if(NOT SOLAR_OS_BOARD_SPI_DRIVER)
    set(SOLAR_OS_BOARD_SPI_DRIVER "esp_idf")
    list(APPEND SOLAR_OS_BOARD_REQUIRES
        esp_driver_gpio
        esp_driver_spi
    )
endif()
