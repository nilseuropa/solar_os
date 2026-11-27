#include "solar_os_components.h"

#include <string.h>
#include "solar_os_config.h"

#define COMPONENT_DECLARE(type, id, name, version, lifecycle, entry, count, dependencies, manifest) extern int entry(int, char **);
SOLAR_OS_BUILTIN_COMPONENTS(COMPONENT_DECLARE)
#undef COMPONENT_DECLARE
#ifdef SOLAR_OS_BUILTIN_APP_MANIFESTS
#define MANIFEST_DECLARE(symbol) extern const solar_os_native_app_manifest_v2_t symbol;
SOLAR_OS_BUILTIN_APP_MANIFESTS(MANIFEST_DECLARE)
#undef MANIFEST_DECLARE
#endif

static const solar_os_component_t components[] = {
#define COMPONENT_ROW(type, id, name, version, lifecycle, entry, count, dependencies, manifest) {type, id, name, version, lifecycle, entry, count, dependencies, manifest},
    SOLAR_OS_BUILTIN_COMPONENTS(COMPONENT_ROW)
#undef COMPONENT_ROW
    {0},
};

size_t solar_os_component_count(void)
{
    return sizeof(components) / sizeof(components[0]) - 1U;
}

const solar_os_component_t *solar_os_component_get(size_t index)
{
    return index < solar_os_component_count() ? &components[index] : NULL;
}

const solar_os_component_t *solar_os_component_find(solar_os_module_type_t type,
                                                    const char *id)
{
    if (!id || !*id) return NULL;
    for (size_t i = 0; i < solar_os_component_count(); ++i) {
        const solar_os_component_t *component = &components[i];
        if ((type == SOLAR_OS_MODULE_TYPE_INVALID || type == component->type) &&
            !strcmp(id, component->id)) return component;
    }
    return NULL;
}
