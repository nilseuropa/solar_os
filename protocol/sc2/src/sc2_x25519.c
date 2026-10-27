/*
 * SC2 portable core: X25519 on the WireGuard reference implementation
 * (components/wireguard_lwip/src/crypto/refc/x25519.c, from STROBE, MIT).
 * Adds RFC 7748 top-bit masking of the peer value and a constant-time
 * all-zero output check.
 * SPDX-License-Identifier: MIT
 */
#include "sc2_crypto.h"

#include <string.h>

#include "crypto/refc/x25519.h"

int sc2_x25519_public(uint8_t pub[SC2_KEY_LEN], const uint8_t priv[SC2_KEY_LEN])
{
    if (pub == NULL || priv == NULL) {
        return SC2_ERR_ARG;
    }
    (void)x25519_base(pub, priv, 1);
    if (sc2_ct_is_zero(pub, SC2_KEY_LEN)) {
        return SC2_ERR_KEY; /* unreachable for clamped scalars; kept as a guard */
    }
    return SC2_OK;
}

int sc2_x25519_generate(uint8_t priv[SC2_KEY_LEN], uint8_t pub[SC2_KEY_LEN], const sc2_rng_t *rng)
{
    if (priv == NULL || pub == NULL) {
        return SC2_ERR_ARG;
    }
    int rc = sc2_random(rng, priv, SC2_KEY_LEN);
    if (rc != SC2_OK) {
        return rc;
    }
    return sc2_x25519_public(pub, priv);
}

int sc2_x25519(uint8_t out[SC2_KEY_LEN], const uint8_t priv[SC2_KEY_LEN], const uint8_t peer[SC2_KEY_LEN])
{
    uint8_t u[SC2_KEY_LEN];
    if (out == NULL || priv == NULL || peer == NULL) {
        return SC2_ERR_ARG;
    }
    memcpy(u, peer, sizeof(u));
    u[31] &= 0x7f;
    (void)x25519(out, priv, u, 1);
    sc2_memzero(u, sizeof(u));
    if (sc2_ct_is_zero(out, SC2_KEY_LEN)) {
        sc2_memzero(out, SC2_KEY_LEN);
        return SC2_ERR_KEY;
    }
    return SC2_OK;
}
