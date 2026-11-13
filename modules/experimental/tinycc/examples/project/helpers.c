#include "project.h"
#include <hello-library/hello_library_api.h>
#include <solar_os_native_abi.h>
#include <solar_os_native_library_abi.h>
uint32_t library_add(uint32_t a,uint32_t b)
{
    const solar_os_native_library_service_api_v1_t *libraries=
        solar_os_native_host_v1()->get_service("library",1,sizeof(*libraries));
    const hello_library_api_v1_t *api=NULL;uint32_t lease=0;
    if(!libraries || libraries->acquire("hello-library",HELLO_LIBRARY_ABI,sizeof(*api),&lease,(const void **)&api)) return 0;
    uint32_t value=api->add(a,b);
    return libraries->release(lease) ? 0 : value;
}
