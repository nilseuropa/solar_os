/*
 * SC2 portable core: relay-protocol frames.
 * SPDX-License-Identifier: MIT
 */
#include "sc2_frame.h"

#include <string.h>

#include "sc2_codec.h"
#include "sc2_crypto.h"
#include "sc2_identity.h"

static const uint8_t LABEL_PUB[] = {'S', 'C', '2', '-', 'P', 'U', 'B', 0x01};

/* ---------------- line assembler ---------------- */

void sc2_lines_init(sc2_lines_t *l)
{
    if (l) {
        l->len = 0;
        l->discarding = false;
        l->buf[0] = '\0';
    }
}

int sc2_lines_feed(sc2_lines_t *l, const uint8_t *data, size_t len, size_t *consumed, const char **line,
                   size_t *line_len)
{
    if (l == NULL || consumed == NULL || line == NULL || line_len == NULL || (data == NULL && len != 0)) {
        return SC2_ERR_ARG;
    }
    size_t i = 0;
    while (i < len) {
        uint8_t c = data[i++];
        if (c == '\n') {
            size_t n = l->len;
            bool was_discarding = l->discarding;
            l->len = 0;
            l->discarding = false;
            if (was_discarding) {
                continue;
            }
            if (n && l->buf[n - 1] == '\r') {
                n--;
            }
            if (n > SC2_LINE_MAX) {
                *consumed = i;
                return SC2_ERR_BOUNDS;
            }
            if (n == 0) {
                continue;
            }
            l->buf[n] = '\0';
            *line = l->buf;
            *line_len = n;
            *consumed = i;
            return 1;
        }
        if (l->discarding) {
            continue;
        }
        if (l->len >= SC2_LINE_MAX + 1u) {
            /* over the limit even allowing for a trailing '\r' */
            l->discarding = true;
            l->len = 0;
            *consumed = i;
            return SC2_ERR_BOUNDS;
        }
        l->buf[l->len++] = (char)c;
    }
    *consumed = i;
    return 0;
}

/* ---------------- helpers ---------------- */

static int get_obj(const sc2_json_val_t *obj, const char *key, sc2_json_val_t *out)
{
    int rc = sc2_json_get(obj, key, out);
    if (rc == SC2_OK && out->type != SC2_JSON_OBJECT) {
        rc = SC2_ERR_FORMAT;
    }
    return rc;
}

static int get_u32(const sc2_json_val_t *obj, const char *key, uint32_t max, uint32_t *out)
{
    uint64_t v;
    int rc = sc2_json_get_u64(obj, key, max, &v);
    if (rc == SC2_OK) {
        *out = (uint32_t)v;
    }
    return rc;
}

static int get_suite(const sc2_json_val_t *obj, const char *key, uint32_t *suite)
{
    int rc = get_u32(obj, key, 255u, suite);
    if (rc == SC2_OK && *suite != SC2_SUITE_1) {
        rc = SC2_ERR_FORMAT; /* unsupported suite */
    }
    return rc;
}

static int expect_type(const sc2_json_val_t *root, const char *type)
{
    sc2_json_val_t t;
    if (root->type != SC2_JSON_OBJECT) {
        return SC2_ERR_FORMAT;
    }
    int rc = sc2_json_get(root, "type", &t);
    if (rc != SC2_OK) {
        return rc == SC2_ERR_NOT_FOUND ? SC2_ERR_FORMAT : rc;
    }
    return sc2_json_string_eq(&t, type) ? SC2_OK : SC2_ERR_STATE;
}

static int get_ct(const sc2_json_val_t *obj, uint8_t *buf, size_t cap, const uint8_t **ct, size_t *ct_len)
{
    sc2_json_val_t v;
    int rc = sc2_json_get(obj, "ct", &v);
    if (rc != SC2_OK) {
        return rc == SC2_ERR_NOT_FOUND ? SC2_ERR_FORMAT : rc;
    }
    size_t n = 0;
    rc = sc2_json_b64(&v, buf, cap, &n);
    if (rc == SC2_OK && n < SC2_TAG_LEN) {
        rc = SC2_ERR_FORMAT;
    }
    if (rc == SC2_OK) {
        *ct = buf;
        *ct_len = n;
    }
    return rc;
}

static int req(int rc)
{
    return rc == SC2_ERR_NOT_FOUND ? SC2_ERR_FORMAT : rc;
}

static void put_rid(sc2_jw_t *w, const char *rid)
{
    if (rid != NULL) {
        size_t n = strlen(rid);
        if (n == 0 || n > SC2_RID_MAX) {
            w->err = w->err ? w->err : SC2_ERR_ARG;
            return;
        }
        sc2_jw_key(w, "rid");
        sc2_jw_string(w, rid, n);
    }
}

/* ---------------- generic ---------------- */

int sc2_frame_peek(const char *line, size_t len, sc2_json_val_t *root, char type[SC2_TYPE_MAX + 1],
                   char rid[SC2_RID_MAX + 1])
{
    if (line == NULL || root == NULL || type == NULL) {
        return SC2_ERR_ARG;
    }
    if (len > SC2_LINE_MAX) {
        return SC2_ERR_BOUNDS;
    }
    int rc = sc2_json_root(line, len, root);
    if (rc != SC2_OK) {
        return rc;
    }
    if (root->type != SC2_JSON_OBJECT) {
        return SC2_ERR_FORMAT;
    }
    rc = req(sc2_json_get_string(root, "type", type, SC2_TYPE_MAX + 1, NULL));
    if (rc != SC2_OK) {
        return rc == SC2_ERR_BOUNDS ? SC2_ERR_FORMAT : rc;
    }
    if (rid) {
        rid[0] = '\0';
        rc = sc2_json_get_string(root, "rid", rid, SC2_RID_MAX + 1, NULL);
        if (rc == SC2_ERR_NOT_FOUND) {
            rc = SC2_OK;
        } else if (rc == SC2_ERR_BOUNDS) {
            rc = SC2_ERR_FORMAT;
        }
    }
    return rc;
}

/* ---------------- handshake ---------------- */

int sc2_frame_hello(char *out, size_t cap, size_t *out_len, const char *client, const char *admit,
                    const uint8_t *nonce)
{
    sc2_jw_t w;
    sc2_jw_init(&w, out, cap);
    sc2_jw_object_begin(&w);
    sc2_jw_key(&w, "type");
    sc2_jw_cstring(&w, "hello");
    sc2_jw_key(&w, "v");
    sc2_jw_u64(&w, 2);
    sc2_jw_key(&w, "proto");
    sc2_jw_cstring(&w, "sc2");
    sc2_jw_key(&w, "suites");
    sc2_jw_array_begin(&w);
    sc2_jw_u64(&w, SC2_SUITE_1);
    sc2_jw_array_end(&w);
    if (client) {
        sc2_jw_key(&w, "client");
        sc2_jw_cstring(&w, client);
    }
    if (admit) {
        sc2_jw_key(&w, "admit");
        sc2_jw_cstring(&w, admit);
    }
    if (nonce) {
        sc2_jw_key(&w, "nonce");
        sc2_jw_b64(&w, nonce, 16u);
    }
    sc2_jw_object_end(&w);
    return sc2_jw_finish(&w, out_len);
}

int sc2_frame_welcome_decode(const char *line, size_t len, sc2_welcome_t *out)
{
    sc2_json_val_t root, relay, v, limits;
    if (line == NULL || out == NULL) {
        return SC2_ERR_ARG;
    }
    memset(out, 0, sizeof(*out));
    int rc = sc2_json_root(line, len, &root);
    if (rc == SC2_OK) {
        rc = expect_type(&root, "welcome");
    }
    if (rc != SC2_OK) {
        return rc;
    }
    uint64_t ver;
    rc = req(sc2_json_get_u64(&root, "v", 255u, &ver));
    if (rc != SC2_OK) {
        return rc;
    }
    if (ver != 2u) {
        return SC2_ERR_STATE;
    }
    rc = req(sc2_json_get(&root, "proto", &v));
    if (rc != SC2_OK) {
        return rc;
    }
    if (!sc2_json_string_eq(&v, "sc2")) {
        return SC2_ERR_STATE;
    }
    rc = req(get_suite(&root, "suite", &out->suite));
    if (rc != SC2_OK) {
        return rc;
    }
    rc = req(get_obj(&root, "relay", &relay));
    if (rc != SC2_OK) {
        return rc;
    }
    size_t id_len = 0;
    rc = req(sc2_json_get_string(&relay, "id", out->relay_id, sizeof(out->relay_id), &id_len));
    if (rc != SC2_OK || id_len == 0 || strlen(out->relay_id) != id_len) {
        return rc != SC2_OK ? (rc == SC2_ERR_BOUNDS ? SC2_ERR_FORMAT : rc) : SC2_ERR_FORMAT;
    }
    rc = sc2_json_get_b64_exact(&relay, "key", out->relay_key, SC2_KEY_LEN);
    if (rc == SC2_OK) {
        out->has_relay_key = true;
    } else if (rc != SC2_ERR_NOT_FOUND) {
        return rc;
    }
    rc = req(sc2_json_get_b64_exact(&root, "challenge", out->challenge, SC2_AUTH_CHALLENGE_LEN));
    if (rc != SC2_OK) {
        return rc;
    }
    rc = sc2_json_get(&root, "legacy_v1", &v);
    if (rc == SC2_OK) {
        rc = sc2_json_bool(&v, &out->legacy_v1);
        if (rc != SC2_OK) {
            return rc;
        }
    } else if (rc != SC2_ERR_NOT_FOUND) {
        return rc;
    }
    rc = sc2_json_get_b64_exact(&root, "relay_sig", out->relay_sig, SC2_SIG_LEN);
    if (rc == SC2_OK) {
        out->has_relay_sig = true;
    } else if (rc != SC2_ERR_NOT_FOUND) {
        return rc;
    }
    sc2_json_val_t rot;
    rc = get_obj(&root, "rotation", &rot);
    if (rc == SC2_OK) {
        rc = req(sc2_json_get_b64_exact(&rot, "from", out->rotation_from, SC2_KEY_LEN));
        if (rc == SC2_OK) {
            rc = req(sc2_json_get_b64_exact(&rot, "sig", out->rotation_sig, SC2_SIG_LEN));
        }
        if (rc != SC2_OK) {
            return rc;
        }
        out->has_rotation = true;
    } else if (rc != SC2_ERR_NOT_FOUND) {
        return rc;
    }
    rc = get_obj(&root, "limits", &limits);
    if (rc == SC2_OK) {
        rc = get_u32(&limits, "line", UINT32_MAX, &out->limit_line);
        if (rc != SC2_OK && rc != SC2_ERR_NOT_FOUND) {
            return rc;
        }
        rc = get_u32(&limits, "msg", UINT32_MAX, &out->limit_msg);
        if (rc != SC2_OK && rc != SC2_ERR_NOT_FOUND) {
            return rc;
        }
    } else if (rc != SC2_ERR_NOT_FOUND) {
        return rc;
    }
    return SC2_OK;
}

int sc2_frame_auth(char *out, size_t cap, size_t *out_len, const char *rid, const uint8_t ik_pub[SC2_KEY_LEN],
                   const uint8_t sig[SC2_SIG_LEN], bool resume)
{
    sc2_jw_t w;
    if (ik_pub == NULL || sig == NULL) {
        return SC2_ERR_ARG;
    }
    sc2_jw_init(&w, out, cap);
    sc2_jw_object_begin(&w);
    sc2_jw_key(&w, "type");
    sc2_jw_cstring(&w, "auth");
    put_rid(&w, rid);
    sc2_jw_key(&w, "ik");
    sc2_jw_b64(&w, ik_pub, SC2_KEY_LEN);
    sc2_jw_key(&w, "sig");
    sc2_jw_b64(&w, sig, SC2_SIG_LEN);
    sc2_jw_key(&w, "resume");
    sc2_jw_bool(&w, resume);
    sc2_jw_object_end(&w);
    return sc2_jw_finish(&w, out_len);
}

/* ---------------- DM ---------------- */

static void write_dm_env(sc2_jw_t *w, const sc2_dm_env_t *env)
{
    sc2_jw_object_begin(w);
    sc2_jw_key(w, "s");
    sc2_jw_u64(w, env->suite);
    sc2_jw_key(w, "from");
    sc2_jw_hex(w, env->from, SC2_DEVICE_ID_LEN);
    if (env->hdr.has_x3dh) {
        sc2_jw_key(w, "x3dh");
        sc2_jw_object_begin(w);
        sc2_jw_key(w, "ek");
        sc2_jw_b64(w, env->hdr.x3dh.ek, SC2_KEY_LEN);
        sc2_jw_key(w, "spk");
        sc2_jw_u64(w, env->hdr.x3dh.spk_id);
        if (env->hdr.x3dh.opk_id != SC2_NO_OPK) {
            sc2_jw_key(w, "opk");
            sc2_jw_u64(w, env->hdr.x3dh.opk_id);
        }
        sc2_jw_object_end(w);
    }
    sc2_jw_key(w, "rk");
    sc2_jw_b64(w, env->hdr.dh, SC2_KEY_LEN);
    sc2_jw_key(w, "pn");
    sc2_jw_u64(w, env->hdr.pn);
    sc2_jw_key(w, "n");
    sc2_jw_u64(w, env->hdr.n);
    sc2_jw_key(w, "nonce");
    sc2_jw_b64(w, env->nonce, SC2_NONCE_LEN);
    sc2_jw_key(w, "ct");
    sc2_jw_b64(w, env->ct, env->ct_len);
    sc2_jw_object_end(w);
}

int sc2_frame_dm_send(char *out, size_t cap, size_t *out_len, const char *rid,
                      const uint8_t to[SC2_DEVICE_ID_LEN], const sc2_dm_env_t *env)
{
    sc2_jw_t w;
    if (to == NULL || env == NULL || (env->ct == NULL && env->ct_len != 0)) {
        return SC2_ERR_ARG;
    }
    if (cap > SC2_LINE_MAX + 1u) {
        cap = SC2_LINE_MAX + 1u;
    }
    sc2_jw_init(&w, out, cap);
    sc2_jw_object_begin(&w);
    sc2_jw_key(&w, "type");
    sc2_jw_cstring(&w, "dm.send");
    put_rid(&w, rid);
    sc2_jw_key(&w, "to");
    sc2_jw_hex(&w, to, SC2_DEVICE_ID_LEN);
    sc2_jw_key(&w, "env");
    write_dm_env(&w, env);
    sc2_jw_object_end(&w);
    return sc2_jw_finish(&w, out_len);
}

int sc2_dm_env_decode(const sc2_json_val_t *env_obj, sc2_dm_env_t *env, uint8_t *ct_buf, size_t ct_cap)
{
    sc2_json_val_t x;
    if (env_obj == NULL || env == NULL || ct_buf == NULL) {
        return SC2_ERR_ARG;
    }
    if (env_obj->type != SC2_JSON_OBJECT) {
        return SC2_ERR_FORMAT;
    }
    memset(env, 0, sizeof(*env));
    env->hdr.x3dh.opk_id = SC2_NO_OPK;
    int rc = req(get_suite(env_obj, "s", &env->suite));
    if (rc == SC2_OK) {
        rc = req(sc2_json_get_hex_exact(env_obj, "from", env->from, SC2_DEVICE_ID_LEN));
    }
    if (rc == SC2_OK) {
        rc = req(sc2_json_get_b64_exact(env_obj, "rk", env->hdr.dh, SC2_KEY_LEN));
    }
    if (rc == SC2_OK) {
        rc = req(get_u32(env_obj, "pn", UINT32_MAX, &env->hdr.pn));
    }
    if (rc == SC2_OK) {
        rc = req(get_u32(env_obj, "n", UINT32_MAX - 1u, &env->hdr.n));
    }
    if (rc == SC2_OK) {
        rc = req(sc2_json_get_b64_exact(env_obj, "nonce", env->nonce, SC2_NONCE_LEN));
    }
    if (rc == SC2_OK) {
        rc = get_obj(env_obj, "x3dh", &x);
        if (rc == SC2_OK) {
            env->hdr.has_x3dh = 1;
            rc = req(sc2_json_get_b64_exact(&x, "ek", env->hdr.x3dh.ek, SC2_KEY_LEN));
            if (rc == SC2_OK) {
                rc = req(get_u32(&x, "spk", UINT32_MAX, &env->hdr.x3dh.spk_id));
            }
            if (rc == SC2_OK) {
                rc = get_u32(&x, "opk", SC2_NO_OPK - 1u, &env->hdr.x3dh.opk_id);
                if (rc == SC2_ERR_NOT_FOUND) {
                    env->hdr.x3dh.opk_id = SC2_NO_OPK;
                    rc = SC2_OK;
                }
            }
        } else if (rc == SC2_ERR_NOT_FOUND) {
            rc = SC2_OK;
        }
    }
    if (rc == SC2_OK) {
        rc = get_ct(env_obj, ct_buf, ct_cap, &env->ct, &env->ct_len);
    }
    return rc;
}

int sc2_frame_dm_msg_decode(const char *line, size_t len, uint64_t *seq, sc2_dm_env_t *env, uint8_t *ct_buf,
                            size_t ct_cap)
{
    sc2_json_val_t root, v, e;
    if (line == NULL || seq == NULL) {
        return SC2_ERR_ARG;
    }
    int rc = sc2_json_root(line, len, &root);
    if (rc == SC2_OK) {
        rc = expect_type(&root, "dm.msg");
    }
    if (rc == SC2_OK) {
        rc = req(sc2_json_get(&root, "seq", &v));
    }
    if (rc == SC2_OK) {
        rc = sc2_json_u64_str(&v, seq);
    }
    if (rc == SC2_OK) {
        rc = req(get_obj(&root, "env", &e));
    }
    if (rc == SC2_OK) {
        rc = sc2_dm_env_decode(&e, env, ct_buf, ct_cap);
    }
    return rc;
}

/* ---------------- room ---------------- */

int sc2_frame_room_send(char *out, size_t cap, size_t *out_len, const char *rid,
                        const uint8_t room[SC2_ROOM_ID_LEN], const sc2_room_env_t *env)
{
    sc2_jw_t w;
    if (room == NULL || env == NULL || (env->ct == NULL && env->ct_len != 0)) {
        return SC2_ERR_ARG;
    }
    if (cap > SC2_LINE_MAX + 1u) {
        cap = SC2_LINE_MAX + 1u;
    }
    sc2_jw_init(&w, out, cap);
    sc2_jw_object_begin(&w);
    sc2_jw_key(&w, "type");
    sc2_jw_cstring(&w, "room.send");
    put_rid(&w, rid);
    sc2_jw_key(&w, "room");
    sc2_jw_hex(&w, room, SC2_ROOM_ID_LEN);
    sc2_jw_key(&w, "env");
    sc2_jw_object_begin(&w);
    sc2_jw_key(&w, "s");
    sc2_jw_u64(&w, env->suite);
    sc2_jw_key(&w, "e");
    sc2_jw_u64(&w, env->epoch);
    sc2_jw_key(&w, "from");
    sc2_jw_hex(&w, env->from, SC2_DEVICE_ID_LEN);
    sc2_jw_key(&w, "n");
    sc2_jw_u64(&w, env->n);
    sc2_jw_key(&w, "nonce");
    sc2_jw_b64(&w, env->nonce, SC2_NONCE_LEN);
    sc2_jw_key(&w, "ct");
    sc2_jw_b64(&w, env->ct, env->ct_len);
    sc2_jw_key(&w, "sig");
    sc2_jw_b64(&w, env->sig, SC2_SIG_LEN);
    sc2_jw_object_end(&w);
    sc2_jw_object_end(&w);
    return sc2_jw_finish(&w, out_len);
}

int sc2_room_env_decode(const sc2_json_val_t *env_obj, sc2_room_env_t *env, uint8_t *ct_buf, size_t ct_cap)
{
    if (env_obj == NULL || env == NULL || ct_buf == NULL) {
        return SC2_ERR_ARG;
    }
    if (env_obj->type != SC2_JSON_OBJECT) {
        return SC2_ERR_FORMAT;
    }
    memset(env, 0, sizeof(*env));
    int rc = req(get_suite(env_obj, "s", &env->suite));
    if (rc == SC2_OK) {
        rc = req(get_u32(env_obj, "e", UINT32_MAX, &env->epoch));
    }
    if (rc == SC2_OK) {
        rc = req(sc2_json_get_hex_exact(env_obj, "from", env->from, SC2_DEVICE_ID_LEN));
    }
    if (rc == SC2_OK) {
        rc = req(get_u32(env_obj, "n", UINT32_MAX - 1u, &env->n));
    }
    if (rc == SC2_OK) {
        rc = req(sc2_json_get_b64_exact(env_obj, "nonce", env->nonce, SC2_NONCE_LEN));
    }
    if (rc == SC2_OK) {
        rc = req(sc2_json_get_b64_exact(env_obj, "sig", env->sig, SC2_SIG_LEN));
    }
    if (rc == SC2_OK) {
        rc = get_ct(env_obj, ct_buf, ct_cap, &env->ct, &env->ct_len);
    }
    return rc;
}

int sc2_frame_room_msg_decode(const char *line, size_t len, uint8_t room[SC2_ROOM_ID_LEN], uint64_t *seq,
                              sc2_room_env_t *env, uint8_t *ct_buf, size_t ct_cap)
{
    sc2_json_val_t root, v, e;
    if (line == NULL || room == NULL || seq == NULL) {
        return SC2_ERR_ARG;
    }
    int rc = sc2_json_root(line, len, &root);
    if (rc == SC2_OK) {
        rc = expect_type(&root, "room.msg");
    }
    if (rc == SC2_OK) {
        rc = req(sc2_json_get_hex_exact(&root, "room", room, SC2_ROOM_ID_LEN));
    }
    if (rc == SC2_OK) {
        rc = req(sc2_json_get(&root, "seq", &v));
    }
    if (rc == SC2_OK) {
        rc = sc2_json_u64_str(&v, seq);
    }
    if (rc == SC2_OK) {
        rc = req(get_obj(&root, "env", &e));
    }
    if (rc == SC2_OK) {
        rc = sc2_room_env_decode(&e, env, ct_buf, ct_cap);
    }
    return rc;
}

/* ---------------- public posts ---------------- */

/* Everything before the body goes into prefix; the body and ref follow as parts. */
#define POST_PREFIX_MAX (sizeof(LABEL_PUB) + 1u + 1u + SC2_RELAY_ID_MAX + 1u + SC2_ROOM_NAME_MAX + SC2_KEY_LEN + \
                         SC2_MSG_ID_LEN + 8u + 1u + 1u + SC2_POST_NAME_MAX + 2u)

static const uint8_t ZERO_REF[SC2_MSG_ID_LEN] = {0};

static int post_parts(uint8_t prefix[POST_PREFIX_MAX], sc2_part_t parts[3], const char *relay_id, const char *room,
                      const sc2_post_t *post)
{
    if (relay_id == NULL || room == NULL || post == NULL || (post->body == NULL && post->body_len != 0)) {
        return SC2_ERR_ARG;
    }
    size_t rl = strlen(relay_id);
    size_t ml = strlen(room);
    if (rl == 0 || rl > SC2_RELAY_ID_MAX || ml == 0 || ml > SC2_ROOM_NAME_MAX || post->name_len > SC2_POST_NAME_MAX ||
        post->body_len > SC2_POST_BODY_MAX || post->suite > 255u) {
        return SC2_ERR_BOUNDS;
    }
    size_t o = 0;
    memcpy(prefix, LABEL_PUB, sizeof(LABEL_PUB));
    o += sizeof(LABEL_PUB);
    prefix[o++] = (uint8_t)post->suite;
    prefix[o++] = (uint8_t)rl;
    memcpy(prefix + o, relay_id, rl);
    o += rl;
    prefix[o++] = (uint8_t)ml;
    memcpy(prefix + o, room, ml);
    o += ml;
    memcpy(prefix + o, post->ik, SC2_KEY_LEN);
    o += SC2_KEY_LEN;
    memcpy(prefix + o, post->id, SC2_MSG_ID_LEN);
    o += SC2_MSG_ID_LEN;
    sc2_store_be64(prefix + o, post->ts);
    o += 8u;
    prefix[o++] = post->t;
    prefix[o++] = (uint8_t)post->name_len;
    memcpy(prefix + o, post->name, post->name_len);
    o += post->name_len;
    sc2_store_be16(prefix + o, (uint16_t)post->body_len);
    o += 2u;
    parts[0].ptr = prefix;
    parts[0].len = o;
    parts[1].ptr = (const uint8_t *)post->body;
    parts[1].len = post->body_len;
    parts[2].ptr = post->has_ref ? post->ref : ZERO_REF;
    parts[2].len = SC2_MSG_ID_LEN;
    return SC2_OK;
}

int sc2_post_signed_bytes(uint8_t *out, size_t cap, size_t *out_len, const char *relay_id, const char *room,
                          const sc2_post_t *post)
{
    uint8_t prefix[POST_PREFIX_MAX];
    sc2_part_t parts[3];
    if (out == NULL || out_len == NULL) {
        return SC2_ERR_ARG;
    }
    int rc = post_parts(prefix, parts, relay_id, room, post);
    if (rc != SC2_OK) {
        return rc;
    }
    size_t need = parts[0].len + parts[1].len + parts[2].len;
    if (cap < need) {
        return SC2_ERR_BOUNDS;
    }
    size_t o = 0;
    for (size_t i = 0; i < 3; i++) {
        if (parts[i].len) {
            memcpy(out + o, parts[i].ptr, parts[i].len);
        }
        o += parts[i].len;
    }
    *out_len = o;
    return SC2_OK;
}

int sc2_post_sign(sc2_post_t *post, const char *relay_id, const char *room, const uint8_t ik_seed[SC2_KEY_LEN])
{
    uint8_t prefix[POST_PREFIX_MAX];
    sc2_part_t parts[3];
    if (post == NULL || ik_seed == NULL) {
        return SC2_ERR_ARG;
    }
    int rc = sc2_ed25519_public(post->ik, ik_seed);
    if (rc == SC2_OK) {
        rc = sc2_device_id(post->from, post->ik);
    }
    if (rc == SC2_OK) {
        rc = post_parts(prefix, parts, relay_id, room, post);
    }
    if (rc == SC2_OK) {
        rc = sc2_ed25519_sign_parts(post->sig, parts, 3, ik_seed);
    }
    return rc;
}

int sc2_post_verify(const sc2_post_t *post, const char *relay_id, const char *room, size_t max_body)
{
    uint8_t prefix[POST_PREFIX_MAX];
    sc2_part_t parts[3];
    uint8_t id[SC2_DEVICE_ID_LEN];
    if (post == NULL || relay_id == NULL || room == NULL) {
        return SC2_ERR_ARG;
    }
    if (post->suite != SC2_SUITE_1 || (post->t != SC2_POST_T_TEXT && post->t != SC2_POST_T_DEL)) {
        return SC2_ERR_FORMAT;
    }
    if (post->t == SC2_POST_T_DEL && !post->has_ref) {
        return SC2_ERR_FORMAT;
    }
    if (post->body_len > max_body || post->body_len > SC2_POST_BODY_MAX || post->name_len > SC2_POST_NAME_MAX) {
        return SC2_ERR_BOUNDS;
    }
    if (!sc2_utf8_valid(post->name, post->name_len) || !sc2_utf8_valid(post->body, post->body_len)) {
        return SC2_ERR_FORMAT;
    }
    int rc = sc2_device_id(id, post->ik);
    if (rc != SC2_OK) {
        return rc;
    }
    if (!sc2_ct_eq(id, post->from, SC2_DEVICE_ID_LEN)) {
        return SC2_ERR_AUTH;
    }
    rc = post_parts(prefix, parts, relay_id, room, post);
    if (rc != SC2_OK) {
        return rc;
    }
    return sc2_ed25519_verify_parts(post->sig, parts, 3, post->ik);
}

int sc2_frame_room_post(char *out, size_t cap, size_t *out_len, const char *rid, const char *room,
                        const sc2_post_t *post)
{
    sc2_jw_t w;
    if (room == NULL || post == NULL || (post->t != SC2_POST_T_TEXT && post->t != SC2_POST_T_DEL)) {
        return SC2_ERR_ARG;
    }
    if (strlen(room) == 0 || strlen(room) > SC2_ROOM_NAME_MAX || post->name_len > SC2_POST_NAME_MAX ||
        post->body_len > SC2_POST_BODY_MAX) {
        return SC2_ERR_BOUNDS;
    }
    if (cap > SC2_LINE_MAX + 1u) {
        cap = SC2_LINE_MAX + 1u;
    }
    sc2_jw_init(&w, out, cap);
    sc2_jw_object_begin(&w);
    sc2_jw_key(&w, "type");
    sc2_jw_cstring(&w, "room.post");
    put_rid(&w, rid);
    sc2_jw_key(&w, "room");
    sc2_jw_cstring(&w, room);
    sc2_jw_key(&w, "post");
    sc2_jw_object_begin(&w);
    sc2_jw_key(&w, "s");
    sc2_jw_u64(&w, post->suite);
    sc2_jw_key(&w, "t");
    sc2_jw_cstring(&w, post->t == SC2_POST_T_TEXT ? "text" : "del");
    sc2_jw_key(&w, "from");
    sc2_jw_hex(&w, post->from, SC2_DEVICE_ID_LEN);
    sc2_jw_key(&w, "ik");
    sc2_jw_b64(&w, post->ik, SC2_KEY_LEN);
    sc2_jw_key(&w, "id");
    sc2_jw_hex(&w, post->id, SC2_MSG_ID_LEN);
    sc2_jw_key(&w, "ts");
    sc2_jw_u64(&w, post->ts);
    sc2_jw_key(&w, "name");
    sc2_jw_string(&w, post->name, post->name_len);
    sc2_jw_key(&w, "body");
    sc2_jw_string(&w, post->body, post->body_len);
    if (post->has_ref) {
        sc2_jw_key(&w, "ref");
        sc2_jw_hex(&w, post->ref, SC2_MSG_ID_LEN);
    }
    sc2_jw_key(&w, "sig");
    sc2_jw_b64(&w, post->sig, SC2_SIG_LEN);
    sc2_jw_object_end(&w);
    sc2_jw_object_end(&w);
    return sc2_jw_finish(&w, out_len);
}

int sc2_post_decode(const sc2_json_val_t *post_obj, sc2_post_t *post, char *body_buf, size_t body_cap)
{
    sc2_json_val_t v;
    if (post_obj == NULL || post == NULL || body_buf == NULL || body_cap == 0) {
        return SC2_ERR_ARG;
    }
    if (post_obj->type != SC2_JSON_OBJECT) {
        return SC2_ERR_FORMAT;
    }
    memset(post, 0, sizeof(*post));
    int rc = req(get_suite(post_obj, "s", &post->suite));
    if (rc == SC2_OK) {
        rc = req(sc2_json_get(post_obj, "t", &v));
    }
    if (rc == SC2_OK) {
        if (sc2_json_string_eq(&v, "text")) {
            post->t = SC2_POST_T_TEXT;
        } else if (sc2_json_string_eq(&v, "del")) {
            post->t = SC2_POST_T_DEL;
        } else {
            rc = SC2_ERR_FORMAT;
        }
    }
    if (rc == SC2_OK) {
        rc = req(sc2_json_get_hex_exact(post_obj, "from", post->from, SC2_DEVICE_ID_LEN));
    }
    if (rc == SC2_OK) {
        rc = req(sc2_json_get_b64_exact(post_obj, "ik", post->ik, SC2_KEY_LEN));
    }
    if (rc == SC2_OK) {
        rc = req(sc2_json_get_hex_exact(post_obj, "id", post->id, SC2_MSG_ID_LEN));
    }
    if (rc == SC2_OK) {
        rc = req(sc2_json_get_u64(post_obj, "ts", UINT64_MAX, &post->ts));
    }
    if (rc == SC2_OK) {
        rc = req(sc2_json_get_string(post_obj, "name", post->name, sizeof(post->name), &post->name_len));
        if (rc == SC2_OK && strlen(post->name) != post->name_len) {
            rc = SC2_ERR_FORMAT; /* embedded NUL */
        }
    }
    if (rc == SC2_OK) {
        rc = req(sc2_json_get_string(post_obj, "body", body_buf, body_cap, &post->body_len));
        if (rc == SC2_OK && (post->body_len > SC2_POST_BODY_MAX || strlen(body_buf) != post->body_len)) {
            rc = post->body_len > SC2_POST_BODY_MAX ? SC2_ERR_BOUNDS : SC2_ERR_FORMAT;
        }
        post->body = body_buf;
    }
    if (rc == SC2_OK) {
        rc = sc2_json_get_hex_exact(post_obj, "ref", post->ref, SC2_MSG_ID_LEN);
        if (rc == SC2_OK) {
            post->has_ref = true;
        } else if (rc == SC2_ERR_NOT_FOUND) {
            rc = SC2_OK;
        }
    }
    if (rc == SC2_OK) {
        rc = req(sc2_json_get_b64_exact(post_obj, "sig", post->sig, SC2_SIG_LEN));
    }
    return rc;
}

int sc2_frame_room_pub_decode(const char *line, size_t len, char room[SC2_ROOM_NAME_MAX + 1], uint64_t *seq,
                              sc2_post_t *post, char *body_buf, size_t body_cap)
{
    sc2_json_val_t root, v, p;
    size_t rl = 0;
    if (line == NULL || room == NULL || seq == NULL) {
        return SC2_ERR_ARG;
    }
    int rc = sc2_json_root(line, len, &root);
    if (rc == SC2_OK) {
        rc = expect_type(&root, "room.pub");
    }
    if (rc == SC2_OK) {
        rc = req(sc2_json_get_string(&root, "room", room, SC2_ROOM_NAME_MAX + 1, &rl));
        if (rc == SC2_OK && (rl == 0 || strlen(room) != rl)) {
            rc = SC2_ERR_FORMAT;
        }
    }
    if (rc == SC2_OK) {
        rc = req(sc2_json_get(&root, "seq", &v));
    }
    if (rc == SC2_OK) {
        rc = sc2_json_u64_str(&v, seq);
    }
    if (rc == SC2_OK) {
        rc = req(get_obj(&root, "post", &p));
    }
    if (rc == SC2_OK) {
        rc = sc2_post_decode(&p, post, body_buf, body_cap);
    }
    return rc;
}

/* ---------------- private-room membership and invites ---------------- */

static int commit_frame(char *out, size_t cap, size_t *out_len, const char *type, const char *rid,
                        const uint8_t room[SC2_ROOM_ID_LEN], const uint8_t rec[SC2_COMMIT_LEN],
                        const uint8_t sig[SC2_SIG_LEN], bool create)
{
    sc2_jw_t w;
    sc2_commit_t c;
    if (out == NULL || room == NULL || rec == NULL || sig == NULL) {
        return SC2_ERR_ARG;
    }
    if (sc2_commit_decode(&c, rec) != SC2_OK || memcmp(c.room, room, SC2_ROOM_ID_LEN) != 0 ||
        (create && c.op != SC2_COMMIT_CREATE) || (!create && c.op == SC2_COMMIT_CREATE)) {
        return SC2_ERR_ARG;
    }
    sc2_jw_init(&w, out, cap);
    sc2_jw_object_begin(&w);
    sc2_jw_key(&w, "type");
    sc2_jw_cstring(&w, type);
    put_rid(&w, rid);
    sc2_jw_key(&w, "room");
    sc2_jw_hex(&w, room, SC2_ROOM_ID_LEN);
    if (create) {
        sc2_jw_key(&w, "kind");
        sc2_jw_cstring(&w, "private");
    }
    sc2_jw_key(&w, "commit");
    sc2_jw_b64(&w, rec, SC2_COMMIT_LEN);
    sc2_jw_key(&w, "sig");
    sc2_jw_b64(&w, sig, SC2_SIG_LEN);
    sc2_jw_object_end(&w);
    return sc2_jw_finish(&w, out_len);
}

int sc2_frame_room_create_private(char *out, size_t cap, size_t *out_len, const char *rid,
                                  const uint8_t room[SC2_ROOM_ID_LEN], const uint8_t rec[SC2_COMMIT_LEN],
                                  const uint8_t sig[SC2_SIG_LEN])
{
    return commit_frame(out, cap, out_len, "room.create", rid, room, rec, sig, true);
}

int sc2_frame_room_commit(char *out, size_t cap, size_t *out_len, const char *rid,
                          const uint8_t room[SC2_ROOM_ID_LEN], const uint8_t rec[SC2_COMMIT_LEN],
                          const uint8_t sig[SC2_SIG_LEN])
{
    return commit_frame(out, cap, out_len, "room.commit", rid, room, rec, sig, false);
}

int sc2_frame_room_commit_decode(const char *line, size_t len, uint8_t room[SC2_ROOM_ID_LEN], uint64_t *seq,
                                 uint8_t rec[SC2_COMMIT_LEN], uint8_t sig[SC2_SIG_LEN])
{
    sc2_json_val_t root, v;
    if (line == NULL || room == NULL || seq == NULL || rec == NULL || sig == NULL) {
        return SC2_ERR_ARG;
    }
    int rc = sc2_json_root(line, len, &root);
    if (rc == SC2_OK) {
        rc = expect_type(&root, "room.commit");
    }
    if (rc == SC2_OK) {
        rc = req(sc2_json_get_hex_exact(&root, "room", room, SC2_ROOM_ID_LEN));
    }
    if (rc == SC2_OK) {
        rc = req(sc2_json_get(&root, "seq", &v));
    }
    if (rc == SC2_OK) {
        rc = sc2_json_u64_str(&v, seq);
    }
    if (rc == SC2_OK) {
        rc = req(sc2_json_get_b64_exact(&root, "commit", rec, SC2_COMMIT_LEN));
    }
    if (rc == SC2_OK) {
        rc = req(sc2_json_get_b64_exact(&root, "sig", sig, SC2_SIG_LEN));
    }
    if (rc == SC2_OK && memcmp(rec + 12, room, SC2_ROOM_ID_LEN) != 0) {
        rc = SC2_ERR_FORMAT; /* record names another room */
    }
    return rc;
}

int sc2_frame_room_invite(char *out, size_t cap, size_t *out_len, const char *rid,
                          const uint8_t room[SC2_ROOM_ID_LEN], const uint8_t key[SC2_INVITE_KEY_LEN], bool revoke)
{
    sc2_jw_t w;
    if (out == NULL || room == NULL || key == NULL) {
        return SC2_ERR_ARG;
    }
    sc2_jw_init(&w, out, cap);
    sc2_jw_object_begin(&w);
    sc2_jw_key(&w, "type");
    sc2_jw_cstring(&w, "room.invite");
    put_rid(&w, rid);
    sc2_jw_key(&w, "room");
    sc2_jw_hex(&w, room, SC2_ROOM_ID_LEN);
    sc2_jw_key(&w, "key");
    sc2_jw_b64(&w, key, SC2_INVITE_KEY_LEN);
    if (revoke) {
        sc2_jw_key(&w, "revoke");
        sc2_jw_bool(&w, true);
    }
    sc2_jw_object_end(&w);
    return sc2_jw_finish(&w, out_len);
}

int sc2_frame_room_join_private(char *out, size_t cap, size_t *out_len, const char *rid,
                                const uint8_t room[SC2_ROOM_ID_LEN], const uint8_t proof[32])
{
    sc2_jw_t w;
    if (out == NULL || room == NULL) {
        return SC2_ERR_ARG;
    }
    sc2_jw_init(&w, out, cap);
    sc2_jw_object_begin(&w);
    sc2_jw_key(&w, "type");
    sc2_jw_cstring(&w, "room.join");
    put_rid(&w, rid);
    sc2_jw_key(&w, "room");
    sc2_jw_hex(&w, room, SC2_ROOM_ID_LEN);
    if (proof != NULL) {
        sc2_jw_key(&w, "invite");
        sc2_jw_b64(&w, proof, 32);
    }
    sc2_jw_object_end(&w);
    return sc2_jw_finish(&w, out_len);
}

/* ---------------- relay-signed moderation ---------------- */

int sc2_frame_room_mod_decode(const char *line, size_t len, sc2_mod_t *mod, uint8_t sig[SC2_SIG_LEN])
{
    sc2_json_val_t root, v;
    char action[8];
    size_t n = 0;
    if (line == NULL || mod == NULL || sig == NULL) {
        return SC2_ERR_ARG;
    }
    memset(mod, 0, sizeof(*mod));
    int rc = sc2_json_root(line, len, &root);
    if (rc == SC2_OK) {
        rc = expect_type(&root, "room.mod");
    }
    if (rc == SC2_OK) {
        rc = req(sc2_json_get_string(&root, "room", mod->room, SC2_MOD_ROOM_MAX + 1u, &mod->room_len));
        if (rc == SC2_ERR_BOUNDS || (rc == SC2_OK && (mod->room_len == 0 || strlen(mod->room) != mod->room_len))) {
            rc = SC2_ERR_FORMAT;
        }
    }
    if (rc == SC2_OK) {
        rc = req(sc2_json_get(&root, "seq", &v));
    }
    if (rc == SC2_OK) {
        rc = sc2_json_u64_str(&v, &mod->seq);
    }
    if (rc == SC2_OK) {
        rc = req(sc2_json_get_string(&root, "action", action, sizeof(action), &n));
        if (rc == SC2_ERR_BOUNDS) {
            rc = SC2_ERR_FORMAT;
        }
    }
    if (rc == SC2_OK) {
        mod->action = sc2_mod_action_parse(action, n);
        if (mod->action == 0) {
            rc = SC2_ERR_FORMAT;
        }
    }
    if (rc == SC2_OK) {
        rc = req(sc2_json_get_u64(&root, "ts", UINT64_MAX, &mod->ts));
    }
    if (rc == SC2_OK) {
        rc = sc2_json_get_hex_exact(&root, "ref", mod->ref, SC2_MSG_ID_LEN);
        if (rc == SC2_ERR_NOT_FOUND && mod->action != SC2_MOD_REMOVE) {
            rc = SC2_OK;
        }
        rc = req(rc);
    }
    if (rc == SC2_OK) {
        rc = req(sc2_json_get_hex_exact(&root, "device", mod->device, SC2_DEVICE_ID_LEN));
    }
    if (rc == SC2_OK) {
        rc = sc2_json_get(&root, "until", &v);
        if (rc == SC2_ERR_NOT_FOUND) {
            rc = SC2_OK;
        } else if (rc == SC2_OK && mod->action != SC2_MOD_BAN) {
            rc = SC2_ERR_FORMAT;
        } else if (rc == SC2_OK && v.type != SC2_JSON_NULL) {
            rc = sc2_json_u64(&v, UINT64_MAX, &mod->until);
        }
    }
    if (rc == SC2_OK) {
        rc = req(sc2_json_get(&root, "by", &v));
        if (rc == SC2_OK && !sc2_json_string_eq(&v, "relay")) {
            rc = SC2_ERR_FORMAT;
        }
    }
    if (rc == SC2_OK) {
        rc = req(sc2_json_get_b64_exact(&root, "sig", sig, SC2_SIG_LEN));
    }
    if (rc == SC2_OK) {
        uint8_t rec[SC2_MOD_LEN];
        rc = sc2_mod_encode(rec, mod) == SC2_OK ? SC2_OK : SC2_ERR_FORMAT; /* field rules */
    }
    return rc;
}

int sc2_frame_room_mod(char *out, size_t cap, size_t *out_len, const sc2_mod_t *mod, const uint8_t sig[SC2_SIG_LEN])
{
    sc2_jw_t w;
    uint8_t rec[SC2_MOD_LEN];
    if (out == NULL || mod == NULL || sig == NULL || sc2_mod_encode(rec, mod) != SC2_OK) {
        return SC2_ERR_ARG;
    }
    sc2_jw_init(&w, out, cap);
    sc2_jw_object_begin(&w);
    sc2_jw_key(&w, "type");
    sc2_jw_cstring(&w, "room.mod");
    sc2_jw_key(&w, "room");
    sc2_jw_string(&w, mod->room, mod->room_len);
    sc2_jw_key(&w, "seq");
    sc2_jw_u64_str(&w, mod->seq);
    sc2_jw_key(&w, "action");
    sc2_jw_cstring(&w, sc2_mod_action_name(mod->action));
    sc2_jw_key(&w, "ts");
    sc2_jw_u64(&w, mod->ts);
    if (mod->action == SC2_MOD_REMOVE) {
        sc2_jw_key(&w, "ref");
        sc2_jw_hex(&w, mod->ref, SC2_MSG_ID_LEN);
    }
    sc2_jw_key(&w, "device");
    sc2_jw_hex(&w, mod->device, SC2_DEVICE_ID_LEN);
    if (mod->action == SC2_MOD_BAN) {
        sc2_jw_key(&w, "until");
        if (mod->until == 0) {
            sc2_jw_null(&w);
        } else {
            sc2_jw_u64(&w, mod->until);
        }
    }
    sc2_jw_key(&w, "by");
    sc2_jw_cstring(&w, "relay");
    sc2_jw_key(&w, "sig");
    sc2_jw_b64(&w, sig, SC2_SIG_LEN);
    sc2_jw_object_end(&w);
    return sc2_jw_finish(&w, out_len);
}
