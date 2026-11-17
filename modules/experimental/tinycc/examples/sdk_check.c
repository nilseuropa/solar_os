#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <stdint.h>
#ifdef AUX_ONLY
#error macros must not leak between translation units
#endif
static int private_value=19;
extern int sdk_aux(void);
int main(void)
{
    if(private_value!=19 || sdk_aux()!=23) return 1;
    unsigned char *data=calloc(16,1);
    if(!data || ((uintptr_t)data&15U)) return 2;
    for(unsigned i=0;i<16;i++) if(data[i]) return 3;
    memcpy(data,"SolarOS",8);data=realloc(data,64);
    if(!data || strcmp((char *)data,"SolarOS")) return 4;
    void *failed=realloc(data,(size_t)-1);
    if(failed || strcmp((char *)data,"SolarOS")) return 5;
    free(data);
    char text[32],small[4];strcpy(text,"native");strcat(text," C");
    if(strcmp(text,"native C") || strncmp(text,"native!",6) ||
       strchr(text,'C')!=text+7 || strrchr(text,'a')!=text+1 ||
       strstr(text,"ive")!=text+3 || memchr(text,'C',8)!=text+7) return 6;
    memset(text,'X',sizeof(text));strncpy(text,"ok",sizeof(text));
    if(strcmp(text,"ok") || text[31]) return 7;
    if(snprintf(small,sizeof(small),"%s:%u","abcdef",42U)!=9 || strcmp(small,"abc")) return 8;
    if(abs(-7)!=7 || labs(-8)!=8 || atoi(" -42xyz")!=-42) return 9;
    volatile double x=-1.25;
    if(floor(x)!=-2.0 || fabs(x)!=1.25 || ldexp(1.25,2)!=5.0 || atof("1.25")!=1.25) return 10;
    if(sqrtf(9.0f)!=3.0f || floorf(-1.25f)!=-2.0f || fabsf(-2.0f)!=2.0f ||
       powf(2.0f,3.0f)!=8.0f || fmodf(5.5f,2.0f)!=1.5f) return 11;
    if(!isnan(NAN) || !isinf(INFINITY) || isfinite(INFINITY) || !isfinite(1.0) || !signbit(-0.0)) return 12;
    double once=1.0;if(!isfinite(once++) || once!=2.0) return 13;
    if(printf("SDK_FORMAT_PASS %llu %.2f %s\n",0x100000002ULL,1.25,"native")<0) return 14;
    if(printf("%5000s","x")!=EOF) return 15;
    puts("SDK_C_RUNTIME_PASS");return 0;
}
