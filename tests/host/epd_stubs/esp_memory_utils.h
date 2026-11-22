#pragma once
#include <stdbool.h>
bool esp_ptr_external_ram(const void *pointer);
bool esp_ptr_dma_ext_capable(const void *pointer);
