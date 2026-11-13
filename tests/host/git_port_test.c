#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include "git2.h"

int solar_os_git_rename(const char *, const char *);
char *p_realpath(const char *, char *);

static void write_text(const char *path, const char *text)
{
    FILE *f = fopen(path, "wb"); assert(f);
    assert(fwrite(text, 1, strlen(text), f) == strlen(text));
    assert(fclose(f) == 0);
}

static void expect_text(const char *path, const char *expected)
{
    char text[64] = {0}; FILE *f = fopen(path, "rb"); assert(f);
    size_t n = fread(text, 1, sizeof(text) - 1, f); assert(n == strlen(expected));
    assert(fclose(f) == 0); assert(!strcmp(text, expected));
}

int main(int argc, char **argv)
{
    assert(argc == 2);
    assert(chdir(argv[1]) == 0);
    assert(git_libgit2_init() > 0);
    write_text("old", "original"); write_text("new", "updated");
    assert(solar_os_git_rename("new", "old") == 0);
    expect_text("old", "updated"); assert(access("old.solaros-backup", F_OK) != 0);
    assert(solar_os_git_rename("missing", "old") != 0);
    expect_text("old", "updated"); assert(access("old.solaros-backup", F_OK) != 0);
    write_text("old.solaros-backup", "recovery"); write_text("new", "pending");
    assert(solar_os_git_rename("new", "old") != 0);
    expect_text("old", "updated"); expect_text("old.solaros-backup", "recovery");
    expect_text("new", "pending");
    assert(mkdir("directory", 0700) == 0);
    assert(solar_os_git_rename("new", "directory") != 0);
    expect_text("new", "pending");
    char path[4096], resolved[4096];
    snprintf(path, sizeof(path), "%s/directory/.././old", argv[1]);
    assert(p_realpath(path, resolved));
    snprintf(path, sizeof(path), "%s/old", argv[1]);
    assert(!strcmp(path, resolved));
    assert(!p_realpath("relative", resolved));
    assert(git_libgit2_shutdown() == 0);
    return 0;
}
