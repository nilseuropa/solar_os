#pragma once

#include "git2.h"
#include "solar_os_git.h"

typedef struct {
    char host[96];
    char username[64];
    char path[512];
    unsigned short port;
} solar_os_git_ssh_url_t;

bool solar_os_git_parse_ssh_url(const char *url, solar_os_git_ssh_url_t *out);
int solar_os_git_transport_init(const solar_os_git_options_t *options);
void solar_os_git_transport_shutdown(void);
int solar_os_git_transport_factory(git_transport **out, git_remote *owner, void *payload);
