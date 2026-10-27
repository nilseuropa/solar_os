// SPDX-License-Identifier: GPL-2.0-or-later
#ifndef SOLAR_OS_DOOM_PORT_IMPLEMENTATION
#define SOLAR_OS_DOOM_PORT_IMPLEMENTATION
#endif
#include "port.h"
#include "doomdef.h"
#include "doomstat.h"
#include "i_system.h"
#include "i_main.h"
#include "lprintf.h"
#include "r_fps.h"
#include "d_deh.h"
#undef malloc
#undef calloc
#undef realloc
#undef free
#undef strdup

int solar_os_doom_errno;
typedef union solar_os_doom_block {
    struct { union solar_os_doom_block *next, *prev; size_t size; } h;
    long double alignment;
} solar_os_doom_block;
static solar_os_doom_block *blocks;

void *solar_os_doom_malloc(size_t size)
{
    if (size > SIZE_MAX - sizeof(solar_os_doom_block)) return NULL;
    solar_os_doom_block *block = solar_os_doom_memory->alloc(sizeof(*block) + size);
    if (!block) return NULL;
    block->h.next = blocks; block->h.prev = NULL; block->h.size = size;
    if (blocks) blocks->h.prev = block;
    blocks = block;
    return block + 1;
}
void solar_os_doom_free(void *pointer)
{
    if (!pointer) return;
    solar_os_doom_block *block = (solar_os_doom_block *)pointer - 1;
    if (block->h.prev) block->h.prev->h.next = block->h.next;
    else blocks = block->h.next;
    if (block->h.next) block->h.next->h.prev = block->h.prev;
    (solar_os_doom_memory->free)(block);
}
void *solar_os_doom_calloc(size_t count, size_t size)
{
    if (size && count > SIZE_MAX / size) return NULL;
    void *p = solar_os_doom_malloc(count * size);
    if (p) memset(p, 0, count * size);
    return p;
}
void *solar_os_doom_realloc(void *pointer, size_t size)
{
    if (!size) { solar_os_doom_free(pointer); return NULL; }
    void *next = solar_os_doom_malloc(size);
    if (!next) return NULL;
    if (pointer) {
        size_t old = ((solar_os_doom_block *)pointer - 1)->h.size;
        memcpy(next, pointer, old < size ? old : size);
        solar_os_doom_free(pointer);
    }
    return next;
}
char *solar_os_doom_strdup(const char *text)
{
    size_t size = strlen(text) + 1;
    char *copy = solar_os_doom_malloc(size);
    if (copy) memcpy(copy, text, size);
    return copy;
}
void solar_os_doom_cleanup(void)
{
    while (blocks) solar_os_doom_free(blocks + 1);
}
_Noreturn void solar_os_doom_exit(int status)
{
    solar_os_doom_result = status;
    longjmp(solar_os_doom_trap, 1);
}
_Noreturn void solar_os_doom_abort(void)
{
    solar_os_doom_printf("doom: engine assertion failed\n");
    solar_os_doom_exit(1);
}
int solar_os_doom_snprintf(char *out, size_t size, const char *format, ...)
{
    va_list args; va_start(args, format);
    int result = solar_os_doom_libc->format(out, size, format, args);
    va_end(args); return result;
}
int solar_os_doom_sprintf(char *out, const char *format, ...)
{
    va_list args; va_start(args, format);
    int result = solar_os_doom_libc->format(out, INT_MAX, format, args);
    va_end(args); return result;
}
int solar_os_doom_vsprintf(char *out, const char *format, va_list args)
{ return solar_os_doom_libc->format(out, INT_MAX, format, args); }
static int report(const char *format, va_list args)
{
    char buffer[512];
    int result = solar_os_doom_libc->format(buffer, sizeof(buffer), format, args);
    solar_os_doom_host->write_utf8(buffer, strlen(buffer));
    return result;
}
int solar_os_doom_printf(const char *format, ...)
{
    va_list args; va_start(args, format);
    int result = report(format, args); va_end(args); return result;
}
int solar_os_doom_fprintf(FILE *file, const char *format, ...)
{
    va_list args; va_start(args, format);
    int result = report(format, args); va_end(args); return result;
}
int cons_error_mask = -1, cons_output_mask = -1;
int lprintf(OutputLevels priority, const char *format, ...)
{
    if (!(priority & (LO_ERROR | LO_WARN | LO_CONFIRM))) return 0;
    va_list args; va_start(args, format);
    int result = report(format, args); va_end(args); return result;
}
void I_Error(const char *format, ...)
{
    va_list args; va_start(args, format); report(format, args); va_end(args);
    solar_os_doom_exit(1);
}

/* The upstream engine disables config/save files. No process-wide VFS state. */
char *solar_os_doom_getenv(const char *name) { return NULL; }
int solar_os_doom_stat(const char *path, struct stat *status) { return -1; }
int solar_os_doom_access(const char *path, int mode)
{
    uint32_t size;
    return solar_os_doom_app->file_size(path, &size) ? -1 : 0;
}
FILE *solar_os_doom_fopen(const char *path, const char *mode) { return NULL; }
int solar_os_doom_fclose(FILE *file) { return 0; }
size_t solar_os_doom_fread(void *p, size_t s, size_t n, FILE *f) { return 0; }
size_t solar_os_doom_fwrite(const void *p, size_t s, size_t n, FILE *f) { return 0; }
int solar_os_doom_fseek(FILE *f, long offset, int whence) { return -1; }
long solar_os_doom_ftell(FILE *f) { return -1; }
char *solar_os_doom_fgets(char *out, int size, FILE *f) { return NULL; }
int solar_os_doom_remove(const char *path) { return -1; }
int solar_os_doom_scan(const char *text, const char *format, ...) { return 0; }
int solar_os_doom_fscan(FILE *file, const char *format, ...) { return EOF; }
int solar_os_doom_feof(FILE *file) { return 1; }
int solar_os_doom_fgetc(FILE *file) { return EOF; }
int solar_os_doom_fputc(int ch, FILE *file) { return EOF; }
void solar_os_doom_setbuf(FILE *file, char *buffer) {}
char *solar_os_doom_strlwr(char *text)
{
    for (char *p = text; *p; p++) if (*p >= 'A' && *p <= 'Z') *p += 'a' - 'A';
    return text;
}
void ProcessDehFile(const char *name, const char *out, int lump)
{ I_Error("doom: DeHackEd patches are unsupported\n"); }

typedef struct { bool used; uint32_t offset, size; } solar_os_doom_file;
static solar_os_doom_file files[8];
static uint32_t wad_size;
static void read_exact(uint32_t offset, void *data, size_t length)
{
    if (offset > wad_size || length > wad_size - offset) I_Error("doom: WAD read outside file\n");
    uint8_t *out = data;
    while (length) {
        solar_os_doom_poll();
        size_t chunk = length > 65536 ? 65536 : length, actual = 0;
        if (solar_os_doom_app->read_file(solar_os_doom_path, offset, out, chunk, &actual) || actual != chunk)
            I_Error("doom: WAD read failed\n");
        offset += chunk; out += chunk; length -= chunk;
    }
}
static uint32_t le32(const uint8_t *p)
{ return p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24; }
void solar_os_doom_validate_wad(void)
{
    uint8_t header[12];
    if (solar_os_doom_app->file_size(solar_os_doom_path, &wad_size) || wad_size < 12 || wad_size > INT32_MAX)
        I_Error("doom: WAD missing or invalid size\n");
    read_exact(0, header, sizeof(header));
    uint32_t count = le32(header + 4), offset = le32(header + 8);
    if (memcmp(header, "IWAD", 4) || !count || count > 65535 || offset > wad_size || count > (wad_size - offset) / 16)
        I_Error("doom: invalid IWAD directory\n");
    uint8_t *entries = solar_os_doom_malloc(4096);
    if (!entries) I_Error("doom: no memory for WAD directory\n");
    for (uint32_t i = 0; i < count;) {
        uint32_t batch = count - i;
        if (batch > 256) batch = 256;
        read_exact(offset + i * 16, entries, batch * 16);
        for (uint32_t j = 0; j < batch; j++) {
            const uint8_t *entry = entries + j * 16;
            uint32_t pos = le32(entry), size = le32(entry + 4);
            if (pos > wad_size || size > wad_size - pos) I_Error("doom: invalid WAD lump\n");
        }
        i += batch;
    }
    solar_os_doom_free(entries);
}
int I_Open(const char *path, int flags)
{
    if (strcmp(path, "DOOM1.WAD")) return -1;
    for (unsigned i = 1; i < sizeof(files) / sizeof(files[0]); i++) {
        if (!files[i].used) { files[i] = (solar_os_doom_file){true, 0, wad_size}; return i; }
    }
    return -1;
}
static solar_os_doom_file *file_at(int fd)
{
    if (fd <= 0 || fd >= sizeof(files) / sizeof(files[0]) || !files[fd].used) I_Error("doom: invalid WAD handle\n");
    return &files[fd];
}
int I_Filelength(int fd) { return file_at(fd)->size; }
int I_Lseek(int fd, off_t offset, int whence)
{
    solar_os_doom_file *file = file_at(fd);
    int64_t base = whence == SEEK_SET ? 0 : whence == SEEK_CUR ? file->offset : file->size;
    int64_t next = base + offset;
    if (next < 0 || next > file->size) I_Error("doom: invalid WAD seek\n");
    return file->offset = next;
}
void I_Read(int fd, void *data, size_t length)
{
    solar_os_doom_file *file = file_at(fd);
    read_exact(file->offset, data, length); file->offset += length;
}
void I_Close(int fd) { file_at(fd)->used = false; }
/* Track each cached lump independently. Unlocks can occur out of order. */
typedef struct solar_os_doom_mapping {
    struct solar_os_doom_mapping *next;
    uint32_t offset; size_t size; unsigned refs;
    uint64_t last_used;
    uint8_t data[];
} solar_os_doom_mapping;
static solar_os_doom_mapping *maps;
static size_t cached_bytes;
/* Reuse recently unlocked lumps without reopening storage every frame. Only
 * unlocked buffers can be evicted; locked engine pointers stay valid. */
static bool evict_mapping(void)
{
    solar_os_doom_mapping **oldest = NULL;
    for (solar_os_doom_mapping **p = &maps; *p; p = &(*p)->next)
        if (!(*p)->refs && (!oldest || (*p)->last_used < (*oldest)->last_used)) oldest = p;
    if (!oldest) return false;
    solar_os_doom_mapping *map = *oldest;
    *oldest = map->next; cached_bytes -= map->size;
    solar_os_doom_free(map);
    return true;
}
void *I_Mmap(void *addr, size_t length, int prot, int flags, int fd, off_t offset)
{
    file_at(fd);
    if (offset < 0 || (uint64_t)offset > wad_size || length > wad_size - offset)
        I_Error("doom: invalid lump range\n");
    for (solar_os_doom_mapping *map = maps; map; map = map->next)
        if (map->offset == offset && map->size == length) {
            if (!map->refs) cached_bytes -= map->size;
            map->refs++; return map->data;
        }
    while (cached_bytes > 512U * 1024U && evict_mapping()) {}
    if (length > SIZE_MAX - sizeof(solar_os_doom_mapping)) I_Error("doom: lump too large\n");
    solar_os_doom_mapping *map = solar_os_doom_malloc(sizeof(*map) + length);
    while (!map && evict_mapping()) map = solar_os_doom_malloc(sizeof(*map) + length);
    if (!map) I_Error("doom: no memory for WAD lump\n");
    map->offset = offset; map->size = length; map->refs = 1;
    map->last_used = 0;
    map->next = maps; maps = map;
    read_exact(offset, map->data, length);
    return map->data;
}
int I_Munmap(void *addr, size_t length)
{
    solar_os_doom_mapping **link = &maps;
    while (*link && ((*link)->data != addr || (*link)->size != length)) link = &(*link)->next;
    if (!*link) I_Error("doom: invalid lump unlock\n");
    solar_os_doom_mapping *map = *link;
    if (!map->refs) I_Error("doom: excess lump unlock\n");
    if (!--map->refs) { cached_bytes += map->size; map->last_used = solar_os_doom_app->time_us(); }
    return 0;
}
const char *I_DoomExeDir(void) { return ""; }
char *I_FindFile(const char *name, const char *ext) { return NULL; }
void I_SetAffinityMask(void) {}
int realtime;
int I_GetTime_RealTime(void)
{ solar_os_doom_poll(); return (solar_os_doom_app->time_us() - solar_os_doom_epoch) * TICRATE / 1000000; }
fixed_t I_GetTimeFrac(void) { return FRACUNIT; }
void I_GetTime_SaveMS(void) {}
unsigned long I_GetRandomTimeSeed(void) { return solar_os_doom_app->time_us(); }
void I_uSleep(unsigned long usecs)
{ solar_os_doom_poll(); solar_os_doom_app->sleep_ms(usecs / 1000 + (usecs % 1000 != 0)); }
const char *I_GetVersionString(char *out, size_t size)
{ solar_os_doom_snprintf(out, size, "SolarOS PrBoom 2.5.0"); return out; }
const char *I_SigString(char *out, size_t size, int signum) { return "DOOM error"; }
