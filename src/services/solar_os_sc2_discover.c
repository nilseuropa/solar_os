/*
 * SC2 relay discovery over HTTPS (see solar_os_sc2_discover.h).
 * SPDX-License-Identifier: MIT
 */
#include "solar_os_sc2_discover.h"

#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include "sdkconfig.h"
#include "esp_crt_bundle.h"
#include "esp_http_client.h"
#include "solar_os_log.h"
#include "solar_os_memory.h"

static const char *TAG = "sc2-discover";

bool solar_os_sc2_discover_domain_valid(const char *domain)
{
    size_t n = domain != NULL ? strlen(domain) : 0;
    if (n == 0 || n > SC2_DESC_HOST_MAX || domain[0] == '.' || domain[0] == '-' || domain[n - 1] == '.' ||
        domain[n - 1] == '-' || strchr(domain, '.') == NULL || strstr(domain, "..") != NULL) {
        return false;
    }
    for (size_t i = 0; i < n; i++) {
        char c = domain[i];
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-' || c == '.')) {
            return false;
        }
    }
    return true;
}

static esp_err_t fail(char *err, size_t cap, esp_err_t code, const char *fmt, const char *arg)
{
    if (err != NULL && cap != 0) {
        snprintf(err, cap, fmt, arg != NULL ? arg : "");
    }
    return code;
}

esp_err_t solar_os_sc2_discover(const char *domain, const uint8_t *expect_key, sc2_desc_t *out, char *err,
                                size_t err_cap)
{
    if (out == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!solar_os_sc2_discover_domain_valid(domain)) {
        return fail(err, err_cap, ESP_ERR_INVALID_ARG, "not a domain name: %s", domain);
    }
#if !CONFIG_MBEDTLS_CERTIFICATE_BUNDLE
    return fail(err, err_cap, ESP_ERR_NOT_SUPPORTED, "%sthis build has no CA bundle; discovery needs one", NULL);
#else
    char url[24 + SC2_DESC_HOST_MAX + 1];
    snprintf(url, sizeof(url), "https://%s/.well-known/solaros-relay", domain);
    char *body = solar_os_memory_alloc(SOLAR_OS_SC2_DISCOVER_BODY_MAX, SOLAR_OS_MEMORY_EXTERNAL_PREFERRED,
                                       "sc2.discover");
    if (body == NULL) {
        return fail(err, err_cap, ESP_ERR_NO_MEM, "%sout of memory", NULL);
    }
    esp_http_client_config_t hc = {
        .url = url,
        .transport_type = HTTP_TRANSPORT_OVER_SSL,
        .crt_bundle_attach = esp_crt_bundle_attach, /* CA chain; host name checked by esp-tls */
        .skip_cert_common_name_check = false,
        .disable_auto_redirect = true,
        .max_redirection_count = 0,
        .timeout_ms = 10000,
        .buffer_size = 1024,
        .buffer_size_tx = 512,
        .user_agent = "solaros-sc2-discover/1",
    };
    esp_err_t result = ESP_OK;
    size_t len = 0;
    esp_http_client_handle_t client = esp_http_client_init(&hc);
    if (client == NULL) {
        result = fail(err, err_cap, ESP_ERR_NO_MEM, "%sHTTPS client init failed", NULL);
    } else if (esp_http_client_open(client, 0) != ESP_OK) {
        result = fail(err, err_cap, ESP_ERR_INVALID_RESPONSE,
                      "HTTPS to %s failed (unreachable, or the certificate does not verify for this name)", domain);
    } else {
        (void)esp_http_client_fetch_headers(client);
        int status = esp_http_client_get_status_code(client);
        if (status != 200) {
            char code[12];
            snprintf(code, sizeof(code), "%d", status);
            result = fail(err, err_cap, ESP_ERR_NOT_FOUND,
                          "no SC2 relay descriptor there (HTTP %s; redirects are not followed)", code);
        } else {
            /* the descriptor fields come first; the rest of the body is not needed */
            while (len < SOLAR_OS_SC2_DISCOVER_BODY_MAX) {
                int n = esp_http_client_read(client, body + len, (int)(SOLAR_OS_SC2_DISCOVER_BODY_MAX - len));
                if (n <= 0) {
                    break;
                }
                len += (size_t)n;
            }
        }
        esp_http_client_close(client);
    }
    if (client != NULL) {
        esp_http_client_cleanup(client);
    }
    if (result == ESP_OK) {
        int rc = sc2_wellknown_parse(body, len, out);
        if (rc == SC2_ERR_NOT_FOUND) {
            result = fail(err, err_cap, ESP_ERR_NOT_FOUND, "%sthe relay does not publish a signed descriptor", NULL);
        } else if (rc == SC2_ERR_AUTH) {
            result = fail(err, err_cap, ESP_ERR_INVALID_RESPONSE, "%sdescriptor signature does not verify", NULL);
        } else if (rc != SC2_OK) {
            result = fail(err, err_cap, ESP_ERR_INVALID_RESPONSE, "%smalformed relay descriptor", NULL);
        } else if (expect_key != NULL && memcmp(expect_key, out->key, SC2_KEY_LEN) != 0) {
            result = fail(err, err_cap, ESP_ERR_INVALID_RESPONSE, "%srelay key differs from key=", NULL);
        } else if (!out->tls) {
            result = fail(err, err_cap, ESP_ERR_NOT_SUPPORTED, "%sthe relay offers no TLS endpoint", NULL);
        }
    }
    memset(body, 0, SOLAR_OS_SC2_DISCOVER_BODY_MAX);
    solar_os_memory_free(body);
    if (result != ESP_OK) {
        memset(out, 0, sizeof(*out));
        SOLAR_OS_LOGW(TAG, "%s: %s", domain, err != NULL ? err : "failed");
    }
    return result;
#endif
}
