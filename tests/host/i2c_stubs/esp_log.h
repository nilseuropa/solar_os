#pragma once
#include <stdio.h>
#define ESP_LOGI(tag, format, ...) \
    do { (void)(tag); if (0) fprintf(stderr, format, ##__VA_ARGS__); } while (0)
