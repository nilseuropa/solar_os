// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include "solar_os_native_abi.h"
#include "solar_os_native_app_abi.h"
#include "solar_os_native_libc_abi.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <stdint.h>
#include <limits.h>
#include <stdarg.h>
#include <setjmp.h>
#include <assert.h>
#include <sys/stat.h>
#include <errno.h>
#include <ctype.h>
#undef isalnum
#undef isdigit
#undef isspace
#undef toupper
#undef tolower
static inline int solar_os_doom_isdigit(int c) { return (unsigned)c - '0' < 10U; }
static inline int solar_os_doom_isalnum(int c) { return solar_os_doom_isdigit(c) || (unsigned)(c | 32) - 'a' < 26U; }
static inline int solar_os_doom_isspace(int c) { return c == ' ' || (unsigned)c - '\t' < 5U; }
static inline int solar_os_doom_toupper(int c) { return (unsigned)c - 'a' < 26U ? c - 32 : c; }
static inline int solar_os_doom_tolower(int c) { return (unsigned)c - 'A' < 26U ? c + 32 : c; }
#define isdigit solar_os_doom_isdigit
#define isalnum solar_os_doom_isalnum
#define isspace solar_os_doom_isspace
#define toupper solar_os_doom_toupper
#define tolower solar_os_doom_tolower
#ifndef F_OK
#define F_OK 0
#define R_OK 4
#endif

extern const solar_os_native_host_api_v1_t *solar_os_doom_host;
extern const solar_os_native_foreground_api_v1_t *solar_os_doom_app;
extern const solar_os_native_memory_api_v1_t *solar_os_doom_memory;
extern const solar_os_native_libc_api_v1_t *solar_os_doom_libc;
extern jmp_buf solar_os_doom_trap;
extern int solar_os_doom_result;
extern int solar_os_doom_errno;
extern char solar_os_doom_path[256];
extern uint64_t solar_os_doom_epoch;
void *solar_os_doom_malloc(size_t size);
void *solar_os_doom_calloc(size_t count, size_t size);
void *solar_os_doom_realloc(void *ptr, size_t size);
void solar_os_doom_free(void *ptr);
char *solar_os_doom_strdup(const char *text);
void solar_os_doom_cleanup(void);
void solar_os_doom_poll(void);
void solar_os_doom_reset_keys(void);
void solar_os_doom_sound_suspend(void);
void solar_os_doom_sound_resume(void);
void solar_os_doom_sound_close(void);
_Noreturn void solar_os_doom_exit(int status);
_Noreturn void solar_os_doom_abort(void);
int solar_os_doom_snprintf(char *out, size_t size, const char *format, ...);
int solar_os_doom_sprintf(char *out, const char *format, ...);
int solar_os_doom_printf(const char *format, ...);
int solar_os_doom_fprintf(FILE *file, const char *format, ...);
int solar_os_doom_vsprintf(char *out, const char *format, va_list args);
char *solar_os_doom_getenv(const char *name);
int solar_os_doom_access(const char *path, int mode);
int solar_os_doom_stat(const char *path, struct stat *status);
FILE *solar_os_doom_fopen(const char *path, const char *mode);
int solar_os_doom_fclose(FILE *file);
size_t solar_os_doom_fread(void *data, size_t size, size_t count, FILE *file);
size_t solar_os_doom_fwrite(const void *data, size_t size, size_t count, FILE *file);
int solar_os_doom_fseek(FILE *file, long offset, int whence);
long solar_os_doom_ftell(FILE *file);
char *solar_os_doom_fgets(char *out, int size, FILE *file);
int solar_os_doom_remove(const char *path);
int solar_os_doom_scan(const char *text, const char *format, ...);
int solar_os_doom_fscan(FILE *file, const char *format, ...);
int solar_os_doom_feof(FILE *file);
int solar_os_doom_fgetc(FILE *file);
int solar_os_doom_fputc(int ch, FILE *file);
void solar_os_doom_setbuf(FILE *file, char *buffer);
char *solar_os_doom_strlwr(char *text);

/* Keep module allocations and process-global libc state out of the firmware.
 * z_zone.h replaces the function-like allocation macros; (malloc) there still
 * names this module's tracked allocator. */
#ifndef SOLAR_OS_DOOM_PORT_IMPLEMENTATION
#define malloc solar_os_doom_malloc
#define calloc solar_os_doom_calloc
#define realloc solar_os_doom_realloc
#define free solar_os_doom_free
#define strdup solar_os_doom_strdup
#define exit solar_os_doom_exit
#define abort solar_os_doom_abort
#define printf solar_os_doom_printf
#define fprintf solar_os_doom_fprintf
#define snprintf solar_os_doom_snprintf
#define sprintf solar_os_doom_sprintf
#define vsnprintf solar_os_doom_libc->format
#define vsprintf solar_os_doom_vsprintf
#define getenv solar_os_doom_getenv
#define access solar_os_doom_access
#define stat(path, status) solar_os_doom_stat(path, status)
#define fopen solar_os_doom_fopen
#define fclose solar_os_doom_fclose
#define fread solar_os_doom_fread
#define fwrite solar_os_doom_fwrite
#define fseek solar_os_doom_fseek
#define ftell solar_os_doom_ftell
#define fgets solar_os_doom_fgets
#define remove solar_os_doom_remove
#define sscanf solar_os_doom_scan
#define fscanf solar_os_doom_fscan
#define feof solar_os_doom_feof
#define fgetc solar_os_doom_fgetc
#define fputc solar_os_doom_fputc
#define setbuf solar_os_doom_setbuf
#define strlwr solar_os_doom_strlwr
#undef assert
#define assert(test) ((test) ? (void)0 : solar_os_doom_abort())
#undef errno
#define errno solar_os_doom_errno
#undef stdout
#undef stderr
#define stdout ((FILE *)0)
#define stderr ((FILE *)0)
#endif
