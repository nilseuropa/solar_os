#include <assert.h>
#include <dirent.h>
#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include "sdk_cli.h"
#include "solar_os_native_storage_abi.h"
#include "solar_os_native_time_abi.h"

static char root[160],output[65536];
static unsigned allocations,writes,cancel_write,fail_write;
static bool cancel,fail_alloc,old_storage,cancel_output;
static solar_os_native_storage_api_v1_t storage;
static int64_t ticks;
static unsigned yields;
static int64_t now_us(void) { ticks+=1000;return ticks; }
static void sleep_ms(uint32_t ms) { assert(ms==1);yields++;ticks+=ms*1000; }
static const solar_os_native_time_api_v1_t clock_api={1,sizeof(clock_api),now_us,sleep_ms};
static const solar_os_cc_header_v1_t headers[]={
#ifdef SOLAR_OS_TEST_BUILTIN_SDK
#include "sdk_browser_builtin_headers.inc"
#else
    {"solar_os_native_abi.h","#define NATIVE 1\n",17},
    {"stdio.h","#define STDIO 1\n",16},
#endif
};
static solar_os_cc_api_v1_t compiler={.abi_version=1,.struct_size=sizeof(compiler),
    .sdk_version="0.6.0",.sdk_count=sizeof(headers)/sizeof(headers[0]),.sdk_headers=headers};
static int write_text(const char *s,size_t n)
{ size_t used=strlen(output);assert(used+n<sizeof(output));memcpy(output+used,s,n);output[used+n]=0;if(cancel_output && n>=256)cancel=true;return 0; }
static void *allocate(size_t n) { if(fail_alloc)return NULL;void *p=calloc(1,n);assert(p);allocations++;return p; }
static void release(void *p) { assert(p && allocations);allocations--;free(p); }
static int resolve(void *ctx,const char *path,char *out,size_t n)
{ (void)ctx;int size=snprintf(out,n,"%s/%s",root,path[0]=='/'?path+1:path);return size<0 || (size_t)size>=n?-10:0; }
static int file_size(void *ctx,const char *path,uint32_t *n)
{ (void)ctx;struct stat s;if(stat(path,&s))return -5;if(!S_ISREG(s.st_mode))return -10;*n=s.st_size;return 0; }
static int read_file(void *ctx,const char *path,uint32_t offset,void *out,size_t n,size_t *actual)
{ (void)ctx;FILE *f=fopen(path,"rb");if(!f)return -5;assert(!fseek(f,offset,SEEK_SET));*actual=fread(out,1,n,f);assert(!fclose(f));return 0; }
static int replace(void *ctx,const char *path,const void *data,size_t n)
{
    (void)ctx;if(++writes==fail_write)return -1;
    FILE *f=fopen(path,"wb");if(!f)return -1;assert(fwrite(data,1,n,f)==n);assert(!fclose(f));
    if(writes==cancel_write)cancel=true;
    return 0;
}
static bool stopped(void *ctx) { (void)ctx;return cancel; }
static int entry(void *ctx,const char *path,uint32_t cursor,solar_os_native_storage_entry_v1_t *out,bool *more)
{
    (void)ctx;DIR *d=opendir(path);if(!d)return -5;
    struct dirent *e;uint32_t at=0;int code=-5;*more=false;
    while((e=readdir(d))) {
        if(!strcmp(e->d_name,".") || !strcmp(e->d_name,".."))continue;
        if(at++<cursor)continue;
        if(code==0){*more=true;break;}
        assert(strlen(e->d_name)<sizeof(out->name));strcpy(out->name,e->d_name);
        char child[256];snprintf(child,sizeof(child),"%s/%s",path,e->d_name);
        struct stat s;assert(!stat(child,&s));out->bytes=s.st_size;
        out->flags=S_ISREG(s.st_mode)?SOLAR_OS_NATIVE_STORAGE_ENTRY_FILE:SOLAR_OS_NATIVE_STORAGE_ENTRY_DIRECTORY;code=0;
    }
    assert(!closedir(d));return code;
}
static int mkdirs(void *ctx,const char *path,bool exist_ok)
{
    (void)ctx;char work[160];assert(strlen(path)<sizeof(work));strcpy(work,path);
    for(char *p=work+1;*p;++p)if(*p=='/'){*p=0;if(mkdir(work,0700) && errno!=EEXIST)return -1;*p='/';}
    if(!mkdir(work,0700))return 0;
    struct stat s;return exist_ok && errno==EEXIST && !stat(work,&s) && S_ISDIR(s.st_mode)?0:-3;
}
static int remove_path(void *ctx,const char *path,bool directory)
{ (void)ctx;int code=directory?rmdir(path):unlink(path);return !code?0:errno==ENOENT?-5:-1; }
static const void *get_service(const char *name,uint32_t abi,uint32_t size)
{ assert(abi==1);if(!strcmp(name,"time"))return size<=sizeof(clock_api)?&clock_api:NULL;
  assert(!strcmp(name,"storage"));return old_storage || size>sizeof(storage)?NULL:&storage; }
static const solar_os_native_host_api_v1_t host={.abi_version=1,.struct_size=sizeof(host),.write_utf8=write_text,.get_service=get_service};
static const solar_os_native_libc_api_v1_t libc_api={.format=vsnprintf};
static const solar_os_native_memory_api_v1_t memory={.alloc=allocate,.free=release};
static int run(const char *op,const char *argument)
{
    memset(output,0,sizeof(output));char *args[]={"cc","sdk",(char *)op,(char *)argument};
    int code=solar_os_cc_sdk_command(&host,&libc_api,&compiler,&memory,argument?4:3,args);
    assert(!allocations);return code;
}
static void expect(const char *relative,const char *text)
{
    char path[160];assert(snprintf(path,sizeof(path),"%s/%s",root,relative)<(int)sizeof(path));
    char data[256]={0};FILE *f=fopen(path,"rb");assert(f);size_t n=fread(data,1,sizeof(data),f);assert(!fclose(f));assert(n==strlen(text) && !memcmp(data,text,n));
}
int main(int argc,char **argv)
{
    assert(argc>=3 && strlen(argv[1])<sizeof(root));strcpy(root,argv[1]);
    storage=(solar_os_native_storage_api_v1_t){.abi_version=1,.struct_size=sizeof(storage),.resolve_path=resolve,
        .file_size=file_size,.read_file=read_file,.replace_file=replace,.stop_requested=stopped,
        .directory_entry=entry,.make_directories=mkdirs,.remove_path=remove_path};
    if(!strcmp(argv[2],"builtins")) {
        assert(!run("list",NULL) && strstr(output,"solar_os_native_client.h"));
        for (unsigned i=0;i<compiler.sdk_count;i++)
            assert(!run("show",headers[i].name) && !strcmp(output,headers[i].text));
        assert(!run("export","sdk"));
    } else if(!strcmp(argv[2],"library")) {
        assert(argc>=6);
        assert(!run("list",NULL) && strstr(output,argv[3]) && strstr(output,argv[4]));
        for(int i=5;i<argc;i++) assert(!run("show",argv[i]) && output[0]);
        char count[64];snprintf(count,sizeof(count),"exported %d headers",argc-3);
        assert(!run("export","sdk") && strstr(output,count));
    } else if(!strcmp(argv[2],"robot")) {
        assert(!run("list",NULL));
        assert(strstr(output,"robot/solar_os_robot_abi.h") && strstr(output,"1.0.0"));
        assert(strstr(output,"robot/solar_os_robot_model.h") && strstr(output,"robot/solar_os_robot_diff_drive.h"));
        assert(!run("show","robot/solar_os_robot_abi.h") && strstr(output,"#define SOLAR_OS_ROBOT_ABI 3U"));
        assert(!run("show","robot/solar_os_robot_model.h") && strstr(output,"solar_os_robot_model_info_t"));
        assert(!run("show","robot/solar_os_robot_diff_drive.h") && strstr(output,"solar_os_robot_diff_drive_status_t"));
        assert(!run("export","sdk") && strstr(output,"exported 5 headers"));
    } else if(!strcmp(argv[2],"valid")) {
        assert(!run("list",NULL));assert(strstr(output,"stdio.h") && strstr(output,"0.6.0") && strstr(output,"testlib/api.h") && strstr(output,"1.2.3"));
        assert(!strstr(output,"plain"));
        assert(!run("show","stdio.h") && !strcmp(output,headers[1].text));
        assert(!run("show","testlib/api.h") && !strcmp(output,"#define ANSWER 42\n"));
        assert(!run("show","testlib/detail/inner.h") && !strcmp(output,"#include \"../api.h\"\n"));
        assert(run("show","../api.h")==-5 && strstr(output,"header not found"));
        assert(!run("export","sdk"));assert(strstr(output,"exported 4 headers"));
        expect("sdk/stdio.h",headers[1].text);expect("sdk/testlib/api.h","#define ANSWER 42\n");expect("sdk/testlib/detail/inner.h","#include \"../api.h\"\n");
        char edited[160];assert(snprintf(edited,sizeof(edited),"%s/sdk/stdio.h",root)<(int)sizeof(edited));
        FILE *f=fopen(edited,"wb");assert(f);assert(fwrite("user edit",1,9,f)==9);assert(!fclose(f));
        unsigned before=writes;assert(run("export","sdk")==-3 && writes==before);expect("sdk/stdio.h","user edit");
        fail_write=writes+4;assert(run("export","failed")==-1);fail_write=0;
        char path[160];snprintf(path,sizeof(path),"%s/failed",root);assert(access(path,F_OK));
        cancel_write=writes+3;assert(run("export","cancelled")==-7);cancel_write=0;cancel=false;
        snprintf(path,sizeof(path),"%s/cancelled",root);assert(access(path,F_OK));
        old_storage=true;assert(run("list",NULL)==-6 && strstr(output,"update firmware"));old_storage=false;
        compiler.struct_size=SOLAR_OS_CC_API_BASE_SIZE;assert(run("list",NULL)==-6 && strstr(output,"update TinyCC"));compiler.struct_size=sizeof(compiler);
        fail_alloc=true;assert(run("list",NULL)==-4);fail_alloc=false;
        cancel=true;assert(run("list",NULL)==-7);cancel=false;
        char large[512];memset(large,'x',sizeof(large));
        solar_os_cc_header_v1_t large_header={"large.h",large,sizeof(large)};
        compiler.sdk_headers=&large_header;compiler.sdk_count=1;cancel_output=true;
        assert(run("show","large.h")==-7);cancel=false;cancel_output=false;
        compiler.sdk_headers=headers;compiler.sdk_count=2;
        assert(run("export",NULL)==-2);assert(run("nonsense",NULL)==-2);
        assert(yields);
    } else if(!strcmp(argv[2],"collision")) {
        assert(!run("list",NULL));assert(run("export","bad")==-10 && !writes);
    } else if(!strcmp(argv[2],"nul")) {
        assert(!run("list",NULL));assert(run("show","testlib/api.h")==-10);
        assert(run("export","bad")==-10 && !writes);
    } else {
        assert(run("list",NULL)<0);assert(run("export","bad")<0 && !writes);
    }
    puts("SDK browser: list, versions, show, export, preservation, rollback, cancellation and compatibility passed");
}
