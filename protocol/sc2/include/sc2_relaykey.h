/*
 * SC2 portable core: the relay's long-term Ed25519 key outside moderation.
 *
 *   relay descriptor  /.well-known/solaros-relay carries "descriptor" (base64
 *                     of a short signed text, below) and "descriptor_sig".
 *                     The signature is checked against the key the text
 *                     carries; the HTTPS fetch (CA chain + host name) anchors
 *                     that key, and the device pins it.
 *   welcome proof     hello may carry a 16-byte "nonce"; welcome then carries
 *                     "relay_sig", so a device with a pinned key knows it
 *                     talks to the key holder, not just to a valid TLS peer.
 *   key rotation      welcome may carry "rotation": {"from": old key,
 *                     "sig": old key's signature over the new key}. A device
 *                     whose pin is "from" accepts the new key; anything else
 *                     is a hard mismatch. One step only.
 *
 * Signed bytes (domain separated from the 122-byte room.mod record):
 *   descriptor  "SC2-DESC\x01" || text
 *   welcome     "SC2-WELC\x01" || u8 len || relay_id || nonce(16) || challenge(32)
 *   rotation    "SC2-ROTK\x01" || u8 len || relay_id || old key(32) || new key(32)
 *
 * Descriptor text: UTF-8 lines "<name> <value>\n", at most 1024 bytes:
 *   sc2-relay 1                    first line, required
 *   id <relay_id>                  required
 *   key <base64 32>                required: the relay key
 *   host <dns name>                optional: else the domain fetched
 *   port <1..65535>                required
 *   tls 1|0                        required
 *   pin sha256/<base64 32>         optional SPKI pin of the TLS key
 *   fallback <url> <base64 32>     0..3: a fallback relay URL and its key
 *   ts <unix seconds>              optional
 * Unknown names are ignored, so later versions can add lines.
 * SPDX-License-Identifier: MIT
 */
#ifndef SC2_RELAYKEY_H
#define SC2_RELAYKEY_H

#include <stdbool.h>

#include "sc2_common.h"
#include "sc2_identity.h"

#ifdef __cplusplus
extern "C" {
#endif

#define SC2_HELLO_NONCE_LEN 16u
#define SC2_DESC_TEXT_MAX 1024u
#define SC2_DESC_HOST_MAX 63u
#define SC2_DESC_URL_MAX 127u
#define SC2_DESC_PIN_MAX 71u
#define SC2_DESC_FALLBACK_MAX 3u

typedef struct {
    char url[SC2_DESC_URL_MAX + 1];
    uint8_t key[SC2_KEY_LEN];
} sc2_desc_fallback_t;

typedef struct {
    char relay_id[SC2_RELAY_ID_MAX + 1];
    uint8_t key[SC2_KEY_LEN];
    char host[SC2_DESC_HOST_MAX + 1]; /* empty: the fetched domain */
    uint16_t port;
    bool tls;
    char pin[SC2_DESC_PIN_MAX + 1];   /* empty: none */
    uint8_t fallback_count;
    sc2_desc_fallback_t fallback[SC2_DESC_FALLBACK_MAX];
    uint64_t ts;
} sc2_desc_t;

/* Parse descriptor text (not NUL-terminated). SC2_ERR_FORMAT on anything
 * malformed or missing; values are not interpreted beyond syntax (URLs
 * are left to the caller). */
int sc2_desc_parse(const char *text, size_t len, sc2_desc_t *out);

/* Verify sig over the descriptor text with key. */
int sc2_desc_verify(const uint8_t sig[SC2_SIG_LEN], const char *text, size_t len, const uint8_t key[SC2_KEY_LEN]);

/* From a /.well-known/solaros-relay body (possibly cut short after the two
 * descriptor fields, which the relay sends first): find "descriptor" and
 * "descriptor_sig", decode the text in place at the start of body (body is
 * overwritten, no second buffer), parse it and verify it against the key
 * it carries. SC2_ERR_NOT_FOUND when the body has no descriptor,
 * SC2_ERR_AUTH on a bad signature. */
int sc2_wellknown_parse(char *body, size_t len, sc2_desc_t *out);
int sc2_welcome_verify(const uint8_t sig[SC2_SIG_LEN], const uint8_t key[SC2_KEY_LEN], const char *relay_id,
                       const uint8_t nonce[SC2_HELLO_NONCE_LEN], const uint8_t challenge[SC2_AUTH_CHALLENGE_LEN]);
int sc2_rotation_verify(const uint8_t sig[SC2_SIG_LEN], const uint8_t old_key[SC2_KEY_LEN], const char *relay_id,
                        const uint8_t new_key[SC2_KEY_LEN]);

/* Signing side (relays, tests). seed is the relay's Ed25519 seed. */
int sc2_desc_sign(uint8_t sig[SC2_SIG_LEN], const char *text, size_t len, const uint8_t seed[SC2_KEY_LEN]);
int sc2_welcome_sign(uint8_t sig[SC2_SIG_LEN], const uint8_t seed[SC2_KEY_LEN], const char *relay_id,
                     const uint8_t nonce[SC2_HELLO_NONCE_LEN], const uint8_t challenge[SC2_AUTH_CHALLENGE_LEN]);
int sc2_rotation_sign(uint8_t sig[SC2_SIG_LEN], const uint8_t old_seed[SC2_KEY_LEN], const char *relay_id,
                      const uint8_t new_key[SC2_KEY_LEN]);

#ifdef __cplusplus
}
#endif

#endif
