#pragma once
#include <stddef.h>
#include <stdint.h>
uint32_t esp_random(void);
void esp_fill_random(void *buf, size_t len);
