#define _POSIX_C_SOURCE 200809L
#include <assert.h>
#include <errno.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include "solar_os_native_client.h"
#include "solar_os_native_fs.h"
#include "solar_os_storage.h"
#include "solar_os_memory.h"

static const char *root;
static bool busy,fail_sync,fail_alloc,cancel_copy;
static uint32_t revision;
static unsigned allocations,notifications,topology_changes,progress_calls;
static unsigned lock_depth;
static pthread_mutex_t topology_lock;
void solar_os_disk_image_registry_lock(void) { assert(!pthread_mutex_lock(&topology_lock));++lock_depth; }
void solar_os_disk_image_registry_unlock(void) { assert(lock_depth);--lock_depth;assert(!pthread_mutex_unlock(&topology_lock)); }
bool solar_os_disk_image_path_busy(const char *path) { assert(path);return busy; }
void *solar_os_memory_calloc(size_t n,size_t size,solar_os_memory_class_t cls,const char *tag)
{
    assert(cls==SOLAR_OS_MEMORY_EXTERNAL_PREFERRED && tag);
    if(fail_alloc)return NULL;
    void *p=calloc(n,size);if(p)++allocations;return p;
}
void solar_os_memory_free(void *p) { if(p)--allocations;free(p); }
uint32_t solar_os_storage_revision(void) { return revision; }
void solar_os_storage_notify_changed(void) { ++revision;++notifications; }

esp_err_t solar_os_storage_normalize_path(const char *in,char *out,size_t capacity)
{
    if(!in || !out || !capacity)return ESP_ERR_INVALID_ARG;
    if(strlen(in)>=capacity)return ESP_ERR_INVALID_SIZE;
    memcpy(out,in,strlen(in)+1);return ESP_OK;
}
esp_err_t solar_os_storage_resolve_path_at(const char *cwd,const char *arg,char *out,size_t capacity)
{
    if(!cwd)cwd=root;
    if(!arg)arg="";
    int size=snprintf(out,capacity,"%s/%s",cwd,arg);
    return size>=0 && (size_t)size<capacity?ESP_OK:ESP_ERR_INVALID_SIZE;
}
esp_err_t solar_os_storage_join_path(const char *base,const char *relative,char *out,size_t capacity)
{ return solar_os_storage_resolve_path_at(base,relative,out,capacity); }
esp_err_t solar_os_storage_default_path(const char *relative,char *out,size_t capacity)
{ return solar_os_storage_resolve_path_at(root,relative,out,capacity); }
esp_err_t solar_os_storage_sibling_path(const char *path,const char *suffix,char *out,size_t capacity)
{
    int size=snprintf(out,capacity,"%s%s",path,suffix);
    return size>=0 && (size_t)size<capacity?ESP_OK:ESP_ERR_INVALID_SIZE;
}
bool solar_os_storage_path_has_mount_prefix(const char *path)
{ return path && !strncmp(path,root,strlen(root)); }
esp_err_t solar_os_storage_path_mount_point(const char *path,char *out,size_t capacity)
{
    if(!solar_os_storage_path_has_mount_prefix(path))return ESP_ERR_NOT_FOUND;
    return solar_os_storage_normalize_path(root,out,capacity);
}
esp_err_t solar_os_storage_stat(const char *path,solar_os_storage_metadata_t *out)
{
    struct stat value;if(stat(path,&value))return errno==ENOENT?ESP_ERR_NOT_FOUND:ESP_FAIL;
    *out=(solar_os_storage_metadata_t){.type=S_ISDIR(value.st_mode)?SOLAR_OS_STORAGE_ENTRY_DIRECTORY:SOLAR_OS_STORAGE_ENTRY_FILE,
        .size_bytes=value.st_size,.modified_seconds=value.st_mtime,.mode=value.st_mode};
    return ESP_OK;
}
esp_err_t solar_os_storage_exists(const char *path,bool *out)
{ struct stat value;*out=stat(path,&value)==0;return ESP_OK; }
esp_err_t solar_os_storage_scandir(const char *path,size_t cursor,size_t limit,
    solar_os_storage_entry_t *entries,size_t *count,size_t *next,bool *more)
{
    assert(!strcmp(path,root) && limit);
    *count=cursor?0:1;*next=cursor+*count;*more=false;
    if(*count) {
        strcpy(entries[0].name,"file");
        entries[0].metadata=(solar_os_storage_metadata_t){SOLAR_OS_STORAGE_ENTRY_FILE,UINT64_C(0x100000003),-7,0644};
    }
    return ESP_OK;
}
esp_err_t solar_os_storage_mkdir(const char *path) { return mkdir(path,0700)?ESP_FAIL:ESP_OK; }
esp_err_t solar_os_storage_makedirs(const char *path,bool exist_ok)
{ return !mkdir(path,0700) || (exist_ok && errno==EEXIST)?ESP_OK:ESP_FAIL; }
esp_err_t solar_os_storage_rmdir(const char *path) { return rmdir(path)?ESP_FAIL:ESP_OK; }
esp_err_t solar_os_storage_remove(const char *path) { return unlink(path)?ESP_FAIL:ESP_OK; }
esp_err_t solar_os_storage_rename(const char *a,const char *b) { return rename(a,b)?ESP_FAIL:ESP_OK; }
esp_err_t solar_os_storage_sync_file(FILE *file)
{
    assert(lock_depth);
    if(fail_sync) { errno=ENOSPC;return ESP_FAIL; }
    return fflush(file) || fsync(fileno(file))?ESP_FAIL:ESP_OK;
}
esp_err_t solar_os_storage_read_file(const char *path,void *data,size_t capacity,size_t *size)
{
    FILE *file=fopen(path,"rb");if(!file)return ESP_ERR_NOT_FOUND;
    *size=capacity?fread(data,1,capacity,file):0;int result=ferror(file)?ESP_FAIL:ESP_OK;
    fclose(file);return result;
}
esp_err_t solar_os_storage_write_file(const char *path,const void *data,size_t size,bool append)
{
    assert(lock_depth);
    if(busy) { errno=EBUSY;return ESP_ERR_INVALID_STATE; }
    FILE *file=fopen(path,append?"ab":"wb");if(!file)return ESP_FAIL;
    int result=size && fwrite(data,1,size,file)!=size?ESP_FAIL:solar_os_storage_sync_file(file);
    fclose(file);solar_os_storage_notify_changed();return result;
}
esp_err_t solar_os_storage_replace_files(const solar_os_storage_replacement_t *items,size_t count)
{
    assert(lock_depth && count==1);
    assert(items[0].staged_path && items[0].active_path && items[0].backup_path);
    return ESP_ERR_INVALID_STATE; /* Test correct public error propagation. */
}
esp_err_t solar_os_storage_copy_file_progress_cancel(const char *a,const char *b,
    solar_os_storage_copy_progress_fn progress,solar_os_storage_cancel_fn cancel,void *user)
{
    assert(lock_depth && a && b);
    if(cancel && cancel(user))return ESP_ERR_TIMEOUT;
    if(progress)progress(UINT64_C(0x100000003),UINT64_C(0x200000004),user);
    return ESP_OK;
}

bool solar_os_storage_is_mounted(void) { return true; }
bool solar_os_storage_flash_is_mounted(void) { return false; }
bool solar_os_storage_flash_is_unrecognized(void) { return true; }
bool solar_os_storage_sd_is_mounted(void) { return true; }
const char *solar_os_storage_mount_point(void) { return root; }
const char *solar_os_storage_flash_mount_point(void) { return "/flash"; }
const char *solar_os_storage_sd_mount_point(void) { return root; }
void solar_os_storage_get_status(char *out,size_t capacity) { snprintf(out,capacity,"mounted"); }
void solar_os_storage_flash_get_status(char *out,size_t capacity) { snprintf(out,capacity,"unrecognized"); }
void solar_os_storage_sd_get_status(char *out,size_t capacity) { snprintf(out,capacity,"sd"); }
bool solar_os_storage_root_is_mounted(void) { return true; }
esp_err_t solar_os_storage_get_usage(solar_os_storage_usage_t *out)
{ *out=(solar_os_storage_usage_t){UINT64_C(1)<<42,100,(UINT64_C(1)<<42)-100};return ESP_OK; }
esp_err_t solar_os_storage_get_usage_for_path(const char *path,solar_os_storage_usage_t *out)
{ assert(!strcmp(path,root));return solar_os_storage_get_usage(out); }
esp_err_t solar_os_storage_get_usage_for_block(const solar_os_storage_block_t *block,solar_os_storage_usage_t *out)
{ assert(!strcmp(block->name,"disk0"));return solar_os_storage_get_usage(out); }
size_t solar_os_storage_block_count(void) { return 1; }
bool solar_os_storage_get_block(size_t index,solar_os_storage_block_t *out)
{
    if(index)return false;
    memset(out,0,sizeof(*out));strcpy(out->name,"disk0");strcpy(out->fs,"FAT");strcpy(out->type_name,"test");
    out->type=SOLAR_OS_STORAGE_BLOCK_PARTITION;out->partition_number=2;
    out->bootable=true;out->mountable=true;out->whole_disk_filesystem=true;out->logical_volume=9;
    out->start_sector=UINT64_C(0x100000003);out->size_bytes=UINT64_C(1)<<42;out->sector_size=512;
    return true;
}
size_t solar_os_storage_mount_count(void) { return 1; }
bool solar_os_storage_get_mount(size_t index,solar_os_storage_mount_info_t *out)
{
    if(index)return false;
    memset(out,0,sizeof(*out));strcpy(out->mount_point,"/usb");strcpy(out->name,"disk0");
    out->type=SOLAR_OS_STORAGE_MOUNT_USB;return true;
}
esp_err_t solar_os_storage_mount(void) { ++topology_changes;return ESP_OK; }
esp_err_t solar_os_storage_mount_volume(const char *target,const char *point)
{ assert(!strcmp(target,"disk0") && !strcmp(point,"/usb"));++topology_changes;return ESP_OK; }
esp_err_t solar_os_storage_unmount(void) { ++topology_changes;return ESP_OK; }
esp_err_t solar_os_storage_unmount_volume(const char *target)
{ assert(!strcmp(target,"disk0"));++topology_changes;return ESP_OK; }
esp_err_t solar_os_storage_format(const char *target)
{ assert(!strcmp(target,"disk0"));++topology_changes;return ESP_ERR_NOT_SUPPORTED; }
esp_err_t solar_os_storage_rescan(void) { ++topology_changes;return ESP_OK; }

static bool copy_cancel(void *user) { assert(user==&progress_calls);return cancel_copy; }
static void copy_progress(uint64_t done,uint64_t total,void *user)
{ assert(user==&progress_calls && done==UINT64_C(0x100000003) && total==UINT64_C(0x200000004));++progress_calls; }

int main(int argc,char **argv)
{
    assert(argc==2);root=argv[1];
    pthread_mutexattr_t attr;assert(!pthread_mutexattr_init(&attr));
    assert(!pthread_mutexattr_settype(&attr,PTHREAD_MUTEX_RECURSIVE));
    assert(!pthread_mutex_init(&topology_lock,&attr));pthread_mutexattr_destroy(&attr);
    const solar_os_native_fs_api_v1_t *fs=solar_os_native_fs_get_service("fs",1,sizeof(*fs));assert(fs);
    const solar_os_native_volumes_api_v1_t *volumes=solar_os_native_fs_get_service("storage.volumes",1,sizeof(*volumes));assert(volumes);
    char path[160],renamed[160];assert(!fs->join(root,"file",path,sizeof(path)));
    size_t size=0;assert(!fs->write(path,"abcdef",6,false));
    char data[16]={0};assert(!fs->read_at(path,2,data,3,&size) && size==3 && !memcmp(data,"cde",3));
    assert(fs->read_at(path,UINT64_MAX,data,1,&size)==SOLAR_OS_NATIVE_ERROR_INVALID_SIZE);
    assert(!fs->write_at(path,2,"XY",2,false,&size) && size==2);
    assert(!fs->read(path,data,sizeof(data),&size) && size==6 && !memcmp(data,"abXYef",6));
    char outside[]="/tmp/native-fs-outside-XXXXXX";
    int fd=mkstemp(outside);assert(fd>=0 && write(fd,"safe",4)==4);close(fd);
    assert(fs->write_at(outside,0,"bad",3,true,&size)==SOLAR_OS_NATIVE_ERROR_NOT_FOUND);
    FILE *checked=fopen(outside,"rb");assert(checked && fread(data,1,4,checked)==4 && !memcmp(data,"safe",4));
    fclose(checked);unlink(outside);
    assert(!fs->join(root,"created",renamed,sizeof(renamed)));
    assert(!fs->write_at(renamed,0,"new",3,true,&size) && size==3);
    assert(!fs->remove(renamed,false));
    busy=true;assert(fs->write_at(path,0,"bad",3,false,&size)==SOLAR_OS_NATIVE_ERROR_BUSY);
    assert(fs->write(path,"bad",3,false)==SOLAR_OS_NATIVE_ERROR_BUSY);busy=false;
    fail_sync=true;
    assert(fs->write_at(path,0,"Z",1,false,&size)==SOLAR_OS_NATIVE_ERROR_NO_SPACE && size==1);
    fail_sync=false;assert(!fs->truncate(path,3));
    solar_os_native_fs_metadata_v1_t metadata;assert(!fs->stat(path,&metadata) && metadata.bytes==3 && metadata.type==SOLAR_OS_NATIVE_FS_FILE);
    assert(!fs->sync(path));
    solar_os_native_fs_entry_v1_t entry;uint32_t count=9;uint64_t next=0;bool more=true;
    assert(!fs->scandir(root,0,1,&entry,&count,&next,&more) && count==1 && next==1 && !more);
    assert(entry.metadata.bytes==UINT64_C(0x100000003) && entry.metadata.modified_seconds==-7);
    fail_alloc=true;assert(fs->scandir(root,0,1,&entry,&count,&next,&more)==SOLAR_OS_NATIVE_ERROR_NO_MEMORY);
    fail_alloc=false;assert(!allocations);
    assert(!fs->copy(path,path,copy_progress,copy_cancel,&progress_calls) && progress_calls==1);
    cancel_copy=true;assert(fs->copy(path,path,copy_progress,copy_cancel,&progress_calls)==SOLAR_OS_NATIVE_ERROR_TIMEOUT);
    assert(!fs->sibling(path,".new",renamed,sizeof(renamed)));
    solar_os_native_fs_replacement_v1_t replacement={path,renamed,path};
    assert(fs->replace(&replacement,1)==SOLAR_OS_NATIVE_ERROR_INVALID_STATE);
    assert(!fs->rename(path,renamed));assert(!fs->remove(renamed,false));
    bool mounted=false,unrecognized=false;
    assert(!volumes->state(SOLAR_OS_NATIVE_VOLUME_FLASH,&mounted,&unrecognized,path,sizeof(path)) && !mounted && unrecognized && !strcmp(path,"/flash"));
    solar_os_native_storage_usage_v1_t usage;
    assert(!volumes->usage(NULL,&usage) && usage.total==(UINT64_C(1)<<42));
    assert(!volumes->block_usage("disk0",&usage));
    solar_os_native_block_v1_t block;assert(!volumes->block(0,&block));
    assert(block.start_sector==UINT64_C(0x100000003) && block.flags==(SOLAR_OS_NATIVE_BLOCK_BOOTABLE|SOLAR_OS_NATIVE_BLOCK_MOUNTABLE|SOLAR_OS_NATIVE_BLOCK_WHOLE_FS));
    solar_os_native_mount_v1_t mount;assert(!volumes->mount_info(0,&mount) && mount.type==SOLAR_OS_NATIVE_MOUNT_USB);
    assert(!topology_changes);assert(!volumes->mount("disk0","/usb"));
    assert(volumes->format("disk0")==SOLAR_OS_NATIVE_ERROR_NOT_SUPPORTED);
    assert(!volumes->unmount("disk0") && !volumes->rescan() && topology_changes==4);
    assert(notifications && !allocations && !lock_depth);
    assert(!pthread_mutex_destroy(&topology_lock));
    puts("native filesystem values, I/O, ownership guards and cancellation passed");return 0;
}
