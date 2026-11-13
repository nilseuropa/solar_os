/* SPDX-License-Identifier: Apache-2.0
 * Target C source compiled once into programs using the supported C library.
 * It has no access to private firmware symbols or the compiler's own runtime. */
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <math.h>
#include <stdint.h>
#include "solar_os_native_abi.h"
#include "solar_os_native_libc_abi.h"
#include "solar_os_native_storage_abi.h"

static const void *service(const char *name,unsigned bytes)
{
    const solar_os_native_host_api_v1_t *host=solar_os_native_host_v1();
    return host && host->struct_size>=sizeof(*host) && host->get_service ? host->get_service(name,1,bytes) : NULL;
}
union allocation_header {unsigned char align[16];size_t bytes;long double wide;};
void *malloc(size_t bytes)
{
    const solar_os_native_memory_api_v1_t *memory=service("memory",sizeof(*memory));
    if(!memory || bytes>(size_t)-1-sizeof(union allocation_header)) return NULL;
    union allocation_header *record=memory->alloc(bytes+sizeof(*record));
    if(!record) return NULL;
    record->bytes=bytes;return record+1;
}
void free(void *pointer)
{
    if(!pointer) return;
    const solar_os_native_memory_api_v1_t *memory=service("memory",sizeof(*memory));
    if(memory) memory->free((union allocation_header *)pointer-1);
}
void *calloc(size_t count,size_t bytes)
{
    if(bytes && count>(size_t)-1/bytes) return NULL;
    void *p=malloc(count*bytes);if(p) memset(p,0,count*bytes);return p;
}
void *realloc(void *old,size_t bytes)
{
    if(!bytes) {free(old);return NULL;}
    void *p=malloc(bytes);if(!p) return NULL;
    if(old) {
        size_t previous=((union allocation_header *)old)[-1].bytes;
        memcpy(p,old,previous<bytes ? previous : bytes);free(old);
    }
    return p;
}
int abs(int x) {return x<0 ? -x : x;}
long labs(long x) {return x<0 ? -x : x;}
int atoi(const char *text)
{
    while(*text==' ' || (*text>='\t' && *text<='\r')) text++;
    int negative=*text=='-';if(*text=='-' || *text=='+') text++;
    unsigned value=0;while(*text>='0' && *text<='9') value=value*10+*text++-'0';
    return negative ? (int)(0U-value) : (int)value;
}
double atof(const char *text)
{
    const solar_os_native_libc_api_v1_t *api=service("libc",offsetof(solar_os_native_libc_api_v1_t,scale_double));
    return api && api->parse_double ? api->parse_double(text,NULL) : (double)NAN;
}
int strcmp(const char *a,const char *b)
{while(*a && *a==*b) {a++;b++;}return (unsigned char)*a-(unsigned char)*b;}
int strncmp(const char *a,const char *b,size_t bytes)
{while(bytes && *a && *a==*b) {a++;b++;bytes--;}return bytes ? (unsigned char)*a-(unsigned char)*b : 0;}
char *strcpy(char *to,const char *from)
{char *result=to;while((*to++=*from++)) {}return result;}
char *strncpy(char *to,const char *from,size_t bytes)
{char *result=to;while(bytes && *from) {*to++=*from++;bytes--;}while(bytes--) *to++=0;return result;}
char *strcat(char *to,const char *from) {strcpy(to+strlen(to),from);return to;}
char *strchr(const char *text,int ch)
{for(;;text++) {if(*text==(char)ch) return (char *)text;if(!*text) return NULL;}}
char *strrchr(const char *text,int ch)
{const char *found=NULL;do {if(*text==(char)ch) found=text;}while(*text++);return (char *)found;}
char *strstr(const char *text,const char *find)
{size_t bytes=strlen(find);do {if(!strncmp(text,find,bytes)) return (char *)text;}while(*text++);return NULL;}
void *memchr(const void *data,int ch,size_t bytes)
{const unsigned char *p=data;while(bytes--) {if(*p==(unsigned char)ch) return (void *)p;p++;}return NULL;}

struct solar_os_c_file {
    const solar_os_native_storage_api_v1_t *storage;
    char path[SOLAR_OS_NATIVE_STORAGE_PATH_MAX];
    unsigned char *data;
    uint32_t offset,length,capacity;
    int mode,error,eof,dirty;
};
static FILE output_stream={.mode=3},error_stream={.mode=3};
FILE *stdout=&output_stream,*stderr=&error_stream;
static FILE *streams[16];
FILE *fopen(const char *name,const char *mode)
{
    const solar_os_native_storage_api_v1_t *storage=service("storage",sizeof(*storage));
    int reading=!strcmp(mode,"r") || !strcmp(mode,"rb");
    int writing=!strcmp(mode,"w") || !strcmp(mode,"wb");
    if(!storage || (!reading && !writing)) return NULL;
    unsigned slot=0;while(slot<16 && streams[slot]) slot++;if(slot==16) return NULL;
    FILE *file=calloc(1,sizeof(*file));if(!file) return NULL;
    file->storage=storage;file->mode=reading ? 1 : 2;
    int code=storage->resolve_path(storage->context,name,file->path,sizeof(file->path));
    if(!code && reading) code=storage->file_size(storage->context,file->path,&file->length);
    if(!code && writing) code=storage->replace_file(storage->context,file->path,"",0);
    if(code) {free(file);return NULL;}
    streams[slot]=file;return file;
}
int fflush(FILE *file)
{
    if(!file) {
        int result=0;for(unsigned i=0;i<16;i++) if(streams[i] && fflush(streams[i])) result=EOF;
        return result;
    }
    if(file->mode!=2 || !file->dirty) return 0;
    int code=file->storage->replace_file(file->storage->context,file->path,file->data,file->length);
    if(code) {file->error=1;return EOF;}
    file->dirty=0;return 0;
}
int fclose(FILE *file)
{
    if(!file) return EOF;
    if(file->mode==3) return fflush(file);
    int result=fflush(file);
    for(unsigned i=0;i<16;i++) if(streams[i]==file) streams[i]=NULL;
    free(file->data);free(file);return result;
}
size_t fread(void *data,size_t size,size_t count,FILE *file)
{
    if(!size || !count) return 0;
    if(!file || file->mode!=1 || count>(size_t)-1/size) {if(file) file->error=1;return 0;}
    size_t wanted=size*count,done=0;
    while(done<wanted && file->offset<file->length) {
        size_t bytes=wanted-done,actual=0;
        if(bytes>file->length-file->offset) bytes=file->length-file->offset;
        if(bytes>SOLAR_OS_NATIVE_STORAGE_READ_MAX) bytes=SOLAR_OS_NATIVE_STORAGE_READ_MAX;
        int code=file->storage->read_file(file->storage->context,file->path,file->offset,(char *)data+done,bytes,&actual);
        if(code || actual>bytes) {file->error=1;break;}
        done+=actual;file->offset+=actual;
        if(actual!=bytes) {file->eof=1;break;}
    }
    if(done<wanted && !file->error) file->eof=1;
    return done/size;
}
size_t fwrite(const void *data,size_t size,size_t count,FILE *file)
{
    if(!size || !count) return 0;
    if(!file || count>(size_t)-1/size) {if(file) file->error=1;return 0;}
    size_t bytes=size*count;
    if(file->mode==3) {
        if(solar_os_native_host_v1()->write_utf8(data,bytes)) {file->error=1;return 0;}
        return count;
    }
    if(file->mode!=2 || file->offset>SOLAR_OS_NATIVE_STORAGE_REPLACE_MAX ||
       bytes>SOLAR_OS_NATIVE_STORAGE_REPLACE_MAX-file->offset) {file->error=1;return 0;}
    uint32_t end=file->offset+bytes;
    if(end>file->capacity) {
        uint32_t capacity=file->capacity ? file->capacity : 256;
        while(capacity<end) capacity*=2;
        unsigned char *p=realloc(file->data,capacity);
        if(!p) {file->error=1;return 0;}
        memset(p+file->capacity,0,capacity-file->capacity);
        file->data=p;file->capacity=capacity;
    }
    memcpy(file->data+file->offset,data,bytes);file->offset=end;
    if(end>file->length) file->length=end;
    file->dirty=1;return count;
}
int fseek(FILE *file,long offset,int whence)
{
    if(!file || file->mode==3 || whence<0 || whence>2) return -1;
    int64_t position=(whence==SEEK_SET ? 0 : whence==SEEK_CUR ? file->offset : file->length)+(int64_t)offset;
    if(position<0 || position>0x7fffffffLL) return -1;
    file->offset=(uint32_t)position;file->eof=0;return 0;
}
long ftell(FILE *file) {return file && file->mode!=3 && file->offset<=0x7fffffffU ? (long)file->offset : -1;}
int feof(FILE *file) {return file ? file->eof : 0;}
int ferror(FILE *file) {return file ? file->error : 1;}
void clearerr(FILE *file) {if(file) file->error=file->eof=0;}
void rewind(FILE *file) {if(file) {fseek(file,0,SEEK_SET);clearerr(file);}}
int fgetc(FILE *file) {unsigned char byte;return fread(&byte,1,1,file)==1 ? byte : EOF;}
int fputc(int ch,FILE *file) {unsigned char byte=ch;return fwrite(&byte,1,1,file)==1 ? byte : EOF;}
int putchar(int ch) {return fputc(ch,stdout);}
int fputs(const char *text,FILE *file) {size_t bytes=strlen(text);return fwrite(text,1,bytes,file)==bytes ? 0 : EOF;}
int puts(const char *text) {return fputs(text,stdout)==EOF || fputc('\n',stdout)==EOF ? EOF : 0;}
int vsnprintf(char *buffer,size_t bytes,const char *format,va_list args)
{
    const solar_os_native_libc_api_v1_t *api=service("libc",offsetof(solar_os_native_libc_api_v1_t,parse_float));
    return api && api->format ? api->format(buffer,bytes,format,args) : -1;
}
int snprintf(char *buffer,size_t bytes,const char *format,...)
{va_list args;va_start(args,format);int n=vsnprintf(buffer,bytes,format,args);va_end(args);return n;}
int vfprintf(FILE *file,const char *format,va_list args)
{
    char local[256],*buffer=local;va_list copy;va_copy(copy,args);
    int n=vsnprintf(local,sizeof(local),format,copy);va_end(copy);
    if(n<0 || n>4096) return EOF;
    if(n>=(int)sizeof(local)) {
        buffer=malloc((size_t)n+1);if(!buffer) return EOF;
        va_copy(copy,args);int actual=vsnprintf(buffer,n+1,format,copy);va_end(copy);
        if(actual!=n) {free(buffer);return EOF;}
    }
    int result=fwrite(buffer,1,n,file)==(size_t)n ? n : EOF;
    if(buffer!=local) free(buffer);return result;
}
int fprintf(FILE *file,const char *format,...)
{va_list args;va_start(args,format);int n=vfprintf(file,format,args);va_end(args);return n;}
int vprintf(const char *format,va_list args) {return vfprintf(stdout,format,args);}
int printf(const char *format,...)
{va_list args;va_start(args,format);int n=vprintf(format,args);va_end(args);return n;}

#define LIBC_PREFIX offsetof(solar_os_native_libc_api_v1_t,parse_double)
#define FLOAT_METHOD(name,member) float name(float x) { \
    const solar_os_native_libc_api_v1_t *api=service("libc",LIBC_PREFIX); \
    return api && api->member ? api->member(x) : NAN; }
FLOAT_METHOD(expf,exp_float) FLOAT_METHOD(floorf,floor_float)
FLOAT_METHOD(logf,log_float) FLOAT_METHOD(sqrtf,sqrt_float)
float fmodf(float x,float y) {const solar_os_native_libc_api_v1_t *a=service("libc",LIBC_PREFIX);return a && a->mod_float ? a->mod_float(x,y) : NAN;}
float powf(float x,float y) {const solar_os_native_libc_api_v1_t *a=service("libc",LIBC_PREFIX);return a && a->pow_float ? a->pow_float(x,y) : NAN;}
double floor(double x) {const solar_os_native_libc_api_v1_t *a=service("libc",LIBC_PREFIX);return a && a->floor_value ? a->floor_value(x) : (double)NAN;}
double ldexp(double x,int exponent) {const solar_os_native_libc_api_v1_t *a=service("libc",sizeof(*a));return a && a->scale_double ? a->scale_double(x,exponent) : (double)NAN;}
float fabsf(float x) {union {float value;uint32_t bits;} u;u.value=x;u.bits&=0x7fffffffU;return u.value;}
double fabs(double x) {union {double value;uint64_t bits;} u;u.value=x;u.bits&=0x7fffffffffffffffULL;return u.value;}
int solar_os_c_isnan(double x) {union {double value;uint64_t bits;} u;u.value=x;return (u.bits&0x7fffffffffffffffULL)>0x7ff0000000000000ULL;}
int solar_os_c_isinf(double x) {union {double value;uint64_t bits;} u;u.value=x;return (u.bits&0x7fffffffffffffffULL)==0x7ff0000000000000ULL;}
int solar_os_c_isfinite(double x) {union {double value;uint64_t bits;} u;u.value=x;return (u.bits&0x7fffffffffffffffULL)<0x7ff0000000000000ULL;}
int solar_os_c_signbit(double x) {union {double value;uint64_t bits;} u;u.value=x;return u.bits>>63;}

/* Native entries do not run a hosted C CRT. Flush and close files when a
 * standard main returns, while its shell-scoped storage capability is valid. */
#ifdef SOLAR_OS_C_WRAP_MAIN
extern int main();
int solar_os_c_main(int argc,char **argv)
{
    int code=main(argc,argv),error=0;
    for(unsigned i=0;i<16;i++) if(streams[i] && fclose(streams[i])) error=1;
    return code ? code : error;
}
#endif
