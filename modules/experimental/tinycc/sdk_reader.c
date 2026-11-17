/* Bounded metadata reads; SDK headers and executable code share one ELF file. */
#include <string.h>
#include "solar_os_native_sdk.h"
#include "sdk_reader.h"

static uint32_t u32(const unsigned char *p)
{return (uint32_t)p[0]|(uint32_t)p[1]<<8|(uint32_t)p[2]<<16|(uint32_t)p[3]<<24;}
static unsigned u16(const unsigned char *p) {return p[0]|p[1]<<8;}
static bool bounds(uint32_t total,uint32_t offset,uint32_t bytes)
{return offset<=total && bytes<=total-offset;}
static bool valid_path(const char *name)
{
    const char *start=name;
    for(const char *p=name;;p++) {
        if(!*p || *p=='/') {
            unsigned n=p-start;
            if(!n || (n==1 && start[0]=='.') || (n==2 && start[0]=='.' && start[1]=='.')) return false;
            if(!*p) return true;
            start=p+1;
        } else if(!((*p>='a' && *p<='z') || (*p>='A' && *p<='Z') ||
                    (*p>='0' && *p<='9') || *p=='_' || *p=='-' || *p=='.')) return false;
    }
}

static int sdk_parse(solar_os_cc_sdk_read_fn read,void *context,uint32_t file_bytes,
    const char *module_id,const char *header,void *buffer,uint32_t capacity,uint32_t *bytes,
    solar_os_cc_sdk_index_t *index)
{
    unsigned char elf[52],section[40],meta[SOLAR_OS_NATIVE_SDK_HEADER_BYTES];
    uint32_t names_at,names_bytes,sdk_at=0,sdk_bytes=0;
    if(!read || !module_id || (!header && !index) || !bytes) return SOLAR_OS_NATIVE_ERROR_INVALID_ARGUMENT;
    if(index) memset(index,0,sizeof(*index));
    *bytes=0;
    if(file_bytes<52) return SOLAR_OS_NATIVE_ERROR_INVALID_SIZE;
    int code=read(context,0,elf,sizeof(elf));if(code) return code;
    if(memcmp(elf,"\177ELF\1\1\1",7) || u16(elf+16)!=3 || u16(elf+18)!=94 || u16(elf+46)!=40)
        return SOLAR_OS_NATIVE_ERROR_NOT_SUPPORTED;
    uint32_t table=u32(elf+32);unsigned count=u16(elf+48),names=u16(elf+50);
    if(!count || count>128 || names>=count || !bounds(file_bytes,table,count*40U))
        return SOLAR_OS_NATIVE_ERROR_INVALID_SIZE;
    code=read(context,table+names*40,section,40);if(code) return code;
    names_at=u32(section+16);names_bytes=u32(section+20);
    if(u32(section+4)!=3 || !bounds(file_bytes,names_at,names_bytes)) return SOLAR_OS_NATIVE_ERROR_INVALID_SIZE;
    for(unsigned i=1;i<count;i++) {
        char name[sizeof(SOLAR_OS_NATIVE_SDK_SECTION)];
        code=read(context,table+i*40,section,40);if(code) return code;
        uint32_t offset=u32(section);
        if(!bounds(names_bytes,offset,sizeof(name))) continue;
        code=read(context,names_at+offset,name,sizeof(name));if(code) return code;
        if(memcmp(name,SOLAR_OS_NATIVE_SDK_SECTION,sizeof(name))) continue;
        if(sdk_bytes || u32(section+4)!=1 || u32(section+8) || u32(section+12))
            return SOLAR_OS_NATIVE_ERROR_INVALID_SIZE;
        sdk_at=u32(section+16);sdk_bytes=u32(section+20);
        if(!bounds(file_bytes,sdk_at,sdk_bytes) || sdk_bytes<sizeof(meta) ||
           sdk_bytes>96+32*104+262144) return SOLAR_OS_NATIVE_ERROR_INVALID_SIZE;
    }
    if(!sdk_bytes) return SOLAR_OS_NATIVE_ERROR_NOT_FOUND;
    code=read(context,sdk_at,meta,sizeof(meta));if(code) return code;
    count=u32(meta+12);
    if(memcmp(meta,SOLAR_OS_NATIVE_SDK_MAGIC,8) || u32(meta+8)!=sdk_bytes || !count || count>32 ||
       !memchr(meta+16,0,48) || !memchr(meta+64,0,32) || !meta[64] ||
       strcmp((const char *)meta+16,module_id)) return SOLAR_OS_NATIVE_ERROR_INVALID_SIZE;
    for(const unsigned char *p=meta+64;*p;++p)
        if(!((*p>='a' && *p<='z') || (*p>='A' && *p<='Z') || (*p>='0' && *p<='9') ||
             *p=='_' || *p=='-' || *p=='.')) return SOLAR_OS_NATIVE_ERROR_INVALID_SIZE;
    if(index) {memcpy(index->version,meta+64,32);index->count=count;}
    unsigned char record[SOLAR_OS_NATIVE_SDK_RECORD_BYTES];
    char previous[96]={0};uint32_t expected=96+count*104,found=0,found_bytes=0,text_bytes=0;
    if(expected>sdk_bytes) return SOLAR_OS_NATIVE_ERROR_INVALID_SIZE;
    for(unsigned i=0;i<count;i++) {
        code=read(context,sdk_at+96+i*104,record,sizeof(record));if(code) return code;
        const char *name=(const char *)record;
        uint32_t offset=u32(record+96),length=u32(record+100);size_t id_bytes=strlen(module_id);
        if(!memchr(name,0,96) || !valid_path(name) || strcmp(name,previous)<=0 ||
           strlen(name)<=id_bytes || memcmp(name,module_id,id_bytes) || name[id_bytes]!='/' ||
           offset!=expected || length>32768 || !bounds(sdk_bytes,offset,length) ||
           length>262144-text_bytes) return SOLAR_OS_NATIVE_ERROR_INVALID_SIZE;
        memcpy(previous,name,strlen(name)+1);expected+=length;text_bytes+=length;
        if(index) {
            memcpy(index->entries[i].name,name,96);
            index->entries[i].offset=sdk_at+offset;index->entries[i].bytes=length;
        }
        if(header && !strcmp(name,header)) {found=offset;found_bytes=length;}
    }
    if(expected!=sdk_bytes) return SOLAR_OS_NATIVE_ERROR_INVALID_SIZE;
    if(index) return 0;
    if(!found) return SOLAR_OS_NATIVE_ERROR_NOT_FOUND;
    *bytes=found_bytes;
    if(!buffer) return 0;
    if(capacity<found_bytes) return SOLAR_OS_NATIVE_ERROR_INVALID_SIZE;
    code=found_bytes ? read(context,sdk_at+found,buffer,found_bytes) : 0;
    if(!code && memchr(buffer,0,found_bytes)) return SOLAR_OS_NATIVE_ERROR_INVALID_SIZE;
    return code;
}

int solar_os_cc_sdk_extract(solar_os_cc_sdk_read_fn read,void *context,uint32_t file_bytes,
    const char *module_id,const char *header,void *buffer,uint32_t capacity,uint32_t *bytes)
{
    return sdk_parse(read,context,file_bytes,module_id,header,buffer,capacity,bytes,NULL);
}
int solar_os_cc_sdk_index(solar_os_cc_sdk_read_fn read,void *context,uint32_t file_bytes,
    const char *module_id,solar_os_cc_sdk_index_t *index)
{
    uint32_t bytes=0;
    if(!index) return SOLAR_OS_NATIVE_ERROR_INVALID_ARGUMENT;
    return sdk_parse(read,context,file_bytes,module_id,NULL,NULL,0,&bytes,index);
}

struct sdk_file {const solar_os_native_storage_api_v1_t *storage;char path[SOLAR_OS_NATIVE_STORAGE_PATH_MAX];};
static int read_exact(void *user,uint32_t offset,void *data,uint32_t bytes)
{
    struct sdk_file *file=user;size_t actual=0;
    int code=file->storage->read_file(file->storage->context,file->path,offset,data,bytes,&actual);
    return code ? code : actual==bytes ? 0 : SOLAR_OS_NATIVE_ERROR_INVALID_SIZE;
}
int solar_os_cc_sdk_library_index(const solar_os_native_storage_api_v1_t *storage,
    const char *path,const char *module_id,solar_os_cc_sdk_index_t *index)
{
    if(!storage || !path || strlen(path)>=SOLAR_OS_NATIVE_STORAGE_PATH_MAX) return SOLAR_OS_NATIVE_ERROR_INVALID_ARGUMENT;
    struct sdk_file file={.storage=storage};memcpy(file.path,path,strlen(path)+1);
    uint32_t bytes=0;int code=storage->file_size(storage->context,path,&bytes);
    if(code) return code;
    if(bytes>2U*1024U*1024U) return SOLAR_OS_NATIVE_ERROR_INVALID_SIZE;
    return solar_os_cc_sdk_index(read_exact,&file,bytes,module_id,index);
}
int solar_os_cc_sdk_read(const solar_os_native_storage_api_v1_t *storage,
    const char *name,void *buffer,uint32_t capacity,uint32_t *bytes)
{
    static const char prefix[]="/module-sdk/";
    if(strlen(name)<sizeof(prefix)-1 || memcmp(name,prefix,sizeof(prefix)-1)) return SOLAR_OS_NATIVE_ERROR_NOT_FOUND;
    name+=sizeof(prefix)-1;
    char id[48],header[96],logical[96];unsigned id_bytes=0,length=0;
    while(name[id_bytes] && name[id_bytes]!='/') {
        char c=name[id_bytes];
        if(id_bytes==47 || !((c>='a' && c<='z') || (c>='0' && c<='9') || c=='_' || c=='-'))
            return SOLAR_OS_NATIVE_ERROR_NOT_FOUND;
        id[id_bytes]=c;id_bytes++;
    }
    if(!id_bytes || name[id_bytes]!='/') return SOLAR_OS_NATIVE_ERROR_NOT_FOUND;
    id[id_bytes]=0;memcpy(header,id,id_bytes);length=id_bytes;header[length++]='/';
    name+=id_bytes+1;
    /* Resolve quoted parent includes inside this module's header namespace. */
    while(*name) {
        const char *end=name;while(*end && *end!='/') end++;
        unsigned n=end-name;
        if(n==1 && name[0]=='.') {}
        else if(n==2 && name[0]=='.' && name[1]=='.') {
            if(length<=id_bytes+1) return SOLAR_OS_NATIVE_ERROR_NOT_FOUND;
            length--;while(length>id_bytes+1 && header[length-1]!='/') length--;
        } else {
            if(!n || n+1>sizeof(header)-length) return SOLAR_OS_NATIVE_ERROR_NOT_FOUND;
            memcpy(header+length,name,n);length+=n;header[length++]='/';
        }
        name=*end ? end+1 : end;
    }
    if(length<=id_bytes+1) return SOLAR_OS_NATIVE_ERROR_NOT_FOUND;
    header[length-1]=0;if(!valid_path(header)) return SOLAR_OS_NATIVE_ERROR_NOT_FOUND;
    const char *root="/modules/libraries/";unsigned at=0;
    while(*root) logical[at++]=*root++;
    memcpy(logical+at,id,id_bytes);at+=id_bytes;memcpy(logical+at,".elf",5);
    struct sdk_file file={.storage=storage};uint32_t file_bytes;
    int code=storage->resolve_path(storage->context,logical,file.path,sizeof(file.path));
    if(!code) code=storage->file_size(storage->context,file.path,&file_bytes);
    if(code) return code;
    return solar_os_cc_sdk_extract(read_exact,&file,file_bytes,id,header,buffer,capacity,bytes);
}
