#pragma once
#include "solar_os_native_sscma_abi.h"

/* Internal camera control payload; camera lock held throughout dispatch. */
typedef struct {
    const char *source, *command, *response_name;
    bool event;
    uint32_t timeout_ms;
    solar_os_native_cancel_fn cancel;
    void *user;
    char **reply;
} solar_os_sscma_exchange_t;

#define SOLAR_OS_SSCMA_SPI_PROTOCOL "camera.sscma.spi"
typedef struct {
    const void *tx;
    void *rx;
    size_t length;
    bool reset;
} solar_os_sscma_spi_t;
