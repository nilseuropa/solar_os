idf_component_register(SRCS "main.c" INCLUDE_DIRS
    "${SOLAR_ROOT}/include"
    "${SOLAR_ROOT}/modules/examples/hello-library/main"
    REQUIRES espressif__elf_loader)
target_add_binary_data(${COMPONENT_LIB} "${SOLAR_ROOT}/.pio/tinycc/library_probe.elf" BINARY RENAME_TO library_probe)
target_add_binary_data(${COMPONENT_LIB} "${SOLAR_ROOT}/.pio/tinycc/scalar_probe.elf" BINARY RENAME_TO scalar_probe)
target_add_binary_data(${COMPONENT_LIB} "${SOLAR_ROOT}/.pio/tinycc/hello-library.elf" BINARY RENAME_TO provider)
target_add_binary_data(${COMPONENT_LIB} "${SOLAR_ROOT}/.pio/tinycc/library-user.elf" BINARY RENAME_TO worker)
