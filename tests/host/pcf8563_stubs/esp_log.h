#pragma once

#include <stdbool.h>

void rtc_pcf8563_test_log(bool error, const char *tag, const char *format, ...);

#define ESP_LOGE(...) rtc_pcf8563_test_log(true, __VA_ARGS__)
#define ESP_LOGW(...) rtc_pcf8563_test_log(false, __VA_ARGS__)
