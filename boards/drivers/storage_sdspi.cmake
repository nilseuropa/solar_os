set(SOLAR_OS_BOARD_STORAGE_DRIVER "sdspi")
include("${CMAKE_CURRENT_LIST_DIR}/spi_esp_idf.cmake")
list(APPEND SOLAR_OS_BOARD_SRCS
    "board/solar_os_board_storage_sd.c"
    "drivers/sd_card.c"
    "drivers/spi_bus.c"
)
list(APPEND SOLAR_OS_BOARD_REQUIRES
    esp_driver_gpio
    esp_driver_sdspi
    fatfs
    sdmmc
)
