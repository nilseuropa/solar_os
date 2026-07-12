# AW88298 class-D speaker amp, output only. CoreS3's mic (ES7210, same
# I2C/I2S bus) is a separate codec not wired by this fragment yet. Amp
# power is gated by the AW9523B I2C expander's P0.2 pin
# (io_expander_aw9523b_set_speaker_enable()), not a raw ESP32 GPIO, so
# boards including this fragment must also include
# drivers/io_expander_aw9523b.cmake and drivers/i2c_esp_idf.cmake.
set(SOLAR_OS_BOARD_AUDIO_DRIVER "aw88298")
set(SOLAR_OS_BOARD_AUDIO_NEEDS_I2C ON)
list(APPEND SOLAR_OS_BOARD_SRCS
    "board/solar_os_board_audio_aw88298.c"
    "drivers/audio_aw88298_board.c"
)
list(APPEND SOLAR_OS_BOARD_REQUIRES
    esp_codec_dev
    esp_driver_i2s
)
