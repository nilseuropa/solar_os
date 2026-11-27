#define _POSIX_C_SOURCE 200809L
#include "solar_os_native_fs.h"
#include "solar_os_native_fs_abi.h"
#include "solar_os_native_volumes_abi.h"
#include "solar_os_native_support.h"
#include "solar_os_native_storage_values.h"
#include "solar_os_storage.h"
#include "solar_os_disk_image.h"
#include "solar_os_memory.h"
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

static int fs_errno(void)
{
    switch(errno) {
    case EINVAL: return SOLAR_OS_NATIVE_ERROR_INVALID_ARGUMENT;
    case ENOMEM: return SOLAR_OS_NATIVE_ERROR_NO_MEMORY;
    case ENOENT: case ENODEV: return SOLAR_OS_NATIVE_ERROR_NOT_FOUND;
    case EEXIST: return SOLAR_OS_NATIVE_ERROR_EXISTS;
    case ENOTEMPTY: return SOLAR_OS_NATIVE_ERROR_NOT_EMPTY;
    case ENOSPC: return SOLAR_OS_NATIVE_ERROR_NO_SPACE;
    case EACCES: case EPERM: case EROFS: return SOLAR_OS_NATIVE_ERROR_PERMISSION;
    case EBUSY: return SOLAR_OS_NATIVE_ERROR_BUSY;
    case EFBIG: case ENAMETOOLONG: case EOVERFLOW: return SOLAR_OS_NATIVE_ERROR_INVALID_SIZE;
    case ENOSYS: return SOLAR_OS_NATIVE_ERROR_NOT_SUPPORTED;
    default: return SOLAR_OS_NATIVE_ERROR_IO;
    }
}
static int fs_result(esp_err_t error)
{
    if (!error) return SOLAR_OS_NATIVE_OK;
    if (error==ESP_FAIL || errno==EBUSY) return fs_errno();
    return solar_os_native_result(error);
}
#define FS_CALL(operation) (errno=0,fs_result(operation))
static int fs_resolve(const char *cwd,const char *argument,char *path,size_t capacity)
{ return FS_CALL(solar_os_storage_resolve_path_at(cwd,argument,path,capacity)); }
static int fs_normalize(const char *path,char *out,size_t capacity)
{ return FS_CALL(solar_os_storage_normalize_path(path,out,capacity)); }
static int fs_join(const char *base,const char *relative,char *out,size_t capacity)
{ return FS_CALL(solar_os_storage_join_path(base,relative,out,capacity)); }
static int fs_default(const char *relative,char *out,size_t capacity)
{ return FS_CALL(solar_os_storage_default_path(relative,out,capacity)); }
static int fs_sibling(const char *path,const char *suffix,char *out,size_t capacity)
{ return FS_CALL(solar_os_storage_sibling_path(path,suffix,out,capacity)); }
static int fs_mount_point(const char *path,char *out,size_t capacity)
{ return FS_CALL(solar_os_storage_path_mount_point(path,out,capacity)); }
static void metadata_copy(const solar_os_storage_metadata_t *in,solar_os_native_fs_metadata_v1_t *out)
{
    *out=(solar_os_native_fs_metadata_v1_t){.bytes=in->size_bytes,.modified_seconds=in->modified_seconds,
        .mode=in->mode,.type=in->type==SOLAR_OS_STORAGE_ENTRY_FILE?SOLAR_OS_NATIVE_FS_FILE:
        in->type==SOLAR_OS_STORAGE_ENTRY_DIRECTORY?SOLAR_OS_NATIVE_FS_DIRECTORY:SOLAR_OS_NATIVE_FS_OTHER};
}
static int fs_stat(const char *path,solar_os_native_fs_metadata_v1_t *out)
{
    if (!out) return SOLAR_OS_NATIVE_ERROR_INVALID_ARGUMENT;
    memset(out,0,sizeof(*out));solar_os_storage_metadata_t value={0};
    solar_os_disk_image_registry_lock();
    int result=FS_CALL(solar_os_storage_stat(path,&value));
    solar_os_disk_image_registry_unlock();
    if (!result) metadata_copy(&value,out);
    return result;
}
static int fs_exists(const char *path,bool *exists)
{
    if (!exists) return SOLAR_OS_NATIVE_ERROR_INVALID_ARGUMENT;
    *exists=false;solar_os_disk_image_registry_lock();
    int result=FS_CALL(solar_os_storage_exists(path,exists));
    solar_os_disk_image_registry_unlock();return result;
}
static int fs_scandir(const char *path,uint64_t cursor,uint32_t limit,
    solar_os_native_fs_entry_v1_t *entries,uint32_t *count,uint64_t *next,bool *more)
{
    if (!entries || !count || !next || !more || !limit || limit>SOLAR_OS_NATIVE_FS_SCAN_MAX)
        return SOLAR_OS_NATIVE_ERROR_INVALID_ARGUMENT;
    *count=0;*next=cursor;*more=false;
    if (cursor>UINT32_MAX) return SOLAR_OS_NATIVE_ERROR_INVALID_SIZE;
    solar_os_storage_entry_t *values=solar_os_memory_calloc(limit,sizeof(*values),
        SOLAR_OS_MEMORY_EXTERNAL_PREFERRED,"native.scandir");
    if (!values) return SOLAR_OS_NATIVE_ERROR_NO_MEMORY;
    size_t actual=0,position=0;
    solar_os_disk_image_registry_lock();
    int result=FS_CALL(solar_os_storage_scandir(path,(size_t)cursor,limit,values,&actual,&position,more));
    solar_os_disk_image_registry_unlock();
    if (!result && actual>limit) result=SOLAR_OS_NATIVE_ERROR_INVALID_SIZE;
    if (!result) {
        for (size_t i=0;i<actual;++i) {
            memset(&entries[i],0,sizeof(entries[i]));
            memcpy(entries[i].name,values[i].name,sizeof(entries[i].name));
            metadata_copy(&values[i].metadata,&entries[i].metadata);
        }
        *count=actual;*next=position;
    } else *more=false;
    solar_os_memory_free(values);return result;
}
static int fs_mkdir(const char *path,bool parents,bool exist_ok)
{
    if (parents) return FS_CALL(solar_os_storage_makedirs(path,exist_ok));
    if (!exist_ok) return FS_CALL(solar_os_storage_mkdir(path));
    solar_os_storage_metadata_t value;
    int result=FS_CALL(solar_os_storage_stat(path,&value));
    if (!result) return value.type==SOLAR_OS_STORAGE_ENTRY_DIRECTORY?SOLAR_OS_NATIVE_OK:SOLAR_OS_NATIVE_ERROR_EXISTS;
    return FS_CALL(solar_os_storage_mkdir(path));
}
static int fs_remove(const char *path,bool directory)
{
    solar_os_disk_image_registry_lock();
    int result=directory?FS_CALL(solar_os_storage_rmdir(path)):FS_CALL(solar_os_storage_remove(path));
    solar_os_disk_image_registry_unlock();return result;
}
static int fs_rename(const char *from,const char *to)
{
    solar_os_disk_image_registry_lock();
    int result=FS_CALL(solar_os_storage_rename(from,to));
    solar_os_disk_image_registry_unlock();return result;
}
static int fs_read(const char *path,void *bytes,size_t capacity,size_t *size)
{
    if (!size || (!bytes && capacity) || capacity>SOLAR_OS_NATIVE_FS_IO_MAX)
        return SOLAR_OS_NATIVE_ERROR_INVALID_ARGUMENT;
    *size=0;solar_os_disk_image_registry_lock();
    int result=FS_CALL(solar_os_storage_read_file(path,bytes,capacity,size));
    solar_os_disk_image_registry_unlock();return result;
}
static int fs_write(const char *path,const void *bytes,size_t size,bool append)
{
    if ((!bytes && size) || size>SOLAR_OS_NATIVE_FS_IO_MAX) return SOLAR_OS_NATIVE_ERROR_INVALID_ARGUMENT;
    solar_os_disk_image_registry_lock();
    int result=FS_CALL(solar_os_storage_write_file(path,bytes,size,append));
    solar_os_disk_image_registry_unlock();return result;
}
static int file_open(const char *path,const char *mode,bool mutation,bool create,FILE **out,char normalized[160])
{
    *out=NULL;
    if (!path || *path!='/') return SOLAR_OS_NATIVE_ERROR_INVALID_ARGUMENT;
    int result=fs_normalize(path,normalized,SOLAR_OS_NATIVE_FS_PATH_MAX);
    char mount[SOLAR_OS_STORAGE_MOUNT_POINT_MAX];
    if (!result) result=fs_mount_point(normalized,mount,sizeof(mount));
    if (!result && mutation && solar_os_disk_image_path_busy(normalized)) result=SOLAR_OS_NATIVE_ERROR_BUSY;
    if (result) return result;
    errno=0;*out=fopen(normalized,mode);
    if (!*out && errno==ENOENT && create) {
        /* Creation is attempted only after mount and ownership validation.
         * O_CREAT preserves a file concurrently created by another writer. */
        int fd=open(normalized,O_RDWR|O_CREAT,0666);
        if (fd<0) return fs_errno();
        *out=fdopen(fd,mode);
        if (!*out) { int saved=errno;close(fd);errno=saved; }
    }
    return *out?SOLAR_OS_NATIVE_OK:fs_errno();
}
static int file_finish(FILE *file,int result,bool mutation)
{
    if (file) {
        if (mutation) {
            if (!result) result=FS_CALL(solar_os_storage_sync_file(file));
            solar_os_storage_notify_changed();
        }
        if (fclose(file) && !result) result=fs_errno();
    }
    return result;
}
static int fs_read_at(const char *path,uint64_t offset,void *bytes,size_t capacity,size_t *size)
{
    if (!size || (!bytes && capacity) || capacity>SOLAR_OS_NATIVE_FS_IO_MAX)
        return SOLAR_OS_NATIVE_ERROR_INVALID_ARGUMENT;
    *size=0;
    if (offset>INT32_MAX) return SOLAR_OS_NATIVE_ERROR_INVALID_SIZE;
    solar_os_disk_image_registry_lock();FILE *file=NULL;char normalized[160];
    int result=file_open(path,"rb",false,false,&file,normalized);
    if (!result && fseek(file,(long)offset,SEEK_SET)) result=fs_errno();
    if (!result && capacity) {
        *size=fread(bytes,1,capacity,file);
        if (ferror(file)) result=fs_errno();
    }
    result=file_finish(file,result,false);
    solar_os_disk_image_registry_unlock();return result;
}
static int fs_write_at(const char *path,uint64_t offset,const void *bytes,size_t size,bool create,size_t *written)
{
    if (!written || (!bytes && size) || size>SOLAR_OS_NATIVE_FS_IO_MAX)
        return SOLAR_OS_NATIVE_ERROR_INVALID_ARGUMENT;
    *written=0;
    if (offset>INT32_MAX || size>(uint64_t)INT32_MAX-offset) return SOLAR_OS_NATIVE_ERROR_INVALID_SIZE;
    solar_os_disk_image_registry_lock();FILE *file=NULL;char normalized[160];
    int result=file_open(path,"r+b",true,create,&file,normalized);
    if (!result && fseek(file,(long)offset,SEEK_SET)) result=fs_errno();
    if (!result && size) {
        *written=fwrite(bytes,1,size,file);
        if (*written!=size) result=fs_errno();
    }
    result=file_finish(file,result,true);
    solar_os_disk_image_registry_unlock();return result;
}
static int fs_truncate(const char *path,uint64_t size)
{
    if (size>INT32_MAX) return SOLAR_OS_NATIVE_ERROR_INVALID_SIZE;
    solar_os_disk_image_registry_lock();FILE *file=NULL;char normalized[160];
    int result=file_open(path,"r+b",true,false,&file,normalized);
    if (!result && ftruncate(fileno(file),(off_t)size)) result=fs_errno();
    result=file_finish(file,result,true);
    solar_os_disk_image_registry_unlock();return result;
}
static int fs_sync(const char *path)
{
    solar_os_disk_image_registry_lock();FILE *file=NULL;char normalized[160];
    int result=file_open(path,"rb",false,false,&file,normalized);
    if (!result) result=FS_CALL(solar_os_storage_sync_file(file));
    result=file_finish(file,result,false);
    solar_os_disk_image_registry_unlock();return result;
}
static int fs_replace(const solar_os_native_fs_replacement_v1_t *items,uint32_t count)
{
    if (!items || !count || count>SOLAR_OS_NATIVE_FS_REPLACE_MAX) return SOLAR_OS_NATIVE_ERROR_INVALID_ARGUMENT;
    solar_os_storage_replacement_t values[SOLAR_OS_NATIVE_FS_REPLACE_MAX];
    for (uint32_t i=0;i<count;++i)
        values[i]=(solar_os_storage_replacement_t){items[i].staged,items[i].active,items[i].backup};
    solar_os_disk_image_registry_lock();
    int result=FS_CALL(solar_os_storage_replace_files(values,count));
    solar_os_disk_image_registry_unlock();return result;
}
static int fs_copy(const char *from,const char *to,solar_os_native_fs_progress_fn progress,
    solar_os_native_cancel_fn cancel,void *user)
{
    solar_os_disk_image_registry_lock();
    int result=FS_CALL(solar_os_storage_copy_file_progress_cancel(from,to,progress,cancel,user));
    solar_os_disk_image_registry_unlock();return result;
}
static const char *fs_entry_type_name(uint32_t type)
{
    switch (type) {
    case SOLAR_OS_NATIVE_FS_FILE: return solar_os_storage_entry_type_name(SOLAR_OS_STORAGE_ENTRY_FILE);
    case SOLAR_OS_NATIVE_FS_DIRECTORY: return solar_os_storage_entry_type_name(SOLAR_OS_STORAGE_ENTRY_DIRECTORY);
    default: return solar_os_storage_entry_type_name(SOLAR_OS_STORAGE_ENTRY_OTHER);
    }
}

static const solar_os_native_fs_api_v1_t fs_api={
    .abi_version=SOLAR_OS_NATIVE_FS_ABI,.struct_size=sizeof(fs_api),
    .resolve=fs_resolve,.normalize=fs_normalize,.join=fs_join,.default_path=fs_default,.sibling=fs_sibling,
    .mount_point=fs_mount_point,.has_mount_prefix=solar_os_storage_path_has_mount_prefix,
    .stat=fs_stat,.exists=fs_exists,.scandir=fs_scandir,.mkdir=fs_mkdir,.remove=fs_remove,.rename=fs_rename,
    .read=fs_read,.write=fs_write,.read_at=fs_read_at,.write_at=fs_write_at,.truncate=fs_truncate,
    .sync=fs_sync,.replace=fs_replace,.copy=fs_copy,
    .revision=solar_os_storage_revision,.notify_changed=solar_os_storage_notify_changed,
    .entry_type_name=fs_entry_type_name,
};

static int volume_state(uint32_t role,bool *mounted,bool *unrecognized,char *path,size_t capacity)
{
    if (!mounted || !unrecognized || !path || !capacity || role>SOLAR_OS_NATIVE_VOLUME_SD)
        return SOLAR_OS_NATIVE_ERROR_INVALID_ARGUMENT;
    *mounted=false;*unrecognized=false;*path=0;const char *value=NULL;
    if (role==SOLAR_OS_NATIVE_VOLUME_FLASH) {
        *mounted=solar_os_storage_flash_is_mounted();*unrecognized=solar_os_storage_flash_is_unrecognized();
        value=solar_os_storage_flash_mount_point();
    } else if (role==SOLAR_OS_NATIVE_VOLUME_SD) {
        *mounted=solar_os_storage_sd_is_mounted();value=solar_os_storage_sd_mount_point();
    } else { *mounted=solar_os_storage_is_mounted();value=solar_os_storage_mount_point(); }
    if (value) {
        if (strlen(value)>=capacity) return SOLAR_OS_NATIVE_ERROR_INVALID_SIZE;
        memcpy(path,value,strlen(value)+1);
    }
    return SOLAR_OS_NATIVE_OK;
}
static int volume_status(uint32_t role,char *out,size_t capacity)
{
    if (!out || !capacity || role>SOLAR_OS_NATIVE_VOLUME_SD) return SOLAR_OS_NATIVE_ERROR_INVALID_ARGUMENT;
    *out=0;
    if (role==SOLAR_OS_NATIVE_VOLUME_FLASH) solar_os_storage_flash_get_status(out,capacity);
    else if (role==SOLAR_OS_NATIVE_VOLUME_SD) solar_os_storage_sd_get_status(out,capacity);
    else solar_os_storage_get_status(out,capacity);
    return SOLAR_OS_NATIVE_OK;
}
static int volume_usage(const char *path,solar_os_native_storage_usage_v1_t *out)
{
    if (!out) return SOLAR_OS_NATIVE_ERROR_INVALID_ARGUMENT;
    memset(out,0,sizeof(*out));solar_os_storage_usage_t value={0};
    int result=path?FS_CALL(solar_os_storage_get_usage_for_path(path,&value)):FS_CALL(solar_os_storage_get_usage(&value));
    if (!result) *out=(solar_os_native_storage_usage_v1_t){value.total_bytes,value.used_bytes,value.free_bytes};
    return result;
}
static int volume_block_usage(const char *name,solar_os_native_storage_usage_v1_t *out)
{
    if (!name || !out) return SOLAR_OS_NATIVE_ERROR_INVALID_ARGUMENT;
    memset(out,0,sizeof(*out));
    for (size_t i=0;i<solar_os_storage_block_count();++i) {
        solar_os_storage_block_t block={0};
        if (!solar_os_storage_get_block(i,&block) || strcmp(name,block.name)) continue;
        solar_os_storage_usage_t value={0};
        int result=FS_CALL(solar_os_storage_get_usage_for_block(&block,&value));
        if (!result) *out=(solar_os_native_storage_usage_v1_t){value.total_bytes,value.used_bytes,value.free_bytes};
        return result;
    }
    return SOLAR_OS_NATIVE_ERROR_NOT_FOUND;
}
static uint32_t volume_blocks(void) { return solar_os_storage_block_count(); }
static int volume_block(uint32_t index,solar_os_native_block_v1_t *out)
{
    if (!out) return SOLAR_OS_NATIVE_ERROR_INVALID_ARGUMENT;
    memset(out,0,sizeof(*out));solar_os_storage_block_t value={0};
    if (!solar_os_storage_get_block(index,&value)) return SOLAR_OS_NATIVE_ERROR_NOT_FOUND;
    return solar_os_native_storage_block_value(&value,out);
}
static uint32_t volume_mounts(void) { return solar_os_storage_mount_count(); }
static int volume_mount_info(uint32_t index,solar_os_native_mount_v1_t *out)
{
    if (!out) return SOLAR_OS_NATIVE_ERROR_INVALID_ARGUMENT;
    memset(out,0,sizeof(*out));solar_os_storage_mount_info_t value={0};
    if (!solar_os_storage_get_mount(index,&value)) return SOLAR_OS_NATIVE_ERROR_NOT_FOUND;
    return solar_os_native_storage_mount_value(&value,out);
}
static int volume_mount(const char *target,const char *path)
{ return target?FS_CALL(solar_os_storage_mount_volume(target,path)):FS_CALL(solar_os_storage_mount()); }
static int volume_unmount(const char *target)
{ return target?FS_CALL(solar_os_storage_unmount_volume(target)):FS_CALL(solar_os_storage_unmount()); }
static int volume_format(const char *target)
{
    if (!target) return SOLAR_OS_NATIVE_ERROR_INVALID_ARGUMENT;
    return FS_CALL(solar_os_storage_format(target));
}
static int volume_rescan(void) { return FS_CALL(solar_os_storage_rescan()); }
static const solar_os_native_volumes_api_v1_t volumes_api={
    .abi_version=SOLAR_OS_NATIVE_VOLUMES_ABI,.struct_size=sizeof(volumes_api),
    .state=volume_state,.status=volume_status,.root_mounted=solar_os_storage_root_is_mounted,
    .usage=volume_usage,.block_usage=volume_block_usage,.block_count=volume_blocks,.block=volume_block,
    .mount_count=volume_mounts,.mount_info=volume_mount_info,.mount=volume_mount,.unmount=volume_unmount,
    .format=volume_format,.rescan=volume_rescan,
};
const void *solar_os_native_fs_get_service(const char *name,uint32_t abi,uint32_t minimum)
{
#include "generated/native_fs_dispatch.inc"
    return NULL;
}
