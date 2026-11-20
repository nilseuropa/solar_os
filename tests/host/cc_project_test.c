#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include "project.h"

static char root[160];
static const char *mode;
static unsigned allocations, yields;
static bool cancelled;
static void *allocate(size_t n)
{ if (!strcmp(mode,"oom")) return NULL; void *p=malloc(n); assert(p); allocations++; return p; }
static void release(void *p)
{ assert(p && allocations); allocations--; free(p); }
static int resolve(void *ctx,const char *name,char *out,size_t capacity)
{
    (void)ctx;
    if (!strcmp(mode,"resolve-fail")) return -10;
    char joined[400];
    int n=snprintf(joined,sizeof(joined),"%s%s",name[0]=='/'?"":"/other/",name);
    if(n<0 || (size_t)n>=sizeof(joined))return -10;
    size_t used=0;
    for(const char *p=joined;*p;) {
        if(*p=='/'){p++;continue;}
        const char *end=strchr(p,'/');if(!end)end=p+strlen(p);
        size_t bytes=end-p;
        if(bytes==1 && p[0]=='.') {}
        else if(bytes==2 && p[0]=='.' && p[1]=='.') {while(used && out[--used]!='/'){};}
        else {
            if(used+1+bytes>=capacity)return -10;
            out[used++]='/';memcpy(out+used,p,bytes);used+=bytes;
        }
        p=end;
    }
    if(!used){if(capacity<2)return -10;out[used++]='/';}
    out[used]=0;return 0;
}
static void physical(const char *path,char *out,size_t n)
{ assert(snprintf(out,n,"%s%s",root,path)<(int)n); }
static int file_size(void *ctx,const char *path,uint32_t *bytes)
{
    (void)ctx;char name[400];physical(path,name,sizeof(name));struct stat s;
    if(stat(name,&s))return -5;
    if(!S_ISREG(s.st_mode))return -10;
    *bytes=s.st_size;return 0;
}
static int read_file(void *ctx,const char *path,uint32_t offset,void *out,size_t bytes,size_t *actual)
{
    (void)ctx;
    if(!strcmp(mode,"read-fail"))return -1;
    if(!strcmp(mode,"cancel-sdk") && !strncmp(path,"/modules/",9))return -7;
    char name[400];physical(path,name,sizeof(name));FILE *f=fopen(name,"rb");if(!f)return -5;
    assert(!fseek(f,offset,SEEK_SET));*actual=fread(out,1,bytes,f);assert(!fclose(f));
    if(!strcmp(mode,"short") && *actual)(*actual)--;
    return 0;
}
static bool stopped(void *ctx) { (void)ctx;return cancelled; }
static void sleep_ms(uint32_t ms) { assert(ms==1);yields++;if(!strcmp(mode,"clock-cancel"))cancelled=true; }
static const solar_os_native_storage_api_v1_t storage={.resolve_path=resolve,.file_size=file_size,
    .read_file=read_file,.stop_requested=stopped};
static const solar_os_native_memory_api_v1_t memory={.alloc=allocate,.free=release};
static const solar_os_native_time_api_v1_t clock_api={.sleep_ms=sleep_ms};

static unsigned char text[SOLAR_OS_CC_PROJECT_BYTES_MAX+1];
static void check_fuzz(void)
{
    const char *valid="sources=['main.c','other.c']\ninclude_dirs=['include']\noutput='build/app.elf'\n"
        "dependencies=[{id='jet',version='1.12.0'}]\n";
    struct {unsigned char before[16];solar_os_cc_project_t project;unsigned char after[16];} guard;
    unsigned state=12345;char detail[160];size_t length=strlen(valid);
    for(unsigned n=0;n<12000;++n) {
        memcpy(text,valid,length);
        size_t bytes=n<length?n:length;
        if(n>=length) {
            state=state*1664525U+1013904223U;size_t index=state%length;
            state=state*1664525U+1013904223U;text[index]=(state>>8)&255;
        }
        memset(&guard,0x5a,sizeof(guard));
        int code=solar_os_cc_project_parse((const char *)text,bytes,&guard.project,detail,sizeof(detail));
        for(unsigned i=0;i<16;++i)assert(guard.before[i]==0x5a && guard.after[i]==0x5a);
        assert(memchr(detail,0,sizeof(detail)));
        if(!code) {
            assert(guard.project.source_count && guard.project.source_count<=8 &&
                guard.project.include_count<=8 && guard.project.dependency_count<=8);
            assert(memchr(guard.project.output,0,sizeof(guard.project.output)));
        }
    }
    puts("12000 bounded parser mutations/truncations passed");
}
int main(int argc,char **argv)
{
    assert(argc>=2);
    if(!strcmp(argv[1],"fuzz")){check_fuzz();return 0;}
    solar_os_cc_project_t project;char detail[512];int code;
    if(!strcmp(argv[1],"parse")) {
        assert(argc==3);FILE *f=fopen(argv[2],"rb");assert(f);
        size_t bytes=fread(text,1,sizeof(text),f);assert(!fclose(f));
        code=solar_os_cc_project_parse((const char *)text,bytes,&project,detail,sizeof(detail));
    } else {
        assert(argc==5);assert(strlen(argv[2])<sizeof(root));strcpy(root,argv[2]);mode=argv[4];
        cancelled=!strcmp(mode,"cancel");
        code=solar_os_cc_project_load(&storage,&memory,&clock_api,argv[3],&project,detail,sizeof(detail));
        assert(!allocations);
        if(!code && project.dependency_count)assert(yields);
    }
    printf("%d\n",code);
    if(code)puts(detail);
    else {
        printf("output\t%s\n",project.output);
        for(unsigned i=0;i<project.source_count;++i)printf("source\t%s\n",project.sources[i]);
        for(unsigned i=0;i<project.include_count;++i)printf("include\t%s\n",project.include_dirs[i]);
        for(unsigned i=0;i<project.dependency_count;++i)printf("dependency\t%s\t%s\n",project.dependencies[i].id,project.dependencies[i].version);
    }
    return 0;
}
