#pragma once

#include <sys/types.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <unistd.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include "solar_os_git_state.h"
struct passwd;
struct git_config;

int solar_os_git_chmod(const char *path, mode_t mode);
int solar_os_git_rename(const char *from, const char *to);
int solar_os_git_futimes(int fd, const struct timeval times[2]);
int solar_os_git_getentropy(void *data, size_t len);
int solar_os_git_parse_int(int *out, const char *value);
int solar_os_git_config_int(int *out, const struct git_config *config, const char *key);
int solar_os_git_strntoint(int *out, const char *str, size_t len, const char **end, int base);
const char *solar_os_git_gai_strerror(int code);
ssize_t solar_os_git_readlink(const char *path, char *out, size_t size);
int solar_os_git_symlink(const char *target, const char *path);
int solar_os_git_utimes(const char *path, const struct timeval times[2]);
int solar_os_git_link(const char *from, const char *to);
void *solar_os_git_workspace_alloc(size_t size);
void solar_os_git_workspace_release(uint32_t **workspace);
int solar_os_git_getpwuid_r(uid_t uid, struct passwd *pwd, char *buf,
                          size_t size, struct passwd **result);

#define chmod solar_os_git_chmod
#define rename solar_os_git_rename
#define lstat stat
#define futimes solar_os_git_futimes
#define getentropy solar_os_git_getentropy
#define getpwuid_r solar_os_git_getpwuid_r
#define gai_strerror solar_os_git_gai_strerror
#define readlink solar_os_git_readlink
#define symlink solar_os_git_symlink
#define utimes solar_os_git_utimes
#define link solar_os_git_link
#define geteuid() ((uid_t)0)
#define getuid() ((uid_t)0)
#define getgid() ((gid_t)0)
#define getppid() ((pid_t)0)
#define getpgid(pid) ((void)(pid), (pid_t)0)
#define getsid(pid) ((void)(pid), (pid_t)0)
#ifndef _SC_GETPW_R_SIZE_MAX
#define _SC_GETPW_R_SIZE_MAX _SC_PAGESIZE
#endif
