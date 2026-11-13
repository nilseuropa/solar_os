idf_component_register(SRCS "main.c" INCLUDE_DIRS
    "${SOLAR_ROOT}/include"
    "${SOLAR_ROOT}/modules/experimental/tinycc/include"
    "${SOLAR_ROOT}/modules/examples/hello-library/main"
    "${SOLAR_ROOT}/.pio/tinycc/native"
    REQUIRES espressif__elf_loader esp_timer)
target_add_binary_data(${COMPONENT_LIB} "${SOLAR_ROOT}/.pio/tinycc/native/tinycc.library.elf" BINARY RENAME_TO compiler)
target_add_binary_data(${COMPONENT_LIB} "${SOLAR_ROOT}/.pio/tinycc/native/cc.app.elf" BINARY RENAME_TO client)
if(SOLAR_CC_DIFFERENTIAL)
    target_compile_definitions(${COMPONENT_LIB} PRIVATE SOLAR_CC_DIFFERENTIAL=1)
    target_include_directories(${COMPONENT_LIB} PRIVATE
        "${SOLAR_ROOT}/.pio/tinycc/differential"
        "${SOLAR_ROOT}/src/services")
    target_sources(${COMPONENT_LIB} PRIVATE
        "${SOLAR_ROOT}/modules/experimental/tinycc/differential/runner.c"
        "${SOLAR_ROOT}/src/services/solar_os_native_elf.c")
endif()
