#include <assert.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>
typedef int esp_err_t;
#define ESP_OK 0
#define ESP_ERR_INVALID_ARG 1
#define ESP_ERR_NOT_FOUND 2
#define ESP_ERR_INVALID_SIZE 3
#define SOLAR_OS_STORAGE_PATH_MAX 256
#define SOLAR_OS_NET_SSH_DIR ".ssh"
#define SOLAR_OS_NET_HOSTS "hosts"
typedef bool (*solar_os_net_host_visitor_t)(const char *, const char *, void *);
static const char *hosts_path;
static bool mounted = true;
static size_t strlcpy(char *dst, const char *src, size_t size)
{
    if (size) snprintf(dst, size, "%s", src);
    return strlen(src);
}
static bool solar_os_storage_is_mounted(void) { return mounted; }
static esp_err_t solar_os_storage_default_path(const char *dir, char *out, size_t size)
{ (void)dir; strlcpy(out, ".ssh", size); return ESP_OK; }
static esp_err_t solar_os_storage_join_path(const char *dir, const char *file,
                                           char *out, size_t size)
{ (void)dir; (void)file; strlcpy(out, hosts_path, size); return ESP_OK; }

/* HOSTS_SOURCE */

static bool collect(const char *address, const char *alias, void *user)
{
    unsigned *count = user;
    assert(address[0]);
    assert(strcmp(alias, "comment"));
    (*count)++;
    return true;
}
static bool stop(const char *address, const char *alias, void *user)
{ collect(address, alias, user); return false; }
int main(int argc, char **argv)
{
    assert(argc == 2);
    hosts_path = argv[1];
    unsigned count = 0;
    assert(solar_os_net_hosts_visit(collect, &count) == ESP_OK && count == 4);
    count = 0;
    assert(solar_os_net_hosts_visit(stop, &count) == ESP_OK && count == 1);
    char resolved[128];
    assert(net_lookup_hosts_file("REMOTE", resolved, sizeof(resolved)) == ESP_OK);
    assert(!strcmp(resolved, "192.0.2.1"));
    assert(net_lookup_hosts_file("unknown", resolved, sizeof(resolved)) == ESP_ERR_NOT_FOUND);
    assert(!strcmp(resolved, "unknown"));
    assert(solar_os_net_hosts_visit(NULL, NULL) == ESP_ERR_INVALID_ARG);
    mounted = false;
    assert(net_lookup_hosts_file("remote", resolved, sizeof(resolved)) == ESP_ERR_NOT_FOUND);
    assert(!strcmp(resolved, "remote"));
    mounted = true;
    hosts_path = "/missing-solaros-test-hosts";
    assert(solar_os_net_hosts_visit(collect, &count) == ESP_ERR_NOT_FOUND);
    return 0;
}
