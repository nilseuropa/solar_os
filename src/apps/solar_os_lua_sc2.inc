/*
 * solaros.sc2 for Lua: the same SC2 calls as the MicroPython module.
 * Secrets stay in the C service; scripts only see frames, plaintext and
 * public metadata.
 */

#include <time.h>

static int solua_sc2_check(lua_State *L, esp_err_t err)
{
    if (err != ESP_OK) {
        return luaL_error(L, "sc2: %s", solar_os_sc2_err_text(err));
    }
    return 0;
}

static const char *solua_sc2_rid(lua_State *L, int index)
{
    const char *rid = solua_optional_str(L, index, NULL);
    return (rid != NULL && rid[0] != '\0') ? rid : NULL;
}

typedef esp_err_t (*solua_sc2_frame_fn)(void *ctx, char *out, size_t cap, size_t *out_len);

/* Pushes the produced frame as a Lua string. */
static int solua_sc2_frame(lua_State *L, solua_sc2_frame_fn fn, void *ctx)
{
    luaL_Buffer buffer;
    size_t len = 0;
    char *out = luaL_buffinitsize(L, &buffer, SOLAR_OS_SC2_FRAME_MAX);
    const esp_err_t err = fn(ctx, out, SOLAR_OS_SC2_FRAME_MAX, &len);
    if (err != ESP_OK) {
        luaL_pushresultsize(&buffer, 0);
        lua_pop(L, 1);
        return solua_sc2_check(L, err);
    }
    luaL_pushresultsize(&buffer, len);
    return 1;
}

static uint32_t solua_sc2_u32(lua_State *L, int index, uint32_t fallback)
{
    if (lua_isnoneornil(L, index)) {
        return fallback;
    }
    const lua_Integer v = luaL_checkinteger(L, index);
    if (v < 0 || v > (lua_Integer)UINT32_MAX) {
        luaL_error(L, "expected integer 0..4294967295");
    }
    return (uint32_t)v;
}

static int solua_sc2_identity(lua_State *L)
{
    sc2e_identity_info_t info;
    solua_sc2_check(L, solar_os_sc2_identity(&info));
    lua_createtable(L, 0, 6);
    solua_set_str(L, -1, "device_id", info.device_id);
    solua_set_str(L, -1, "fingerprint", info.fingerprint);
    solua_set_str(L, -1, "ik", info.ik_b64);
    solua_set_str(L, -1, "dk", info.dk_b64);
    solua_set_int(L, -1, "spk_id", (lua_Integer)info.spk_id);
    solua_set_int(L, -1, "opk_count", (lua_Integer)info.opk_count);
    return 1;
}

static int solua_sc2_status(lua_State *L)
{
    solar_os_sc2_status_t st;
    const esp_err_t err = solar_os_sc2_get_status(&st);
    lua_createtable(L, 0, 10);
    solua_set_bool(L, -1, "ready", err == ESP_OK && st.ready);
    solua_set_str(L, -1, "core", SC2_VERSION_STRING);
    solua_set_str(L, -1, "hash", sc2_hash_backend());
    if (err != ESP_OK) {
        solua_set_str(L, -1, "error", solar_os_sc2_err_text(err));
        return 1;
    }
    solua_set_str(L, -1, "device_id", st.device_id);
    solua_set_str(L, -1, "store", st.store_path);
    solua_set_bool(L, -1, "psram", st.work_in_psram);
    solua_set_int(L, -1, "peers", (lua_Integer)st.peers);
    solua_set_int(L, -1, "sessions", (lua_Integer)st.sessions);
    solua_set_int(L, -1, "rooms", (lua_Integer)st.rooms);
    solua_set_int(L, -1, "opk_count", (lua_Integer)st.opk_count);
    return 1;
}

static int solua_sc2_safety_number(lua_State *L)
{
    sc2e_safety_t safety;
    solua_sc2_check(L, solar_os_sc2_safety(luaL_checkstring(L, 1), &safety));
    lua_createtable(L, 0, 3);
    solua_set_str(L, -1, "short", safety.short_form);
    solua_set_str(L, -1, "words", safety.words);
    solua_set_str(L, -1, "full", safety.full_form);
    return 1;
}

static int solua_sc2_peer(lua_State *L)
{
    sc2e_peer_info_t info;
    solua_sc2_check(L, solar_os_sc2_peer_info(luaL_checkstring(L, 1), &info));
    if (!info.known) {
        lua_pushnil(L);
        return 1;
    }
    lua_createtable(L, 0, 4);
    solua_set_bool(L, -1, "session", info.has_session);
    solua_set_bool(L, -1, "opk", info.opk_available);
    solua_set_int(L, -1, "spk_id", (lua_Integer)info.spk_id);
    solua_set_str(L, -1, "fingerprint", info.fingerprint);
    return 1;
}

static int solua_sc2_forget(lua_State *L)
{
    solua_sc2_check(L, solar_os_sc2_forget_peer(luaL_checkstring(L, 1)));
    return 0;
}

typedef struct {
    const char *rid;
    uint32_t target;
} solua_sc2_publish_t;

static esp_err_t solua_sc2_publish_fn(void *ctx, char *out, size_t cap, size_t *len)
{
    const solua_sc2_publish_t *p = ctx;
    return solar_os_sc2_keys_publish(p->rid, p->target, out, cap, len);
}

static int solua_sc2_keys_publish(lua_State *L)
{
    solua_sc2_publish_t p = {
        .target = solua_sc2_u32(L, 1, SC2E_OPK_DEFAULT),
        .rid = solua_sc2_rid(L, 2),
    };
    return solua_sc2_frame(L, solua_sc2_publish_fn, &p);
}

static int solua_sc2_add_bundle(lua_State *L)
{
    size_t len = 0;
    const char *line = luaL_checklstring(L, 1, &len);
    char device[SC2_DEVICE_ID_HEX_LEN + 1];
    solua_sc2_check(L, solar_os_sc2_add_bundle(line, len, device));
    lua_pushstring(L, device);
    return 1;
}

typedef struct {
    const char *target;
    const char *peer;
    const char *rid;
    const char *inner;
    size_t inner_len;
} solua_sc2_seal_t;

static esp_err_t solua_sc2_dm_fn(void *ctx, char *out, size_t cap, size_t *len)
{
    const solua_sc2_seal_t *s = ctx;
    return solar_os_sc2_dm_encrypt(s->target, s->rid, s->inner, s->inner_len, out, cap, len);
}

static esp_err_t solua_sc2_room_fn(void *ctx, char *out, size_t cap, size_t *len)
{
    const solua_sc2_seal_t *s = ctx;
    return solar_os_sc2_room_encrypt(s->target, s->rid, s->inner, s->inner_len, out, cap, len);
}

static esp_err_t solua_sc2_share_fn(void *ctx, char *out, size_t cap, size_t *len)
{
    const solua_sc2_seal_t *s = ctx;
    return solar_os_sc2_room_share(s->target, s->peer, s->rid, out, cap, len);
}

static int solua_sc2_dm_encrypt(lua_State *L)
{
    solua_sc2_seal_t s = {.target = luaL_checkstring(L, 1)};
    s.inner = luaL_checklstring(L, 2, &s.inner_len);
    s.rid = solua_sc2_rid(L, 3);
    return solua_sc2_frame(L, solua_sc2_dm_fn, &s);
}

static const char *solua_sc2_dm_kind(int kind)
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

static int solua_sc2_dm_decrypt(lua_State *L)
{
    size_t len = 0, pt_len = 0;
    const char *line = luaL_checklstring(L, 1, &len);
    sc2e_dm_result_t res;
    luaL_Buffer buffer;
    char *pt = luaL_buffinitsize(L, &buffer, SOLAR_OS_SC2_INNER_MAX + 1u);
    const esp_err_t err = solar_os_sc2_dm_decrypt(line, len, &res, pt, SOLAR_OS_SC2_INNER_MAX + 1u, &pt_len);
    luaL_pushresultsize(&buffer, err == ESP_OK ? pt_len : 0);
    if (err != ESP_OK) {
        lua_pop(L, 1);
        return solua_sc2_check(L, err);
    }
    const int body = lua_gettop(L);
    lua_createtable(L, 0, 7);
    solua_set_str(L, -1, "from", res.from);
    solua_set_int(L, -1, "seq", (lua_Integer)res.seq);
    solua_set_str(L, -1, "kind", solua_sc2_dm_kind(res.kind));
    solua_set_bool(L, -1, "new_session", res.new_session);
    if (res.kind == SC2E_DM_MESSAGE) {
        lua_pushvalue(L, body);
        lua_setfield(L, -2, "body");
    } else {
        solua_set_str(L, -1, "room", res.room);
        solua_set_int(L, -1, "epoch", (lua_Integer)res.epoch);
    }
    lua_remove(L, body);
    return 1;
}

static int solua_sc2_room_create(lua_State *L)
{
    char room[SC2_DEVICE_ID_HEX_LEN + 1];
    uint32_t epoch = 0;
    solua_sc2_check(L, solar_os_sc2_room_create(solua_optional_str(L, 1, NULL), room, &epoch));
    lua_createtable(L, 0, 2);
    solua_set_str(L, -1, "room", room);
    solua_set_int(L, -1, "epoch", (lua_Integer)epoch);
    return 1;
}

static int solua_sc2_room_rotate(lua_State *L)
{
    uint32_t epoch = 0;
    solua_sc2_check(L, solar_os_sc2_room_rotate(luaL_checkstring(L, 1), &epoch));
    lua_pushinteger(L, (lua_Integer)epoch);
    return 1;
}

static int solua_sc2_room_share(lua_State *L)
{
    solua_sc2_seal_t s = {
        .target = luaL_checkstring(L, 1),
        .peer = luaL_checkstring(L, 2),
        .rid = solua_sc2_rid(L, 3),
    };
    return solua_sc2_frame(L, solua_sc2_share_fn, &s);
}

static int solua_sc2_room_encrypt(lua_State *L)
{
    solua_sc2_seal_t s = {.target = luaL_checkstring(L, 1)};
    s.inner = luaL_checklstring(L, 2, &s.inner_len);
    s.rid = solua_sc2_rid(L, 3);
    return solua_sc2_frame(L, solua_sc2_room_fn, &s);
}

static int solua_sc2_room_decrypt(lua_State *L)
{
    size_t len = 0, pt_len = 0;
    const char *line = luaL_checklstring(L, 1, &len);
    sc2e_room_result_t res;
    luaL_Buffer buffer;
    char *pt = luaL_buffinitsize(L, &buffer, SOLAR_OS_SC2_INNER_MAX + 1u);
    const esp_err_t err = solar_os_sc2_room_decrypt(line, len, &res, pt, SOLAR_OS_SC2_INNER_MAX + 1u, &pt_len);
    luaL_pushresultsize(&buffer, err == ESP_OK ? pt_len : 0);
    if (err != ESP_OK) {
        lua_pop(L, 1);
        return solua_sc2_check(L, err);
    }
    const int body = lua_gettop(L);
    lua_createtable(L, 0, 6);
    solua_set_str(L, -1, "room", res.room);
    solua_set_str(L, -1, "from", res.from);
    solua_set_int(L, -1, "seq", (lua_Integer)res.seq);
    solua_set_int(L, -1, "epoch", (lua_Integer)res.epoch);
    solua_set_int(L, -1, "n", (lua_Integer)res.n);
    lua_pushvalue(L, body);
    lua_setfield(L, -2, "body");
    lua_remove(L, body);
    return 1;
}

static int solua_sc2_room_info(lua_State *L)
{
    sc2e_room_info_t info;
    solua_sc2_check(L, solar_os_sc2_room_info(luaL_checkstring(L, 1), &info));
    if (!info.known) {
        lua_pushnil(L);
        return 1;
    }
    lua_createtable(L, 0, 5);
    solua_set_str(L, -1, "admin", info.admin);
    solua_set_bool(L, -1, "is_admin", info.is_admin);
    solua_set_int(L, -1, "epoch", (lua_Integer)info.epoch);
    solua_set_int(L, -1, "sent", (lua_Integer)info.sent);
    solua_set_int(L, -1, "senders", (lua_Integer)info.senders);
    return 1;
}

static int solua_sc2_room_forget(lua_State *L)
{
    solua_sc2_check(L, solar_os_sc2_room_forget(luaL_checkstring(L, 1)));
    return 0;
}

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
} solua_sc2_post_t;

static esp_err_t solua_sc2_post_fn(void *ctx, char *out, size_t cap, size_t *len)
{
    const solua_sc2_post_t *p = ctx;
    return solar_os_sc2_post_sign(p->relay, p->room, p->name, p->body, p->body_len, p->ts, p->ref, p->deleted,
                                  p->rid, out, cap, len);
}

/* post_sign(relay, room, name, body, ts=0, ref=nil, delete=false, rid=nil) */
static int solua_sc2_post_sign(lua_State *L)
{
    solua_sc2_post_t p = {
        .relay = luaL_checkstring(L, 1),
        .room = luaL_checkstring(L, 2),
        .name = luaL_checkstring(L, 3),
    };
    p.body = luaL_checklstring(L, 4, &p.body_len);
    if (!lua_isnoneornil(L, 5)) {
        const lua_Integer ts = luaL_checkinteger(L, 5);
        if (ts < 0) {
            return luaL_error(L, "expected non-negative timestamp");
        }
        p.ts = (uint64_t)ts;
    }
    if (p.ts == 0) {
        const time_t now = time(NULL);
        p.ts = now > 0 ? (uint64_t)now : 0u;
    }
    p.ref = solua_optional_str(L, 6, NULL);
    p.deleted = lua_toboolean(L, 7) != 0;
    p.rid = solua_sc2_rid(L, 8);
    return solua_sc2_frame(L, solua_sc2_post_fn, &p);
}

static int solua_sc2_post_verify(lua_State *L)
{
    const char *relay = luaL_checkstring(L, 1);
    size_t len = 0;
    const char *line = luaL_checklstring(L, 2, &len);
    const uint32_t max_body = solua_sc2_u32(L, 3, 1024);
    sc2e_post_result_t res;
    luaL_Buffer buffer;
    char *body = luaL_buffinitsize(L, &buffer, SC2_POST_BODY_MAX + 1u);
    const esp_err_t err = solar_os_sc2_post_verify(relay, line, len, max_body, &res, body, SC2_POST_BODY_MAX + 1u);
    const bool soft = err == ESP_OK || err == SOLAR_OS_SC2_ESP_ERR(SC2_ERR_AUTH) ||
                      err == SOLAR_OS_SC2_ESP_ERR(SC2_ERR_BOUNDS);
    luaL_pushresultsize(&buffer, soft && res.body_len <= SC2_POST_BODY_MAX ? res.body_len : 0);
    if (!soft) {
        lua_pop(L, 1);
        return solua_sc2_check(L, err);
    }
    const int text = lua_gettop(L);
    lua_createtable(L, 0, 10);
    solua_set_bool(L, -1, "valid", res.valid);
    solua_set_str(L, -1, "room", res.room);
    solua_set_str(L, -1, "from", res.from);
    solua_set_str(L, -1, "id", res.id);
    solua_set_str(L, -1, "ref", res.ref);
    solua_set_str(L, -1, "name", res.name);
    solua_set_int(L, -1, "ts", (lua_Integer)res.ts);
    solua_set_int(L, -1, "seq", (lua_Integer)res.seq);
    solua_set_bool(L, -1, "deleted", res.deleted);
    lua_pushvalue(L, text);
    lua_setfield(L, -2, "body");
    lua_remove(L, text);
    return 1;
}

static int solua_sc2_selftest(lua_State *L)
{
    unsigned passed = 0, failed = 0;
    const esp_err_t err = solar_os_sc2_selftest(NULL, NULL, &passed, &failed);
    if (err != ESP_OK && passed == 0 && failed == 0) {
        return solua_sc2_check(L, err);
    }
    lua_createtable(L, 0, 2);
    solua_set_int(L, -1, "passed", (lua_Integer)passed);
    solua_set_int(L, -1, "failed", (lua_Integer)failed);
    return 1;
}
