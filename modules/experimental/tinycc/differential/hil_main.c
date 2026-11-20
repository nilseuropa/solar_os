/* Appended to each corpus translation unit for file-based device testing. */
#include "solar_os_native_library_abi.h"
#include "hil_api.h"

struct cc_hil_observed {
    uint32_t before, words[DIFF_WORDS], after, result;
};

static struct cc_hil_observed cc_hil_observe(diff_probe_t probe,
    uint32_t x, uint32_t y, const diff_api_t *callbacks)
{
    struct cc_hil_observed value;
    value.before=0x13579bdfU; value.after=0x2468ace0U;
    for(unsigned i=0;i<DIFF_WORDS;i++) value.words[i]=0;
    value.result=probe(x,y,value.words,callbacks);
    return value;
}

int main(void)
{
    const solar_os_native_host_api_v1_t *host=solar_os_native_host_v1();
    const solar_os_native_library_service_api_v1_t *libraries=
        host->get_service("library",1,sizeof(*libraries));
    const solar_os_cc_hil_api_t *api=0;
    uint32_t lease=0;
    if(!libraries || libraries->acquire("tinycc-diff-check",1,sizeof(*api),
            &lease,(const void **)&api)) return 1;
    int failed=CC_DIFF_CASE>=api->case_count || !api->vector_count;
    for(unsigned v=0;!failed && v<api->vector_count;v++) {
        uint32_t x=api->inputs[v][0],y=api->inputs[v][1];
        struct cc_hil_observed actual=cc_hil_observe(diff_probe,x,y,api->callbacks);
        struct cc_hil_observed gcc0=cc_hil_observe(api->references[CC_DIFF_CASE][0],x,y,api->callbacks);
        struct cc_hil_observed gcc2=cc_hil_observe(api->references[CC_DIFF_CASE][1],x,y,api->callbacks);
        failed=actual.before!=0x13579bdfU || actual.after!=0x2468ace0U ||
               gcc0.before!=actual.before || gcc0.after!=actual.after ||
               gcc2.before!=actual.before || gcc2.after!=actual.after ||
               actual.result!=gcc0.result || actual.result!=gcc2.result;
        for(unsigned w=0;w<DIFF_WORDS;w++)
            if(actual.words[w]!=gcc0.words[w] || actual.words[w]!=gcc2.words[w]) failed=1;
    }
    if(libraries->release(lease)) failed=1;
    if(failed) {
        static const char text[]="ABI_HIL_FAIL name=" CC_DIFF_NAME "\n";
        host->write_utf8(text,sizeof(text)-1); return 1;
    }
    static const char text[]="ABI_HIL_PASS name=" CC_DIFF_NAME "\n";
    host->write_utf8(text,sizeof(text)-1);
    return 0;
}
