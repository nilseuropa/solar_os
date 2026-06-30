set(SOLAR_OS_BOARD_STORAGE_DRIVER "sdmmc")
list(APPEND SOLAR_OS_BOARD_SRCS
    "board/solar_os_board_storage_sdmmc.c"
    "drivers/sd_card.c"
)
list(APPEND SOLAR_OS_BOARD_REQUIRES
    esp_driver_gpio
    esp_driver_sdmmc
    fatfs
    sdmmc
)
