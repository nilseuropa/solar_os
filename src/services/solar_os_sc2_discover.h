#pragma once
/*
 * SC2 relay discovery: `messages add sc2 <name> <domain>`.
 *
 * Fetches https://<domain>/.well-known/solaros-relay with the ESP-IDF CA
 * bundle and host name verification, nothing else: no http://, no
 * redirects, no unverified or pinned-on-first-use TLS. The signed
 * descriptor it carries (protocol/sc2/include/sc2_relaykey.h) must verify
 * against the relay key it names; that key is then pinned in the profile
 * and checked at every login. Transient memory: one 1.6 KiB body buffer
 * (the text is decoded in place) plus the parsed descriptor, both freed
 * before returning; the TLS session itself is esp_http_client's.
 */
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"
#include "sc2_relaykey.h"

#define SOLAR_OS_SC2_DISCOVER_BODY_MAX 1600u

/* Fetch and verify. expect_key (32 bytes, may be NULL) must equal the
 * descriptor's key. On failure err gets a one-line reason. */
esp_err_t solar_os_sc2_discover(const char *domain, const uint8_t *expect_key, sc2_desc_t *out, char *err,
                                size_t err_cap);

/* Domain names accepted for discovery: letters, digits, '-' and '.',
 * 1..63 characters, at least one dot. */
bool solar_os_sc2_discover_domain_valid(const char *domain);
