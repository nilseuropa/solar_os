#include "solar_os_httpd_job.h"

#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

#include "esp_http_server.h"
#include "solar_os_jobs.h"
#include "solar_os_log.h"
#include "solar_os_memory.h"
#include "solar_os_storage.h"
#include "solar_os_task.h"

#define HTTPD_JOB_ROOT_MAX SOLAR_OS_STORAGE_PATH_MAX
#define HTTPD_JOB_PATH_MAX 256
#define HTTPD_JOB_CHUNK_SIZE 1024
#define HTTPD_JOB_STACK_SIZE 6144

static const char *TAG = "solar_os_httpd";

typedef struct {
    bool running;
    httpd_handle_t server;
    char root[HTTPD_JOB_ROOT_MAX];
    uint32_t request_count;
    uint32_t file_count;
    uint32_t listing_count;
    esp_err_t last_error;
} httpd_job_state_t;

static httpd_job_state_t httpd_job = {
    .last_error = ESP_OK,
};

static int hex_value(char ch)
{
    if (ch >= '0' && ch <= '9') {
        return ch - '0';
    }
    ch = (char)tolower((unsigned char)ch);
    if (ch >= 'a' && ch <= 'f') {
        return ch - 'a' + 10;
    }
    return -1;
}

static bool relative_path_has_parent_ref(const char *path)
{
    if (path == NULL) {
        return true;
    }

    const char *segment = path;
    while (*segment != '\0') {
        while (*segment == '/') {
            segment++;
        }

        const char *end = segment;
        while (*end != '\0' && *end != '/') {
            end++;
        }

        if ((size_t)(end - segment) == 2 && segment[0] == '.' && segment[1] == '.') {
            return true;
        }

        segment = end;
    }

    return false;
}

static bool uri_to_relative_path(const char *uri, char *out, size_t out_len)
{
    if (uri == NULL || out == NULL || out_len == 0 || uri[0] != '/') {
        return false;
    }

    const char *src = uri + 1;
    size_t out_pos = 0;
    while (*src != '\0' && *src != '?' && *src != '#') {
        char ch = *src++;
        if (ch == '%') {
            const int hi = hex_value(src[0]);
            const int lo = hex_value(src[1]);
            if (hi < 0 || lo < 0) {
                return false;
            }
            ch = (char)((hi << 4) | lo);
            src += 2;
        }

        if (ch == '\0' || ch == '\\') {
            return false;
        }
        if (out_pos + 1 >= out_len) {
            return false;
        }
        out[out_pos++] = ch;
    }
    out[out_pos] = '\0';

    return !relative_path_has_parent_ref(out);
}

static bool join_path(const char *root, const char *relative, char *out, size_t out_len)
{
    if (root == NULL || root[0] == '\0' || relative == NULL || out == NULL || out_len == 0) {
        return false;
    }

    if (relative[0] == '\0') {
        return strlcpy(out, root, out_len) < out_len;
    }

    const size_t root_len = strlen(root);
    if (root_len > 0 && root[root_len - 1] == '/') {
        return snprintf(out, out_len, "%s%s", root, relative) < (int)out_len;
    }

    return snprintf(out, out_len, "%s/%s", root, relative) < (int)out_len;
}

static const char *content_type_for_path(const char *path)
{
    const char *ext = path != NULL ? strrchr(path, '.') : NULL;
    if (ext == NULL) {
        return HTTPD_TYPE_OCTET;
    }

    if (strcmp(ext, ".html") == 0 || strcmp(ext, ".htm") == 0) {
        return "text/html";
    }
    if (strcmp(ext, ".txt") == 0 || strcmp(ext, ".log") == 0 || strcmp(ext, ".md") == 0) {
        return "text/plain";
    }
    if (strcmp(ext, ".css") == 0) {
        return "text/css";
    }
    if (strcmp(ext, ".js") == 0) {
        return "application/javascript";
    }
    if (strcmp(ext, ".json") == 0) {
        return "application/json";
    }
    if (strcmp(ext, ".png") == 0) {
        return "image/png";
    }
    if (strcmp(ext, ".jpg") == 0 || strcmp(ext, ".jpeg") == 0) {
        return "image/jpeg";
    }
    if (strcmp(ext, ".gif") == 0) {
        return "image/gif";
    }
    if (strcmp(ext, ".webp") == 0) {
        return "image/webp";
    }
    if (strcmp(ext, ".svg") == 0) {
        return "image/svg+xml";
    }
    if (strcmp(ext, ".ico") == 0) {
        return "image/x-icon";
    }
    if (strcmp(ext, ".wav") == 0) {
        return "audio/wav";
    }
    if (strcmp(ext, ".mp3") == 0) {
        return "audio/mpeg";
    }
    if (strcmp(ext, ".pdf") == 0) {
        return "application/pdf";
    }

    return HTTPD_TYPE_OCTET;
}

static esp_err_t send_html_escaped(httpd_req_t *req, const char *text)
{
    if (text == NULL) {
        return ESP_OK;
    }

    for (const char *p = text; *p != '\0'; p++) {
        const char *escaped = NULL;
        switch (*p) {
        case '&':
            escaped = "&amp;";
            break;
        case '<':
            escaped = "&lt;";
            break;
        case '>':
            escaped = "&gt;";
            break;
        case '"':
            escaped = "&quot;";
            break;
        default:
            break;
        }

        esp_err_t ret = ESP_OK;
        if (escaped != NULL) {
            ret = httpd_resp_sendstr_chunk(req, escaped);
        } else {
            char ch[2] = {*p, '\0'};
            ret = httpd_resp_send_chunk(req, ch, 1);
        }
        if (ret != ESP_OK) {
            return ret;
        }
    }

    return ESP_OK;
}

static esp_err_t send_url_escaped(httpd_req_t *req, const char *text)
{
    static const char hex[] = "0123456789ABCDEF";

    if (text == NULL) {
        return ESP_OK;
    }

    for (const unsigned char *p = (const unsigned char *)text; *p != '\0'; p++) {
        const bool safe = isalnum(*p) ||
            *p == '-' ||
            *p == '_' ||
            *p == '.' ||
            *p == '~';
        if (safe) {
            const char ch[2] = {(char)*p, '\0'};
            esp_err_t ret = httpd_resp_send_chunk(req, ch, 1);
            if (ret != ESP_OK) {
                return ret;
            }
            continue;
        }

        const char encoded[4] = {'%', hex[*p >> 4], hex[*p & 0x0f], '\0'};
        esp_err_t ret = httpd_resp_send_chunk(req, encoded, 3);
        if (ret != ESP_OK) {
            return ret;
        }
    }

    return ESP_OK;
}

static esp_err_t send_file(httpd_req_t *req, const char *path)
{
    FILE *file = fopen(path, "rb");
    if (file == NULL) {
        return httpd_resp_send_404(req);
    }

    char *chunk = solar_os_memory_alloc(HTTPD_JOB_CHUNK_SIZE,
                                        SOLAR_OS_MEMORY_TRANSIENT,
                                        "httpd.chunk");
    if (chunk == NULL) {
        fclose(file);
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "out of memory");
    }

    (void)httpd_resp_set_type(req, content_type_for_path(path));

    esp_err_t ret = ESP_OK;
    while (true) {
        const size_t read_len = fread(chunk, 1, HTTPD_JOB_CHUNK_SIZE, file);
        if (read_len > 0) {
            ret = httpd_resp_send_chunk(req, chunk, read_len);
            if (ret != ESP_OK) {
                break;
            }
        }
        if (read_len < HTTPD_JOB_CHUNK_SIZE) {
            if (ferror(file)) {
                ret = ESP_FAIL;
            }
            break;
        }
    }

    solar_os_memory_free(chunk);
    fclose(file);

    if (ret == ESP_OK) {
        ret = httpd_resp_send_chunk(req, NULL, 0);
    }
    return ret;
}

static esp_err_t redirect_to_slash(httpd_req_t *req)
{
    char location[CONFIG_HTTPD_MAX_URI_LEN + 2];
    const char *query = strchr(req->uri, '?');
    const size_t path_len = query != NULL ? (size_t)(query - req->uri) : strlen(req->uri);
    if (path_len + 2 > sizeof(location)) {
        return httpd_resp_send_err(req, HTTPD_414_URI_TOO_LONG, "URI too long");
    }

    memcpy(location, req->uri, path_len);
    location[path_len] = '/';
    location[path_len + 1] = '\0';

    (void)httpd_resp_set_status(req, "301 Moved Permanently");
    (void)httpd_resp_set_hdr(req, "Location", location);
    return httpd_resp_sendstr(req, "redirect");
}

static esp_err_t send_directory_listing(httpd_req_t *req, const char *relative, const char *path)
{
    DIR *dir = opendir(path);
    if (dir == NULL) {
        return httpd_resp_send_err(req, HTTPD_403_FORBIDDEN, "cannot open directory");
    }

    (void)httpd_resp_set_type(req, "text/html");
    esp_err_t ret = httpd_resp_sendstr_chunk(req,
                                             "<!doctype html><html><head>"
                                             "<meta charset=\"utf-8\">"
                                             "<meta name=\"viewport\" content=\"width=device-width,initial-scale=1\">"
                                             "<title>SolarOS httpd</title>"
                                             "<style>body{font-family:monospace;margin:1rem;}"
                                             "a{display:block;padding:.2rem 0;}</style>"
                                             "</head><body><h1>/");
    if (ret == ESP_OK) {
        ret = send_html_escaped(req, relative);
    }
    if (ret == ESP_OK) {
        ret = httpd_resp_sendstr_chunk(req, "</h1>");
    }
    if (ret == ESP_OK && relative != NULL && relative[0] != '\0') {
        ret = httpd_resp_sendstr_chunk(req, "<a href=\"../\">../</a>");
    }

    struct dirent *entry = NULL;
    while (ret == ESP_OK && (entry = readdir(dir)) != NULL) {
        if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) {
            continue;
        }

        char child_path[HTTPD_JOB_PATH_MAX];
        const bool path_ok = join_path(path, entry->d_name, child_path, sizeof(child_path));
        struct stat st;
        const bool is_dir = path_ok && stat(child_path, &st) == 0 && S_ISDIR(st.st_mode);

        ret = httpd_resp_sendstr_chunk(req, "<a href=\"");
        if (ret == ESP_OK) {
            ret = send_url_escaped(req, entry->d_name);
        }
        if (ret == ESP_OK && is_dir) {
            ret = httpd_resp_sendstr_chunk(req, "/");
        }
        if (ret == ESP_OK) {
            ret = httpd_resp_sendstr_chunk(req, "\">");
        }
        if (ret == ESP_OK) {
            ret = send_html_escaped(req, entry->d_name);
        }
        if (ret == ESP_OK && is_dir) {
            ret = httpd_resp_sendstr_chunk(req, "/");
        }
        if (ret == ESP_OK) {
            ret = httpd_resp_sendstr_chunk(req, "</a>");
        }
    }

    closedir(dir);

    if (ret == ESP_OK) {
        ret = httpd_resp_sendstr_chunk(req, "</body></html>");
    }
    if (ret == ESP_OK) {
        ret = httpd_resp_send_chunk(req, NULL, 0);
    }
    return ret;
}

static esp_err_t httpd_get_handler(httpd_req_t *req)
{
    httpd_job.request_count++;

    char relative[CONFIG_HTTPD_MAX_URI_LEN + 1];
    if (!uri_to_relative_path(req->uri, relative, sizeof(relative))) {
        return httpd_resp_send_err(req, HTTPD_403_FORBIDDEN, "invalid path");
    }

    char path[HTTPD_JOB_PATH_MAX];
    if (!join_path(httpd_job.root, relative, path, sizeof(path))) {
        return httpd_resp_send_err(req, HTTPD_414_URI_TOO_LONG, "path too long");
    }

    struct stat st;
    if (stat(path, &st) != 0) {
        return httpd_resp_send_404(req);
    }

    if (S_ISDIR(st.st_mode)) {
        const char *query = strchr(req->uri, '?');
        const size_t uri_path_len = query != NULL ? (size_t)(query - req->uri) : strlen(req->uri);
        if (uri_path_len == 0 || req->uri[uri_path_len - 1] != '/') {
            return redirect_to_slash(req);
        }

        char index_path[HTTPD_JOB_PATH_MAX];
        if (join_path(path, "index.html", index_path, sizeof(index_path)) &&
            stat(index_path, &st) == 0 &&
            S_ISREG(st.st_mode)) {
            httpd_job.file_count++;
            return send_file(req, index_path);
        }

        httpd_job.listing_count++;
        return send_directory_listing(req, relative, path);
    }

    if (!S_ISREG(st.st_mode)) {
        return httpd_resp_send_err(req, HTTPD_403_FORBIDDEN, "not a regular file");
    }

    httpd_job.file_count++;
    return send_file(req, path);
}

static esp_err_t httpd_job_start(solar_os_context_t *ctx, int argc, char **argv)
{
    (void)ctx;

    if (argc != 2 || argv == NULL || argv[1] == NULL || argv[1][0] == '\0') {
        return ESP_ERR_INVALID_ARG;
    }

    const char *root_arg = argv[1];
    char root[HTTPD_JOB_ROOT_MAX];
    esp_err_t err = solar_os_storage_resolve_path(root_arg, root, sizeof(root));
    if (err != ESP_OK) {
        httpd_job.last_error = err;
        return err;
    }
    size_t root_len = strlen(root);
    while (root_len > 1 && root[root_len - 1] == '/') {
        root[--root_len] = '\0';
    }

    struct stat st;
    if (stat(root, &st) != 0 || !S_ISDIR(st.st_mode)) {
        httpd_job.last_error = ESP_ERR_NOT_FOUND;
        return ESP_ERR_NOT_FOUND;
    }

    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.stack_size = HTTPD_JOB_STACK_SIZE;
    config.max_open_sockets = 4;
    config.lru_purge_enable = true;
    config.uri_match_fn = httpd_uri_match_wildcard;

    httpd_handle_t server = NULL;
    solar_os_task_managed_admission_t admission;
    if (!solar_os_task_admit_managed("httpd",
                                     HTTPD_JOB_STACK_SIZE,
                                     SOLAR_OS_TASK_ROLE_BACKGROUND,
                                     false,
                                     &admission)) {
        httpd_job.last_error = ESP_ERR_NO_MEM;
        return ESP_ERR_NO_MEM;
    }
    esp_err_t ret = httpd_start(&server, &config);
    solar_os_task_note_managed_result("httpd",
                                      HTTPD_JOB_STACK_SIZE,
                                      SOLAR_OS_TASK_ROLE_BACKGROUND,
                                      &admission,
                                      ret == ESP_OK);
    if (ret != ESP_OK) {
        httpd_job.last_error = ret;
        return ret;
    }

    httpd_uri_t get_uri = {
        .uri = "/*",
        .method = HTTP_GET,
        .handler = httpd_get_handler,
        .user_ctx = NULL,
    };
    ret = httpd_register_uri_handler(server, &get_uri);
    if (ret != ESP_OK) {
        (void)httpd_stop(server);
        httpd_job.last_error = ret;
        return ret;
    }

    strlcpy(httpd_job.root, root, sizeof(httpd_job.root));
    httpd_job.server = server;
    httpd_job.running = true;
    httpd_job.request_count = 0;
    httpd_job.file_count = 0;
    httpd_job.listing_count = 0;
    httpd_job.last_error = ESP_OK;
    (void)solar_os_jobs_note_resource(solar_os_httpd_job.name,
                                      SOLAR_OS_JOB_RESOURCE_FILE,
                                      httpd_job.root,
                                      "serve");
    char port[16];
    snprintf(port, sizeof(port), "tcp:%u", (unsigned)config.server_port);
    (void)solar_os_jobs_note_resource(solar_os_httpd_job.name,
                                      SOLAR_OS_JOB_RESOURCE_NET,
                                      port,
                                      "listen");

    SOLAR_OS_LOGI(TAG, "started on port %u root=%s", (unsigned)config.server_port, httpd_job.root);
    return ESP_OK;
}

static void httpd_job_stop(solar_os_context_t *ctx)
{
    (void)ctx;

    if (httpd_job.server != NULL) {
        (void)httpd_stop(httpd_job.server);
    }

    SOLAR_OS_LOGI(TAG,
             "stopped: requests=%u files=%u listings=%u root=%s",
             (unsigned)httpd_job.request_count,
             (unsigned)httpd_job.file_count,
             (unsigned)httpd_job.listing_count,
             httpd_job.root[0] != '\0' ? httpd_job.root : "?");

    httpd_job.server = NULL;
    httpd_job.running = false;
    httpd_job.last_error = ESP_OK;
}

const solar_os_job_t solar_os_httpd_job = {
    .name = "httpd",
    .summary = "static HTTP file server",
    .start = httpd_job_start,
    .stop = httpd_job_stop,
    .event = NULL,
    .worker_stack_bytes = HTTPD_JOB_STACK_SIZE,
};
