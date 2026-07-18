#include "solar_os_ssh_keys.h"

#include <errno.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "mbedtls/base64.h"
#include "mbedtls/pk.h"
#include "mbedtls/rsa.h"
#include "solar_os_crypto.h"
#include "solar_os_identity.h"
#include "solar_os_memory.h"
#include "solar_os_storage.h"

#define SSH_KEYS_DIR ".ssh"
#define SSH_KEYS_PRIVATE "id_rsa"
#define SSH_KEYS_PUBLIC "id_rsa.pub"
#define SSH_KEYS_EXPONENT 65537
#define SSH_KEYS_PRIVATE_PEM_MAX 8192
#define SSH_KEYS_PUBLIC_BLOB_MAX 768
#define SSH_KEYS_PUBLIC_B64_MAX 1024
#define SSH_KEYS_PUBLIC_LINE_MAX (SSH_KEYS_PUBLIC_B64_MAX + SOLAR_OS_IDENTITY_USER_MAX + SOLAR_OS_IDENTITY_HOSTNAME_MAX + 16)

typedef struct {
    uint8_t blob[SSH_KEYS_PUBLIC_BLOB_MAX];
    uint8_t b64[SSH_KEYS_PUBLIC_B64_MAX];
    char comment[SOLAR_OS_IDENTITY_USER_MAX + SOLAR_OS_IDENTITY_HOSTNAME_MAX + 2];
    char line[SSH_KEYS_PUBLIC_LINE_MAX];
    uint8_t mpint_tmp[512];
} ssh_keys_public_write_work_t;

static void *ssh_keys_calloc(size_t size)
{
    return solar_os_memory_calloc(1,
                                  size,
                                  SOLAR_OS_MEMORY_TRANSIENT,
                                  "ssh.keys");
}

static bool file_exists(const char *path, uint32_t *size)
{
    struct stat st;
    if (path == NULL || stat(path, &st) != 0 || !S_ISREG(st.st_mode)) {
        if (size != NULL) {
            *size = 0;
        }
        return false;
    }

    if (size != NULL) {
        *size = st.st_size > 0 ? (uint32_t)st.st_size : 0;
    }
    return true;
}

static esp_err_t ssh_keys_dir(char *path, size_t path_len)
{
    if (path == NULL || path_len == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!solar_os_storage_is_mounted()) {
        return ESP_ERR_INVALID_STATE;
    }

    return solar_os_storage_default_path(SSH_KEYS_DIR, path, path_len);
}

static esp_err_t ensure_ssh_keys_dir(void)
{
    char dir[160];
    esp_err_t ret = ssh_keys_dir(dir, sizeof(dir));
    if (ret != ESP_OK) {
        return ret;
    }

    struct stat st;
    if (stat(dir, &st) == 0) {
        return S_ISDIR(st.st_mode) ? ESP_OK : ESP_ERR_INVALID_STATE;
    }

    if (mkdir(dir, 0777) == 0 || errno == EEXIST) {
        return ESP_OK;
    }
    return ESP_FAIL;
}

static esp_err_t ssh_key_path(char *path, size_t path_len, const char *name)
{
    char dir[160];
    esp_err_t ret = ssh_keys_dir(dir, sizeof(dir));
    if (ret != ESP_OK) {
        return ret;
    }

    const int written = snprintf(path, path_len, "%s/%s", dir, name);
    if (written < 0 || (size_t)written >= path_len) {
        return ESP_ERR_INVALID_SIZE;
    }
    return ESP_OK;
}

esp_err_t solar_os_ssh_keys_default_paths(char *private_key_path,
                                          size_t private_key_path_len,
                                          char *public_key_path,
                                          size_t public_key_path_len)
{
    esp_err_t ret = ESP_OK;
    if (private_key_path != NULL && private_key_path_len > 0) {
        ret = ssh_key_path(private_key_path, private_key_path_len, SSH_KEYS_PRIVATE);
        if (ret != ESP_OK) {
            return ret;
        }
    }
    if (public_key_path != NULL && public_key_path_len > 0) {
        ret = ssh_key_path(public_key_path, public_key_path_len, SSH_KEYS_PUBLIC);
    }
    return ret;
}

bool solar_os_ssh_keys_default_exists(void)
{
    char private_key_path[160];
    char public_key_path[160];
    if (solar_os_ssh_keys_default_paths(private_key_path,
                                        sizeof(private_key_path),
                                        public_key_path,
                                        sizeof(public_key_path)) != ESP_OK) {
        return false;
    }

    return file_exists(private_key_path, NULL) && file_exists(public_key_path, NULL);
}

esp_err_t solar_os_ssh_keys_get_status(solar_os_ssh_key_status_t *status)
{
    if (status == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    memset(status, 0, sizeof(*status));
    esp_err_t ret = solar_os_ssh_keys_default_paths(status->private_key_path,
                                                   sizeof(status->private_key_path),
                                                   status->public_key_path,
                                                   sizeof(status->public_key_path));
    if (ret != ESP_OK) {
        return ret;
    }

    status->private_key_exists =
        file_exists(status->private_key_path, &status->private_key_size);
    status->public_key_exists =
        file_exists(status->public_key_path, &status->public_key_size);
    return ESP_OK;
}

static bool put_u32(uint8_t *buffer, size_t buffer_len, size_t *offset, uint32_t value)
{
    if (buffer == NULL || offset == NULL || *offset + 4 > buffer_len) {
        return false;
    }

    buffer[(*offset)++] = (uint8_t)(value >> 24);
    buffer[(*offset)++] = (uint8_t)(value >> 16);
    buffer[(*offset)++] = (uint8_t)(value >> 8);
    buffer[(*offset)++] = (uint8_t)value;
    return true;
}

static bool put_bytes(uint8_t *buffer,
                      size_t buffer_len,
                      size_t *offset,
                      const uint8_t *data,
                      size_t data_len)
{
    if (buffer == NULL || offset == NULL || data == NULL || *offset + data_len > buffer_len) {
        return false;
    }

    memcpy(&buffer[*offset], data, data_len);
    *offset += data_len;
    return true;
}

static bool put_ssh_string(uint8_t *buffer,
                           size_t buffer_len,
                           size_t *offset,
                           const char *value)
{
    const size_t value_len = strlen(value);
    return put_u32(buffer, buffer_len, offset, (uint32_t)value_len) &&
        put_bytes(buffer, buffer_len, offset, (const uint8_t *)value, value_len);
}

static bool put_ssh_mpint(uint8_t *buffer,
                          size_t buffer_len,
                          size_t *offset,
                          const mbedtls_mpi *value,
                          uint8_t *tmp,
                          size_t tmp_len)
{
    const size_t value_len = mbedtls_mpi_size(value);
    if (tmp == NULL || value_len == 0 || value_len > tmp_len) {
        return false;
    }
    if (mbedtls_mpi_write_binary(value, tmp, value_len) != 0) {
        return false;
    }

    const bool needs_zero_prefix = (tmp[0] & 0x80U) != 0;
    const size_t encoded_len = value_len + (needs_zero_prefix ? 1U : 0U);
    if (!put_u32(buffer, buffer_len, offset, (uint32_t)encoded_len)) {
        return false;
    }
    if (needs_zero_prefix) {
        const uint8_t zero = 0;
        if (!put_bytes(buffer, buffer_len, offset, &zero, 1)) {
            return false;
        }
    }
    return put_bytes(buffer, buffer_len, offset, tmp, value_len);
}

static esp_err_t write_file(const char *path, const void *data, size_t len)
{
    FILE *file = fopen(path, "w");
    if (file == NULL) {
        return ESP_FAIL;
    }

    const size_t written = fwrite(data, 1, len, file);
    const int close_ret = fclose(file);
    return written == len && close_ret == 0 ? ESP_OK : ESP_FAIL;
}

static esp_err_t write_public_key_file(const char *path, const mbedtls_pk_context *pk)
{
    ssh_keys_public_write_work_t *work = ssh_keys_calloc(sizeof(*work));
    if (work == NULL) {
        return ESP_ERR_NO_MEM;
    }

    mbedtls_mpi n;
    mbedtls_mpi e;
    mbedtls_mpi_init(&n);
    mbedtls_mpi_init(&e);

    const mbedtls_rsa_context *rsa = mbedtls_pk_rsa(*pk);
    int rc = mbedtls_rsa_export(rsa, &n, NULL, NULL, NULL, &e);
    if (rc != 0) {
        mbedtls_mpi_free(&n);
        mbedtls_mpi_free(&e);
        solar_os_memory_free(work);
        return ESP_FAIL;
    }

    size_t blob_len = 0;
    bool ok = put_ssh_string(work->blob, sizeof(work->blob), &blob_len, "ssh-rsa") &&
        put_ssh_mpint(work->blob,
                      sizeof(work->blob),
                      &blob_len,
                      &e,
                      work->mpint_tmp,
                      sizeof(work->mpint_tmp)) &&
        put_ssh_mpint(work->blob,
                      sizeof(work->blob),
                      &blob_len,
                      &n,
                      work->mpint_tmp,
                      sizeof(work->mpint_tmp));

    mbedtls_mpi_free(&n);
    mbedtls_mpi_free(&e);
    if (!ok) {
        solar_os_memory_free(work);
        return ESP_ERR_INVALID_SIZE;
    }

    size_t b64_len = 0;
    rc = mbedtls_base64_encode(work->b64,
                               sizeof(work->b64),
                               &b64_len,
                               work->blob,
                               blob_len);
    if (rc != 0 || b64_len + 20 > SSH_KEYS_PUBLIC_B64_MAX) {
        solar_os_memory_free(work);
        return ESP_FAIL;
    }

    solar_os_identity_format(work->comment, sizeof(work->comment));
    const int written = snprintf(work->line,
                                 sizeof(work->line),
                                 "ssh-rsa %.*s %s\n",
                                 (int)b64_len,
                                 (const char *)work->b64,
                                 work->comment);
    if (written < 0 || (size_t)written >= sizeof(work->line)) {
        solar_os_memory_free(work);
        return ESP_ERR_INVALID_SIZE;
    }

    const esp_err_t ret = write_file(path, work->line, (size_t)written);
    solar_os_memory_free(work);
    return ret;
}

esp_err_t solar_os_ssh_keys_generate_rsa(uint32_t bits, bool overwrite)
{
    if (bits == 0) {
        bits = SOLAR_OS_SSH_KEY_DEFAULT_BITS;
    }
    if (bits < SOLAR_OS_SSH_KEY_MIN_BITS || bits > SOLAR_OS_SSH_KEY_MAX_BITS ||
        (bits % 1024U) != 0) {
        return ESP_ERR_INVALID_ARG;
    }

    esp_err_t ret = ensure_ssh_keys_dir();
    if (ret != ESP_OK) {
        return ret;
    }

    char private_key_path[160];
    char public_key_path[160];
    ret = solar_os_ssh_keys_default_paths(private_key_path,
                                          sizeof(private_key_path),
                                          public_key_path,
                                          sizeof(public_key_path));
    if (ret != ESP_OK) {
        return ret;
    }

    if (!overwrite &&
        (file_exists(private_key_path, NULL) || file_exists(public_key_path, NULL))) {
        return ESP_ERR_INVALID_STATE;
    }

    solar_os_crypto_rng_t rng = {0};
    mbedtls_pk_context pk;
    mbedtls_pk_init(&pk);

    ret = solar_os_crypto_rng_init(&rng, "SolarOS sshkey");
    if (ret != ESP_OK) {
        goto cleanup;
    }

    int rc = mbedtls_pk_setup(&pk, mbedtls_pk_info_from_type(MBEDTLS_PK_RSA));
    if (rc != 0) {
        ret = ESP_FAIL;
        goto cleanup;
    }

    rc = mbedtls_rsa_gen_key(mbedtls_pk_rsa(pk),
                             solar_os_crypto_rng_mbedtls,
                             &rng,
                             bits,
                             SSH_KEYS_EXPONENT);
    if (rc != 0) {
        ret = ESP_FAIL;
        goto cleanup;
    }

    unsigned char *private_pem = ssh_keys_calloc(SSH_KEYS_PRIVATE_PEM_MAX);
    if (private_pem == NULL) {
        ret = ESP_ERR_NO_MEM;
        goto cleanup;
    }

    rc = mbedtls_pk_write_key_pem(&pk, private_pem, SSH_KEYS_PRIVATE_PEM_MAX);
    if (rc != 0) {
        solar_os_memory_free(private_pem);
        ret = ESP_FAIL;
        goto cleanup;
    }

    ret = write_file(private_key_path, private_pem, strlen((const char *)private_pem));
    solar_os_memory_free(private_pem);
    if (ret != ESP_OK) {
        (void)unlink(private_key_path);
        (void)unlink(public_key_path);
        goto cleanup;
    }

    ret = write_public_key_file(public_key_path, &pk);
    if (ret != ESP_OK) {
        (void)unlink(private_key_path);
        (void)unlink(public_key_path);
        goto cleanup;
    }

cleanup:
    mbedtls_pk_free(&pk);
    solar_os_crypto_rng_free(&rng);
    return ret;
}

esp_err_t solar_os_ssh_keys_remove_default(void)
{
    char private_key_path[160];
    char public_key_path[160];
    esp_err_t ret = solar_os_ssh_keys_default_paths(private_key_path,
                                                   sizeof(private_key_path),
                                                   public_key_path,
                                                   sizeof(public_key_path));
    if (ret != ESP_OK) {
        return ret;
    }

    bool removed = false;
    if (unlink(private_key_path) == 0) {
        removed = true;
    } else if (errno != ENOENT) {
        return ESP_FAIL;
    }
    if (unlink(public_key_path) == 0) {
        removed = true;
    } else if (errno != ENOENT) {
        return ESP_FAIL;
    }

    return removed ? ESP_OK : ESP_ERR_NOT_FOUND;
}
