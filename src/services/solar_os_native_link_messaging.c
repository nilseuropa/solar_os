#include "solar_os_native_link_messaging.h"
#include "solar_os_config.h"
#include <stddef.h>
#include <string.h>

#if SOLAR_OS_PACKAGE_SERVICE_LINK && SOLAR_OS_PACKAGE_SERVICE_MESSAGING
#include "solar_os_link_messaging.h"
#include "solar_os_native_link_messaging_abi.h"
#include "solar_os_native_support.h"

static int messaging_start(const char *link)
{
    return solar_os_native_result(solar_os_link_messaging_start(link));
}

static void messaging_note_transmit(const char *link, const solar_os_link_frame_t *frame,
                                    int result, uint32_t now_ms)
{
    solar_os_link_messaging_note_transmit(link, frame, solar_os_native_callback_result(result), now_ms);
}

static int messaging_note_ingest(const char *link, const solar_os_link_ingest_result_t *result,
                                 uint32_t now_ms)
{
    return solar_os_native_result(solar_os_link_messaging_note_ingest(link, result, now_ms));
}

static const solar_os_native_link_messaging_api_v1_t link_messaging_api = {
    .abi_version = SOLAR_OS_NATIVE_LINK_MESSAGING_ABI,
    .struct_size = sizeof(link_messaging_api),
    .start = messaging_start,
    .stop = solar_os_link_messaging_stop,
    .process = solar_os_link_messaging_process,
    .note_transmit = messaging_note_transmit,
    .note_ingest = messaging_note_ingest,
};
#endif

const void *solar_os_native_link_messaging_get_service(const char *name, uint32_t abi, uint32_t minimum)
{
#if SOLAR_OS_PACKAGE_SERVICE_LINK
#include "generated/native_link_messaging_dispatch.inc"
#endif
    (void)name;
    (void)abi;
    (void)minimum;
    return NULL;
}
