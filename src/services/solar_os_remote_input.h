#pragma once

#include <stddef.h>

#include "esp_err.h"

/*
 * Thread-safe key queue between the remote (web screen share) job's
 * HTTP handlers and the main loop's input dispatch -- same pattern as
 * every other input source (BLE keyboard, CardKB, buttons): a producer
 * fills a queue from its own task, main.c drains it every tick via
 * *_read_chars() and feeds dispatch_input_chars().
 */
esp_err_t solar_os_remote_input_push(const char *chars, size_t len);
size_t solar_os_remote_input_read_chars(char *buffer, size_t buffer_len);
