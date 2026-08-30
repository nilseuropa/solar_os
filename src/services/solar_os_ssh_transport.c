#include "solar_os_ssh_transport.h"

#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>

#include "solar_os_log.h"
#include "lwip/netdb.h"
#include "solar_os_ssh.h"
#include "solar_os_ssh_keys.h"
#include "solar_os_storage.h"
#include "solar_os_wifi.h"

#define SOLAR_OS_SSH_TRANSPORT_SOCKET_WAIT_MS 100
#define SOLAR_OS_SSH_TRANSPORT_DIR ".ssh"
#define SOLAR_OS_SSH_TRANSPORT_KNOWN_HOSTS "known_hosts"
#define SOLAR_OS_SSH_TRANSPORT_HOSTS "hosts"

static bool transport_should_stop(const solar_os_ssh_transport_config_t *config)
{
    return config == NULL ||
        (config->should_stop != NULL && config->should_stop(config->user));
}

static void transport_send_status(const solar_os_ssh_transport_config_t *config,
                                  const char *message)
{
    if (config != NULL && config->status != NULL) {
        config->status(config->user, message);
    }
}

static void transport_send_error(const solar_os_ssh_transport_config_t *config,
                                 const char *message)
{
    if (config != NULL && config->error != NULL) {
        config->error(config->user, message);
    }
}

static bool transport_make_storage_path(char *path, size_t path_len, const char *name)
{
    if (path == NULL || path_len == 0 || name == NULL) {
        return false;
    }

    char dir[SOLAR_OS_STORAGE_PATH_MAX];
    return solar_os_storage_default_path(SOLAR_OS_SSH_TRANSPORT_DIR, dir, sizeof(dir)) == ESP_OK &&
        solar_os_storage_join_path(dir, name, path, path_len) == ESP_OK;
}

static esp_err_t transport_ensure_config_dir(const solar_os_ssh_transport_config_t *config,
                                             char *dir_path,
                                             size_t dir_path_len)
{
    if (!solar_os_storage_is_mounted()) {
        transport_send_error(config, "storage required for SSH config");
        return ESP_ERR_INVALID_STATE;
    }

    if (solar_os_storage_default_path(SOLAR_OS_SSH_TRANSPORT_DIR,
                                      dir_path,
                                      dir_path_len) != ESP_OK) {
        transport_send_error(config, "SSH config path too long");
        return ESP_ERR_INVALID_SIZE;
    }

    struct stat st;
    if (stat(dir_path, &st) == 0) {
        if (S_ISDIR(st.st_mode)) {
            return ESP_OK;
        }
        transport_send_error(config, ".ssh is not a directory");
        return ESP_ERR_INVALID_STATE;
    }

    if (mkdir(dir_path, 0777) == 0 || errno == EEXIST) {
        return ESP_OK;
    }

    transport_send_error(config, "SSH config dir create failed");
    return ESP_FAIL;
}

static esp_err_t transport_make_config_file_path(const solar_os_ssh_transport_config_t *config,
                                                 const char *name,
                                                 char *path,
                                                 size_t path_len)
{
    char dir_path[SOLAR_OS_STORAGE_PATH_MAX];
    esp_err_t ret = transport_ensure_config_dir(config, dir_path, sizeof(dir_path));
    if (ret != ESP_OK) {
        return ret;
    }

    if (!transport_make_storage_path(path, path_len, name)) {
        transport_send_error(config, "SSH config file path too long");
        return ESP_ERR_INVALID_SIZE;
    }
    return ESP_OK;
}

typedef struct {
    const char *host;
    char *resolved_host;
    size_t resolved_host_len;
    bool found;
} transport_host_lookup_t;

static bool transport_lookup_host_visitor(const char *address,
                                          const char *alias,
                                          void *user)
{
    transport_host_lookup_t *lookup = (transport_host_lookup_t *)user;

    if (strcasecmp(alias, lookup->host) != 0) {
        return true;
    }
    strlcpy(lookup->resolved_host, address, lookup->resolved_host_len);
    lookup->found = true;
    return false;
}

static esp_err_t transport_visit_hosts_path(const char *hosts_path,
                                            solar_os_ssh_host_visitor_t visitor,
                                            void *user)
{
    if (hosts_path == NULL || visitor == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    FILE *file = fopen(hosts_path, "r");
    if (file == NULL) {
        return ESP_ERR_NOT_FOUND;
    }

    char line[192];
    bool keep_visiting = true;
    while (keep_visiting && fgets(line, sizeof(line), file) != NULL) {
        char *comment = strchr(line, '#');
        if (comment != NULL) {
            *comment = '\0';
        }

        char *saveptr = NULL;
        const char *address = strtok_r(line, " \t\r\n", &saveptr);
        if (address == NULL) {
            continue;
        }

        const char *alias = NULL;
        while ((alias = strtok_r(NULL, " \t\r\n", &saveptr)) != NULL) {
            if (!visitor(address, alias, user)) {
                keep_visiting = false;
                break;
            }
        }
    }

    fclose(file);
    return ESP_OK;
}

static esp_err_t transport_lookup_hosts_file(const solar_os_ssh_transport_config_t *config,
                                             char *resolved_host,
                                             size_t resolved_host_len)
{
    if (config == NULL ||
        config->host == NULL ||
        resolved_host == NULL ||
        resolved_host_len == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    strlcpy(resolved_host, config->host, resolved_host_len);
    char hosts_path[SOLAR_OS_STORAGE_PATH_MAX];
    esp_err_t ret = transport_make_config_file_path(config,
                                                    SOLAR_OS_SSH_TRANSPORT_HOSTS,
                                                    hosts_path,
                                                    sizeof(hosts_path));
    if (ret != ESP_OK) {
        return ret;
    }
    transport_host_lookup_t lookup = {
        .host = config->host,
        .resolved_host = resolved_host,
        .resolved_host_len = resolved_host_len,
    };
    ret = transport_visit_hosts_path(hosts_path, transport_lookup_host_visitor, &lookup);
    if (ret != ESP_OK) {
        return ret;
    }
    return lookup.found ? ESP_OK : ESP_ERR_NOT_FOUND;
}

esp_err_t solar_os_ssh_hosts_visit(solar_os_ssh_host_visitor_t visitor, void *user)
{
    if (visitor == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!solar_os_storage_is_mounted()) {
        return ESP_ERR_NOT_FOUND;
    }

    char hosts_path[SOLAR_OS_STORAGE_PATH_MAX];
    if (!transport_make_storage_path(hosts_path,
                                     sizeof(hosts_path),
                                     SOLAR_OS_SSH_TRANSPORT_HOSTS)) {
        return ESP_ERR_INVALID_SIZE;
    }

    return transport_visit_hosts_path(hosts_path, visitor, user);
}

int solar_os_ssh_transport_wait_socket(const solar_os_ssh_transport_config_t *config,
                                       int socket_fd,
                                       LIBSSH2_SESSION *session)
{
    while (!transport_should_stop(config)) {
        struct timeval timeout = {
            .tv_sec = 0,
            .tv_usec = SOLAR_OS_SSH_TRANSPORT_SOCKET_WAIT_MS * 1000,
        };
        fd_set readfds;
        fd_set writefds;
        fd_set *readfd = NULL;
        fd_set *writefd = NULL;
        int direction = LIBSSH2_SESSION_BLOCK_INBOUND | LIBSSH2_SESSION_BLOCK_OUTBOUND;

        FD_ZERO(&readfds);
        FD_ZERO(&writefds);
        FD_SET(socket_fd, &readfds);
        FD_SET(socket_fd, &writefds);

        if (session != NULL) {
            direction = libssh2_session_block_directions(session);
        }
        if ((direction & LIBSSH2_SESSION_BLOCK_INBOUND) != 0) {
            readfd = &readfds;
        }
        if ((direction & LIBSSH2_SESSION_BLOCK_OUTBOUND) != 0) {
            writefd = &writefds;
        }
        if (readfd == NULL && writefd == NULL) {
            readfd = &readfds;
            writefd = &writefds;
        }

        const int rc = select(socket_fd + 1, readfd, writefd, NULL, &timeout);
        if (rc >= 0) {
            return rc;
        }
        if (errno != EINTR) {
            return rc;
        }
    }

    return -1;
}

static esp_err_t transport_connect_socket(const solar_os_ssh_transport_config_t *config,
                                          int *socket_fd)
{
    if (config == NULL ||
        socket_fd == NULL ||
        config->host == NULL ||
        config->port == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    char port_str[8];
    struct addrinfo hints = {
        .ai_family = AF_INET,
        .ai_socktype = SOCK_STREAM,
    };
    struct addrinfo *result = NULL;
    snprintf(port_str, sizeof(port_str), "%" PRIu16, config->port);

    char resolved_host[SOLAR_OS_SSH_HOST_MAX];
    esp_err_t lookup_ret = transport_lookup_hosts_file(config,
                                                       resolved_host,
                                                       sizeof(resolved_host));
    if (lookup_ret == ESP_OK && strcmp(resolved_host, config->host) != 0) {
        char message[128];
        snprintf(message, sizeof(message), "hosts: %s -> %s", config->host, resolved_host);
        transport_send_status(config, message);
    } else if (lookup_ret != ESP_OK && lookup_ret != ESP_ERR_NOT_FOUND) {
        return lookup_ret;
    }

    transport_send_status(config, "resolving host");
    if (config->log_resolve && config->log_tag != NULL) {
        SOLAR_OS_LOGI(config->log_tag, "resolve %s:%" PRIu16, resolved_host, config->port);
    }
    const int gai = getaddrinfo(resolved_host, port_str, &hints, &result);
    if (gai != 0 || result == NULL) {
        if (config->log_resolve && config->log_tag != NULL) {
            SOLAR_OS_LOGE(config->log_tag,
                     "getaddrinfo failed for %s:%" PRIu16 " gai=%d",
                     resolved_host,
                     config->port,
                     gai);
        }
        transport_send_error(config, "host resolution failed");
        return ESP_ERR_NOT_FOUND;
    }

    esp_err_t ret = ESP_ERR_NOT_FOUND;
    for (struct addrinfo *rp = result; rp != NULL && !transport_should_stop(config); rp = rp->ai_next) {
        const int sock = socket(rp->ai_family, rp->ai_socktype, rp->ai_protocol);
        if (sock < 0) {
            continue;
        }

        const int flags = fcntl(sock, F_GETFL, 0);
        if (flags >= 0) {
            (void)fcntl(sock, F_SETFL, flags | O_NONBLOCK);
        }

        transport_send_status(config, "connecting");
        int rc = connect(sock, rp->ai_addr, rp->ai_addrlen);
        if (rc < 0 && errno == EINPROGRESS) {
            while (!transport_should_stop(config)) {
                struct timeval timeout = {
                    .tv_sec = 0,
                    .tv_usec = SOLAR_OS_SSH_TRANSPORT_SOCKET_WAIT_MS * 1000,
                };
                fd_set writefds;
                FD_ZERO(&writefds);
                FD_SET(sock, &writefds);

                rc = select(sock + 1, NULL, &writefds, NULL, &timeout);
                if (rc > 0) {
                    int sock_error = 0;
                    socklen_t sock_error_len = sizeof(sock_error);
                    if (getsockopt(sock, SOL_SOCKET, SO_ERROR, &sock_error, &sock_error_len) == 0 &&
                        sock_error == 0) {
                        rc = 0;
                    } else {
                        errno = sock_error != 0 ? sock_error : errno;
                        rc = -1;
                    }
                    break;
                }
                if (rc < 0 && errno != EINTR) {
                    break;
                }
            }
        }

        if (rc == 0) {
            *socket_fd = sock;
            ret = ESP_OK;
            break;
        }

        close(sock);
    }

    freeaddrinfo(result);
    if (ret != ESP_OK && !transport_should_stop(config)) {
        if (config->log_connect_fail && config->log_tag != NULL) {
            SOLAR_OS_LOGE(config->log_tag,
                     "TCP connect failed for %s:%" PRIu16,
                     resolved_host,
                     config->port);
        }
        transport_send_error(config, "TCP connect failed");
    }
    return ret;
}

static int transport_knownhost_key_type(int hostkey_type)
{
    switch (hostkey_type) {
    case LIBSSH2_HOSTKEY_TYPE_RSA:
        return LIBSSH2_KNOWNHOST_KEY_SSHRSA;
    case LIBSSH2_HOSTKEY_TYPE_DSS:
        return LIBSSH2_KNOWNHOST_KEY_SSHDSS;
    case LIBSSH2_HOSTKEY_TYPE_ECDSA_256:
        return LIBSSH2_KNOWNHOST_KEY_ECDSA_256;
    case LIBSSH2_HOSTKEY_TYPE_ECDSA_384:
        return LIBSSH2_KNOWNHOST_KEY_ECDSA_384;
    case LIBSSH2_HOSTKEY_TYPE_ECDSA_521:
        return LIBSSH2_KNOWNHOST_KEY_ECDSA_521;
    case LIBSSH2_HOSTKEY_TYPE_ED25519:
        return LIBSSH2_KNOWNHOST_KEY_ED25519;
    default:
        return LIBSSH2_KNOWNHOST_KEY_UNKNOWN;
    }
}

static esp_err_t transport_verify_host_key(const solar_os_ssh_transport_config_t *config,
                                           LIBSSH2_SESSION *session)
{
    if (config == NULL || session == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    if (!solar_os_storage_is_mounted() &&
        config->allow_unverified_host_key_without_storage) {
        transport_send_status(config, "host key not verified: no storage");
        if (config->log_tag != NULL) {
            SOLAR_OS_LOGW(config->log_tag,
                          "host key not verified for %s:%" PRIu16 ": no storage",
                          config->host,
                          config->port);
        }
        return ESP_OK;
    }

    char known_hosts_path[SOLAR_OS_STORAGE_PATH_MAX];
    esp_err_t ret = transport_make_config_file_path(config,
                                                    SOLAR_OS_SSH_TRANSPORT_KNOWN_HOSTS,
                                                    known_hosts_path,
                                                    sizeof(known_hosts_path));
    if (ret != ESP_OK) {
        return ret;
    }

    LIBSSH2_KNOWNHOSTS *known_hosts = libssh2_knownhost_init(session);
    if (known_hosts == NULL) {
        transport_send_error(config, "known_hosts init failed");
        return ESP_ERR_NO_MEM;
    }

    (void)libssh2_knownhost_readfile(known_hosts,
                                     known_hosts_path,
                                     LIBSSH2_KNOWNHOST_FILE_OPENSSH);

    size_t key_len = 0;
    int hostkey_type = LIBSSH2_HOSTKEY_TYPE_UNKNOWN;
    const char *hostkey = libssh2_session_hostkey(session, &key_len, &hostkey_type);
    if (hostkey == NULL || key_len == 0) {
        libssh2_knownhost_free(known_hosts);
        transport_send_error(config, "host key unavailable");
        return ESP_FAIL;
    }

    const int key_type = transport_knownhost_key_type(hostkey_type);
    if (key_type == LIBSSH2_KNOWNHOST_KEY_UNKNOWN) {
        libssh2_knownhost_free(known_hosts);
        transport_send_error(config, "unsupported host key type");
        return ESP_ERR_NOT_SUPPORTED;
    }

    const int typemask = LIBSSH2_KNOWNHOST_TYPE_PLAIN |
        LIBSSH2_KNOWNHOST_KEYENC_RAW |
        key_type;
    struct libssh2_knownhost *known_host = NULL;
    const int check = libssh2_knownhost_checkp(known_hosts,
                                               config->host,
                                               config->port,
                                               hostkey,
                                               key_len,
                                               typemask,
                                               &known_host);

    if (check == LIBSSH2_KNOWNHOST_CHECK_MATCH) {
        if (config->log_host_key_match && config->log_tag != NULL) {
            SOLAR_OS_LOGI(config->log_tag,
                     "host key matched for %s:%" PRIu16,
                     config->host,
                     config->port);
        }
        libssh2_knownhost_free(known_hosts);
        return ESP_OK;
    }
    if (check == LIBSSH2_KNOWNHOST_CHECK_MISMATCH) {
        libssh2_knownhost_free(known_hosts);
        transport_send_error(config, "host key mismatch");
        return ESP_ERR_INVALID_STATE;
    }
    if (check != LIBSSH2_KNOWNHOST_CHECK_NOTFOUND) {
        libssh2_knownhost_free(known_hosts);
        transport_send_error(config, "host key check failed");
        return ESP_FAIL;
    }

    const char comment[] = "SolarOS";
    int rc = libssh2_knownhost_addc(known_hosts,
                                    config->host,
                                    NULL,
                                    hostkey,
                                    key_len,
                                    comment,
                                    strlen(comment),
                                    typemask,
                                    NULL);
    if (rc != 0) {
        libssh2_knownhost_free(known_hosts);
        transport_send_error(config, "host key save failed");
        return ESP_FAIL;
    }

    rc = libssh2_knownhost_writefile(known_hosts,
                                     known_hosts_path,
                                     LIBSSH2_KNOWNHOST_FILE_OPENSSH);
    libssh2_knownhost_free(known_hosts);
    if (rc < 0) {
        transport_send_error(config, "known_hosts write failed");
        return ESP_FAIL;
    }

    transport_send_status(config, "new host key saved");
    return ESP_OK;
}

void solar_os_ssh_transport_send_libssh2_error(const solar_os_ssh_transport_config_t *config,
                                               LIBSSH2_SESSION *session,
                                               const char *prefix,
                                               int code)
{
    char message[160];
    char *last_error = NULL;
    int last_error_len = 0;

    if (session != NULL) {
        (void)libssh2_session_last_error(session, &last_error, &last_error_len, 0);
    }

    if (config != NULL && config->include_error_code) {
        if (last_error != NULL && last_error_len > 0) {
            snprintf(message,
                     sizeof(message),
                     "%s: %.*s (rc=%d)",
                     prefix,
                     last_error_len,
                     last_error,
                     code);
        } else {
            snprintf(message, sizeof(message), "%s (rc=%d)", prefix, code);
        }
    } else if (last_error != NULL && last_error_len > 0) {
        snprintf(message,
                 sizeof(message),
                 "%s: %.*s",
                 prefix,
                 last_error_len,
                 last_error);
    } else {
        snprintf(message, sizeof(message), "%s: %d", prefix, code);
    }

    transport_send_error(config, message);
}

static esp_err_t transport_handshake(const solar_os_ssh_transport_config_t *config,
                                     LIBSSH2_SESSION *session,
                                     int socket_fd)
{
    transport_send_status(config, "starting SSH handshake");
    int rc;
    while (!transport_should_stop(config) &&
           (rc = libssh2_session_handshake(session, socket_fd)) == LIBSSH2_ERROR_EAGAIN) {
        (void)solar_os_ssh_transport_wait_socket(config, socket_fd, session);
    }

    if (transport_should_stop(config)) {
        return ESP_ERR_INVALID_STATE;
    }
    if (rc != 0) {
        solar_os_ssh_transport_send_libssh2_error(config, session, "SSH handshake failed", rc);
        return ESP_FAIL;
    }

    if (config->log_handshake_complete && config->log_tag != NULL) {
        SOLAR_OS_LOGI(config->log_tag, "SSH handshake complete");
    }
    return ESP_OK;
}

static esp_err_t transport_authenticate(const solar_os_ssh_transport_config_t *config,
                                        LIBSSH2_SESSION *session,
                                        int socket_fd)
{
    if (config == NULL || session == NULL || config->username == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    const char *password = config->password != NULL ? config->password : "";
    if (password[0] != '\0') {
        if (config->include_username_in_auth_status) {
            char message[128];
            snprintf(message,
                     sizeof(message),
                     "authenticating %s with password",
                     config->username);
            transport_send_status(config, message);
        } else {
            transport_send_status(config, "authenticating with password");
        }

        int rc;
        while (!transport_should_stop(config) &&
               (rc = libssh2_userauth_password(session, config->username, password)) ==
                   LIBSSH2_ERROR_EAGAIN) {
            (void)solar_os_ssh_transport_wait_socket(config, socket_fd, session);
        }

        if (transport_should_stop(config)) {
            return ESP_ERR_INVALID_STATE;
        }
        if (rc != 0) {
            solar_os_ssh_transport_send_libssh2_error(config,
                                                      session,
                                                      "password authentication failed",
                                                      rc);
            return ESP_ERR_INVALID_CRC;
        }
        if (config->report_password_success) {
            transport_send_status(config, "password accepted");
        }
        return ESP_OK;
    }

    solar_os_ssh_key_status_t key_status;
    if (solar_os_ssh_keys_get_status(&key_status) != ESP_OK ||
        !key_status.private_key_exists ||
        !key_status.public_key_exists) {
        transport_send_error(config, "no SSH key and no password");
        return ESP_ERR_INVALID_CRC;
    }

    if (config->include_username_in_auth_status) {
        char message[128];
        snprintf(message,
                 sizeof(message),
                 "authenticating %s with public key",
                 config->username);
        transport_send_status(config, message);
    } else {
        transport_send_status(config, "authenticating with public key");
    }
    if (config->log_key_paths && config->log_tag != NULL) {
        SOLAR_OS_LOGI(config->log_tag,
                 "using public key private=%s public=%s",
                 key_status.private_key_path,
                 key_status.public_key_path);
    }

    int key_rc;
    while (!transport_should_stop(config) &&
           (key_rc = libssh2_userauth_publickey_fromfile_ex(session,
                                                            config->username,
                                                            (unsigned int)strlen(config->username),
                                                            key_status.public_key_path,
                                                            key_status.private_key_path,
                                                            "")) ==
               LIBSSH2_ERROR_EAGAIN) {
        (void)solar_os_ssh_transport_wait_socket(config, socket_fd, session);
    }

    if (transport_should_stop(config)) {
        return ESP_ERR_INVALID_STATE;
    }
    if (key_rc != 0) {
        solar_os_ssh_transport_send_libssh2_error(config,
                                                  session,
                                                  "public key authentication failed",
                                                  key_rc);
        return ESP_ERR_INVALID_CRC;
    }

    if (config->report_publickey_success) {
        transport_send_status(config, "public key accepted");
    }
    return ESP_OK;
}

esp_err_t solar_os_ssh_transport_open(const solar_os_ssh_transport_config_t *config,
                                      solar_os_ssh_transport_t *transport)
{
    if (config == NULL ||
        transport == NULL ||
        config->host == NULL ||
        config->host[0] == '\0' ||
        config->username == NULL ||
        config->username[0] == '\0' ||
        config->port == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    *transport = (solar_os_ssh_transport_t){
        .socket_fd = -1,
    };

    solar_os_wifi_status_t wifi_status;
    solar_os_wifi_get_status(&wifi_status);
    if (!wifi_status.has_ip) {
        transport_send_error(config, "WiFi is not connected");
        return ESP_ERR_INVALID_STATE;
    }

    if (libssh2_init(0) != 0) {
        transport_send_error(config, "libssh2 init failed");
        return ESP_FAIL;
    }
    transport->libssh2_ready = true;

    esp_err_t ret = transport_connect_socket(config, &transport->socket_fd);
    if (ret != ESP_OK || transport_should_stop(config)) {
        solar_os_ssh_transport_close(transport, NULL);
        return ret != ESP_OK ? ret : ESP_ERR_INVALID_STATE;
    }

    transport->session = libssh2_session_init();
    if (transport->session == NULL) {
        transport_send_error(config, "SSH session allocation failed");
        solar_os_ssh_transport_close(transport, NULL);
        return ESP_ERR_NO_MEM;
    }
    libssh2_session_set_blocking(transport->session, 0);

    ret = transport_handshake(config, transport->session, transport->socket_fd);
    if (ret == ESP_OK) {
        ret = transport_verify_host_key(config, transport->session);
    }
    if (ret == ESP_OK) {
        ret = transport_authenticate(config, transport->session, transport->socket_fd);
    }
    if (ret != ESP_OK) {
        solar_os_ssh_transport_close(transport, NULL);
        return ret;
    }

    return ESP_OK;
}

void solar_os_ssh_transport_close(solar_os_ssh_transport_t *transport,
                                  const char *disconnect_message)
{
    if (transport == NULL) {
        return;
    }

    if (transport->session != NULL) {
        (void)libssh2_session_disconnect(transport->session,
                                         disconnect_message != NULL ? disconnect_message : "SolarOS shutdown");
        (void)libssh2_session_free(transport->session);
        transport->session = NULL;
    }
    if (transport->socket_fd >= 0) {
        (void)shutdown(transport->socket_fd, SHUT_RDWR);
        close(transport->socket_fd);
        transport->socket_fd = -1;
    }
    if (transport->libssh2_ready) {
        libssh2_exit();
        transport->libssh2_ready = false;
    }
}
