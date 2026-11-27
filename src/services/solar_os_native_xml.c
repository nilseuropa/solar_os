#include "solar_os_native_xml.h"
#include "solar_os_native_xml_abi.h"
#include "solar_os_native_support.h"
#include "solar_os_memory.h"
#include "solar_os_config.h"
#include <string.h>
#include <strings.h>
#if SOLAR_OS_PACKAGE_SERVICE_DOCUMENT
#include "solar_os_xml.h"
typedef struct {
    solar_os_native_xml_event_fn callback;
    void *user;
    int error;
    solar_os_native_xml_attribute_v1_t attributes[16];
} native_xml_bridge_t;
static esp_err_t xml_event(const solar_os_xml_event_t *in, void *user)
{
    native_xml_bridge_t *bridge = user;
    solar_os_native_xml_event_v1_t out = {.name = in->name, .text = in->text, .text_size = in->text_len,
        .attributes = bridge->attributes, .self_closing = in->self_closing};
    if (in->attr_count > 16 || (in->attr_count && !in->attrs)) { bridge->error = SOLAR_OS_NATIVE_ERROR_INVALID_SIZE; return ESP_FAIL; }
    switch (in->type) {
    case SOLAR_OS_XML_EVENT_START: out.type = SOLAR_OS_NATIVE_XML_START; break;
    case SOLAR_OS_XML_EVENT_END: out.type = SOLAR_OS_NATIVE_XML_END; break;
    case SOLAR_OS_XML_EVENT_TEXT: out.type = SOLAR_OS_NATIVE_XML_TEXT; break;
    case SOLAR_OS_XML_EVENT_CDATA: out.type = SOLAR_OS_NATIVE_XML_CDATA; break;
    case SOLAR_OS_XML_EVENT_COMMENT: out.type = SOLAR_OS_NATIVE_XML_COMMENT; break;
    case SOLAR_OS_XML_EVENT_PI: out.type = SOLAR_OS_NATIVE_XML_PI; break;
    default: bridge->error = SOLAR_OS_NATIVE_ERROR_INVALID_STATE; return ESP_FAIL;
    }
    out.attribute_count = in->attr_count;
    for (size_t i = 0; i < in->attr_count; ++i) {
        _Static_assert(sizeof(bridge->attributes[i].name) == sizeof(in->attrs[i].name), "XML attribute name capacity");
        _Static_assert(sizeof(bridge->attributes[i].value) == sizeof(in->attrs[i].value), "XML attribute value capacity");
        memcpy(bridge->attributes[i].name, in->attrs[i].name, sizeof(in->attrs[i].name));
        memcpy(bridge->attributes[i].value, in->attrs[i].value, sizeof(in->attrs[i].value));
    }
    bridge->error = bridge->callback(&out, bridge->user); return bridge->error ? ESP_FAIL : ESP_OK;
}
static int xml_parse(const char *source, size_t size, solar_os_native_xml_event_fn callback, void *user)
{
    if (!callback || (!source && size)) return SOLAR_OS_NATIVE_ERROR_INVALID_ARGUMENT;
    native_xml_bridge_t *bridge = solar_os_memory_calloc(1, sizeof(*bridge), SOLAR_OS_MEMORY_EXTERNAL_PREFERRED, "native.xml");
    if (!bridge) return SOLAR_OS_NATIVE_ERROR_NO_MEMORY;
    bridge->callback = callback; bridge->user = user;
    int result = solar_os_native_result(solar_os_xml_parse(source, size, xml_event, bridge));
    if (bridge->error) result = bridge->error;
    solar_os_memory_free(bridge); return result;
}
static const char *xml_attribute(const solar_os_native_xml_event_v1_t *event, const char *name)
{
    if (!event || !name || event->attribute_count > 16 || (event->attribute_count && !event->attributes)) return NULL;
    for (uint32_t i = 0; i < event->attribute_count; ++i) {
        const solar_os_native_xml_attribute_v1_t *attribute = &event->attributes[i];
        if (!memchr(attribute->name, 0, sizeof(attribute->name)) || !memchr(attribute->value, 0, sizeof(attribute->value))) return NULL;
        if (!strcasecmp(attribute->name, name) || solar_os_xml_name_eq(attribute->name, name)) return attribute->value;
    }
    return NULL;
}
static int xml_decode(const char *source, size_t size, char *text, size_t capacity)
{ return solar_os_native_result(solar_os_xml_decode_entities(source, size, text, capacity)); }
static const solar_os_native_xml_api_v1_t xml_api = {
    .abi_version = SOLAR_OS_NATIVE_XML_ABI, .struct_size = sizeof(xml_api), .parse = xml_parse,
    .local_name = solar_os_xml_local_name, .name_eq = solar_os_xml_name_eq, .attribute = xml_attribute, .decode_entities = xml_decode,
};
#endif
const void *solar_os_native_xml_get_service(const char *name, uint32_t abi, uint32_t minimum)
{
#include "generated/native_xml_dispatch.inc"
    (void)name; (void)abi; (void)minimum; return NULL;
}
