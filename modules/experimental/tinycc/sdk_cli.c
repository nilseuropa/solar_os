/* Browse and export the installed SDK without executing library providers. */
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include "sdk_cli.h"
#include "sdk_reader.h"
#include "solar_os_native_time_abi.h"

#define SDK_PROVIDERS_MAX 17U
#define SDK_LIBRARY_HEADERS_MAX ((SDK_PROVIDERS_MAX - 1U) * SOLAR_OS_NATIVE_SDK_FILES_MAX)
#define SDK_DIRECTORY_MAX 128U
typedef struct {
    char id[48], version[32], path[SOLAR_OS_NATIVE_STORAGE_PATH_MAX];
    uint32_t file_bytes;
} sdk_provider_t;
typedef struct {
    char name[96];
    uint32_t provider, offset, bytes;
} sdk_header_t;
typedef struct {
    const solar_os_native_host_api_v1_t *host;
    const solar_os_native_libc_api_v1_t *libc;
    const solar_os_cc_api_v1_t *compiler;
    const solar_os_cc_api_v2_t *packed;
    const solar_os_native_storage_api_v1_t *storage;
    const solar_os_native_time_api_v1_t *clock;
    int64_t last_yield;
    sdk_provider_t providers[SDK_PROVIDERS_MAX];
    uint32_t provider_count, header_count, header_capacity;
    solar_os_cc_sdk_index_t index;
    solar_os_native_storage_entry_v1_t entry;
    unsigned char buffer[SOLAR_OS_NATIVE_SDK_FILE_MAX];
    char directory[SOLAR_OS_NATIVE_STORAGE_PATH_MAX], path[SOLAR_OS_NATIVE_STORAGE_PATH_MAX];
    sdk_header_t headers[];
} sdk_work_t;

static int stopped(sdk_work_t *w)
{
    int64_t now=w->clock->now_us();
    if(now-w->last_yield>=5000) {
        w->clock->sleep_ms(1);w->last_yield=w->clock->now_us();
    }
    return w->storage->stop_requested(w->storage->context) ? SOLAR_OS_NATIVE_ERROR_TIMEOUT : 0;
}
static int message(sdk_work_t *w,const char *format,...)
{
    char text[256];va_list args;va_start(args,format);
    int bytes=w->libc->format(text,sizeof(text),format,args);va_end(args);
    if(bytes<0) return SOLAR_OS_NATIVE_ERROR_FAILED;
    return w->host->write_utf8(text,bytes<(int)sizeof(text)?(size_t)bytes:sizeof(text)-1);
}
static bool component(const char *name,bool module)
{
    if(!name || !*name || !strcmp(name,".") || !strcmp(name,"..")) return false;
    for(const unsigned char *p=(const unsigned char *)name;*p;++p)
        if(!((*p>='a' && *p<='z') || (*p>='0' && *p<='9') || *p=='_' || *p=='-' ||
             (!module && ((*p>='A' && *p<='Z') || *p=='.')))) return false;
    return true;
}
static bool same_path_prefix(const char *a,const char *b,size_t bytes)
{
    for(size_t i=0;i<bytes;++i) {
        unsigned x=(unsigned char)a[i],y=(unsigned char)b[i];
        if(x>='A' && x<='Z')x+='a'-'A';
        if(y>='A' && y<='Z')y+='a'-'A';
        if(x!=y)return false;
    }
    return true;
}
static bool safe_name(const char *name,size_t capacity)
{
    size_t length=0;while(length<capacity && name[length]) length++;
    if(!length || length==capacity || name[0]=='/') return false;
    const char *start=name;
    for(const char *p=name;;++p) {
        if(!*p || *p=='/') {
            size_t n=p-start;
            if(!n || (n==1 && start[0]=='.') || (n==2 && start[0]=='.' && start[1]=='.')) return false;
            if(!*p) return true;
            start=p+1;
        } else if(!((*p>='a' && *p<='z') || (*p>='A' && *p<='Z') ||
                     (*p>='0' && *p<='9') || *p=='_' || *p=='-' || *p=='.')) return false;
    }
}
static int add_header(sdk_work_t *w,const char *name,uint32_t provider,uint32_t offset,uint32_t bytes)
{
    if(w->header_count==w->header_capacity || !safe_name(name,sizeof(w->headers[0].name)) ||
        bytes>SOLAR_OS_NATIVE_SDK_FILE_MAX) return SOLAR_OS_NATIVE_ERROR_INVALID_SIZE;
    sdk_header_t *h=&w->headers[w->header_count++];
    memcpy(h->name,name,strlen(name)+1);h->provider=provider;h->offset=offset;h->bytes=bytes;
    return 0;
}
static int inventory(sdk_work_t *w)
{
    const solar_os_cc_api_v1_t *api=w->compiler;
    size_t version_bytes=0;while(version_bytes<32 && api->sdk_version[version_bytes])version_bytes++;
    if(version_bytes==32 || !component(api->sdk_version,false)) return SOLAR_OS_NATIVE_ERROR_INVALID_SIZE;
    memcpy(w->providers[0].id,"tinycc",7);
    memcpy(w->providers[0].version,api->sdk_version,version_bytes+1);w->provider_count=1;
    for(uint32_t i=0;i<api->sdk_count;++i) {
        uint32_t bytes = 0;
        const char *name;
        if (w->packed) {
            name = w->packed->sdk_header_info(i, &bytes);
        } else {
            const solar_os_cc_header_v1_t *header = &api->sdk_headers[i];
            if (!header->text || header->bytes > SOLAR_OS_NATIVE_SDK_FILE_MAX || memchr(header->text, 0, header->bytes))
                return SOLAR_OS_NATIVE_ERROR_INVALID_SIZE;
            name = header->name;
            bytes = header->bytes;
        }
        int code = add_header(w, name, 0, i, bytes);
        if (code) return code;
    }
    int code=w->storage->resolve_path(w->storage->context,"/modules/libraries",w->directory,sizeof(w->directory));
    if(code)return code;
    for(uint32_t cursor=0;cursor<SDK_DIRECTORY_MAX;++cursor) {
        if((code=stopped(w)))return code;
        bool more=false;
        code=w->storage->directory_entry(w->storage->context,w->directory,cursor,&w->entry,&more);
        if(code==SOLAR_OS_NATIVE_ERROR_NOT_FOUND)break;
        if(code)return code;
        size_t n=0;while(n<sizeof(w->entry.name) && w->entry.name[n])n++;
        if(n==sizeof(w->entry.name))return SOLAR_OS_NATIVE_ERROR_INVALID_SIZE;
        if((w->entry.flags & SOLAR_OS_NATIVE_STORAGE_ENTRY_FILE) && n>4 && n-4<48 &&
            !strcmp(w->entry.name+n-4,".elf")) {
            char id[48];memcpy(id,w->entry.name,n-4);id[n-4]=0;
            if(component(id,true)) {
                int length=snprintf(w->path,sizeof(w->path),"%s/%s",w->directory,w->entry.name);
                if(length<0 || (size_t)length>=sizeof(w->path))return SOLAR_OS_NATIVE_ERROR_INVALID_SIZE;
                code=solar_os_cc_sdk_library_index(w->storage,w->path,id,&w->index);
                if(code && code!=SOLAR_OS_NATIVE_ERROR_NOT_FOUND) {
                    message(w,"cc sdk: invalid SDK in %s (%d)\n",w->path,code);return code;
                }
                if(!code) {
                    if(w->provider_count==SDK_PROVIDERS_MAX)return SOLAR_OS_NATIVE_ERROR_INVALID_SIZE;
                    uint32_t p=w->provider_count++;
                    sdk_provider_t *provider=&w->providers[p];
                    memcpy(provider->id,id,strlen(id)+1);memcpy(provider->version,w->index.version,32);
                    memcpy(provider->path,w->path,strlen(w->path)+1);provider->file_bytes=w->entry.bytes;
                    for(uint32_t i=0;i<w->index.count;++i) {
                        solar_os_cc_sdk_entry_t *entry=&w->index.entries[i];
                        code=add_header(w,entry->name,p,entry->offset,entry->bytes);if(code)return code;
                    }
                }
            }
        }
        if(!more)break;
        if(cursor+1==SDK_DIRECTORY_MAX)return SOLAR_OS_NATIVE_ERROR_INVALID_SIZE;
    }
    for(uint32_t i=1;i<w->header_count;++i) {
        sdk_header_t item=w->headers[i];uint32_t at=i;
        while(at && strcmp(w->headers[at-1].name,item.name)>0) {w->headers[at]=w->headers[at-1];at--;}
        w->headers[at]=item;
    }
    for(uint32_t i=1;i<w->header_count;++i)
        if(!strcmp(w->headers[i-1].name,w->headers[i].name))return SOLAR_OS_NATIVE_ERROR_INVALID_SIZE;
    return 0;
}
static int read_header(sdk_work_t *w,const sdk_header_t *header,const void **data)
{
    int code=stopped(w);if(code)return code;
    if(!header->provider) {
        if (w->packed) {
            code = w->packed->sdk_header_read(header->offset, w->buffer, header->bytes);
            if (code) return code;
            if (memchr(w->buffer, 0, header->bytes)) return SOLAR_OS_NATIVE_ERROR_INVALID_SIZE;
            *data = w->buffer;
            return SOLAR_OS_NATIVE_OK;
        }
        *data=w->compiler->sdk_headers[header->offset].text;return 0;
    }
    sdk_provider_t *p=&w->providers[header->provider];uint32_t bytes=0;
    code=w->storage->file_size(w->storage->context,p->path,&bytes);
    if(code)return code;
    if(bytes!=p->file_bytes)return SOLAR_OS_NATIVE_ERROR_INVALID_STATE;
    size_t actual=0;
    code=w->storage->read_file(w->storage->context,p->path,header->offset,w->buffer,header->bytes,&actual);
    if(code)return code;
    if(actual!=header->bytes || memchr(w->buffer,0,actual))return SOLAR_OS_NATIVE_ERROR_INVALID_SIZE;
    *data=w->buffer;return 0;
}
static int show(sdk_work_t *w,const char *name)
{
    for(uint32_t i=0;i<w->header_count;++i) {
        const sdk_header_t *h=&w->headers[i];if(strcmp(h->name,name))continue;
        const void *data=NULL;int code=read_header(w,h,&data);if(code)return code;
        for(uint32_t offset=0;offset<h->bytes;) {
            if((code=stopped(w)))return code;
            size_t bytes=h->bytes-offset;if(bytes>256)bytes=256;
            while(bytes && offset+bytes<h->bytes &&
                (((const unsigned char *)data)[offset+bytes]&0xc0)==0x80)bytes--;
            if(!bytes)return SOLAR_OS_NATIVE_ERROR_INVALID_SIZE;
            code=w->host->write_utf8((const char *)data+offset,bytes);if(code)return code;
            offset+=bytes;
        }
        return 0;
    }
    message(w,"cc sdk: header not found: %s\n",name);return SOLAR_OS_NATIVE_ERROR_NOT_FOUND;
}
static bool cleanup(sdk_work_t *w,const char *root,uint32_t written)
{
    bool ok=true;size_t root_bytes=strlen(root);
    for(uint32_t i=0;i<written;++i) {
        snprintf(w->path,sizeof(w->path),"%s/%s",root,w->headers[i].name);
        int code=w->storage->remove_path(w->storage->context,w->path,false);
        if(code && code!=SOLAR_OS_NATIVE_ERROR_NOT_FOUND)ok=false;
    }
    for(uint32_t i=0;i<w->header_count;++i) {
        snprintf(w->path,sizeof(w->path),"%s/%s",root,w->headers[i].name);
        for(size_t n=strlen(w->path);n>root_bytes;--n)if(w->path[n]=='/') {
            w->path[n]=0;(void)w->storage->remove_path(w->storage->context,w->path,true);
        }
    }
    int code=w->storage->remove_path(w->storage->context,root,true);
    return ok && (!code || code==SOLAR_OS_NATIVE_ERROR_NOT_FOUND);
}
static int export(sdk_work_t *w,const char *destination)
{
    int code=w->storage->resolve_path(w->storage->context,destination,w->directory,sizeof(w->directory));
    if(code)return code;
    size_t root_bytes=strlen(w->directory);
    for(uint32_t i=0;i<w->header_count;++i) {
        sdk_header_t *h=&w->headers[i];
        if(root_bytes+1+strlen(h->name)+sizeof(".native.tmp")>sizeof(w->path))return SOLAR_OS_NATIVE_ERROR_INVALID_SIZE;
        for(uint32_t j=0;j<w->header_count;++j) {
            size_t n=strlen(w->headers[j].name);
            /* The export must also be representable on case-insensitive FAT. */
            size_t length=strlen(h->name);
            if(i!=j && n<=length && (n==length || h->name[n]=='/') &&
                same_path_prefix(h->name,w->headers[j].name,n))
                return SOLAR_OS_NATIVE_ERROR_INVALID_SIZE;
        }
        const void *data=NULL;code=read_header(w,h,&data);if(code)return code;
    }
    code=w->storage->make_directories(w->storage->context,w->directory,false);
    if(code) {message(w,"cc sdk: export requires a new destination directory: %s\n",w->directory);return code;}
    uint32_t written=0;
    for(uint32_t i=0;i<w->header_count;++i) {
        sdk_header_t *h=&w->headers[i];const void *data=NULL;
        code=read_header(w,h,&data);if(code)break;
        snprintf(w->path,sizeof(w->path),"%s/%s",w->directory,h->name);
        size_t prefix_bytes=root_bytes+1;
        for(size_t n=prefix_bytes;w->path[n];++n)if(w->path[n]=='/') {
            w->path[n]=0;code=w->storage->make_directories(w->storage->context,w->path,true);w->path[n]='/';
            if(code)break;
        }
        if(code)break;
        code=w->storage->replace_file(w->storage->context,w->path,data,h->bytes);if(code)break;
        written++;
    }
    if(code) {
        if(!cleanup(w,w->directory,written))message(w,"cc sdk: incomplete export remains at %s\n",w->directory);
        return code;
    }
    return message(w,"cc sdk: exported %u headers to %s\n",written,w->directory);
}
int solar_os_cc_sdk_command(const solar_os_native_host_api_v1_t *host,
    const solar_os_native_libc_api_v1_t *libc_api,const solar_os_cc_api_v1_t *compiler,
    const solar_os_native_memory_api_v1_t *memory,int argc,char **argv)
{
    if(argc<3 || !((argc==3 && !strcmp(argv[2],"list")) ||
        (argc==4 && (!strcmp(argv[2],"show") || !strcmp(argv[2],"export"))))) {
        static const char usage[]="usage: cc sdk list | show HEADER | export NEW_DIRECTORY\n";
        host->write_utf8(usage,sizeof(usage)-1);return SOLAR_OS_NATIVE_ERROR_INVALID_ARGUMENT;
    }
    const solar_os_native_storage_api_v1_t *storage=host->get_service("storage",1,sizeof(*storage));
    if(!storage || !storage->directory_entry || !storage->make_directories || !storage->remove_path) {
        static const char text[]="cc sdk: update firmware for SDK directory operations\n";
        host->write_utf8(text,sizeof(text)-1);return SOLAR_OS_NATIVE_ERROR_NOT_SUPPORTED;
    }
    const solar_os_native_time_api_v1_t *clock=host->get_service("time",1,sizeof(*clock));
    if(!clock || !clock->now_us || !clock->sleep_ms)return SOLAR_OS_NATIVE_ERROR_NOT_SUPPORTED;
    const solar_os_cc_api_v2_t *packed = NULL;
    if (compiler->abi_version == SOLAR_OS_CC_PACKED_ABI && compiler->struct_size >= sizeof(*packed)) {
        const solar_os_cc_api_v2_t *candidate = (const void *)compiler;
        if (candidate->sdk_header_info && candidate->sdk_header_read) packed = candidate;
    }
    if (compiler->struct_size < sizeof(*compiler) || !compiler->sdk_version ||
        (!packed && !compiler->sdk_headers) || !compiler->sdk_count) {
        static const char text[] = "cc sdk: update TinyCC for the built-in header index\n";
        host->write_utf8(text, sizeof(text) - 1);
        return SOLAR_OS_NATIVE_ERROR_NOT_SUPPORTED;
    }
    if (compiler->sdk_count > UINT32_MAX - SDK_LIBRARY_HEADERS_MAX) {
        return SOLAR_OS_NATIVE_ERROR_INVALID_SIZE;
    }
    const uint32_t capacity = compiler->sdk_count + SDK_LIBRARY_HEADERS_MAX;
    if ((uint64_t)capacity * sizeof(sdk_header_t) > SIZE_MAX - sizeof(sdk_work_t)) {
        return SOLAR_OS_NATIVE_ERROR_INVALID_SIZE;
    }
    const size_t bytes = sizeof(sdk_work_t) + (size_t)capacity * sizeof(sdk_header_t);
    sdk_work_t *w = memory->alloc(bytes);
    if (!w) return SOLAR_OS_NATIVE_ERROR_NO_MEMORY;
    memset(w, 0, sizeof(*w));
    w->host = host;
    w->libc = libc_api;
    w->compiler = compiler;
    w->packed = packed;
    w->storage = storage;
    w->clock = clock;
    w->header_capacity = capacity;
    int code=inventory(w);
    if(!code && !strcmp(argv[2],"list")) {
        for(uint32_t provider=0;provider<w->provider_count && !code;++provider) {
            sdk_provider_t *p=&w->providers[provider];
            code=message(w,"%s %s\n",p->id,p->version);
            for(uint32_t i=0;i<w->header_count && !code;++i) {
                sdk_header_t *h=&w->headers[i];if(h->provider!=provider)continue;
                if(!(code=stopped(w)))code=message(w,"  %s (%u B)\n",h->name,h->bytes);
            }
        }
    } else if(!code && !strcmp(argv[2],"show"))code=show(w,argv[3]);
    else if(!code)code=export(w,argv[3]);
    if(code)message(w,"cc sdk: %s failed (%d)\n",argv[2],code);
    memory->free(w);return code;
}
