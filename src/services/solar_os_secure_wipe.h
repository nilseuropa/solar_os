#pragma once

#include <stddef.h>
#include <stdint.h>

/* Use volatile writes when clearing a secret whose buffer is otherwise dead. */
static inline void solar_os_secure_wipe(void *buffer, size_t length)
{
    if (!buffer) return;
    volatile uint8_t *bytes = buffer;
    while (length--) *bytes++ = 0;
}
