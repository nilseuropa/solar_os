#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "solar_os_native_client.h"
#include "solar_os_native_xml.h"
#include "solar_os_memory.h"
static unsigned allocations, events, starts, texts, comments, cdatas, instructions, ends;
static bool stop, fail_allocation;
static const solar_os_native_xml_api_v1_t *api;
void *solar_os_memory_alloc(size_t size, solar_os_memory_class_t cls, const char *tag)
{ assert((cls == SOLAR_OS_MEMORY_EXTERNAL_PREFERRED || cls == SOLAR_OS_MEMORY_EXTERNAL_REQUIRED) && tag); if (fail_allocation) return NULL; void *p = malloc(size); if (p) ++allocations; return p; }
void *solar_os_memory_calloc(size_t n, size_t size, solar_os_memory_class_t cls, const char *tag)
{ void *p = solar_os_memory_alloc(n * size, cls, tag); if (p) memset(p, 0, n * size); return p; }
void solar_os_memory_free(void *p) { if (p) --allocations; free(p); }
static int visit(const solar_os_native_xml_event_v1_t *event, void *user)
{
    assert(user == &events); ++events;
    if (stop) return SOLAR_OS_NATIVE_ERROR_CANCELLED;
    switch (event->type) {
    case SOLAR_OS_NATIVE_XML_START:
        ++starts; assert(api->name_eq(event->name, "node")); assert(event->attribute_count == 1);
        assert(!strcmp(api->attribute(event, "X"), "a&b") && !strcmp(api->attribute(event, "ns:x"), "a&b")); break;
    case SOLAR_OS_NATIVE_XML_END: ++ends; assert(api->name_eq(event->name, "node")); break;
    case SOLAR_OS_NATIVE_XML_TEXT: ++texts; assert(event->text_size == 3 && !memcmp(event->text, "a&b", 3)); break;
    case SOLAR_OS_NATIVE_XML_COMMENT: ++comments; assert(event->text_size == 1 && event->text[0] == 'c'); break;
    case SOLAR_OS_NATIVE_XML_CDATA: ++cdatas; assert(event->text_size == 3 && !memcmp(event->text, "raw", 3)); break;
    case SOLAR_OS_NATIVE_XML_PI: ++instructions; break;
    default: assert(false);
    }
    return SOLAR_OS_NATIVE_OK;
}
int main(void)
{
    api = solar_os_native_xml_get_service("xml", 1, sizeof(*api)); assert(api);
    const char *source = "<?p?><ns:node ns:x=\"a&amp;b\">a&amp;b<!--c--><![CDATA[raw]]></ns:node>";
    assert(!api->parse(source, strlen(source), visit, &events));
    assert(starts == 1 && ends == 1 && texts == 1 && comments == 1 && cdatas == 1 && instructions == 1 && !allocations);
    unsigned before = events; stop = true; assert(api->parse(source, strlen(source), visit, &events) == SOLAR_OS_NATIVE_ERROR_CANCELLED);
    assert(events == before + 1 && !allocations); stop = false;
    assert(api->parse("<broken", 7, visit, &events) == SOLAR_OS_NATIVE_ERROR_FAILED && !allocations);
    const char *name = "prefix:node"; assert(api->local_name(name) == name + 7);
    char text[16]; assert(!api->decode_entities("a&amp;b", 7, text, sizeof(text)) && !strcmp(text, "a&b"));
    fail_allocation = true; assert(api->parse(source, strlen(source), visit, &events) == SOLAR_OS_NATIVE_ERROR_NO_MEMORY && !allocations);
    puts("native XML with real parser: values, namespace attributes and callback lifetime passed"); return 0;
}
