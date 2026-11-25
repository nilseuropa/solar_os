#pragma once
#include "solar_os_native_service_abi.h"
#define SOLAR_OS_NATIVE_XML_SERVICE "xml"
#define SOLAR_OS_NATIVE_XML_ABI 1U
#define SOLAR_OS_NATIVE_XML_START 0U
#define SOLAR_OS_NATIVE_XML_END 1U
#define SOLAR_OS_NATIVE_XML_TEXT 2U
#define SOLAR_OS_NATIVE_XML_CDATA 3U
#define SOLAR_OS_NATIVE_XML_COMMENT 4U
#define SOLAR_OS_NATIVE_XML_PI 5U
typedef struct { char name[64], value[256]; } solar_os_native_xml_attribute_v1_t;
typedef struct {
    const char *name, *text;
    const solar_os_native_xml_attribute_v1_t *attributes;
    size_t text_size;
    uint32_t type, attribute_count, self_closing;
} solar_os_native_xml_event_v1_t;
typedef int (*solar_os_native_xml_event_fn)(const solar_os_native_xml_event_v1_t *event, void *user);
/* Parse callbacks run synchronously on the caller's task. Events, strings and
 * copied attributes are borrowed only until the callback returns. Return OK
 * to continue or a native error to stop; that exact error is returned by parse.
 * local_name returns a suffix of the caller's string, not an allocation.
 * Parsing preserves the OS tokenizer's behavior; it is not schema validation. */
typedef struct {
    uint32_t abi_version, struct_size;
    int (*parse)(const char *source, size_t size, solar_os_native_xml_event_fn callback, void *user);
    const char *(*local_name)(const char *name);
    bool (*name_eq)(const char *name, const char *local_name);
    const char *(*attribute)(const solar_os_native_xml_event_v1_t *event, const char *name);
    int (*decode_entities)(const char *source, size_t size, char *text, size_t capacity);
} solar_os_native_xml_api_v1_t;
