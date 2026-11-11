#include "solar_os_flash.h"

#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <unistd.h>

#include "esp_loader.h"
#include "esp_loader_io.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "solar_os_buses.h"
#include "solar_os_crypto.h"
#include "solar_os_gpio.h"
#include "solar_os_http_client.h"
#include "solar_os_json.h"
#include "solar_os_memory.h"
#include "solar_os_ota.h"
#include "solar_os_ota_key.h"
#include "solar_os_port.h"
#include "solar_os_storage.h"
#include "solar_os_uart.h"
#include "solar_os_zip.h"

#ifndef SOLAR_OS_VERSION
#define SOLAR_OS_VERSION "0.0.0"
#endif

#define FLASH_ROOT_RELATIVE ".flash"
#define FLASH_CATALOG_FILE "catalog.json"
#define FLASH_SIGNATURE_FILE "catalog.sig"
#define FLASH_CATALOG_MAX (192U * 1024U)
#define FLASH_SIGNATURE_MAX 256U
#define FLASH_ARTIFACT_MAX 128U
#define FLASH_HTTP_TIMEOUT_MS 15000U
#define FLASH_HTTP_DEADLINE_MS 300000U
#define FLASH_IO_BLOCK 4096U
#define FLASH_LOADER_BLOCK 1024U
#define FLASH_OWNER "flash"

static esp_err_t flash_url_parent(const char *url, char *parent,
                                  size_t parent_len) {
  if (url == NULL || parent == NULL || parent_len == 0U ||
      (strncmp(url, "https://", 8U) != 0 && strncmp(url, "http://", 7U) != 0)) {
    return ESP_ERR_INVALID_ARG;
  }
  size_t url_len = strlen(url);
  while (url_len > 0U && url[url_len - 1U] == '/')
    url_len--;
  const char *slash = NULL;
  for (size_t i = url_len; i > 0U; i--) {
    if (url[i - 1U] == '/') {
      slash = url + i - 1U;
      break;
    }
  }
  const char *authority = strstr(url, "://");
  if (slash == NULL || authority == NULL || slash <= authority + 2) {
    return ESP_ERR_INVALID_ARG;
  }
  const size_t length = (size_t)(slash - url);
  if (length == 0U || length >= parent_len)
    return ESP_ERR_INVALID_SIZE;
  memcpy(parent, url, length);
  parent[length] = '\0';
  return ESP_OK;
}

static esp_err_t flash_repository_url(char *repository_url,
                                      size_t repository_url_len) {
  char index_url[SOLAR_OS_OTA_ARTIFACT_URL_MAX];
  char release_url[SOLAR_OS_FLASH_URL_MAX];
  esp_err_t err = solar_os_ota_get_index_url(index_url, sizeof(index_url));
  if (err == ESP_OK)
    err = flash_url_parent(index_url, release_url, sizeof(release_url));
  if (err == ESP_OK)
    err = flash_url_parent(release_url, repository_url, repository_url_len);
  return err;
}

static esp_err_t flash_catalog_urls(char *catalog_url, size_t catalog_url_len,
                                    char *signature_url,
                                    size_t signature_url_len) {
  char repository_url[SOLAR_OS_FLASH_URL_MAX];
  esp_err_t err = flash_repository_url(repository_url, sizeof(repository_url));
  int written = -1;
  if (err == ESP_OK) {
    written = snprintf(catalog_url, catalog_url_len, "%s/flash/%s",
                       repository_url, FLASH_CATALOG_FILE);
    if (written < 0 || (size_t)written >= catalog_url_len)
      err = ESP_ERR_INVALID_SIZE;
  }
  if (err == ESP_OK) {
    written = snprintf(signature_url, signature_url_len, "%s/flash/%s",
                       repository_url, FLASH_SIGNATURE_FILE);
    if (written < 0 || (size_t)written >= signature_url_len)
      err = ESP_ERR_INVALID_SIZE;
  }
  return err;
}

typedef struct {
  char *data;
  size_t capacity;
  size_t length;
  solar_os_flash_progress_fn callback;
  void *user;
  solar_os_flash_progress_t progress;
} flash_http_buffer_t;

typedef struct {
  FILE *file;
  solar_os_crypto_sha256_t sha;
  uint32_t expected_size;
  uint32_t received;
  solar_os_flash_progress_fn callback;
  void *user;
  solar_os_flash_progress_t progress;
} flash_http_file_t;

typedef struct {
  esp_loader_port_t base;
  const char *uart;
  solar_os_port_handle_t serial;
  bool usb, usb_jtag, usb_reset;
  esp_err_t io_error;
  int boot_pin;
  int reset_pin;
  int64_t deadline_us;
} flash_loader_port_t;

static void flash_report(solar_os_flash_progress_fn callback, void *user,
                         solar_os_flash_progress_stage_t stage, uint32_t done,
                         uint32_t total, bool total_known) {
  if (callback == NULL) {
    return;
  }
  const solar_os_flash_progress_t progress = {
      .stage = stage,
      .bytes_done = done,
      .bytes_total = total,
      .total_known = total_known,
  };
  callback(&progress, user);
}

const char *
solar_os_flash_progress_stage_name(solar_os_flash_progress_stage_t stage) {
  switch (stage) {
  case SOLAR_OS_FLASH_PROGRESS_CATALOG:
    return "catalog";
  case SOLAR_OS_FLASH_PROGRESS_SIGNATURE:
    return "signature";
  case SOLAR_OS_FLASH_PROGRESS_VERIFYING:
    return "verify";
  case SOLAR_OS_FLASH_PROGRESS_ARCHIVE:
    return "download";
  case SOLAR_OS_FLASH_PROGRESS_EXTRACTING:
    return "extract";
  case SOLAR_OS_FLASH_PROGRESS_CONNECTING:
    return "connect";
  case SOLAR_OS_FLASH_PROGRESS_IDENTIFYING:
    return "identify";
  case SOLAR_OS_FLASH_PROGRESS_ERASING:
    return "erase";
  case SOLAR_OS_FLASH_PROGRESS_WRITING:
    return "write";
  case SOLAR_OS_FLASH_PROGRESS_TARGET_VERIFY:
    return "target verify";
  case SOLAR_OS_FLASH_PROGRESS_DONE:
    return "done";
  default:
    return "unknown";
  }
}

static bool flash_name_valid(const char *value) {
  if (value == NULL || value[0] == '\0' || strcmp(value, ".") == 0 ||
      strcmp(value, "..") == 0) {
    return false;
  }
  for (const unsigned char *p = (const unsigned char *)value; *p != '\0'; p++) {
    if (!isalnum(*p) && *p != '.' && *p != '_' && *p != '-') {
      return false;
    }
  }
  return true;
}

static bool flash_relative_path_valid(const char *path) {
  if (path == NULL || path[0] == '\0' || path[0] == '/' ||
      strchr(path, '\\') != NULL) {
    return false;
  }
  const char *segment = path;
  while (*segment != '\0') {
    const char *end = strchr(segment, '/');
    const size_t len = end != NULL ? (size_t)(end - segment) : strlen(segment);
    if (len == 0U || (len == 1U && segment[0] == '.') ||
        (len == 2U && segment[0] == '.' && segment[1] == '.')) {
      return false;
    }
    if (end == NULL) {
      break;
    }
    segment = end + 1U;
  }
  return true;
}

static esp_err_t flash_root(char *path, size_t path_len) {
  if (!solar_os_storage_sd_is_mounted()) {
    return ESP_ERR_INVALID_STATE;
  }
  return solar_os_storage_join_path(solar_os_storage_sd_mount_point(),
                                    FLASH_ROOT_RELATIVE, path, path_len);
}

static esp_err_t flash_join(const char *base, const char *relative, char *path,
                            size_t path_len) {
  return solar_os_storage_join_path(base, relative, path, path_len);
}

static esp_err_t flash_mkdir_one(const char *path) {
  if (mkdir(path, 0775) == 0 || errno == EEXIST) {
    struct stat st;
    return stat(path, &st) == 0 && S_ISDIR(st.st_mode) ? ESP_OK : ESP_FAIL;
  }
  return ESP_FAIL;
}

static esp_err_t flash_ensure_root(char *root, size_t root_len) {
  esp_err_t err = flash_root(root, root_len);
  return err == ESP_OK ? flash_mkdir_one(root) : err;
}

static esp_err_t flash_mkdir_relative(const char *root, const char *relative) {
  if (!flash_relative_path_valid(relative)) {
    return ESP_ERR_INVALID_ARG;
  }
  char path[SOLAR_OS_STORAGE_PATH_MAX];
  if (strlcpy(path, root, sizeof(path)) >= sizeof(path)) {
    return ESP_ERR_INVALID_SIZE;
  }
  const char *cursor = relative;
  while (*cursor != '\0') {
    const char *slash = strchr(cursor, '/');
    const size_t len =
        slash != NULL ? (size_t)(slash - cursor) : strlen(cursor);
    const size_t used = strlen(path);
    if (used + 1U + len >= sizeof(path)) {
      return ESP_ERR_INVALID_SIZE;
    }
    path[used] = '/';
    memcpy(path + used + 1U, cursor, len);
    path[used + 1U + len] = '\0';
    esp_err_t err = flash_mkdir_one(path);
    if (err != ESP_OK) {
      return err;
    }
    if (slash == NULL) {
      break;
    }
    cursor = slash + 1U;
  }
  return ESP_OK;
}

static esp_err_t flash_remove_tree(const char *path) {
  struct stat st;
  if (stat(path, &st) != 0) {
    return errno == ENOENT ? ESP_OK : ESP_FAIL;
  }
  if (!S_ISDIR(st.st_mode)) {
    return remove(path) == 0 ? ESP_OK : ESP_FAIL;
  }
  DIR *directory = opendir(path);
  if (directory == NULL) {
    return ESP_FAIL;
  }
  esp_err_t err = ESP_OK;
  struct dirent *entry;
  while (err == ESP_OK && (entry = readdir(directory)) != NULL) {
    if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) {
      continue;
    }
    char child[SOLAR_OS_STORAGE_PATH_MAX];
    if (flash_join(path, entry->d_name, child, sizeof(child)) != ESP_OK) {
      err = ESP_ERR_INVALID_SIZE;
    } else {
      err = flash_remove_tree(child);
    }
  }
  closedir(directory);
  if (err == ESP_OK && rmdir(path) != 0) {
    err = ESP_FAIL;
  }
  return err;
}

static esp_err_t flash_read_file(const char *path, size_t max_len, char **body,
                                 size_t *body_len) {
  struct stat st;
  if (body == NULL || body_len == NULL) {
    return ESP_ERR_INVALID_ARG;
  }
  *body = NULL;
  *body_len = 0U;
  if (stat(path, &st) != 0 || !S_ISREG(st.st_mode)) {
    return ESP_ERR_NOT_FOUND;
  }
  if (st.st_size < 0 || (uint64_t)st.st_size > max_len) {
    return ESP_ERR_INVALID_SIZE;
  }
  const size_t len = (size_t)st.st_size;
  char *data =
      solar_os_memory_alloc(len + 1U, SOLAR_OS_MEMORY_TRANSIENT, "flash.file");
  if (data == NULL) {
    return ESP_ERR_NO_MEM;
  }
  FILE *file = fopen(path, "rb");
  const size_t read_len =
      file != NULL && len > 0U ? fread(data, 1U, len, file) : 0U;
  const bool failed = file == NULL || read_len != len || ferror(file);
  if (file != NULL) {
    fclose(file);
  }
  if (failed) {
    solar_os_memory_free(data);
    return ESP_FAIL;
  }
  data[len] = '\0';
  *body = data;
  *body_len = len;
  return ESP_OK;
}

static esp_err_t flash_write_file(const char *path, const void *data,
                                  size_t len) {
  FILE *file = fopen(path, "wb");
  if (file == NULL) {
    return ESP_FAIL;
  }
  const size_t written = len > 0U ? fwrite(data, 1U, len, file) : 0U;
  const bool failed =
      written != len || fflush(file) != 0 || fsync(fileno(file)) != 0;
  fclose(file);
  return failed ? ESP_FAIL : ESP_OK;
}

static esp_err_t flash_verify_signature(const char *catalog, size_t catalog_len,
                                        const char *signature) {
  uint8_t der[SOLAR_OS_CRYPTO_ECDSA_P256_DER_SIGNATURE_MAX];
  size_t der_len = 0U;
  esp_err_t err =
      solar_os_crypto_base64_decode(signature, der, sizeof(der), &der_len);
  if (err == ESP_OK) {
    err = solar_os_crypto_ecdsa_p256_sha256_verify_pem(
        SOLAR_OS_OTA_PUBLIC_KEY_PEM, catalog, catalog_len, der, der_len);
  }
  return err;
}

static esp_err_t flash_http_buffer_event(const solar_os_http_event_t *event,
                                         void *user_data) {
  flash_http_buffer_t *buffer = (flash_http_buffer_t *)user_data;
  if (event == NULL || buffer == NULL) {
    return ESP_ERR_INVALID_ARG;
  }
  if (event->type == SOLAR_OS_HTTP_EVENT_HEADER) {
    if (event->header_name != NULL && event->header_value != NULL &&
        strcasecmp(event->header_name, "Content-Length") == 0) {
      char *end = NULL;
      const unsigned long parsed = strtoul(event->header_value, &end, 10);
      if (end != event->header_value && *end == '\0' &&
          parsed < buffer->capacity) {
        buffer->progress.bytes_total = (uint32_t)parsed;
        buffer->progress.total_known = true;
      }
    }
    return ESP_OK;
  }
  if (event->type != SOLAR_OS_HTTP_EVENT_DATA) {
    return ESP_OK;
  }
  if (event->data_len > buffer->capacity - buffer->length - 1U) {
    return ESP_ERR_INVALID_SIZE;
  }
  memcpy(buffer->data + buffer->length, event->data, event->data_len);
  buffer->length += event->data_len;
  buffer->data[buffer->length] = '\0';
  buffer->progress.bytes_done = (uint32_t)buffer->length;
  if (buffer->callback != NULL) {
    buffer->callback(&buffer->progress, buffer->user);
  }
  return ESP_OK;
}

static esp_err_t flash_download_memory(const char *url, size_t max_len,
                                       solar_os_flash_progress_stage_t stage,
                                       char **body, size_t *body_len,
                                       solar_os_flash_progress_fn callback,
                                       void *user) {
  if (url == NULL || body == NULL || body_len == NULL || max_len == 0U) {
    return ESP_ERR_INVALID_ARG;
  }
  *body = NULL;
  *body_len = 0U;
  char *data = solar_os_memory_alloc(max_len + 1U, SOLAR_OS_MEMORY_TRANSIENT,
                                     "flash.http");
  if (data == NULL) {
    return ESP_ERR_NO_MEM;
  }
  data[0] = '\0';
  flash_http_buffer_t buffer = {
      .data = data,
      .capacity = max_len + 1U,
      .callback = callback,
      .user = user,
      .progress = {.stage = stage},
  };
  flash_report(callback, user, stage, 0U, 0U, false);
  const solar_os_http_request_options_t options = {
      .url = url,
      .method = SOLAR_OS_HTTP_METHOD_GET,
      .user_agent = "SolarOS-flash/" SOLAR_OS_VERSION,
      .follow_redirects = true,
      .timeout_ms = FLASH_HTTP_TIMEOUT_MS,
      .deadline_ms = FLASH_HTTP_DEADLINE_MS,
      .receive_buffer_size = 2048U,
      .transmit_buffer_size = 1024U,
      .event_handler = flash_http_buffer_event,
      .user_data = &buffer,
  };
  solar_os_http_request_t *request = NULL;
  solar_os_http_response_t response = {0};
  esp_err_t err = solar_os_http_request_create(&options, &request);
  if (err == ESP_OK) {
    err = solar_os_http_request_perform(request, &response);
  }
  if (request != NULL) {
    const esp_err_t destroy_err = solar_os_http_request_destroy(request);
    if (err == ESP_OK) {
      err = destroy_err;
    }
  }
  if (err == ESP_OK && response.status_code != 200) {
    err = ESP_ERR_NOT_FOUND;
  }
  if (err != ESP_OK) {
    solar_os_memory_free(data);
    return err;
  }
  *body = data;
  *body_len = buffer.length;
  return ESP_OK;
}

static esp_err_t flash_catalog_parse(const char *data, size_t data_len,
                                     solar_os_flash_catalog_t **out_catalog) {
  if (data == NULL || out_catalog == NULL) {
    return ESP_ERR_INVALID_ARG;
  }
  *out_catalog = NULL;
  solar_os_json_doc_t *document = NULL;
  esp_err_t err = solar_os_json_parse(data, data_len, &document);
  const solar_os_json_value_t *root =
      err == ESP_OK ? solar_os_json_root(document) : NULL;
  char schema[32] = "";
  uint32_t schema_version = 0U;
  if (err == ESP_OK) {
    err = solar_os_json_get_path_string(root, "schema", schema, sizeof(schema));
  }
  if (err == ESP_OK) {
    err =
        solar_os_json_get_path_uint32(root, "schema_version", &schema_version);
  }
  const solar_os_json_value_t *artifacts =
      err == ESP_OK ? solar_os_json_object_get(root, "artifacts") : NULL;
  const size_t count =
      artifacts != NULL ? solar_os_json_array_size(artifacts) : 0U;
  if (err == ESP_OK &&
      (strcmp(schema, "solaros.flash_catalog") != 0 || schema_version != 1U ||
       !solar_os_json_is_array(artifacts) || count > FLASH_ARTIFACT_MAX)) {
    err = ESP_ERR_INVALID_RESPONSE;
  }

  solar_os_flash_catalog_t *catalog = NULL;
  if (err == ESP_OK) {
    catalog = solar_os_memory_calloc(
        1U, sizeof(*catalog), SOLAR_OS_MEMORY_TRANSIENT, "flash.catalog");
    if (catalog == NULL) {
      err = ESP_ERR_NO_MEM;
    }
  }
  if (err == ESP_OK && count > 0U) {
    catalog->artifacts =
        solar_os_memory_calloc(count, sizeof(*catalog->artifacts),
                               SOLAR_OS_MEMORY_TRANSIENT, "flash.entries");
    if (catalog->artifacts == NULL) {
      err = ESP_ERR_NO_MEM;
    }
  }
  if (err == ESP_OK) {
    err = flash_repository_url(catalog->base_url, sizeof(catalog->base_url));
  }
  if (err == ESP_OK) {
    catalog->count = count;
  }

  char cache_root[SOLAR_OS_STORAGE_PATH_MAX] = "";
  if (err == ESP_OK) {
    (void)flash_root(cache_root, sizeof(cache_root));
  }
  for (size_t i = 0U; err == ESP_OK && i < count; i++) {
    const solar_os_json_value_t *value = solar_os_json_array_get(artifacts, i);
    solar_os_flash_artifact_t *artifact = &catalog->artifacts[i];
    err = solar_os_json_get_path_string(value, "board_id", artifact->board_id,
                                        sizeof(artifact->board_id));
    if (err == ESP_OK)
      err = solar_os_json_get_path_string(value, "board_name",
                                          artifact->board_name,
                                          sizeof(artifact->board_name));
    if (err == ESP_OK)
      err = solar_os_json_get_path_string(value, "flavor", artifact->flavor,
                                          sizeof(artifact->flavor));
    if (err == ESP_OK)
      err = solar_os_json_get_path_string(value, "version", artifact->version,
                                          sizeof(artifact->version));
    if (err == ESP_OK)
      err = solar_os_json_get_path_string(value, "chip", artifact->chip,
                                          sizeof(artifact->chip));
    if (err == ESP_OK)
      err = solar_os_json_get_path_string(value, "archive", artifact->archive,
                                          sizeof(artifact->archive));
    if (err == ESP_OK)
      err =
          solar_os_json_get_path_uint32(value, "size", &artifact->archive_size);
    if (err == ESP_OK)
      err = solar_os_json_get_path_string(value, "sha256",
                                          artifact->archive_sha256,
                                          sizeof(artifact->archive_sha256));
    if (err == ESP_OK)
      err = solar_os_json_get_path_uint32(value, "factory_size",
                                          &artifact->factory_size);
    if (err == ESP_OK)
      err = solar_os_json_get_path_string(value, "factory_sha256",
                                          artifact->factory_sha256,
                                          sizeof(artifact->factory_sha256));
    if (err == ESP_OK)
      err = solar_os_json_get_path_uint32(value, "flash_size",
                                          &artifact->flash_size);
    if (err == ESP_OK &&
        (!flash_name_valid(artifact->board_id) ||
         !flash_name_valid(artifact->flavor) ||
         !flash_name_valid(artifact->version) ||
         !flash_name_valid(artifact->chip) ||
         !flash_relative_path_valid(artifact->archive) ||
         artifact->archive_size == 0U || artifact->factory_size == 0U ||
         artifact->flash_size == 0U ||
         !solar_os_crypto_sha256_hex_is_valid(artifact->archive_sha256) ||
         !solar_os_crypto_sha256_hex_is_valid(artifact->factory_sha256))) {
      err = ESP_ERR_INVALID_RESPONSE;
    }
    if (err == ESP_OK && cache_root[0] != '\0') {
      char relative[SOLAR_OS_STORAGE_PATH_MAX];
      char factory[SOLAR_OS_STORAGE_PATH_MAX];
      const int written =
          snprintf(relative, sizeof(relative), "%s/%s/%s/factory.bin",
                   artifact->board_id, artifact->flavor, artifact->version);
      if (written > 0 && (size_t)written < sizeof(relative) &&
          flash_join(cache_root, relative, factory, sizeof(factory)) ==
              ESP_OK) {
        struct stat st;
        artifact->cached = stat(factory, &st) == 0 && S_ISREG(st.st_mode) &&
                           st.st_size >= 0 &&
                           (uint64_t)st.st_size == artifact->factory_size;
      }
    }
  }

  solar_os_json_free(document);
  if (err != ESP_OK) {
    solar_os_flash_catalog_free(catalog);
    return err;
  }
  *out_catalog = catalog;
  return ESP_OK;
}

void solar_os_flash_catalog_free(solar_os_flash_catalog_t *catalog) {
  if (catalog == NULL) {
    return;
  }
  solar_os_memory_free(catalog->artifacts);
  solar_os_memory_free(catalog);
}

static esp_err_t flash_cached_catalog_read(char **catalog, size_t *catalog_len,
                                           char **signature,
                                           size_t *signature_len) {
  char root[SOLAR_OS_STORAGE_PATH_MAX];
  char path[SOLAR_OS_STORAGE_PATH_MAX];
  esp_err_t err = flash_root(root, sizeof(root));
  if (err == ESP_OK)
    err = flash_join(root, FLASH_CATALOG_FILE, path, sizeof(path));
  if (err == ESP_OK)
    err = flash_read_file(path, FLASH_CATALOG_MAX, catalog, catalog_len);
  if (err == ESP_OK)
    err = flash_join(root, FLASH_SIGNATURE_FILE, path, sizeof(path));
  if (err == ESP_OK)
    err = flash_read_file(path, FLASH_SIGNATURE_MAX, signature, signature_len);
  if (err != ESP_OK) {
    solar_os_memory_free(*catalog);
    *catalog = NULL;
  }
  return err;
}

esp_err_t solar_os_flash_catalog_load(solar_os_flash_catalog_t **catalog) {
  if (catalog == NULL) {
    return ESP_ERR_INVALID_ARG;
  }
  *catalog = NULL;
  char *data = NULL;
  char *signature = NULL;
  size_t data_len = 0U;
  size_t signature_len = 0U;
  esp_err_t err =
      flash_cached_catalog_read(&data, &data_len, &signature, &signature_len);
  if (err == ESP_OK) {
    err = flash_verify_signature(data, data_len, signature);
  }
  if (err == ESP_OK) {
    err = flash_catalog_parse(data, data_len, catalog);
  }
  solar_os_memory_free(signature);
  solar_os_memory_free(data);
  return err;
}

static esp_err_t flash_activate_catalog(const char *catalog, size_t catalog_len,
                                        const char *signature,
                                        size_t signature_len) {
  char root[SOLAR_OS_STORAGE_PATH_MAX];
  char catalog_path[SOLAR_OS_STORAGE_PATH_MAX];
  char signature_path[SOLAR_OS_STORAGE_PATH_MAX];
  char catalog_new[SOLAR_OS_STORAGE_PATH_MAX];
  char signature_new[SOLAR_OS_STORAGE_PATH_MAX];
  char catalog_old[SOLAR_OS_STORAGE_PATH_MAX];
  char signature_old[SOLAR_OS_STORAGE_PATH_MAX];
  esp_err_t err = flash_ensure_root(root, sizeof(root));
  if (err == ESP_OK)
    err = flash_join(root, FLASH_CATALOG_FILE, catalog_path,
                     sizeof(catalog_path));
  if (err == ESP_OK)
    err = flash_join(root, FLASH_SIGNATURE_FILE, signature_path,
                     sizeof(signature_path));
  if (err == ESP_OK)
    err =
        flash_join(root, "catalog.json.new", catalog_new, sizeof(catalog_new));
  if (err == ESP_OK)
    err = flash_join(root, "catalog.sig.new", signature_new,
                     sizeof(signature_new));
  if (err == ESP_OK)
    err =
        flash_join(root, "catalog.json.old", catalog_old, sizeof(catalog_old));
  if (err == ESP_OK)
    err = flash_join(root, "catalog.sig.old", signature_old,
                     sizeof(signature_old));
  if (err != ESP_OK)
    return err;

  (void)remove(catalog_new);
  (void)remove(signature_new);
  (void)remove(catalog_old);
  (void)remove(signature_old);
  err = flash_write_file(catalog_new, catalog, catalog_len);
  if (err == ESP_OK)
    err = flash_write_file(signature_new, signature, signature_len);
  bool catalog_backed_up = false;
  bool signature_backed_up = false;
  if (err == ESP_OK && rename(catalog_path, catalog_old) == 0)
    catalog_backed_up = true;
  else if (err == ESP_OK && errno != ENOENT)
    err = ESP_FAIL;
  if (err == ESP_OK && rename(signature_path, signature_old) == 0)
    signature_backed_up = true;
  else if (err == ESP_OK && errno != ENOENT)
    err = ESP_FAIL;
  if (err == ESP_OK && rename(signature_new, signature_path) != 0)
    err = ESP_FAIL;
  if (err == ESP_OK && rename(catalog_new, catalog_path) != 0)
    err = ESP_FAIL;
  if (err != ESP_OK) {
    (void)remove(catalog_path);
    (void)remove(signature_path);
    if (catalog_backed_up)
      (void)rename(catalog_old, catalog_path);
    if (signature_backed_up)
      (void)rename(signature_old, signature_path);
  } else {
    (void)remove(catalog_old);
    (void)remove(signature_old);
  }
  (void)remove(catalog_new);
  (void)remove(signature_new);
  return err;
}

esp_err_t solar_os_flash_catalog_refresh(solar_os_flash_progress_fn progress,
                                         void *user) {
  if (!solar_os_storage_sd_is_mounted()) {
    return ESP_ERR_INVALID_STATE;
  }
  char *catalog = NULL;
  char *signature = NULL;
  size_t catalog_len = 0U;
  size_t signature_len = 0U;
  char catalog_url[SOLAR_OS_FLASH_URL_MAX];
  char signature_url[SOLAR_OS_FLASH_URL_MAX];
  esp_err_t err = flash_catalog_urls(catalog_url, sizeof(catalog_url),
                                     signature_url, sizeof(signature_url));
  if (err == ESP_OK) {
    err = flash_download_memory(catalog_url, FLASH_CATALOG_MAX,
                                SOLAR_OS_FLASH_PROGRESS_CATALOG, &catalog,
                                &catalog_len, progress, user);
  }
  if (err == ESP_OK) {
    err = flash_download_memory(signature_url, FLASH_SIGNATURE_MAX,
                                SOLAR_OS_FLASH_PROGRESS_SIGNATURE, &signature,
                                &signature_len, progress, user);
  }
  flash_report(progress, user, SOLAR_OS_FLASH_PROGRESS_VERIFYING, 0U, 0U,
               false);
  if (err == ESP_OK) {
    err = flash_verify_signature(catalog, catalog_len, signature);
  }
  solar_os_flash_catalog_t *parsed = NULL;
  if (err == ESP_OK) {
    err = flash_catalog_parse(catalog, catalog_len, &parsed);
  }
  solar_os_flash_catalog_free(parsed);
  if (err == ESP_OK) {
    err =
        flash_activate_catalog(catalog, catalog_len, signature, signature_len);
  }
  solar_os_memory_free(signature);
  solar_os_memory_free(catalog);
  if (err == ESP_OK) {
    flash_report(progress, user, SOLAR_OS_FLASH_PROGRESS_DONE, 1U, 1U, true);
  }
  return err;
}

const solar_os_flash_artifact_t *
solar_os_flash_catalog_find(const solar_os_flash_catalog_t *catalog,
                            const char *board_id, const char *flavor,
                            const char *version) {
  if (catalog == NULL || board_id == NULL || flavor == NULL) {
    return NULL;
  }
  for (size_t i = 0U; i < catalog->count; i++) {
    const solar_os_flash_artifact_t *artifact = &catalog->artifacts[i];
    if (strcmp(artifact->board_id, board_id) == 0 &&
        strcmp(artifact->flavor, flavor) == 0 &&
        (version == NULL || version[0] == '\0' ||
         strcmp(artifact->version, version) == 0)) {
      return artifact;
    }
  }
  return NULL;
}

static esp_err_t flash_http_file_event(const solar_os_http_event_t *event,
                                       void *user_data) {
  flash_http_file_t *output = (flash_http_file_t *)user_data;
  if (event == NULL || output == NULL) {
    return ESP_ERR_INVALID_ARG;
  }
  if (event->type == SOLAR_OS_HTTP_EVENT_HEADER) {
    if (event->header_name != NULL && event->header_value != NULL &&
        strcasecmp(event->header_name, "Content-Length") == 0) {
      char *end = NULL;
      const unsigned long parsed = strtoul(event->header_value, &end, 10);
      if (end == event->header_value || *end != '\0' ||
          parsed != output->expected_size) {
        return ESP_ERR_INVALID_SIZE;
      }
    }
    return ESP_OK;
  }
  if (event->type != SOLAR_OS_HTTP_EVENT_DATA) {
    return ESP_OK;
  }
  if (event->data_len > output->expected_size - output->received ||
      fwrite(event->data, 1U, event->data_len, output->file) !=
          event->data_len) {
    return ESP_FAIL;
  }
  esp_err_t err =
      solar_os_crypto_sha256_update(&output->sha, event->data, event->data_len);
  if (err == ESP_OK) {
    output->received += (uint32_t)event->data_len;
    output->progress.bytes_done = output->received;
    if (output->callback != NULL) {
      output->callback(&output->progress, output->user);
    }
  }
  return err;
}

static esp_err_t
flash_download_archive(const char *url, const char *path,
                       const solar_os_flash_artifact_t *artifact,
                       solar_os_flash_progress_fn callback, void *user) {
  FILE *file = fopen(path, "wb");
  if (file == NULL) {
    return ESP_FAIL;
  }
  flash_http_file_t output = {
      .file = file,
      .expected_size = artifact->archive_size,
      .callback = callback,
      .user = user,
      .progress =
          {
              .stage = SOLAR_OS_FLASH_PROGRESS_ARCHIVE,
              .bytes_total = artifact->archive_size,
              .total_known = true,
          },
  };
  solar_os_crypto_sha256_init(&output.sha);
  esp_err_t err = solar_os_crypto_sha256_start(&output.sha);
  flash_report(callback, user, SOLAR_OS_FLASH_PROGRESS_ARCHIVE, 0U,
               artifact->archive_size, true);
  const solar_os_http_request_options_t options = {
      .url = url,
      .method = SOLAR_OS_HTTP_METHOD_GET,
      .user_agent = "SolarOS-flash/" SOLAR_OS_VERSION,
      .follow_redirects = true,
      .timeout_ms = FLASH_HTTP_TIMEOUT_MS,
      .deadline_ms = FLASH_HTTP_DEADLINE_MS,
      .receive_buffer_size = 4096U,
      .transmit_buffer_size = 1024U,
      .event_handler = flash_http_file_event,
      .user_data = &output,
  };
  solar_os_http_request_t *request = NULL;
  solar_os_http_response_t response = {0};
  if (err == ESP_OK)
    err = solar_os_http_request_create(&options, &request);
  if (err == ESP_OK)
    err = solar_os_http_request_perform(request, &response);
  if (request != NULL) {
    const esp_err_t destroy_err = solar_os_http_request_destroy(request);
    if (err == ESP_OK)
      err = destroy_err;
  }
  if (err == ESP_OK && response.status_code != 200)
    err = ESP_ERR_NOT_FOUND;
  uint8_t digest[SOLAR_OS_CRYPTO_SHA256_LEN];
  if (err == ESP_OK)
    err = solar_os_crypto_sha256_finish(&output.sha, digest);
  solar_os_crypto_sha256_free(&output.sha);
  if (fflush(file) != 0 || fsync(fileno(file)) != 0)
    err = ESP_FAIL;
  fclose(file);
  if (err == ESP_OK &&
      (output.received != artifact->archive_size ||
       !solar_os_crypto_sha256_matches_hex(digest, artifact->archive_sha256))) {
    err = ESP_ERR_INVALID_CRC;
  }
  if (err != ESP_OK)
    (void)remove(path);
  return err;
}

static esp_err_t flash_file_sha256(const char *path, uint32_t expected_size,
                                   const char *expected_sha) {
  struct stat st;
  if (stat(path, &st) != 0 || !S_ISREG(st.st_mode) || st.st_size < 0 ||
      (uint64_t)st.st_size != expected_size) {
    return ESP_ERR_INVALID_SIZE;
  }
  FILE *file = fopen(path, "rb");
  uint8_t *buffer = solar_os_memory_alloc(
      FLASH_IO_BLOCK, SOLAR_OS_MEMORY_TRANSIENT, "flash.hash");
  if (file == NULL || buffer == NULL) {
    if (file != NULL)
      fclose(file);
    solar_os_memory_free(buffer);
    return buffer == NULL ? ESP_ERR_NO_MEM : ESP_FAIL;
  }
  solar_os_crypto_sha256_t sha;
  solar_os_crypto_sha256_init(&sha);
  esp_err_t err = solar_os_crypto_sha256_start(&sha);
  while (err == ESP_OK) {
    const size_t count = fread(buffer, 1U, FLASH_IO_BLOCK, file);
    if (count > 0U)
      err = solar_os_crypto_sha256_update(&sha, buffer, count);
    if (count < FLASH_IO_BLOCK) {
      if (ferror(file))
        err = ESP_FAIL;
      break;
    }
  }
  uint8_t digest[SOLAR_OS_CRYPTO_SHA256_LEN];
  if (err == ESP_OK)
    err = solar_os_crypto_sha256_finish(&sha, digest);
  solar_os_crypto_sha256_free(&sha);
  fclose(file);
  solar_os_memory_free(buffer);
  if (err == ESP_OK &&
      !solar_os_crypto_sha256_matches_hex(digest, expected_sha)) {
    err = ESP_ERR_INVALID_CRC;
  }
  return err;
}

static esp_err_t
flash_verify_manifest(const char *stage,
                      const solar_os_flash_artifact_t *artifact) {
  char path[SOLAR_OS_STORAGE_PATH_MAX];
  char *body = NULL;
  size_t body_len = 0U;
  esp_err_t err = flash_join(stage, "flash-manifest.json", path, sizeof(path));
  if (err == ESP_OK)
    err = flash_read_file(path, 8192U, &body, &body_len);
  solar_os_json_doc_t *document = NULL;
  if (err == ESP_OK)
    err = solar_os_json_parse(body, body_len, &document);
  const solar_os_json_value_t *root =
      err == ESP_OK ? solar_os_json_root(document) : NULL;
  char schema[32] = "", board[SOLAR_OS_FLASH_BOARD_ID_MAX] = "";
  char flavor[SOLAR_OS_FLASH_FLAVOR_MAX] = "",
       version[SOLAR_OS_FLASH_VERSION_MAX] = "";
  char chip[SOLAR_OS_FLASH_CHIP_MAX] = "", factory_name[32] = "", sha[65] = "";
  uint32_t schema_version = 0U, size = 0U, offset = UINT32_MAX;
  uint32_t flash_size = 0U;
  char erase[16] = "", security[40] = "";
  if (err == ESP_OK)
    err = solar_os_json_get_path_string(root, "schema", schema, sizeof(schema));
  if (err == ESP_OK)
    err =
        solar_os_json_get_path_uint32(root, "schema_version", &schema_version);
  if (err == ESP_OK)
    err = solar_os_json_get_path_string(root, "board.id", board, sizeof(board));
  if (err == ESP_OK)
    err = solar_os_json_get_path_string(root, "flavor", flavor, sizeof(flavor));
  if (err == ESP_OK)
    err = solar_os_json_get_path_string(root, "version", version,
                                        sizeof(version));
  if (err == ESP_OK)
    err =
        solar_os_json_get_path_string(root, "target.chip", chip, sizeof(chip));
  if (err == ESP_OK)
    err = solar_os_json_get_path_uint32(root, "target.image_offset", &offset);
  if (err == ESP_OK)
    err = solar_os_json_get_path_uint32(root, "target.flash_size_bytes",
                                        &flash_size);
  if (err == ESP_OK)
    err = solar_os_json_get_path_string(root, "target.erase", erase,
                                        sizeof(erase));
  if (err == ESP_OK)
    err = solar_os_json_get_path_string(root, "target.security", security,
                                        sizeof(security));
  if (err == ESP_OK)
    err = solar_os_json_get_path_string(root, "artifact.factory_firmware",
                                        factory_name, sizeof(factory_name));
  if (err == ESP_OK)
    err = solar_os_json_get_path_uint32(root, "artifact.size", &size);
  if (err == ESP_OK)
    err = solar_os_json_get_path_string(root, "artifact.sha256", sha,
                                        sizeof(sha));
  if (err == ESP_OK &&
      (strcmp(schema, "solaros.flash_manifest") != 0 || schema_version != 1U ||
       strcmp(board, artifact->board_id) != 0 ||
       strcmp(flavor, artifact->flavor) != 0 ||
       strcmp(version, artifact->version) != 0 ||
       strcmp(chip, artifact->chip) != 0 || offset != 0U ||
       flash_size != artifact->flash_size || strcmp(erase, "chip") != 0 ||
       strcmp(security, "unencrypted_uart_download") != 0 ||
       strcmp(factory_name, "factory.bin") != 0 ||
       size != artifact->factory_size ||
       strcmp(sha, artifact->factory_sha256) != 0)) {
    err = ESP_ERR_INVALID_RESPONSE;
  }
  solar_os_json_free(document);
  solar_os_memory_free(body);
  if (err == ESP_OK)
    err = flash_join(stage, "factory.bin", path, sizeof(path));
  if (err == ESP_OK)
    err = flash_file_sha256(path, artifact->factory_size,
                            artifact->factory_sha256);
  return err;
}

esp_err_t
solar_os_flash_artifact_download(const solar_os_flash_catalog_t *catalog,
                                 const solar_os_flash_artifact_t *artifact,
                                 solar_os_flash_progress_fn progress,
                                 void *user) {
  if (catalog == NULL || artifact == NULL || catalog->base_url[0] == '\0') {
    return ESP_ERR_INVALID_ARG;
  }
  char root[SOLAR_OS_STORAGE_PATH_MAX];
  char stage[SOLAR_OS_STORAGE_PATH_MAX];
  char archive_path[SOLAR_OS_STORAGE_PATH_MAX];
  char url[SOLAR_OS_FLASH_URL_MAX];
  esp_err_t err = flash_ensure_root(root, sizeof(root));
  if (err == ESP_OK)
    err = flash_join(root, ".stage", stage, sizeof(stage));
  if (err == ESP_OK)
    err = flash_remove_tree(stage);
  if (err == ESP_OK)
    err = flash_mkdir_one(stage);
  if (err == ESP_OK)
    err = flash_join(stage, "flash.zip", archive_path, sizeof(archive_path));
  const int url_len =
      snprintf(url, sizeof(url), "%s/%s", catalog->base_url, artifact->archive);
  if (err == ESP_OK && (url_len < 0 || (size_t)url_len >= sizeof(url))) {
    err = ESP_ERR_INVALID_SIZE;
  }
  if (err == ESP_OK) {
    err = flash_download_archive(url, archive_path, artifact, progress, user);
  }
  if (err == ESP_OK) {
    flash_report(progress, user, SOLAR_OS_FLASH_PROGRESS_EXTRACTING, 0U, 0U,
                 false);
    err = solar_os_zip_extract(archive_path, stage, NULL);
  }
  if (err == ESP_OK && remove(archive_path) != 0)
    err = ESP_FAIL;
  if (err == ESP_OK) {
    flash_report(progress, user, SOLAR_OS_FLASH_PROGRESS_VERIFYING, 0U, 0U,
                 false);
    err = flash_verify_manifest(stage, artifact);
  }

  char parent_relative[SOLAR_OS_STORAGE_PATH_MAX];
  char parent[SOLAR_OS_STORAGE_PATH_MAX];
  char final_path[SOLAR_OS_STORAGE_PATH_MAX];
  char backup[SOLAR_OS_STORAGE_PATH_MAX];
  if (err == ESP_OK) {
    const int written = snprintf(parent_relative, sizeof(parent_relative),
                                 "%s/%s", artifact->board_id, artifact->flavor);
    if (written < 0 || (size_t)written >= sizeof(parent_relative))
      err = ESP_ERR_INVALID_SIZE;
  }
  if (err == ESP_OK)
    err = flash_mkdir_relative(root, parent_relative);
  if (err == ESP_OK)
    err = flash_join(root, parent_relative, parent, sizeof(parent));
  if (err == ESP_OK)
    err = flash_join(parent, artifact->version, final_path, sizeof(final_path));
  char backup_name[SOLAR_OS_FLASH_VERSION_MAX + 8U];
  if (err == ESP_OK) {
    const int written = snprintf(backup_name, sizeof(backup_name), ".%s.old",
                                 artifact->version);
    if (written < 0 || (size_t)written >= sizeof(backup_name))
      err = ESP_ERR_INVALID_SIZE;
  }
  if (err == ESP_OK)
    err = flash_join(parent, backup_name, backup, sizeof(backup));
  bool backed_up = false;
  if (err == ESP_OK)
    err = flash_remove_tree(backup);
  if (err == ESP_OK && rename(final_path, backup) == 0)
    backed_up = true;
  else if (err == ESP_OK && errno != ENOENT)
    err = ESP_FAIL;
  if (err == ESP_OK && rename(stage, final_path) != 0)
    err = ESP_FAIL;
  if (err != ESP_OK) {
    (void)flash_remove_tree(stage);
    if (backed_up) {
      (void)flash_remove_tree(final_path);
      (void)rename(backup, final_path);
    }
  } else {
    (void)flash_remove_tree(backup);
    flash_report(progress, user, SOLAR_OS_FLASH_PROGRESS_DONE, 1U, 1U, true);
  }
  return err;
}

static flash_loader_port_t *flash_loader_port(esp_loader_port_t *base) {
  return container_of(base, flash_loader_port_t, base);
}

static esp_loader_error_t flash_port_init(esp_loader_port_t *base) {
  (void)base;
  return ESP_LOADER_SUCCESS;
}

static void flash_port_deinit(esp_loader_port_t *base) { (void)base; }

static void flash_port_delay(esp_loader_port_t *base, uint32_t ms) {
  (void)base;
  vTaskDelay(pdMS_TO_TICKS(ms));
}

static void flash_port_start_timer(esp_loader_port_t *base, uint32_t ms) {
  flash_loader_port(base)->deadline_us =
      esp_timer_get_time() + (int64_t)ms * 1000;
}

static uint32_t flash_port_remaining(esp_loader_port_t *base) {
  const int64_t remaining =
      flash_loader_port(base)->deadline_us - esp_timer_get_time();
  return remaining > 0 ? (uint32_t)(remaining / 1000) : 0U;
}

static void flash_port_lines(flash_loader_port_t *port, bool dtr, bool rts) {
  if (port->io_error != ESP_OK) return;
  const solar_os_port_serial_config_t config = {.dtr = dtr, .rts = rts};
  port->io_error = solar_os_port_serial_configure(&port->serial, &config,
      SOLAR_OS_SERIAL_DTR | SOLAR_OS_SERIAL_RTS);
}

static void flash_port_usb_reset(flash_loader_port_t *port) {
  flash_port_lines(port, false, true);
  flash_port_delay(&port->base, 100U);
  flash_port_lines(port, false, false);
}

static void flash_port_enter_bootloader(esp_loader_port_t *base) {
  flash_loader_port_t *port = flash_loader_port(base);
  if (port->usb_reset) {
    if (port->usb_jtag) {
      flash_port_lines(port, true, false);
      flash_port_delay(base, 100U);
      flash_port_lines(port, true, true);
      flash_port_usb_reset(port);
    } else {
      flash_port_lines(port, false, true);
      flash_port_delay(base, 100U);
      flash_port_lines(port, true, false);
      flash_port_delay(base, 100U);
      flash_port_lines(port, false, false);
    }
    return;
  }
  if (port->boot_pin >= 0)
    (void)solar_os_gpio_write(port->boot_pin, false);
  if (port->reset_pin >= 0) {
    (void)solar_os_gpio_write(port->reset_pin, false);
    flash_port_delay(base, 100U);
    (void)solar_os_gpio_write(port->reset_pin, true);
    flash_port_delay(base, 50U);
    if (port->boot_pin >= 0)
      (void)solar_os_gpio_write(port->boot_pin, true);
  }
}

static void flash_port_reset_target(esp_loader_port_t *base) {
  flash_loader_port_t *port = flash_loader_port(base);
  if (port->usb_reset) { flash_port_usb_reset(port); return; }
  if (port->reset_pin >= 0) {
    (void)solar_os_gpio_write(port->reset_pin, false);
    flash_port_delay(base, 100U);
    (void)solar_os_gpio_write(port->reset_pin, true);
  }
}

static esp_loader_error_t flash_port_change_rate(esp_loader_port_t *base,
                                                 uint32_t baud_rate) {
  flash_loader_port_t *port = flash_loader_port(base);
  if (port->io_error != ESP_OK) return ESP_LOADER_ERROR_FAIL;
  const solar_os_port_serial_config_t config = {.baud_rate = baud_rate};
  port->io_error = port->usb ? solar_os_port_serial_configure(&port->serial,
      &config, SOLAR_OS_SERIAL_BAUD) : solar_os_uart_bus_set_baud_rate_owned(
      port->uart, baud_rate, FLASH_OWNER);
  return port->io_error == ESP_OK ? ESP_LOADER_SUCCESS : ESP_LOADER_ERROR_FAIL;
}

static esp_loader_error_t flash_port_write(esp_loader_port_t *base,
                                           const uint8_t *data, uint16_t size,
                                           uint32_t timeout) {
  (void)timeout;
  flash_loader_port_t *port = flash_loader_port(base);
  if (port->io_error != ESP_OK) return ESP_LOADER_ERROR_FAIL;
  size_t offset = 0U;
  while (offset < size) {
    size_t written = 0U;
    port->io_error = port->usb ? solar_os_port_write(&port->serial, data + offset,
        size - offset, &written) : solar_os_bus_uart_write(port->uart,
        data + offset, size - offset, &written);
    if (port->io_error != ESP_OK || written == 0U || written > size - offset) {
      return ESP_LOADER_ERROR_FAIL;
    }
    offset += written;
  }
  return ESP_LOADER_SUCCESS;
}

static esp_loader_error_t flash_port_read(esp_loader_port_t *base,
                                          uint8_t *data, uint16_t size,
                                          uint32_t timeout) {
  flash_loader_port_t *port = flash_loader_port(base);
  if (port->io_error != ESP_OK) return ESP_LOADER_ERROR_FAIL;
  const int64_t deadline = esp_timer_get_time() + (int64_t)timeout * 1000;
  size_t offset = 0U;
  while (offset < size) {
    const int64_t remaining_us = deadline - esp_timer_get_time();
    if (remaining_us <= 0)
      return ESP_LOADER_ERROR_TIMEOUT;
    size_t read_len = 0U;
    const uint32_t wait_ms = (uint32_t)((remaining_us + 999) / 1000);
    esp_err_t err = port->usb ? solar_os_port_read(&port->serial,
        data + offset, size - offset, wait_ms, &read_len) : solar_os_bus_uart_read(
        port->uart, data + offset, size - offset, wait_ms, &read_len);
    if (err == ESP_ERR_TIMEOUT)
      return ESP_LOADER_ERROR_TIMEOUT;
    if (err != ESP_OK || read_len > size - offset) {
      port->io_error = err != ESP_OK ? err : ESP_ERR_INVALID_SIZE;
      return ESP_LOADER_ERROR_FAIL;
    }
    offset += read_len;
  }
  return ESP_LOADER_SUCCESS;
}

static const esp_loader_port_ops_t flash_port_ops = {
    .init = flash_port_init,
    .deinit = flash_port_deinit,
    .enter_bootloader = flash_port_enter_bootloader,
    .reset_target = flash_port_reset_target,
    .start_timer = flash_port_start_timer,
    .remaining_time = flash_port_remaining,
    .delay_ms = flash_port_delay,
    .change_transmission_rate = flash_port_change_rate,
    .write = flash_port_write,
    .read = flash_port_read,
};

static const char *flash_target_name(target_chip_t target) {
  switch (target) {
  case ESP32_CHIP:
    return "esp32";
  case ESP32S2_CHIP:
    return "esp32s2";
  case ESP32C3_CHIP:
    return "esp32c3";
  case ESP32S3_CHIP:
    return "esp32s3";
  case ESP32C2_CHIP:
    return "esp32c2";
  case ESP32C5_CHIP:
    return "esp32c5";
  case ESP32H2_CHIP:
    return "esp32h2";
  case ESP32C6_CHIP:
    return "esp32c6";
  case ESP32P4_CHIP:
    return "esp32p4";
  case ESP32C61_CHIP:
    return "esp32c61";
  default:
    return "unknown";
  }
}

static esp_err_t flash_loader_error(esp_loader_error_t error) {
  switch (error) {
  case ESP_LOADER_SUCCESS:
    return ESP_OK;
  case ESP_LOADER_ERROR_TIMEOUT:
    return ESP_ERR_TIMEOUT;
  case ESP_LOADER_ERROR_IMAGE_SIZE:
    return ESP_ERR_INVALID_SIZE;
  case ESP_LOADER_ERROR_INVALID_MD5:
    return ESP_ERR_INVALID_CRC;
  case ESP_LOADER_ERROR_UNSUPPORTED_CHIP:
  case ESP_LOADER_ERROR_UNSUPPORTED_FUNC:
    return ESP_ERR_NOT_SUPPORTED;
  case ESP_LOADER_ERROR_INVALID_PARAM:
    return ESP_ERR_INVALID_ARG;
  default:
    return ESP_FAIL;
  }
}

static esp_err_t flash_claim_gpio(flash_loader_port_t *port) {
  int pins[2];
  const char *labels[2];
  size_t count = 0U;
  if (port->boot_pin >= 0) {
    pins[count] = port->boot_pin;
    labels[count++] = "target boot";
  }
  if (port->reset_pin >= 0) {
    if (port->reset_pin == port->boot_pin)
      return ESP_ERR_INVALID_ARG;
    pins[count] = port->reset_pin;
    labels[count++] = "target reset";
  }
  if (count == 0U)
    return ESP_OK;
  esp_err_t err =
      solar_os_gpio_claim_pins(pins, labels, count, FLASH_OWNER, NULL);
  for (size_t i = 0U; err == ESP_OK && i < count; i++) {
    err = solar_os_gpio_configure_owned(pins[i], SOLAR_OS_GPIO_MODE_OUTPUT,
                                        SOLAR_OS_GPIO_PULL_UP, FLASH_OWNER);
    if (err == ESP_OK)
      err = solar_os_gpio_write(pins[i], true);
  }
  if (err != ESP_OK) {
    for (size_t i = 0U; i < count; i++) {
      (void)solar_os_gpio_release_owned(pins[i], FLASH_OWNER);
    }
  }
  return err;
}

static void flash_release_gpio(const flash_loader_port_t *port) {
  if (port->boot_pin >= 0)
    (void)solar_os_gpio_release_owned(port->boot_pin, FLASH_OWNER);
  if (port->reset_pin >= 0 && port->reset_pin != port->boot_pin) {
    (void)solar_os_gpio_release_owned(port->reset_pin, FLASH_OWNER);
  }
}

static esp_err_t flash_factory_path(const solar_os_flash_artifact_t *artifact,
                                    char *path, size_t path_len) {
  char root[SOLAR_OS_STORAGE_PATH_MAX];
  char relative[SOLAR_OS_STORAGE_PATH_MAX];
  esp_err_t err = flash_root(root, sizeof(root));
  const int written =
      snprintf(relative, sizeof(relative), "%s/%s/%s/factory.bin",
               artifact->board_id, artifact->flavor, artifact->version);
  if (err == ESP_OK && (written < 0 || (size_t)written >= sizeof(relative))) {
    err = ESP_ERR_INVALID_SIZE;
  }
  return err == ESP_OK ? flash_join(root, relative, path, path_len) : err;
}

esp_err_t
solar_os_flash_artifact_delete(const solar_os_flash_artifact_t *artifact) {
  if (artifact == NULL || !flash_name_valid(artifact->board_id) ||
      !flash_name_valid(artifact->flavor) ||
      !flash_name_valid(artifact->version)) {
    return ESP_ERR_INVALID_ARG;
  }

  char root[SOLAR_OS_STORAGE_PATH_MAX];
  char board[SOLAR_OS_STORAGE_PATH_MAX];
  char flavor[SOLAR_OS_STORAGE_PATH_MAX];
  char version[SOLAR_OS_STORAGE_PATH_MAX];
  esp_err_t err = flash_root(root, sizeof(root));
  if (err == ESP_OK)
    err = flash_join(root, artifact->board_id, board, sizeof(board));
  if (err == ESP_OK)
    err = flash_join(board, artifact->flavor, flavor, sizeof(flavor));
  if (err == ESP_OK)
    err = flash_join(flavor, artifact->version, version, sizeof(version));
  if (err != ESP_OK)
    return err;

  struct stat st;
  if (stat(version, &st) != 0)
    return errno == ENOENT ? ESP_ERR_NOT_FOUND : ESP_FAIL;
  if (!S_ISDIR(st.st_mode))
    return ESP_ERR_INVALID_STATE;

  err = flash_remove_tree(version);
  if (err == ESP_OK) {
    (void)rmdir(flavor);
    (void)rmdir(board);
  }
  return err;
}

esp_err_t
solar_os_flash_artifact_program(const solar_os_flash_artifact_t *artifact,
                                const solar_os_flash_program_options_t *options,
                                solar_os_flash_progress_fn progress,
                                void *user) {
  if (artifact == NULL || options == NULL || options->port == NULL ||
      options->port[0] == '\0' || artifact->factory_size % 4U != 0U) {
    return ESP_ERR_INVALID_ARG;
  }
  char factory_path[SOLAR_OS_STORAGE_PATH_MAX];
  esp_err_t err =
      flash_factory_path(artifact, factory_path, sizeof(factory_path));
  if (err == ESP_OK) {
    flash_report(progress, user, SOLAR_OS_FLASH_PROGRESS_VERIFYING, 0U,
                 artifact->factory_size, true);
    err = flash_file_sha256(factory_path, artifact->factory_size,
                            artifact->factory_sha256);
  }
  FILE *factory = err == ESP_OK ? fopen(factory_path, "rb") : NULL;
  if (err == ESP_OK && factory == NULL)
    err = ESP_ERR_NOT_FOUND;
  uint8_t *buffer = NULL;
  if (err == ESP_OK) {
    buffer = solar_os_memory_alloc(
        FLASH_LOADER_BLOCK, SOLAR_OS_MEMORY_INTERNAL_PREFERRED, "flash.loader");
    if (buffer == NULL)
      err = ESP_ERR_NO_MEM;
  }

  solar_os_uart_status_t previous = {0};
  solar_os_port_serial_config_t previous_serial = {0};
  bool uart_acquired = false, serial_acquired = false, serial_saved = false;
  solar_os_port_info_t port_info = {0};
  const bool usb = solar_os_port_get_info(options->port, &port_info) == ESP_OK &&
      (port_info.capabilities & SOLAR_OS_PORT_CAP_SERIAL);
  flash_loader_port_t port = {
      .base = {.ops = &flash_port_ops}, .uart = options->port,
      .serial = SOLAR_OS_PORT_HANDLE_INIT, .usb = usb,
      .usb_jtag = (port_info.capabilities & SOLAR_OS_PORT_CAP_USB_JTAG) != 0,
      .usb_reset = usb && options->boot_pin < 0 && options->reset_pin < 0,
      .boot_pin = options->boot_pin, .reset_pin = options->reset_pin,
  };
  if (err == ESP_OK) {
    flash_report(progress, user, SOLAR_OS_FLASH_PROGRESS_CONNECTING, 0U, 0U, false);
    if (usb) {
      err = solar_os_port_claim(options->port, FLASH_OWNER, &port.serial);
      serial_acquired = err == ESP_OK;
      if (err == ESP_OK) {
        err = solar_os_port_serial_get(&port.serial, &previous_serial);
        serial_saved = err == ESP_OK;
      }
      const solar_os_port_serial_config_t config = {.baud_rate = 115200, .data_bits = 8};
      if (err == ESP_OK) err = solar_os_port_serial_configure(&port.serial, &config,
          SOLAR_OS_SERIAL_BAUD | SOLAR_OS_SERIAL_BITS | SOLAR_OS_SERIAL_PARITY | SOLAR_OS_SERIAL_STOP);
    } else {
      if (!solar_os_uart_get_bus_status(options->port, &previous)) err = ESP_ERR_NOT_FOUND;
      if (err == ESP_OK) {
        err = solar_os_bus_acquire(options->port, SOLAR_OS_BUS_PROTOCOL_UART, FLASH_OWNER);
        uart_acquired = err == ESP_OK;
      }
      if (err == ESP_OK) err = solar_os_uart_bus_set_baud_rate_owned(options->port, 115200U, FLASH_OWNER);
    }
  }

  bool gpio_claimed = false;
  if (err == ESP_OK) {
    err = flash_claim_gpio(&port);
    gpio_claimed = err == ESP_OK;
  }

  esp_loader_t loader;
  bool loader_initialized = false;
  if (err == ESP_OK) {
    err = flash_loader_error(esp_loader_init_serial(&loader, &port.base));
    loader_initialized = err == ESP_OK;
  }
  esp_loader_connect_args_t connect = ESP_LOADER_CONNECT_DEFAULT();
  connect.sync_timeout = 250U;
  connect.trials = 12;
  if (err == ESP_OK) {
    err = flash_loader_error(esp_loader_connect_with_stub(&loader, &connect));
    if (port.io_error != ESP_OK) err = port.io_error;
  }
  if (port.boot_pin >= 0)
    (void)solar_os_gpio_write(port.boot_pin, true);
  if (err == ESP_OK) {
    flash_report(progress, user, SOLAR_OS_FLASH_PROGRESS_IDENTIFYING, 0U, 0U,
                 false);
    const char *target = flash_target_name(esp_loader_get_target(&loader));
    if (strcmp(target, artifact->chip) != 0)
      err = ESP_ERR_NOT_SUPPORTED;
  }
  esp_loader_target_security_info_t security = {0};
  if (err == ESP_OK) {
    err = flash_loader_error(esp_loader_get_security_info(&loader, &security));
  }
  if (err == ESP_OK &&
      (security.secure_download_mode_enabled ||
       security.flash_encryption_enabled || security.secure_boot_enabled)) {
    err = ESP_ERR_NOT_ALLOWED;
  }
  uint32_t flash_size = 0U;
  if (err == ESP_OK)
    err =
        flash_loader_error(esp_loader_flash_detect_size(&loader, &flash_size));
  if (err == ESP_OK && flash_size < artifact->flash_size)
    err = ESP_ERR_INVALID_SIZE;
  const uint32_t baud_rate =
      options->baud_rate != 0U ? options->baud_rate : 460800U;
  if (err == ESP_OK && baud_rate != 115200U) {
    err = flash_loader_error(
        esp_loader_change_transmission_rate(&loader, baud_rate));
  }
  if (err == ESP_OK) {
    flash_report(progress, user, SOLAR_OS_FLASH_PROGRESS_ERASING, 0U, 0U,
                 false);
    err = flash_loader_error(esp_loader_flash_erase(&loader));
  }
  esp_loader_flash_cfg_t config = {
      .offset = 0U,
      .image_size = artifact->factory_size,
      .block_size = FLASH_LOADER_BLOCK,
      .skip_verify = false,
  };
  if (err == ESP_OK)
    err = flash_loader_error(esp_loader_flash_start(&loader, &config));
  uint32_t flashed = 0U;
  while (err == ESP_OK && flashed < artifact->factory_size) {
    const size_t wanted = artifact->factory_size - flashed > FLASH_LOADER_BLOCK
                              ? FLASH_LOADER_BLOCK
                              : artifact->factory_size - flashed;
    const size_t count = fread(buffer, 1U, wanted, factory);
    if (count != wanted) {
      err = ESP_FAIL;
      break;
    }
    err = flash_loader_error(
        esp_loader_flash_write(&loader, &config, buffer, (uint32_t)count));
    if (err == ESP_OK) {
      flashed += (uint32_t)count;
      flash_report(progress, user, SOLAR_OS_FLASH_PROGRESS_WRITING, flashed,
                   artifact->factory_size, true);
    }
  }
  if (err == ESP_OK) {
    flash_report(progress, user, SOLAR_OS_FLASH_PROGRESS_TARGET_VERIFY,
                 artifact->factory_size, artifact->factory_size, true);
    err = flash_loader_error(esp_loader_flash_finish(&loader, &config));
  }
  if (err == ESP_OK && (port.usb_reset || options->reset_pin >= 0)) {
    esp_loader_reset_target(&loader);
    if (port.io_error != ESP_OK) err = port.io_error;
  }
  if (loader_initialized)
    esp_loader_deinit(&loader);
  if (gpio_claimed)
    flash_release_gpio(&port);
  if (serial_acquired) {
    if (serial_saved) (void)solar_os_port_serial_configure(&port.serial, &previous_serial,
        SOLAR_OS_SERIAL_BAUD | SOLAR_OS_SERIAL_BITS | SOLAR_OS_SERIAL_PARITY | SOLAR_OS_SERIAL_STOP);
    if (port.usb_reset) {
      const solar_os_port_serial_config_t inactive = {0};
      (void)solar_os_port_serial_configure(&port.serial, &inactive, SOLAR_OS_SERIAL_DTR | SOLAR_OS_SERIAL_RTS);
    }
    (void)solar_os_port_release(&port.serial);
  }
  if (uart_acquired) {
    (void)solar_os_uart_bus_set_baud_rate_owned(
        options->port, previous.baud_rate, FLASH_OWNER);
    (void)solar_os_bus_release(options->port, SOLAR_OS_BUS_PROTOCOL_UART,
                               FLASH_OWNER);
  }
  if (factory != NULL)
    fclose(factory);
  solar_os_memory_free(buffer);
  if (err == ESP_OK) {
    flash_report(progress, user, SOLAR_OS_FLASH_PROGRESS_DONE,
                 artifact->factory_size, artifact->factory_size, true);
  }
  return err;
}
