#include "solar_os_inference_backend_abi.h"
#include "solar_os_espdl_backend.h"
#include "solar_os_native_library_abi.h"
#include <new>

extern "C" int solar_os_espdl_port_bind(const solar_os_native_library_host_api_v1_t *);
extern "C" void solar_os_espdl_port_cleanup(void);
extern "C" void solar_os_espdl_port_leave_task(void);
extern "C" void __register_frame_info(const void *,void *);
extern "C" void *__deregister_frame_info(const void *);
extern "C" void __cxa_finalize(void *);
extern "C" void *__dso_handle;
extern "C" unsigned char __module_eh_start[];
extern "C" void (*__module_ctor_start[])(), (*__module_ctor_end[])();
extern "C" void (*__module_dtor_start[])(), (*__module_dtor_end[])();
extern "C" const solar_os_inference_backend_api_v1_t *solar_os_inference_espdl_api(void);
static void initialize();
extern "C" void solar_os_espdl_initialize(void) { initialize(); }
static bool initialized;
/* GCC 14.2's private object is 24 bytes on this target. The pinned prototype
 * supplies storage so frame registration cannot silently fail a malloc. */
alignas(16) static unsigned char frame_object[128];
static void initialize()
{
    if (initialized) return;
    __register_frame_info(__module_eh_start,frame_object);
    for (auto p=__module_ctor_start;p<__module_ctor_end;++p)
        if (*p && (uintptr_t)*p!=UINT32_MAX) (*p)();
    initialized=true;
}
static int shutdown()
{
    if (!solar_os_espdl_backend_idle()) return -3;
    if (initialized) {
        for (auto p=__module_dtor_end;p>__module_dtor_start;) {
            --p;if (*p && (uintptr_t)*p!=UINT32_MAX) (*p)();
        }
        __cxa_finalize(__dso_handle);
        __deregister_frame_info(__module_eh_start);
        initialized=false;
    }
    solar_os_espdl_port_cleanup();
    return 0;
}
extern "C" int app_main(int,char **)
{
    const auto *host=solar_os_native_library_host_v1();
    if (solar_os_espdl_port_bind(host)) return -6;
    static const solar_os_native_library_descriptor_v1_t descriptor={1,sizeof(descriptor),
        "espdl","ESP-DL inference engine",(const solar_os_native_library_api_header_v1_t *)solar_os_inference_espdl_api(),shutdown};
    return host->register_library(&descriptor);
}
