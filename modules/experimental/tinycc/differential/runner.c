#include <assert.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include "esp_elf.h"
#include "esp_heap_caps.h"
#include "solar_os_cc_abi.h"
#include "solar_os_native_elf.h"
#include "abi.h"

struct diff_case {
    const char *name, *source;
    const uint8_t *elf;
    uint32_t elf_bytes;
    const uint8_t *gcc0, *gcc2;
    uint32_t gcc0_bytes, gcc2_bytes;
};
#include "suite.inc"

extern void *solar_os_memory_elf_alloc(size_t);
static uint32_t mix8(uint32_t a,uint32_t b,uint32_t c,uint32_t d,
                     uint32_t e,uint32_t f,uint32_t g,uint32_t h)
{ return a+3U*b+5U*c+7U*d+11U*e+13U*f+17U*g+19U*h; }
static int32_t narrow(int8_t a,uint8_t b,int16_t c,uint16_t d) { return a+b+c+d; }
static uint32_t callback(diff_callback_t fn,uint32_t a,uint32_t b)
{ uint32_t keep=a^0xdeadbeefU; return fn(a,b)^fn(b,a)^keep; }
static uint32_t variadic(unsigned count, ...)
{
    va_list ap; va_start(ap,count); uint32_t result=0;
    /* The integer-promotion probe uses int; matching unsigned values in the
     * other probe are representable in both types except x/y. Read those
     * through their declared unsigned type, using a distinct count. */
    for (unsigned i=0;i<count;i++) result=result*33U+(count==3 ? (uint32_t)va_arg(ap,int) : va_arg(ap,uint32_t));
    va_end(ap); return result;
}
static int8_t signed_byte(uint32_t value) { return (int8_t)value; }
static uint8_t unsigned_byte(uint32_t value) { return (uint8_t)value; }
static int16_t signed_half(uint32_t value) { return (int16_t)value; }
static uint16_t unsigned_half(uint32_t value) { return (uint16_t)value; }
static uint32_t callback12(diff_callback12_t fn,uint32_t a,uint32_t b)
{ return fn(a,b,a+b,a-b,a^b,0xffffffffU,0,0x80000000U,17,23,a*3U,b*5U); }
static const diff_api_t api = {mix8,narrow,callback,variadic,signed_byte,unsigned_byte,signed_half,unsigned_half,callback12};

struct observed { uint32_t before, words[DIFF_WORDS], after, result; };
_Static_assert(sizeof(struct observed)==(DIFF_WORDS+3)*sizeof(uint32_t),"observation must not contain padding");
static struct observed observe(diff_probe_t probe,uint32_t x,uint32_t y)
{
    struct observed value;
    memset(&value,0xa5,sizeof(value));
    value.result=probe(x,y,value.words,&api);
    return value;
}
static bool same(const struct observed *left,const struct observed *right)
{ return !memcmp(left,right,sizeof(*left)); }

int solar_cc_differential(const solar_os_cc_api_v1_t *compiler)
{
    enum { CAPACITY=256*1024 };
    void *output=solar_os_memory_elf_alloc(CAPACITY); assert(output);
    unsigned failures=0, comparisons=0;
    for (unsigned i=0;i<sizeof(cases)/sizeof(cases[0]);i++) {
        const struct diff_case *test=&cases[i];
        esp_elf_t reference0,reference2;
        char detail[160];
        assert(!solar_os_native_elf_validate(test->gcc0,test->gcc0_bytes,SOLAR_OS_NATIVE_ELF_MACHINE_XTENSA,NULL,detail,sizeof(detail)));
        assert(!solar_os_native_elf_validate(test->gcc2,test->gcc2_bytes,SOLAR_OS_NATIVE_ELF_MACHINE_XTENSA,NULL,detail,sizeof(detail)));
        assert(!esp_elf_init(&reference0) && !esp_elf_relocate(&reference0,test->gcc0));
        assert(!esp_elf_init(&reference2) && !esp_elf_relocate(&reference2,test->gcc2));
        for (unsigned mode=0;mode<2;mode++) {
            const char *label=mode ? "native" : "host";
            const void *image=test->elf; uint32_t bytes=test->elf_bytes;
            solar_os_cc_result_v1_t metrics={.struct_size=sizeof(metrics)};
            char error[512]={0};
            if (mode) {
                const solar_os_cc_header_v1_t header={"diff_abi.h",abi_header,sizeof(abi_header)-1};
                solar_os_cc_request_v1_t request={.struct_size=sizeof(request),
                    .source=test->source,.source_bytes=strlen(test->source),.entry="diff_probe",
                    .source_name=test->name,.headers=&header,.header_count=1,.timeout_ms=5000};
                int code=compiler->compile(&request,output,CAPACITY,&metrics,error,sizeof(error));
                if (code) {
                    printf("DIFF_FAIL case=%s mode=%s compile=%d diagnostic=%s\n",test->name,label,code,error);
                    failures++; continue;
                }
                image=output; bytes=metrics.output_bytes;
            }
            if (solar_os_native_elf_validate(image,bytes,SOLAR_OS_NATIVE_ELF_MACHINE_XTENSA,NULL,error,sizeof(error))) {
                printf("DIFF_FAIL case=%s mode=%s invalid_elf=%s\n",test->name,label,error);
                failures++; continue;
            }
            esp_elf_t elf;
            assert(!esp_elf_init(&elf));
            if (esp_elf_relocate(&elf,image)) {
                printf("DIFF_FAIL case=%s mode=%s load\n",test->name,label);
                failures++; esp_elf_deinit(&elf); continue;
            }
            unsigned mismatch=0;
            for (unsigned n=0;n<sizeof(inputs)/sizeof(inputs[0]);n++) {
                uint32_t x=inputs[n][0],y=inputs[n][1];
                struct observed gcc0=observe((diff_probe_t)reference0.entry,x,y),gcc2=observe((diff_probe_t)reference2.entry,x,y);
                struct observed actual=observe((diff_probe_t)elf.entry,x,y);
                comparisons++;
                if (!same(&gcc0,&gcc2) || !same(&gcc2,&actual)) {
                    if (mismatch++<3) {
                        printf("DIFF_FAIL case=%s mode=%s vector=%u x=%08lx y=%08lx gcc0=%08lx gcc2=%08lx actual=%08lx\n",
                            test->name,label,n,(unsigned long)x,(unsigned long)y,
                            (unsigned long)gcc0.result,(unsigned long)gcc2.result,(unsigned long)actual.result);
                        for (unsigned w=0;w<DIFF_WORDS;w++) if (gcc0.words[w]!=gcc2.words[w] || gcc2.words[w]!=actual.words[w])
                            printf("DIFF_WORD word=%u gcc0=%08lx gcc2=%08lx actual=%08lx\n",w,
                                (unsigned long)gcc0.words[w],(unsigned long)gcc2.words[w],(unsigned long)actual.words[w]);
                        if (actual.before!=0xa5a5a5a5U || actual.after!=0xa5a5a5a5U)
                            printf("DIFF_GUARD before=%08lx after=%08lx\n",(unsigned long)actual.before,(unsigned long)actual.after);
                    }
                }
            }
            esp_elf_deinit(&elf);
            failures+=mismatch;
            printf("DIFF_CASE name=%s mode=%s vectors=%u mismatches=%u bytes=%lu peak=%lu ms=%lu\n",
                test->name,label,(unsigned)(sizeof(inputs)/sizeof(inputs[0])),mismatch,
                (unsigned long)bytes,(unsigned long)metrics.peak_heap_bytes,(unsigned long)metrics.elapsed_ms);
        }
        esp_elf_deinit(&reference0); esp_elf_deinit(&reference2);
    }
    heap_caps_free(output);
    printf("DIFF_SUMMARY cases=%u comparisons=%u failures=%u\n",
        (unsigned)(sizeof(cases)/sizeof(cases[0])),comparisons,failures);
    return failures ? 1 : 0;
}
