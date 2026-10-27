#include "solar_os_native_library_abi.h"
#include "solar_os_jet_abi.h"
extern "C" int solar_os_espdl_port_bind(const solar_os_native_library_host_api_v1_t *);
extern "C" void solar_os_espdl_port_cleanup(void);
extern "C" void __register_frame_info(const void *,void *);
extern "C" void *__deregister_frame_info(const void *);
extern "C" void __cxa_finalize(void *);
extern "C" void *__dso_handle;
extern "C" unsigned char __module_eh_start[];
extern "C" void (*__module_ctor_start[])(),(*__module_ctor_end[])();
extern "C" void (*__module_dtor_start[])(),(*__module_dtor_end[])();
extern "C" const solar_os_jet_api_v1_t *solar_os_jet_engine_api_v1(void);
extern "C" bool solar_os_jet_engine_idle(void);
static bool initialized;
alignas(16) static unsigned char frame_object[128];
extern "C" void solar_os_jet_initialize(void) {
    if(initialized) return;
    __register_frame_info(__module_eh_start,frame_object);
    for(auto p=__module_ctor_start;p<__module_ctor_end;++p)
        if(*p && (uintptr_t)*p!=UINT32_MAX) (*p)();
    initialized=true;
}
static int shutdown() {
    if(!solar_os_jet_engine_idle()) return SOLAR_OS_NATIVE_ERROR_INVALID_STATE;
    if(initialized) {
        for(auto p=__module_dtor_end;p>__module_dtor_start;) {
            --p; if(*p && (uintptr_t)*p!=UINT32_MAX) (*p)();
        }
        __cxa_finalize(__dso_handle); __deregister_frame_info(__module_eh_start);
        initialized=false;
    }
    solar_os_espdl_port_cleanup(); return 0;
}
extern "C" int app_main(int,char **) {
    const auto *host=solar_os_native_library_host_v1();
    if(!host || host->abi_version!=1 || host->struct_size<sizeof(*host) ||
        !host->register_library || !host->get_service || solar_os_espdl_port_bind(host))
        return SOLAR_OS_NATIVE_ERROR_NOT_SUPPORTED;
    const solar_os_native_library_descriptor_v1_t descriptor={1,sizeof(descriptor),
        SOLAR_OS_JET_LIBRARY,"Jet 3D scene renderer",
        (const solar_os_native_library_api_header_v1_t *)solar_os_jet_engine_api_v1(),shutdown};
    return host->register_library(&descriptor);
}
