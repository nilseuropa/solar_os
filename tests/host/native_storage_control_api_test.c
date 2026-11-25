#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "solar_os_native_client.h"
#include "solar_os_native_storage_control.h"
#include "solar_os_native_handles.h"
#include "solar_os_memory.h"
#include "solar_os_disk_image.h"
#include "solar_os_ramfs.h"
#include "solar_os_nvs_backup.h"
typedef struct { uint32_t handle; const void *owner; int fd, io_lock; bool mounted, registered; char mount[32]; } disk_image_t;
static disk_image_t image;
static int images_lock;
static unsigned allocations, reads, mounts;
static bool fail_sync, unknown_block;
static const solar_os_native_disk_image_api_v1_t *images;
static solar_os_native_handle_t current;
static const char *image_path;
static void _lock_acquire(int *lock) { (void)lock; }
static void _lock_release(int *lock) { (void)lock; }
static void _lock_acquire_recursive(int *lock) { (void)lock; }
static void _lock_release_recursive(int *lock) { (void)lock; }
static disk_image_t *find_handle(uint32_t id, const void *owner) { return id && id==image.handle && owner==image.owner ? &image : NULL; }
int __real_fsync(int fd);
int __wrap_fsync(int fd) { if (fail_sync) { errno=EIO; return -1; } return __real_fsync(fd); }
#include "image_close.inc"
void *solar_os_memory_calloc(size_t n,size_t size,solar_os_memory_class_t cls,const char *tag)
{ assert(cls==SOLAR_OS_MEMORY_EXTERNAL_PREFERRED && tag); void *p=calloc(n,size); if(p)++allocations; return p; }
void solar_os_memory_free(void *p) { if(p)--allocations; free(p); }
esp_err_t solar_os_disk_image_open(const char *path,const void *owner,uint32_t *id,uint32_t *size)
{
    assert(!strcmp(path,image_path)); if(image.handle)return ESP_ERR_INVALID_STATE;
    int fd=open(path,O_CREAT|O_RDWR,0600); assert(fd>=0 && !ftruncate(fd,4096));
    image=(disk_image_t){.handle=1,.owner=owner,.fd=fd}; *id=1; *size=4096; return ESP_OK;
}
esp_err_t solar_os_disk_image_read(uint32_t id,const void *owner,uint32_t offset,void *bytes,size_t size)
{
    assert(find_handle(id,owner)); assert(images->close(current)==SOLAR_OS_NATIVE_ERROR_BUSY); ++reads;
    return pread(image.fd,bytes,size,offset)==(ssize_t)size ? ESP_OK : ESP_FAIL;
}
esp_err_t solar_os_disk_image_write(uint32_t id,const void *owner,uint32_t offset,const void *bytes,size_t size)
{ assert(find_handle(id,owner)); return pwrite(image.fd,bytes,size,offset)==(ssize_t)size ? ESP_OK : ESP_FAIL; }
esp_err_t solar_os_disk_image_sync(uint32_t id,const void *owner) { assert(find_handle(id,owner)); return sync_image(&image); }
esp_err_t solar_os_disk_image_mount(const char *path,const char *mount,unsigned partition)
{ assert(!strcmp(path,image_path) && !strcmp(mount,"/mnt/image") && partition==4); return ESP_OK; }
esp_err_t solar_os_disk_image_unmount(const char *target) { assert(!strcmp(target,"/mnt/image")); return ESP_OK; }
esp_err_t solar_os_disk_image_create(const char *path,uint32_t size_mib) { assert(!strcmp(path,image_path) && size_mib==16); return ESP_OK; }
size_t solar_os_disk_image_mount_count(void) { return 1; }
bool solar_os_disk_image_get_mount(size_t index,solar_os_storage_mount_info_t *out)
{ if(index)return false; *out=(solar_os_storage_mount_info_t){.mount_point="/mnt/image",.name="img0",.type=SOLAR_OS_STORAGE_MOUNT_IMAGE}; return true; }
bool solar_os_disk_image_get_block(size_t index,solar_os_storage_block_t *out)
{
    if(index)return false;
    *out=(solar_os_storage_block_t){.name="img0p4",.type=unknown_block?(solar_os_storage_block_type_t)99:SOLAR_OS_STORAGE_BLOCK_PARTITION,
        .partition_number=4,.mbr_type=0x0b,.bootable=true,.mountable=true,.mounted=true,.logical_volume=255,
        .start_sector=UINT64_C(5000000000),.sector_count=UINT64_C(6000000000),.sector_size=512,.size_bytes=UINT64_C(7000000000),.fs="fat32"};
    return true;
}
bool solar_os_disk_image_path_busy(const char *path) { return !strcmp(path,image_path) && image.handle!=0; }
bool solar_os_disk_image_backing_busy(const char *mount) { return !strcmp(mount,"/sd") && image.handle!=0; }
esp_err_t solar_os_ramfs_mount(const char *path,size_t quota) { assert(!strcmp(path,"/ram") && quota==UINT32_MAX); ++mounts; return ESP_OK; }
esp_err_t solar_os_ramfs_unmount(const char *path) { assert(!strcmp(path,"/ram")); return ESP_OK; }
size_t solar_os_ramfs_mount_count(void) { return 1; }
bool solar_os_ramfs_get_info(size_t index,solar_os_ramfs_info_t *out)
{ if(index)return false; *out=(solar_os_ramfs_info_t){.mount_point="/ram",.total_bytes=4096,.used_bytes=512,.free_bytes=3584,.file_count=7,.dir_count=3,.open_count=16}; return true; }
bool solar_os_ramfs_path_has_mount_prefix(const char *path) { return !strncmp(path,"/ram/",5); }
esp_err_t solar_os_ramfs_path_mount_point(const char *path,char *out,size_t capacity)
{ assert(!strcmp(path,"/ram/file") && capacity>=5); strcpy(out,"/ram"); return ESP_OK; }
esp_err_t solar_os_ramfs_get_usage_for_path(const char *path,uint64_t *total,uint64_t *used,uint64_t *free_bytes)
{ assert(!strcmp(path,"/ram/file")); *total=4096; *used=512; *free_bytes=3584; return ESP_OK; }
esp_err_t solar_os_nvs_backup_create(const char *path,solar_os_nvs_backup_result_t *out)
{ assert(!strcmp(path,SOLAR_OS_NVS_BACKUP_DEFAULT_PATH)); *out=(solar_os_nvs_backup_result_t){UINT32_MAX,65536,0xdeadbeef,false}; return ESP_OK; }
esp_err_t solar_os_nvs_backup_restore(const char *path,solar_os_nvs_backup_result_t *out)
{ (void)solar_os_nvs_backup_create(path,out); out->reboot_required=true; return ESP_FAIL; }
int main(int argc,char **argv)
{
    assert(argc==2); image_path=argv[1];
    images=solar_os_native_storage_control_get_service("storage.images",1,sizeof(*images)); assert(images);
    const solar_os_native_ramfs_api_v1_t *ramfs=solar_os_native_storage_control_get_service("storage.ramfs",1,sizeof(*ramfs)); assert(ramfs);
    const solar_os_native_settings_backup_api_v1_t *backup=solar_os_native_storage_control_get_service("settings.backup",1,sizeof(*backup)); assert(backup);
    uint64_t size; assert(!images->open(image_path,&current,&size) && size==4096);
    assert(!images->write(current,512,"abc",3)); char data[4]={0}; assert(!images->read(current,512,data,3) && !memcmp(data,"abc",3));
    assert(images->read(current,UINT64_C(0x100000000),data,1)==SOLAR_OS_NATIVE_ERROR_INVALID_ARGUMENT && reads==1);
    int fd=image.fd; fail_sync=true; assert(images->close(current)==SOLAR_OS_NATIVE_ERROR_FAILED);
    assert(!image.handle && fcntl(fd,F_GETFD)==-1 && errno==EBADF);
    assert(images->read(current,0,data,1)==SOLAR_OS_NATIVE_ERROR_INVALID_STATE && reads==1);
    solar_os_native_handle_t replacement; fail_sync=false; assert(!images->open(image_path,&replacement,&size));
    const void *new_owner=image.owner; assert(!images->close(current) && image.handle && image.owner==new_owner);
    current=replacement; assert(!images->read(current,512,data,3) && !memcmp(data,"abc",3)); assert(!images->close(current));
    solar_os_native_mount_v1_t mount; assert(!images->mount_info(0,&mount) && mount.type==SOLAR_OS_NATIVE_MOUNT_IMAGE && !strcmp(mount.path,"/mnt/image"));
    solar_os_native_block_v1_t block; assert(!images->block(0,&block) && block.partition==4 && block.logical_volume==255);
    assert(block.start_sector==UINT64_C(5000000000) && block.sector_count==UINT64_C(6000000000) && block.bytes==UINT64_C(7000000000));
    assert(block.flags==(SOLAR_OS_NATIVE_BLOCK_BOOTABLE|SOLAR_OS_NATIVE_BLOCK_MOUNTABLE|SOLAR_OS_NATIVE_BLOCK_MOUNTED));
    unknown_block=true; assert(images->block(0,&block)==SOLAR_OS_NATIVE_ERROR_INVALID_STATE);
    assert(ramfs->mount("/ram",UINT64_C(0x100000000))==SOLAR_OS_NATIVE_ERROR_INVALID_SIZE && !mounts);
    assert(!ramfs->mount("/ram",UINT32_MAX) && mounts==1); solar_os_native_ramfs_info_v1_t ram;
    assert(!ramfs->info(0,&ram) && ram.file_count==7 && ram.directory_count==3 && ram.open_count==16);
    solar_os_native_storage_usage_v1_t usage; assert(!ramfs->usage("/ram/file",&usage) && usage.total==4096 && usage.free==3584);
    solar_os_native_settings_backup_v1_t result; assert(!backup->create(backup->default_path(),&result) && result.partition_size==UINT32_MAX && result.crc32==0xdeadbeef);
    assert(backup->restore(backup->default_path(),&result)==SOLAR_OS_NATIVE_ERROR_FAILED && result.reboot_required);
    assert(!allocations && !solar_os_native_handle_count()); puts("native storage controls: consumed image close, stable metadata, quotas and reboot results passed"); return 0;
}
