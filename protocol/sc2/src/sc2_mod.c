/*
 * SC2 portable core: relay-signed moderation events (see sc2_mod.h).
 * SPDX-License-Identifier: MIT
 */
#include "sc2_mod.h"

#include <string.h>

#include "sc2_crypto.h"

static const uint8_t MOD_LABEL[8] = {'S', 'C', '2', '-', 'M', 'O', 'D', 0x01};

enum { OFF_SUITE = 8, OFF_ACTION = 9, OFF_ROOM = 10, OFF_SEQ = 74, OFF_TS = 82, OFF_REF = 90, OFF_DEV = 98,
       OFF_UNTIL = 114 };

static int all_zero(const uint8_t *p, size_t n)
{
    uint8_t acc = 0;
    for (size_t i = 0; i < n; i++) {
        acc |= p[i];
    }
    return acc == 0;
}

const char *sc2_mod_action_name(uint8_t action)
{
    switch (action) {
    case SC2_MOD_REMOVE:
        return "remove";
    case SC2_MOD_BAN:
        return "ban";
    case SC2_MOD_UNBAN:
        return "unban";
    default:
        return NULL;
    }
}

uint8_t sc2_mod_action_parse(const char *name, size_t len)
{
    if (name == NULL) {
        return 0;
    }
    for (uint8_t a = SC2_MOD_REMOVE; a <= SC2_MOD_UNBAN; a++) {
        const char *n = sc2_mod_action_name(a);
        if (strlen(n) == len && memcmp(n, name, len) == 0) {
            return a;
        }
    }
    return 0;
}

static int fields_ok(const sc2_mod_t *m)
{
    if (sc2_mod_action_name(m->action) == NULL) {
        return 0;
    }
    if (m->room_len == 0 || m->room_len > SC2_MOD_ROOM_MAX || memchr(m->room, '\0', m->room_len) != NULL) {
        return 0;
    }
    if (all_zero(m->device, sizeof(m->device))) {
        return 0;
    }
    if (m->action == SC2_MOD_REMOVE) {
        return !all_zero(m->ref, sizeof(m->ref)) && m->until == 0;
    }
    if (!all_zero(m->ref, sizeof(m->ref))) {
        return 0;
    }
    return m->action == SC2_MOD_BAN || m->until == 0;
}

int sc2_mod_encode(uint8_t out[SC2_MOD_LEN], const sc2_mod_t *m)
{
    if (out == NULL || m == NULL || !fields_ok(m)) {
        return SC2_ERR_ARG;
    }
    memset(out, 0, SC2_MOD_LEN);
    memcpy(out, MOD_LABEL, sizeof(MOD_LABEL));
    out[OFF_SUITE] = (uint8_t)SC2_SUITE_1;
    out[OFF_ACTION] = m->action;
    memcpy(out + OFF_ROOM, m->room, m->room_len);
    sc2_store_be64(out + OFF_SEQ, m->seq);
    sc2_store_be64(out + OFF_TS, m->ts);
    memcpy(out + OFF_REF, m->ref, SC2_MSG_ID_LEN);
    memcpy(out + OFF_DEV, m->device, SC2_DEVICE_ID_LEN);
    sc2_store_be64(out + OFF_UNTIL, m->until);
    return SC2_OK;
}

int sc2_mod_decode(sc2_mod_t *m, const uint8_t rec[SC2_MOD_LEN])
{
    if (m == NULL || rec == NULL) {
        return SC2_ERR_ARG;
    }
    if (memcmp(rec, MOD_LABEL, sizeof(MOD_LABEL)) != 0 || rec[OFF_SUITE] != SC2_SUITE_1) {
        return SC2_ERR_FORMAT;
    }
    memset(m, 0, sizeof(*m));
    m->action = rec[OFF_ACTION];
    size_t n = 0;
    while (n < SC2_MOD_ROOM_FIELD && rec[OFF_ROOM + n] != 0) {
        n++;
    }
    /* the name is followed only by zero padding */
    if (n == SC2_MOD_ROOM_FIELD || !all_zero(rec + OFF_ROOM + n, SC2_MOD_ROOM_FIELD - n)) {
        return SC2_ERR_FORMAT;
    }
    memcpy(m->room, rec + OFF_ROOM, n);
    m->room_len = n;
    m->seq = sc2_load_be64(rec + OFF_SEQ);
    m->ts = sc2_load_be64(rec + OFF_TS);
    memcpy(m->ref, rec + OFF_REF, SC2_MSG_ID_LEN);
    memcpy(m->device, rec + OFF_DEV, SC2_DEVICE_ID_LEN);
    m->until = sc2_load_be64(rec + OFF_UNTIL);
    return fields_ok(m) ? SC2_OK : SC2_ERR_FORMAT;
}

int sc2_mod_sign(uint8_t sig[SC2_SIG_LEN], const sc2_mod_t *m, const uint8_t relay_seed[SC2_KEY_LEN])
{
    uint8_t rec[SC2_MOD_LEN];
    if (sig == NULL || relay_seed == NULL) {
        return SC2_ERR_ARG;
    }
    int rc = sc2_mod_encode(rec, m);
    return rc == SC2_OK ? sc2_ed25519_sign(sig, rec, sizeof(rec), relay_seed) : rc;
}

int sc2_mod_verify(const uint8_t sig[SC2_SIG_LEN], const sc2_mod_t *m, const uint8_t relay_pub[SC2_KEY_LEN])
{
    uint8_t rec[SC2_MOD_LEN];
    if (sig == NULL || relay_pub == NULL) {
        return SC2_ERR_ARG;
    }
    int rc = sc2_mod_encode(rec, m);
    return rc == SC2_OK ? sc2_ed25519_verify(sig, rec, sizeof(rec), relay_pub) : SC2_ERR_FORMAT;
}
