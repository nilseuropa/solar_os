/*
 * solaros.sc2: SC2 end-to-end chat security for MicroPython.
 *
 * Secrets never enter the Python heap. Scripts pass inner JSON (plaintext)
 * and relay frames in and receive frames, plaintext and public metadata
 * back. Identity keys, prekeys, ratchet states and room epoch keys stay in
 * the C service (src/services/solar_os_sc2.c).
 */

#include <time.h>

static void python_sc2_check(esp_err_t err)
{
    if (err != ESP_OK) {
        mp_raise_msg_varg(&mp_type_OSError, MP_ERROR_TEXT("sc2: %s"), solar_os_sc2_err_text(err));
    }
}

static const char *python_sc2_optional_rid(size_t n_args, const mp_obj_t *args, size_t index)
{
    const char *rid = python_optional_str(n_args, args, index, NULL);
    return (rid != NULL && rid[0] != '\0') ? rid : NULL;
}

/* Run a frame-producing call into a Python-owned buffer. The buffer only
 * ever holds public wire data. */
typedef esp_err_t (*python_sc2_frame_fn)(void *ctx, char *out, size_t cap, size_t *out_len);

static mp_obj_t python_sc2_frame(python_sc2_frame_fn fn, void *ctx)
{
    vstr_t frame;
    size_t len = 0;
    vstr_init_len(&frame, SOLAR_OS_SC2_FRAME_MAX);
    const esp_err_t err = fn(ctx, frame.buf, SOLAR_OS_SC2_FRAME_MAX, &len);
    if (err != ESP_OK) {
        vstr_clear(&frame);
        python_sc2_check(err);
    }
    frame.len = len;
    return mp_obj_new_str_from_vstr(&frame);
}

static mp_obj_t solaros_sc2_identity(void)
{
    sc2e_identity_info_t info;
    python_sc2_check(solar_os_sc2_identity(&info));
    mp_obj_t dict = mp_obj_new_dict(6);
    python_dict_store_cstr(dict, "device_id", info.device_id);
    python_dict_store_cstr(dict, "fingerprint", info.fingerprint);
    python_dict_store_cstr(dict, "ik", info.ik_b64);
    python_dict_store_cstr(dict, "dk", info.dk_b64);
    python_dict_store_uint(dict, "spk_id", info.spk_id);
    python_dict_store_uint(dict, "opk_count", info.opk_count);
    return dict;
}
MP_DEFINE_CONST_FUN_OBJ_0(solaros_sc2_identity_obj, solaros_sc2_identity);

static mp_obj_t solaros_sc2_status(void)
{
    solar_os_sc2_status_t st;
    const esp_err_t err = solar_os_sc2_get_status(&st);
    mp_obj_t dict = mp_obj_new_dict(10);
    python_dict_store_bool(dict, "ready", err == ESP_OK && st.ready);
    python_dict_store_cstr(dict, "core", SC2_VERSION_STRING);
    python_dict_store_cstr(dict, "hash", sc2_hash_backend());
    if (err != ESP_OK) {
        python_dict_store_cstr(dict, "error", solar_os_sc2_err_text(err));
        return dict;
    }
    python_dict_store_cstr(dict, "device_id", st.device_id);
    python_dict_store_cstr(dict, "store", st.store_path);
    python_dict_store_bool(dict, "psram", st.work_in_psram);
    python_dict_store_uint(dict, "peers", st.peers);
    python_dict_store_uint(dict, "sessions", st.sessions);
    python_dict_store_uint(dict, "rooms", st.rooms);
    python_dict_store_uint(dict, "opk_count", st.opk_count);
    return dict;
}
MP_DEFINE_CONST_FUN_OBJ_0(solaros_sc2_status_obj, solaros_sc2_status);

static mp_obj_t solaros_sc2_safety_number(mp_obj_t peer)
{
    sc2e_safety_t safety;
    python_sc2_check(solar_os_sc2_safety(mp_obj_str_get_str(peer), &safety));
    mp_obj_t dict = mp_obj_new_dict(3);
    python_dict_store_cstr(dict, "short", safety.short_form);
    python_dict_store_cstr(dict, "words", safety.words);
    python_dict_store_cstr(dict, "full", safety.full_form);
    return dict;
}
MP_DEFINE_CONST_FUN_OBJ_1(solaros_sc2_safety_number_obj, solaros_sc2_safety_number);

static mp_obj_t solaros_sc2_peer(mp_obj_t peer)
{
    sc2e_peer_info_t info;
    python_sc2_check(solar_os_sc2_peer_info(mp_obj_str_get_str(peer), &info));
    if (!info.known) {
        return mp_const_none;
    }
    mp_obj_t dict = mp_obj_new_dict(4);
    python_dict_store_bool(dict, "session", info.has_session);
    python_dict_store_bool(dict, "opk", info.opk_available);
    python_dict_store_uint(dict, "spk_id", info.spk_id);
    python_dict_store_cstr(dict, "fingerprint", info.fingerprint);
    return dict;
}
MP_DEFINE_CONST_FUN_OBJ_1(solaros_sc2_peer_obj, solaros_sc2_peer);

static mp_obj_t solaros_sc2_forget(mp_obj_t peer)
{
    python_sc2_check(solar_os_sc2_forget_peer(mp_obj_str_get_str(peer)));
    return mp_const_none;
}
MP_DEFINE_CONST_FUN_OBJ_1(solaros_sc2_forget_obj, solaros_sc2_forget);

typedef struct {
    const char *rid;
    uint32_t target;
} python_sc2_publish_t;

static esp_err_t python_sc2_publish_fn(void *ctx, char *out, size_t cap, size_t *len)
{
    const python_sc2_publish_t *p = ctx;
    return solar_os_sc2_keys_publish(p->rid, p->target, out, cap, len);
}

static mp_obj_t solaros_sc2_keys_publish(size_t n_args, const mp_obj_t *args)
{
    python_sc2_publish_t p = {
        .target = python_optional_u32(n_args, args, 0, SC2E_OPK_DEFAULT),
        .rid = python_sc2_optional_rid(n_args, args, 1),
    };
    return python_sc2_frame(python_sc2_publish_fn, &p);
}
MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(solaros_sc2_keys_publish_obj, 0, 2, solaros_sc2_keys_publish);

static mp_obj_t solaros_sc2_add_bundle(mp_obj_t frame)
{
    size_t len = 0;
    const char *line = mp_obj_str_get_data(frame, &len);
    char device[SC2_DEVICE_ID_HEX_LEN + 1];
    python_sc2_check(solar_os_sc2_add_bundle(line, len, device));
    return mp_obj_new_str(device, strlen(device));
}
MP_DEFINE_CONST_FUN_OBJ_1(solaros_sc2_add_bundle_obj, solaros_sc2_add_bundle);

typedef struct {
    const char *target; /* peer or room */
    const char *peer;   /* room_share only */
    const char *rid;
    const char *inner;
    size_t inner_len;
} python_sc2_seal_t;

static esp_err_t python_sc2_dm_fn(void *ctx, char *out, size_t cap, size_t *len)
{
    const python_sc2_seal_t *s = ctx;
    return solar_os_sc2_dm_encrypt(s->target, s->rid, s->inner, s->inner_len, out, cap, len);
}

static esp_err_t python_sc2_room_fn(void *ctx, char *out, size_t cap, size_t *len)
{
    const python_sc2_seal_t *s = ctx;
    return solar_os_sc2_room_encrypt(s->target, s->rid, s->inner, s->inner_len, out, cap, len);
}

static esp_err_t python_sc2_share_fn(void *ctx, char *out, size_t cap, size_t *len)
{
    const python_sc2_seal_t *s = ctx;
    return solar_os_sc2_room_share(s->target, s->peer, s->rid, out, cap, len);
}

static mp_obj_t solaros_sc2_dm_encrypt(size_t n_args, const mp_obj_t *args)
{
    python_sc2_seal_t s = {.target = mp_obj_str_get_str(args[0])};
    s.inner = mp_obj_str_get_data(args[1], &s.inner_len);
    s.rid = python_sc2_optional_rid(n_args, args, 2);
    return python_sc2_frame(python_sc2_dm_fn, &s);
}
MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(solaros_sc2_dm_encrypt_obj, 2, 3, solaros_sc2_dm_encrypt);

static const char *python_sc2_dm_kind(int kind)
{
    switch (kind) {
    case SC2E_DM_EPOCH_KEY:
        return "epoch_key";
    case SC2E_DM_HISTORY:
        return "history";
    case SC2E_DM_IGNORED:
        return "ignored";
    default:
        return "message";
    }
}

static mp_obj_t solaros_sc2_dm_decrypt(mp_obj_t frame)
{
    size_t len = 0, pt_len = 0;
    const char *line = mp_obj_str_get_data(frame, &len);
    sc2e_dm_result_t res;
    vstr_t pt;
    vstr_init_len(&pt, SOLAR_OS_SC2_INNER_MAX + 1u);
    const esp_err_t err = solar_os_sc2_dm_decrypt(line, len, &res, pt.buf, SOLAR_OS_SC2_INNER_MAX + 1u, &pt_len);
    if (err != ESP_OK) {
        vstr_clear(&pt);
        python_sc2_check(err);
    }
    mp_obj_t dict = mp_obj_new_dict(7);
    python_dict_store_cstr(dict, "from", res.from);
    python_dict_store_u64(dict, "seq", res.seq);
    python_dict_store_cstr(dict, "kind", python_sc2_dm_kind(res.kind));
    python_dict_store_bool(dict, "new_session", res.new_session);
    if (res.kind == SC2E_DM_MESSAGE) {
        pt.len = pt_len;
        mp_obj_dict_store(dict, MP_OBJ_NEW_QSTR(qstr_from_str("body")), mp_obj_new_str_from_vstr(&pt));
    } else {
        vstr_clear(&pt);
        python_dict_store_cstr(dict, "room", res.room);
        python_dict_store_uint(dict, "epoch", res.epoch);
    }
    return dict;
}
MP_DEFINE_CONST_FUN_OBJ_1(solaros_sc2_dm_decrypt_obj, solaros_sc2_dm_decrypt);

static mp_obj_t solaros_sc2_room_create(size_t n_args, const mp_obj_t *args)
{
    char room[SC2_DEVICE_ID_HEX_LEN + 1];
    uint32_t epoch = 0;
    python_sc2_check(solar_os_sc2_room_create(python_optional_str(n_args, args, 0, NULL), room, &epoch));
    mp_obj_t dict = mp_obj_new_dict(2);
    python_dict_store_cstr(dict, "room", room);
    python_dict_store_uint(dict, "epoch", epoch);
    return dict;
}
MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(solaros_sc2_room_create_obj, 0, 1, solaros_sc2_room_create);

static mp_obj_t solaros_sc2_room_rotate(mp_obj_t room)
{
    uint32_t epoch = 0;
    python_sc2_check(solar_os_sc2_room_rotate(mp_obj_str_get_str(room), &epoch));
    return mp_obj_new_int_from_uint(epoch);
}
MP_DEFINE_CONST_FUN_OBJ_1(solaros_sc2_room_rotate_obj, solaros_sc2_room_rotate);

static mp_obj_t solaros_sc2_room_share(size_t n_args, const mp_obj_t *args)
{
    python_sc2_seal_t s = {
        .target = mp_obj_str_get_str(args[0]),
        .peer = mp_obj_str_get_str(args[1]),
        .rid = python_sc2_optional_rid(n_args, args, 2),
    };
    return python_sc2_frame(python_sc2_share_fn, &s);
}
MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(solaros_sc2_room_share_obj, 2, 3, solaros_sc2_room_share);

static mp_obj_t solaros_sc2_room_encrypt(size_t n_args, const mp_obj_t *args)
{
    python_sc2_seal_t s = {.target = mp_obj_str_get_str(args[0])};
    s.inner = mp_obj_str_get_data(args[1], &s.inner_len);
    s.rid = python_sc2_optional_rid(n_args, args, 2);
    return python_sc2_frame(python_sc2_room_fn, &s);
}
MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(solaros_sc2_room_encrypt_obj, 2, 3, solaros_sc2_room_encrypt);

static mp_obj_t solaros_sc2_room_decrypt(mp_obj_t frame)
{
    size_t len = 0, pt_len = 0;
    const char *line = mp_obj_str_get_data(frame, &len);
    sc2e_room_result_t res;
    vstr_t pt;
    vstr_init_len(&pt, SOLAR_OS_SC2_INNER_MAX + 1u);
    const esp_err_t err = solar_os_sc2_room_decrypt(line, len, &res, pt.buf, SOLAR_OS_SC2_INNER_MAX + 1u, &pt_len);
    if (err != ESP_OK) {
        vstr_clear(&pt);
        python_sc2_check(err);
    }
    mp_obj_t dict = mp_obj_new_dict(6);
    python_dict_store_cstr(dict, "room", res.room);
    python_dict_store_cstr(dict, "from", res.from);
    python_dict_store_u64(dict, "seq", res.seq);
    python_dict_store_uint(dict, "epoch", res.epoch);
    python_dict_store_uint(dict, "n", res.n);
    pt.len = pt_len;
    mp_obj_dict_store(dict, MP_OBJ_NEW_QSTR(qstr_from_str("body")), mp_obj_new_str_from_vstr(&pt));
    return dict;
}
MP_DEFINE_CONST_FUN_OBJ_1(solaros_sc2_room_decrypt_obj, solaros_sc2_room_decrypt);

static mp_obj_t solaros_sc2_room_info(mp_obj_t room)
{
    sc2e_room_info_t info;
    python_sc2_check(solar_os_sc2_room_info(mp_obj_str_get_str(room), &info));
    if (!info.known) {
        return mp_const_none;
    }
    mp_obj_t dict = mp_obj_new_dict(5);
    python_dict_store_cstr(dict, "admin", info.admin);
    python_dict_store_bool(dict, "is_admin", info.is_admin);
    python_dict_store_uint(dict, "epoch", info.epoch);
    python_dict_store_uint(dict, "sent", info.sent);
    python_dict_store_uint(dict, "senders", info.senders);
    return dict;
}
MP_DEFINE_CONST_FUN_OBJ_1(solaros_sc2_room_info_obj, solaros_sc2_room_info);

static mp_obj_t solaros_sc2_room_forget(mp_obj_t room)
{
    python_sc2_check(solar_os_sc2_room_forget(mp_obj_str_get_str(room)));
    return mp_const_none;
}
MP_DEFINE_CONST_FUN_OBJ_1(solaros_sc2_room_forget_obj, solaros_sc2_room_forget);

typedef struct {
    const char *relay;
    const char *room;
    const char *name;
    const char *body;
    size_t body_len;
    uint64_t ts;
    const char *ref;
    bool deleted;
    const char *rid;
} python_sc2_post_t;

static esp_err_t python_sc2_post_fn(void *ctx, char *out, size_t cap, size_t *len)
{
    const python_sc2_post_t *p = ctx;
    return solar_os_sc2_post_sign(p->relay, p->room, p->name, p->body, p->body_len, p->ts, p->ref, p->deleted,
                                  p->rid, out, cap, len);
}

/* post_sign(relay, room, name, body, ts=0, ref=None, delete=False, rid=None) */
static mp_obj_t solaros_sc2_post_sign(size_t n_args, const mp_obj_t *args)
{
    python_sc2_post_t p = {
        .relay = mp_obj_str_get_str(args[0]),
        .room = mp_obj_str_get_str(args[1]),
        .name = mp_obj_str_get_str(args[2]),
    };
    p.body = mp_obj_str_get_data(args[3], &p.body_len);
    p.ts = python_optional_u32(n_args, args, 4, 0);
    if (p.ts == 0) {
        const time_t now = time(NULL);
        p.ts = now > 0 ? (uint64_t)now : 0u;
    }
    p.ref = python_optional_str(n_args, args, 5, NULL);
    p.deleted = n_args > 6 && mp_obj_is_true(args[6]);
    p.rid = python_sc2_optional_rid(n_args, args, 7);
    return python_sc2_frame(python_sc2_post_fn, &p);
}
MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(solaros_sc2_post_sign_obj, 4, 8, solaros_sc2_post_sign);

/* post_verify(relay, frame, max_body=1024): verification failures are
 * returned with valid=False so scripts can still show the post as such. */
static mp_obj_t solaros_sc2_post_verify(size_t n_args, const mp_obj_t *args)
{
    const char *relay = mp_obj_str_get_str(args[0]);
    size_t len = 0;
    const char *line = mp_obj_str_get_data(args[1], &len);
    const uint32_t max_body = python_optional_u32(n_args, args, 2, 1024);
    sc2e_post_result_t res;
    vstr_t body;
    vstr_init_len(&body, SC2_POST_BODY_MAX + 1u);
    const esp_err_t err = solar_os_sc2_post_verify(relay, line, len, max_body, &res, body.buf, SC2_POST_BODY_MAX + 1u);
    if (err != ESP_OK && err != SOLAR_OS_SC2_ESP_ERR(SC2_ERR_AUTH) && err != SOLAR_OS_SC2_ESP_ERR(SC2_ERR_BOUNDS)) {
        vstr_clear(&body);
        python_sc2_check(err);
    }
    mp_obj_t dict = mp_obj_new_dict(10);
    python_dict_store_bool(dict, "valid", res.valid);
    python_dict_store_cstr(dict, "room", res.room);
    python_dict_store_cstr(dict, "from", res.from);
    python_dict_store_cstr(dict, "id", res.id);
    python_dict_store_cstr(dict, "ref", res.ref);
    python_dict_store_cstr(dict, "name", res.name);
    python_dict_store_u64(dict, "ts", res.ts);
    python_dict_store_u64(dict, "seq", res.seq);
    python_dict_store_bool(dict, "deleted", res.deleted);
    body.len = res.body_len <= SC2_POST_BODY_MAX ? res.body_len : 0;
    mp_obj_dict_store(dict, MP_OBJ_NEW_QSTR(qstr_from_str("body")), mp_obj_new_str_from_vstr(&body));
    return dict;
}
MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(solaros_sc2_post_verify_obj, 2, 3, solaros_sc2_post_verify);

static mp_obj_t solaros_sc2_selftest(void)
{
    unsigned passed = 0, failed = 0;
    const esp_err_t err = solar_os_sc2_selftest(NULL, NULL, &passed, &failed);
    if (err != ESP_OK && passed == 0 && failed == 0) {
        python_sc2_check(err);
    }
    mp_obj_t dict = mp_obj_new_dict(2);
    python_dict_store_uint(dict, "passed", passed);
    python_dict_store_uint(dict, "failed", failed);
    return dict;
}
MP_DEFINE_CONST_FUN_OBJ_0(solaros_sc2_selftest_obj, solaros_sc2_selftest);
