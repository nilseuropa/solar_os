#ifndef LOCALTALK_H
#define LOCALTALK_H
#include <stdint.h>
// Stubs — no LocalTalk on ESP32-S3
static inline void localtalkInit(void) {}
static inline void localtalkSend(uint8_t *data, int len) { (void)data; (void)len; }
static inline void localtalkTick(void) {}
#endif
