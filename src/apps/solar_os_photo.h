#pragma once
#include "solar_os.h"
extern const solar_os_app_t solar_os_photo_app;
/* Completion reads the stream registry only; it never opens a source. */
void solar_os_photo_complete(const char *const *tokens, size_t count,
    void (*emit)(const char *candidate, void *user), void *user);
