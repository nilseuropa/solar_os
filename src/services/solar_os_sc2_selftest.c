/*
 * SC2 selftest and benchmark (see solar_os_sc2_selftest.h).
 * SPDX-License-Identifier: MIT
 */
#include "solar_os_sc2_selftest.h"

#include <string.h>

/* Known-answer vectors copied from protocol/sc2/vectors/sc2_vectors.json
 * (RFC 4231/5869/7748/8032, XChaCha20-Poly1305 draft A.3.1 and SC2-1 room
 * and safety-number vectors). Hex strings; decoded at run time. */
#define KAT_SHA256_MSG \
        "616263"
#define KAT_SHA256_OUT \
        "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad"
#define KAT_HMAC_KEY \
        "0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b"
#define KAT_HMAC_MSG \
        "4869205468657265"
#define KAT_HMAC_OUT \
        "b0344c61d8db38535ca8afceaf0bf12b881dc200c9833da726e9376c2e32cff7"
#define KAT_HKDF_IKM \
        "0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b"
#define KAT_HKDF_SALT \
        "000102030405060708090a0b0c"
#define KAT_HKDF_INFO \
        "f0f1f2f3f4f5f6f7f8f9"
#define KAT_HKDF_OKM \
        "3cb25f25faacd57a90434f64d0362f2a2d2d0a90cf1a5a4c5db02d56ecc4c5bf34007208d5b887185865"
#define KAT_ED_SEED \
        "9d61b19deffd5a60ba844af492ec2cc44449c5697b326919703bac031cae7f60"
#define KAT_ED_PK \
        "d75a980182b10ab7d54bfed3c964073a0ee172f3daa62325af021a68f707511a"
#define KAT_ED_SIG \
        "e5564300c360ac729086e2cc806e828a84877f1eb8e5d974d873e065224901555fb8821590a33bacc61e39701cf9b46b"\
        "d25bf5f0595bbe24655141438e7a100b"
#define KAT_ED_BAD_PK \
        "4de5c575ae6d5a2fcd1093013fe23d268867e6c64575979b5404488e3502c75a"
#define KAT_ED_BAD_MSG \
        "5343322073747269637420766572696679"
#define KAT_ED_BAD_SIG \
        "870ce0e681b2807a8e5c3be5444a4529b798308173ec867bdbbb5d9adf429678d6454b01ead9cdbc6f09990222baa620"\
        "095922b6334e635522b98ca06da8ba1c"
#define KAT_X_SCALAR \
        "a546e36bf0527c9d3b16154b82465edd62144c0ac1fc5a18506a2244ba449ac4"
#define KAT_X_U \
        "e6db6867583030db3594c1a424b15f7c726624ec26b3353b10a903a6d0ab1c4c"
#define KAT_X_OUT \
        "c3da55379de9c6908e94ea4df28d084f32eccf03491c71f754b4075577a28552"
#define KAT_XR_SCALAR \
        "966ae7699fcd671da19265482389625937b68d878e9c8aa5b5b07c9f9d2601bd"
#define KAT_XR_U \
        "0000000000000000000000000000000000000000000000000000000000000000"
#define KAT_AEAD_KEY \
        "808182838485868788898a8b8c8d8e8f909192939495969798999a9b9c9d9e9f"
#define KAT_AEAD_NONCE \
        "404142434445464748494a4b4c4d4e4f5051525354555657"
#define KAT_AEAD_AD \
        "50515253c0c1c2c3c4c5c6c7"
#define KAT_AEAD_PT \
        "4c616469657320616e642047656e746c656d656e206f662074686520636c617373206f66202739393a20496620492063"\
        "6f756c64206f6666657220796f75206f6e6c79206f6e652074697020666f7220746865206675747572652c2073756e73"\
        "637265656e20776f756c642062652069742e"
#define KAT_AEAD_CT \
        "bd6d179d3e83d43b9576579493c0e939572a1700252bfaccbed2902c21396cbb731c7f1b0b4aa6440bf3a82f4eda7e39"\
        "ae64c6708c54c216cb96b72e1213b4522f8c9ba40db5d945b11b69b982c1bb9e3f3fac2bc369488f76b2383565d3fff9"\
        "21f9664c97637da9768812f615c68b13b52ec0875924c1c7987947deafd8780acf49"
#define KAT_ROOM_EPOCH_KEY \
        "e18c0c9c9f74fc01670cbaea50f7451048fece818a3aa4acffcfa15272c73ed7"
#define KAT_ROOM_ID \
        "02d7c69bb45bf2bd0c96a8120e4fef46"
#define KAT_ROOM_SENDER \
        "34d7609a3fcf7dda1bd3614b512ceda0"
#define KAT_ROOM_CK0 \
        "8b81ef0fa0a07626f8563ac6c850145475d1b632b7a2a647f74aa9fb446feec1"
#define KAT_ROOM_MK2 \
        "2e75f03b7dbe51f32f463fcd956b401aa7681219862828dc758e8c788a154283"
#define KAT_ROOM_HDR \
        "534332520102d7c69bb45bf2bd0c96a8120e4fef460000000634d7609a3fcf7dda1bd3614b512ceda000000002"
#define KAT_ROOM_IK_PUB \
        "f14025706aa10110b3cafb24d097191635c61d199d65bfe69d68bac6bae1fdbe"
#define KAT_ROOM_NONCE \
        "9a78fe4135b9ca99f0fb16b9eaa4ac487f30ebdc1e0f95b0"
#define KAT_ROOM_PT \
        "7b2274223a2274657874222c226964223a2230303030303030303030303030306161222c22626f6479223a2268656c6c"\
        "6f20726f6f6d222c227473223a313736303838393630307d"
#define KAT_ROOM_CT \
        "56e1d9557a02c7a3dbdb53d7787548bcc132ca428d79658e5e4c13164ed11998e86ea7b1d0636e9c5a7372834cd0d7ae"\
        "5138f12d4fcdde34de95c67a25082060e999ae04fb3d2bf751dd6a5e08513665dc9305b7c8ff91c34cf7fb77083ffddf"\
        "c715590bf8fe7a16fc299bb83bec38eced102040e891986656ec4c924d9d992fcb10e3e66383d54f940650cf1c76c93a"\
        "3ca7b747b18893fcc884cd5e75f3e2d02a91cc9bd28c7ae12ed5f9353e43ac36a51af82397e19dc4e795ed2552a8842e"\
        "a41a409d7057d8b369e827b2a9cdad09f75f15fe01e832444794e10b84facf1552dac846605a0c86efe74cb8cf42eee5"\
        "dff22c1e11776a95fa4e85eb7b925109ea6433ee62c857d5039b630894fca234"
#define KAT_ROOM_SIG \
        "f3e3de8f10b0726cc840daaf3b6eedb944ace50befd78b9dcb0112770cb62921c327e0d5926448fdf6801b2a22bbe4d7"\
        "9ff2b4239d82f21e9ab49f98d2265509"
#define KAT_SAFETY_IK_A \
        "f90d24bf2101cc7aa12f75e15ecfe42cf701d7239330b35736c59417d62eae52"
#define KAT_SAFETY_IK_B \
        "bd3203abe451d5e3c0117684b40d4c1a075a74650fbfe29a0c697b2ea804e6ea"
#define KAT_HKDF_LEN 42u
#define KAT_ROOM_EPOCH 6u
#define KAT_SAFETY_SHORT "3060 0334 6920 4696 9171 8491"
#define KAT_SAFETY_FULL "98929 32338 23643 29959 17705 34853 08785 56577 42121 34844 57856 54174"
#define KAT_SAFETY_WORDS "pink-suit-galaxy-portion-arm-cry-blame-silly"

#define HI_BOB "{\"t\":\"text\",\"body\":\"hi bob\"}"
#define FORGED "{\"t\":\"epoch_key\"}"
#define ROOM_PING "{\"t\":\"text\",\"body\":\"room ping\"}"
#define TEXT_ONLY "{\"t\":\"text\"}"
#define MEM_RECORDS 40u
#define MEM_RECORD_CAP 4096u
#define FIXED_NOW 1760889600u

struct sc2e_memstore {
    sc2e_test_host_t host;
    uint64_t now;
    struct {
        char name[SC2E_NAME_MAX];
        uint8_t *buf;
        size_t len;
    } rec[MEM_RECORDS];
};

/* ---------------------------------------------------------------------- */
/* In-memory store                                                         */
/* ---------------------------------------------------------------------- */

static int mem_find(const sc2e_memstore_t *m, const char *name)
{
    for (size_t i = 0; i < MEM_RECORDS; i++) {
        if (m->rec[i].buf != NULL && strcmp(m->rec[i].name, name) == 0) {
            return (int)i;
        }
    }
    return -1;
}

static int mem_load(void *ctx, const char *name, uint8_t *buf, size_t cap, size_t *len)
{
    sc2e_memstore_t *m = ctx;
    int i = mem_find(m, name);
    if (i < 0) {
        return SC2_ERR_NOT_FOUND;
    }
    if (m->rec[i].len > cap) {
        return SC2_ERR_BOUNDS;
    }
    memcpy(buf, m->rec[i].buf, m->rec[i].len);
    *len = m->rec[i].len;
    return SC2_OK;
}

static int mem_save(void *ctx, const char *name, const uint8_t *buf, size_t len)
{
    sc2e_memstore_t *m = ctx;
    if (strlen(name) >= SC2E_NAME_MAX || len > MEM_RECORD_CAP) {
        return SC2_ERR_BOUNDS;
    }
    int i = mem_find(m, name);
    if (i < 0) {
        for (size_t j = 0; j < MEM_RECORDS && i < 0; j++) {
            if (m->rec[j].buf == NULL) {
                i = (int)j;
            }
        }
        if (i < 0) {
            return SC2_ERR_BOUNDS;
        }
        m->rec[i].buf = m->host.alloc(m->host.ctx, MEM_RECORD_CAP);
        if (m->rec[i].buf == NULL) {
            return SC2_ERR_BOUNDS;
        }
        strcpy(m->rec[i].name, name);
    }
    memcpy(m->rec[i].buf, buf, len);
    m->rec[i].len = len;
    return SC2_OK;
}

static int mem_remove(void *ctx, const char *name)
{
    sc2e_memstore_t *m = ctx;
    int i = mem_find(m, name);
    if (i >= 0) {
        m->host.release(m->host.ctx, m->rec[i].buf, MEM_RECORD_CAP);
        memset(&m->rec[i], 0, sizeof(m->rec[i]));
    }
    return SC2_OK;
}

static int mem_count(void *ctx, const char *dir, size_t *count)
{
    sc2e_memstore_t *m = ctx;
    size_t n = strlen(dir);
    *count = 0;
    for (size_t i = 0; i < MEM_RECORDS; i++) {
        if (m->rec[i].buf != NULL && strncmp(m->rec[i].name, dir, n) == 0 && m->rec[i].name[n] == '/') {
            (*count)++;
        }
    }
    return SC2_OK;
}

static void *mem_alloc(void *ctx, size_t len)
{
    sc2e_memstore_t *m = ctx;
    return m->host.alloc(m->host.ctx, len);
}

static void mem_release(void *ctx, void *ptr, size_t len)
{
    sc2e_memstore_t *m = ctx;
    m->host.release(m->host.ctx, ptr, len);
}

static uint64_t mem_now(void *ctx)
{
    sc2e_memstore_t *m = ctx;
    return m->now;
}

int sc2e_memstore_create(sc2e_memstore_t **out, const sc2e_test_host_t *host)
{
    if (out == NULL || host == NULL || host->alloc == NULL || host->release == NULL) {
        return SC2_ERR_ARG;
    }
    sc2e_memstore_t *m = host->alloc(host->ctx, sizeof(*m));
    if (m == NULL) {
        return SC2_ERR_BOUNDS;
    }
    m->host = *host;
    m->now = FIXED_NOW;
    *out = m;
    return SC2_OK;
}

void sc2e_memstore_destroy(sc2e_memstore_t *m)
{
    if (m == NULL) {
        return;
    }
    sc2e_test_host_t host = m->host;
    for (size_t i = 0; i < MEM_RECORDS; i++) {
        if (m->rec[i].buf != NULL) {
            host.release(host.ctx, m->rec[i].buf, MEM_RECORD_CAP);
        }
    }
    host.release(host.ctx, m, sizeof(*m));
}

void sc2e_memstore_platform(sc2e_memstore_t *m, sc2e_platform_t *pf)
{
    memset(pf, 0, sizeof(*pf));
    pf->load = mem_load;
    pf->save = mem_save;
    pf->remove = mem_remove;
    pf->count = mem_count;
    pf->alloc = mem_alloc;
    pf->release = mem_release;
    pf->now = mem_now;
    pf->rng = m->host.rng;
    pf->ctx = m;
}

uint8_t *sc2e_memstore_record(sc2e_memstore_t *m, const char *name, size_t *len)
{
    int i = mem_find(m, name);
    if (i < 0) {
        return NULL;
    }
    if (len) {
        *len = m->rec[i].len;
    }
    return m->rec[i].buf;
}

/* ---------------------------------------------------------------------- */
/* Relay emulation                                                         */
/* ---------------------------------------------------------------------- */

typedef struct {
    char *out;
    size_t cap;
    size_t len;
    int err;
} sb_t;

static void sb_raw(sb_t *b, const char *s, size_t n)
{
    if (b->err) {
        return;
    }
    if (b->cap - b->len <= n) {
        b->err = SC2_ERR_BOUNDS;
        return;
    }
    memcpy(b->out + b->len, s, n);
    b->len += n;
    b->out[b->len] = '\0';
}

static void sb_str(sb_t *b, const char *s)
{
    sb_raw(b, s, strlen(s));
}

static void sb_u64(sb_t *b, uint64_t v)
{
    char tmp[24];
    size_t n = 0;
    do {
        tmp[n++] = (char)('0' + (v % 10u));
        v /= 10u;
    } while (v != 0 && n < sizeof(tmp));
    for (size_t i = 0; i < n / 2; i++) {
        char c = tmp[i];
        tmp[i] = tmp[n - 1 - i];
        tmp[n - 1 - i] = c;
    }
    sb_raw(b, tmp, n);
}

static void sb_val(sb_t *b, const sc2_json_val_t *v)
{
    sb_raw(b, v->ptr, v->len);
}

static int sb_finish(sb_t *b, size_t *out_len)
{
    if (b->err == SC2_OK && out_len) {
        *out_len = b->len;
    }
    return b->err;
}

static int relay_fanout(const char *send, size_t len, const char *want, const char *type, const char *body_key,
                        bool with_room, uint64_t seq, char *out, size_t cap, size_t *out_len)
{
    sc2_json_val_t root, body, room;
    char t[SC2_TYPE_MAX + 1];
    if (out == NULL || cap == 0) {
        return SC2_ERR_ARG;
    }
    int rc = sc2_frame_peek(send, len, &root, t, NULL);
    if (rc == SC2_OK && strcmp(t, want) != 0) {
        rc = SC2_ERR_FORMAT;
    }
    if (rc == SC2_OK) {
        rc = sc2_json_get(&root, body_key, &body);
    }
    if (rc == SC2_OK && with_room) {
        rc = sc2_json_get(&root, "room", &room);
    }
    if (rc != SC2_OK) {
        return rc;
    }
    sb_t b = {out, cap, 0, 0};
    out[0] = '\0';
    sb_str(&b, "{\"type\":\"");
    sb_str(&b, type);
    sb_str(&b, "\"");
    if (with_room) {
        sb_str(&b, ",\"room\":");
        sb_val(&b, &room);
    }
    sb_str(&b, ",\"seq\":\"");
    sb_u64(&b, seq);
    sb_str(&b, "\",\"");
    sb_str(&b, body_key);
    sb_str(&b, "\":");
    sb_val(&b, &body);
    sb_str(&b, "}");
    return sb_finish(&b, out_len);
}

int sc2e_relay_dm(const char *send, size_t len, uint64_t seq, char *out, size_t cap, size_t *out_len)
{
    return relay_fanout(send, len, "dm.send", "dm.msg", "env", false, seq, out, cap, out_len);
}

int sc2e_relay_room(const char *send, size_t len, uint64_t seq, char *out, size_t cap, size_t *out_len)
{
    return relay_fanout(send, len, "room.send", "room.msg", "env", true, seq, out, cap, out_len);
}

int sc2e_relay_post(const char *send, size_t len, uint64_t seq, char *out, size_t cap, size_t *out_len)
{
    return relay_fanout(send, len, "room.post", "room.pub", "post", true, seq, out, cap, out_len);
}

int sc2e_relay_bundle(const char *publish, size_t len, const sc2e_identity_info_t *owner, char *out, size_t cap,
                      size_t *out_len)
{
    sc2_json_val_t root, dk, dk_sig, spk, opks, opk;
    char t[SC2_TYPE_MAX + 1];
    size_t pos = 0;
    if (owner == NULL || out == NULL || cap == 0) {
        return SC2_ERR_ARG;
    }
    int rc = sc2_frame_peek(publish, len, &root, t, NULL);
    if (rc == SC2_OK && strcmp(t, "keys.publish") != 0) {
        rc = SC2_ERR_FORMAT;
    }
    if (rc == SC2_OK) {
        rc = sc2_json_get(&root, "dk", &dk);
    }
    if (rc == SC2_OK) {
        rc = sc2_json_get(&root, "dk_sig", &dk_sig);
    }
    if (rc == SC2_OK) {
        rc = sc2_json_get(&root, "spk", &spk);
    }
    if (rc == SC2_OK) {
        rc = sc2_json_get(&root, "opks", &opks);
    }
    if (rc != SC2_OK) {
        return rc;
    }
    bool has_opk = opks.type == SC2_JSON_ARRAY && sc2_json_array_next(&opks, &pos, &opk) == SC2_OK;
    sb_t b = {out, cap, 0, 0};
    out[0] = '\0';
    sb_str(&b, "{\"type\":\"keys.bundle\",\"device\":\"");
    sb_str(&b, owner->device_id);
    sb_str(&b, "\",\"ik\":\"");
    sb_str(&b, owner->ik_b64);
    sb_str(&b, "\",\"dk\":");
    sb_val(&b, &dk);
    sb_str(&b, ",\"dk_sig\":");
    sb_val(&b, &dk_sig);
    sb_str(&b, ",\"spk\":");
    sb_val(&b, &spk);
    if (has_opk) {
        sb_str(&b, ",\"opk\":");
        sb_val(&b, &opk);
    }
    sb_str(&b, "}");
    return sb_finish(&b, out_len);
}

/* ---------------------------------------------------------------------- */
/* Known-answer tests                                                      */
/* ---------------------------------------------------------------------- */

typedef struct {
    uint8_t a[512];
    uint8_t b[512];
    uint8_t c[512];
    uint8_t d[512];
    uint8_t e[512];
} kat_ws_t;

static int unhex(uint8_t *out, size_t cap, const char *hex, size_t *len)
{
    size_t n = strlen(hex);
    if (n % 2u != 0 || n / 2u > cap) {
        return SC2_ERR_BOUNDS;
    }
    if (len) {
        *len = n / 2u;
    }
    return sc2_hex_decode(out, n / 2u, hex, n);
}

static int expect_eq(const uint8_t *got, const char *hex, uint8_t *scratch, size_t cap)
{
    size_t n = 0;
    int rc = unhex(scratch, cap, hex, &n);
    if (rc == SC2_OK && memcmp(got, scratch, n) != 0) {
        rc = SC2_ERR_AUTH;
    }
    return rc;
}

static int kat_sha256(kat_ws_t *w)
{
    size_t n = 0;
    int rc = unhex(w->a, sizeof(w->a), KAT_SHA256_MSG, &n);
    if (rc == SC2_OK) {
        rc = sc2_sha256(w->b, w->a, n);
    }
    return rc == SC2_OK ? expect_eq(w->b, KAT_SHA256_OUT, w->c, sizeof(w->c)) : rc;
}

static int kat_hmac(kat_ws_t *w)
{
    size_t kn = 0, mn = 0;
    int rc = unhex(w->a, sizeof(w->a), KAT_HMAC_KEY, &kn);
    if (rc == SC2_OK) {
        rc = unhex(w->b, sizeof(w->b), KAT_HMAC_MSG, &mn);
    }
    if (rc == SC2_OK) {
        rc = sc2_hmac_sha256(w->c, w->a, kn, w->b, mn);
    }
    return rc == SC2_OK ? expect_eq(w->c, KAT_HMAC_OUT, w->d, sizeof(w->d)) : rc;
}

static int kat_hkdf(kat_ws_t *w)
{
    size_t in = 0, sn = 0, fn = 0;
    int rc = unhex(w->a, sizeof(w->a), KAT_HKDF_IKM, &in);
    if (rc == SC2_OK) {
        rc = unhex(w->b, sizeof(w->b), KAT_HKDF_SALT, &sn);
    }
    if (rc == SC2_OK) {
        rc = unhex(w->c, sizeof(w->c), KAT_HKDF_INFO, &fn);
    }
    if (rc == SC2_OK) {
        rc = sc2_hkdf(w->d, KAT_HKDF_LEN, w->b, sn, w->a, in, w->c, fn);
    }
    return rc == SC2_OK ? expect_eq(w->d, KAT_HKDF_OKM, w->e, sizeof(w->e)) : rc;
}

static int kat_ed25519(kat_ws_t *w)
{
    int rc = unhex(w->a, sizeof(w->a), KAT_ED_SEED, NULL);
    if (rc == SC2_OK) {
        rc = sc2_ed25519_public(w->b, w->a);
    }
    if (rc == SC2_OK) {
        rc = expect_eq(w->b, KAT_ED_PK, w->d, sizeof(w->d));
    }
    if (rc == SC2_OK) {
        rc = sc2_ed25519_sign(w->c, NULL, 0, w->a);
    }
    if (rc == SC2_OK) {
        rc = expect_eq(w->c, KAT_ED_SIG, w->d, sizeof(w->d));
    }
    if (rc == SC2_OK) {
        rc = sc2_ed25519_verify(w->c, NULL, 0, w->b);
    }
    return rc;
}

static int kat_ed25519_strict(kat_ws_t *w)
{
    size_t mn = 0;
    int rc = unhex(w->a, sizeof(w->a), KAT_ED_BAD_PK, NULL);
    if (rc == SC2_OK) {
        rc = unhex(w->b, sizeof(w->b), KAT_ED_BAD_MSG, &mn);
    }
    if (rc == SC2_OK) {
        rc = unhex(w->c, sizeof(w->c), KAT_ED_BAD_SIG, NULL);
    }
    if (rc == SC2_OK) {
        /* S + L is malleated and must be rejected. */
        rc = sc2_ed25519_verify(w->c, w->b, mn, w->a) == SC2_ERR_AUTH ? SC2_OK : SC2_ERR_AUTH;
    }
    return rc;
}

static int kat_x25519(kat_ws_t *w)
{
    int rc = unhex(w->a, sizeof(w->a), KAT_X_SCALAR, NULL);
    if (rc == SC2_OK) {
        rc = unhex(w->b, sizeof(w->b), KAT_X_U, NULL);
    }
    if (rc == SC2_OK) {
        rc = sc2_x25519(w->c, w->a, w->b);
    }
    if (rc == SC2_OK) {
        rc = expect_eq(w->c, KAT_X_OUT, w->d, sizeof(w->d));
    }
    if (rc == SC2_OK) {
        rc = unhex(w->a, sizeof(w->a), KAT_XR_SCALAR, NULL);
    }
    if (rc == SC2_OK) {
        rc = unhex(w->b, sizeof(w->b), KAT_XR_U, NULL);
    }
    if (rc == SC2_OK) {
        rc = sc2_x25519(w->c, w->a, w->b) == SC2_ERR_KEY ? SC2_OK : SC2_ERR_KEY;
    }
    return rc;
}

static int kat_aead(kat_ws_t *w)
{
    size_t an = 0, pn = 0;
    uint8_t key[SC2_KEY_LEN];
    uint8_t nonce[SC2_NONCE_LEN];
    int rc = unhex(key, sizeof(key), KAT_AEAD_KEY, NULL);
    if (rc == SC2_OK) {
        rc = unhex(nonce, sizeof(nonce), KAT_AEAD_NONCE, NULL);
    }
    if (rc == SC2_OK) {
        rc = unhex(w->a, sizeof(w->a), KAT_AEAD_AD, &an);
    }
    if (rc == SC2_OK) {
        rc = unhex(w->b, sizeof(w->b), KAT_AEAD_PT, &pn);
    }
    if (rc == SC2_OK) {
        rc = sc2_aead_seal(w->c, w->b, pn, w->a, an, nonce, key);
    }
    if (rc == SC2_OK) {
        rc = expect_eq(w->c, KAT_AEAD_CT, w->d, sizeof(w->d));
    }
    if (rc == SC2_OK) {
        rc = sc2_aead_open(w->e, w->c, pn + SC2_TAG_LEN, w->a, an, nonce, key);
    }
    if (rc == SC2_OK && memcmp(w->e, w->b, pn) != 0) {
        rc = SC2_ERR_AUTH;
    }
    if (rc == SC2_OK) {
        w->c[3] ^= 0x01u;
        rc = sc2_aead_open(w->e, w->c, pn + SC2_TAG_LEN, w->a, an, nonce, key) == SC2_ERR_AUTH ? SC2_OK
                                                                                                : SC2_ERR_AUTH;
    }
    sc2_memzero(key, sizeof(key));
    return rc;
}

static int kat_room(kat_ws_t *w)
{
    uint8_t key[SC2_KEY_LEN], room[SC2_ROOM_ID_LEN], sender[SC2_DEVICE_ID_LEN], ik[SC2_KEY_LEN];
    uint8_t nonce[SC2_NONCE_LEN], sig[SC2_SIG_LEN], mk[SC2_KEY_LEN], hdr[SC2_ROOM_HDR_LEN];
    sc2_room_chain_t chain;
    size_t ctn = 0, ptn = 0, got = 0;
    int rc = unhex(key, sizeof(key), KAT_ROOM_EPOCH_KEY, NULL);
    if (rc == SC2_OK) {
        rc = unhex(room, sizeof(room), KAT_ROOM_ID, NULL);
    }
    if (rc == SC2_OK) {
        rc = unhex(sender, sizeof(sender), KAT_ROOM_SENDER, NULL);
    }
    if (rc == SC2_OK) {
        rc = sc2_room_chain_init(&chain, key, room, KAT_ROOM_EPOCH, sender);
    }
    if (rc == SC2_OK) {
        rc = expect_eq(chain.ck, KAT_ROOM_CK0, w->a, sizeof(w->a));
    }
    if (rc == SC2_OK) {
        rc = sc2_room_chain_key(&chain, 2, mk, 1);
    }
    if (rc == SC2_OK) {
        rc = expect_eq(mk, KAT_ROOM_MK2, w->a, sizeof(w->a));
    }
    if (rc == SC2_OK) {
        rc = sc2_room_header(hdr, room, KAT_ROOM_EPOCH, sender, 2);
    }
    if (rc == SC2_OK) {
        rc = expect_eq(hdr, KAT_ROOM_HDR, w->a, sizeof(w->a));
    }
    if (rc == SC2_OK) {
        rc = unhex(ik, sizeof(ik), KAT_ROOM_IK_PUB, NULL);
    }
    if (rc == SC2_OK) {
        rc = unhex(nonce, sizeof(nonce), KAT_ROOM_NONCE, NULL);
    }
    if (rc == SC2_OK) {
        rc = unhex(sig, sizeof(sig), KAT_ROOM_SIG, NULL);
    }
    if (rc == SC2_OK) {
        rc = unhex(w->b, sizeof(w->b), KAT_ROOM_CT, &ctn);
    }
    if (rc == SC2_OK) {
        rc = unhex(w->c, sizeof(w->c), KAT_ROOM_PT, &ptn);
    }
    if (rc == SC2_OK) {
        rc = sc2_room_open(w->d, sizeof(w->d), &got, mk, nonce, hdr, w->b, ctn, sig, ik);
    }
    if (rc == SC2_OK && (got != ptn || memcmp(w->d, w->c, ptn) != 0)) {
        rc = SC2_ERR_AUTH;
    }
    if (rc == SC2_OK) {
        /* A replayed index must be refused by the chain. */
        rc = sc2_room_chain_key(&chain, 2, mk, 0) == SC2_ERR_REPLAY ? SC2_OK : SC2_ERR_REPLAY;
    }
    sc2_memzero(key, sizeof(key));
    sc2_memzero(mk, sizeof(mk));
    sc2_memzero(&chain, sizeof(chain));
    return rc;
}

static int kat_safety(kat_ws_t *w)
{
    uint8_t a[SC2_KEY_LEN], b[SC2_KEY_LEN], h[32];
    char s[SC2_SAFETY_FULL_LEN + 1];
    char words[SC2_SAFETY_WORDS_MAX + 1];
    (void)w;
    int rc = unhex(a, sizeof(a), KAT_SAFETY_IK_A, NULL);
    if (rc == SC2_OK) {
        rc = unhex(b, sizeof(b), KAT_SAFETY_IK_B, NULL);
    }
    if (rc == SC2_OK) {
        rc = sc2_safety_hash(h, b, a); /* order independent */
    }
    if (rc == SC2_OK) {
        rc = sc2_safety_short(s, h);
    }
    if (rc == SC2_OK && strcmp(s, KAT_SAFETY_SHORT) != 0) {
        rc = SC2_ERR_AUTH;
    }
    if (rc == SC2_OK) {
        rc = sc2_safety_full(s, h);
    }
    if (rc == SC2_OK && strcmp(s, KAT_SAFETY_FULL) != 0) {
        rc = SC2_ERR_AUTH;
    }
    if (rc == SC2_OK) {
        rc = sc2_safety_words(words, sizeof(words), h, '-');
    }
    if (rc == SC2_OK && strcmp(words, KAT_SAFETY_WORDS) != 0) {
        rc = SC2_ERR_AUTH;
    }
    return rc;
}

static int kat_records(kat_ws_t *w)
{
    const uint8_t *p = NULL;
    size_t n = 0, total = 0;
    memcpy(w->b, "payload", 7);
    int rc = sc2e_record_wrap(w->a, sizeof(w->a), &total, SC2E_REC_PEER, 1, w->b, 7);
    if (rc == SC2_OK) {
        rc = sc2e_record_unwrap(w->a, total, SC2E_REC_PEER, 1, &p, &n);
    }
    if (rc == SC2_OK && (n != 7 || memcmp(p, "payload", 7) != 0)) {
        rc = SC2_ERR_FORMAT;
    }
    if (rc == SC2_OK) {
        rc = sc2e_record_unwrap(w->a, total, SC2E_REC_DM, 1, &p, &n) == SC2_ERR_FORMAT ? SC2_OK : SC2_ERR_FORMAT;
    }
    if (rc == SC2_OK) {
        w->a[13] ^= 0x40u;
        rc = sc2e_record_unwrap(w->a, total, SC2E_REC_PEER, 1, &p, &n) == SC2_ERR_FORMAT ? SC2_OK : SC2_ERR_FORMAT;
    }
    return rc;
}

/* ---------------------------------------------------------------------- */
/* Two-device engine flow                                                  */
/* ---------------------------------------------------------------------- */

typedef struct {
    sc2e_t alice;
    sc2e_t bob;
    sc2e_identity_info_t ai;
    sc2e_identity_info_t bi;
    char f1[SC2E_FRAME_MAX];
    char f2[SC2E_FRAME_MAX];
    char pt[SC2E_INNER_MAX + 1];
    char body[SC2_POST_BODY_MAX + 1];
} flow_ws_t;

typedef struct {
    sc2e_memstore_t *a;
    sc2e_memstore_t *b;
    flow_ws_t *w;
    uint64_t seq;
} flow_t;

static int exchange_bundles(flow_t *f)
{
    flow_ws_t *w = f->w;
    size_t n = 0, m = 0;
    char dev[SC2_DEVICE_ID_HEX_LEN + 1];
    int rc = sc2e_identity(&w->alice, &w->ai);
    if (rc == SC2_OK) {
        rc = sc2e_identity(&w->bob, &w->bi);
    }
    if (rc == SC2_OK) {
        rc = sc2e_keys_publish(&w->bob, "k1", 4, w->f1, sizeof(w->f1), &n);
    }
    if (rc == SC2_OK) {
        rc = sc2e_relay_bundle(w->f1, n, &w->bi, w->f2, sizeof(w->f2), &m);
    }
    if (rc == SC2_OK) {
        rc = sc2e_add_bundle(&w->alice, w->f2, m, dev);
    }
    if (rc == SC2_OK && strcmp(dev, w->bi.device_id) != 0) {
        rc = SC2_ERR_STATE;
    }
    if (rc == SC2_OK) {
        rc = sc2e_keys_publish(&w->alice, NULL, 2, w->f1, sizeof(w->f1), &n);
    }
    if (rc == SC2_OK) {
        rc = sc2e_relay_bundle(w->f1, n, &w->ai, w->f2, sizeof(w->f2), &m);
    }
    if (rc == SC2_OK) {
        rc = sc2e_add_bundle(&w->bob, w->f2, m, dev);
    }
    return rc;
}

/* from sends inner to `to` (device hex) and to decrypts; expect kind. */
static int dm_roundtrip(flow_t *f, sc2e_t *from, sc2e_t *to, const char *to_hex, const char *inner, int kind,
                        bool expect_new)
{
    flow_ws_t *w = f->w;
    sc2e_dm_result_t res;
    size_t n = 0, m = 0, pl = 0;
    int rc = sc2e_dm_encrypt(from, to_hex, "d1", inner, strlen(inner), w->f1, sizeof(w->f1), &n);
    if (rc == SC2_OK) {
        rc = sc2e_relay_dm(w->f1, n, ++f->seq, w->f2, sizeof(w->f2), &m);
    }
    if (rc == SC2_OK) {
        rc = sc2e_dm_decrypt(to, w->f2, m, &res, w->pt, sizeof(w->pt), &pl);
    }
    if (rc == SC2_OK && (res.kind != kind || res.new_session != expect_new || strcmp(res.from, from->self_hex) != 0 ||
                         res.seq != f->seq)) {
        rc = SC2_ERR_STATE;
    }
    if (rc == SC2_OK && kind == SC2E_DM_MESSAGE && (pl != strlen(inner) || strcmp(w->pt, inner) != 0)) {
        rc = SC2_ERR_AUTH;
    }
    return rc;
}

static int flow_dm(flow_t *f)
{
    flow_ws_t *w = f->w;
    sc2e_safety_t sa, sb;
    sc2e_dm_result_t res;
    size_t n = 0, m = 0, pl = 0;
    int rc = exchange_bundles(f);
    if (rc == SC2_OK) {
        rc = sc2e_dm_encrypt(&w->alice, w->bi.device_id, "d1", HI_BOB, strlen(HI_BOB), w->f1,
                             sizeof(w->f1), &n);
    }
    if (rc == SC2_OK) {
        rc = sc2e_relay_dm(w->f1, n, ++f->seq, w->f2, sizeof(w->f2), &m);
    }
    if (rc == SC2_OK) {
        rc = sc2e_dm_decrypt(&w->bob, w->f2, m, &res, w->pt, sizeof(w->pt), &pl);
    }
    if (rc == SC2_OK && (!res.new_session || strcmp(w->pt, HI_BOB) != 0)) {
        rc = SC2_ERR_STATE;
    }
    if (rc == SC2_OK) {
        /* Replaying the X3DH message must not open a second session. */
        rc = sc2e_dm_decrypt(&w->bob, w->f2, m, &res, w->pt, sizeof(w->pt), &pl) != SC2_OK ? SC2_OK : SC2_ERR_REPLAY;
    }
    if (rc == SC2_OK) {
        rc = dm_roundtrip(f, &w->bob, &w->alice, w->ai.device_id, "{\"t\":\"text\",\"body\":\"hi alice\"}",
                          SC2E_DM_MESSAGE, false);
    }
    for (int i = 0; rc == SC2_OK && i < 3; i++) {
        rc = dm_roundtrip(f, &w->alice, &w->bob, w->bi.device_id, "{\"t\":\"text\",\"body\":\"again\"}",
                          SC2E_DM_MESSAGE, false);
    }
    if (rc == SC2_OK) {
        /* Scripts may not forge key-carrying control messages. */
        rc = sc2e_dm_encrypt(&w->alice, w->bi.device_id, NULL, FORGED, strlen(FORGED), w->f1, sizeof(w->f1),
                             &n) == SC2_ERR_ARG
                 ? SC2_OK
                 : SC2_ERR_STATE;
    }
    if (rc == SC2_OK) {
        rc = sc2e_dm_encrypt(&w->alice, w->bi.device_id, NULL, "not json", 8, w->f1, sizeof(w->f1), &n) != SC2_OK
                 ? SC2_OK
                 : SC2_ERR_STATE;
    }
    if (rc == SC2_OK) {
        rc = sc2e_safety(&w->alice, w->bi.device_id, &sa);
    }
    if (rc == SC2_OK) {
        rc = sc2e_safety(&w->bob, w->ai.device_id, &sb);
    }
    if (rc == SC2_OK && (strcmp(sa.short_form, sb.short_form) != 0 || strcmp(sa.words, sb.words) != 0)) {
        rc = SC2_ERR_AUTH;
    }
    return rc;
}

static int flow_room(flow_t *f)
{
    flow_ws_t *w = f->w;
    char room[SC2_DEVICE_ID_HEX_LEN + 1];
    sc2e_room_result_t rr;
    sc2e_dm_result_t dr;
    sc2e_room_info_t info;
    uint32_t epoch = 0;
    size_t n = 0, m = 0, pl = 0;
    const char *msg = "{\"t\":\"text\",\"body\":\"hello room\"}";
    int rc = sc2e_room_create(&w->alice, NULL, room, &epoch);
    if (rc == SC2_OK && epoch != 1) {
        rc = SC2_ERR_STATE;
    }
    if (rc == SC2_OK) {
        rc = sc2e_room_share(&w->alice, room, w->bi.device_id, "s1", w->f1, sizeof(w->f1), &n);
    }
    if (rc == SC2_OK) {
        rc = sc2e_relay_dm(w->f1, n, ++f->seq, w->f2, sizeof(w->f2), &m);
    }
    if (rc == SC2_OK) {
        rc = sc2e_dm_decrypt(&w->bob, w->f2, m, &dr, w->pt, sizeof(w->pt), &pl);
    }
    if (rc == SC2_OK && (dr.kind != SC2E_DM_EPOCH_KEY || dr.epoch != 1 || strcmp(dr.room, room) != 0 || pl != 0)) {
        rc = SC2_ERR_STATE;
    }
    for (int i = 0; rc == SC2_OK && i < 2; i++) {
        rc = sc2e_room_encrypt(&w->alice, room, "r1", msg, strlen(msg), w->f1, sizeof(w->f1), &n);
        if (rc == SC2_OK) {
            rc = sc2e_relay_room(w->f1, n, ++f->seq, w->f2, sizeof(w->f2), &m);
        }
        if (rc == SC2_OK) {
            rc = sc2e_room_decrypt(&w->bob, w->f2, m, &rr, w->pt, sizeof(w->pt), &pl);
        }
        if (rc == SC2_OK && (strcmp(w->pt, msg) != 0 || rr.n != (uint32_t)i || rr.epoch != 1)) {
            rc = SC2_ERR_AUTH;
        }
    }
    if (rc == SC2_OK) {
        /* The same room.msg twice is a replay. */
        rc = sc2e_room_decrypt(&w->bob, w->f2, m, &rr, w->pt, sizeof(w->pt), &pl) == SC2_ERR_REPLAY ? SC2_OK
                                                                                                    : SC2_ERR_STATE;
    }
    if (rc == SC2_OK) {
        rc = sc2e_room_encrypt(&w->bob, room, NULL, msg, strlen(msg), w->f1, sizeof(w->f1), &n);
    }
    if (rc == SC2_OK) {
        rc = sc2e_relay_room(w->f1, n, ++f->seq, w->f2, sizeof(w->f2), &m);
    }
    if (rc == SC2_OK) {
        rc = sc2e_room_decrypt(&w->alice, w->f2, m, &rr, w->pt, sizeof(w->pt), &pl);
    }
    if (rc == SC2_OK && strcmp(rr.from, w->bi.device_id) != 0) {
        rc = SC2_ERR_STATE;
    }
    if (rc == SC2_OK) {
        /* Only the admin distributes epoch keys. */
        rc = sc2e_room_share(&w->bob, room, w->ai.device_id, NULL, w->f1, sizeof(w->f1), &n) == SC2_ERR_STATE
                 ? SC2_OK
                 : SC2_ERR_STATE;
    }
    if (rc == SC2_OK) {
        rc = sc2e_room_rotate(&w->alice, room, &epoch);
    }
    if (rc == SC2_OK && epoch != 2) {
        rc = SC2_ERR_STATE;
    }
    if (rc == SC2_OK) {
        rc = sc2e_room_share(&w->alice, room, w->bi.device_id, NULL, w->f1, sizeof(w->f1), &n);
    }
    if (rc == SC2_OK) {
        rc = sc2e_relay_dm(w->f1, n, ++f->seq, w->f2, sizeof(w->f2), &m);
    }
    if (rc == SC2_OK) {
        rc = sc2e_dm_decrypt(&w->bob, w->f2, m, &dr, w->pt, sizeof(w->pt), &pl);
    }
    if (rc == SC2_OK && (dr.kind != SC2E_DM_EPOCH_KEY || dr.epoch != 2)) {
        rc = SC2_ERR_STATE;
    }
    if (rc == SC2_OK) {
        rc = sc2e_room_encrypt(&w->alice, room, NULL, msg, strlen(msg), w->f1, sizeof(w->f1), &n);
    }
    if (rc == SC2_OK) {
        rc = sc2e_relay_room(w->f1, n, ++f->seq, w->f2, sizeof(w->f2), &m);
    }
    if (rc == SC2_OK) {
        rc = sc2e_room_decrypt(&w->bob, w->f2, m, &rr, w->pt, sizeof(w->pt), &pl);
    }
    if (rc == SC2_OK && (rr.epoch != 2 || rr.n != 0)) {
        rc = SC2_ERR_STATE;
    }
    if (rc == SC2_OK) {
        rc = sc2e_room_info(&w->bob, room, &info);
    }
    if (rc == SC2_OK && (!info.known || info.is_admin || info.epoch != 2 || strcmp(info.admin, w->ai.device_id))) {
        rc = SC2_ERR_STATE;
    }
    return rc;
}

static int flow_post(flow_t *f)
{
    flow_ws_t *w = f->w;
    sc2e_post_result_t pr;
    size_t n = 0, m = 0;
    int rc = sc2e_post_sign(&w->alice, "relay.test", "pub:general", "Ana", "hello #general", 14, FIXED_NOW, NULL,
                            false, "p1", w->f1, sizeof(w->f1), &n);
    if (rc == SC2_OK) {
        rc = sc2e_relay_post(w->f1, n, ++f->seq, w->f2, sizeof(w->f2), &m);
    }
    if (rc == SC2_OK) {
        rc = sc2e_post_verify("relay.test", w->f2, m, 1024, &pr, w->body, sizeof(w->body));
    }
    if (rc == SC2_OK && (!pr.valid || strcmp(pr.body, "hello #general") != 0 || strcmp(pr.name, "Ana") != 0 ||
                         strcmp(pr.from, w->ai.device_id) != 0 || strcmp(pr.room, "pub:general") != 0)) {
        rc = SC2_ERR_STATE;
    }
    if (rc == SC2_OK) {
        /* Same post presented by a different relay must not verify. */
        rc = sc2e_post_verify("other.relay", w->f2, m, 1024, &pr, w->body, sizeof(w->body)) == SC2_ERR_AUTH
                 ? SC2_OK
                 : SC2_ERR_STATE;
    }
    if (rc == SC2_OK) {
        char *hit = strstr(w->f2, "hello #general");
        if (hit == NULL) {
            rc = SC2_ERR_STATE;
        } else {
            hit[0] = 'j';
            rc = sc2e_post_verify("relay.test", w->f2, m, 1024, &pr, w->body, sizeof(w->body)) == SC2_ERR_AUTH
                     ? SC2_OK
                     : SC2_ERR_STATE;
        }
    }
    return rc;
}

static int flow_persist(flow_t *f)
{
    flow_ws_t *w = f->w;
    sc2e_platform_t pf;
    char name[SC2E_NAME_MAX];
    size_t len = 0;
    char before[SC2_DEVICE_ID_HEX_LEN + 1];
    memcpy(before, w->bob.self_hex, sizeof(before));
    sc2e_close(&w->bob);
    sc2e_memstore_platform(f->b, &pf);
    int rc = sc2e_open(&w->bob, &pf);
    if (rc == SC2_OK && (w->bob.created || strcmp(before, w->bob.self_hex) != 0)) {
        rc = SC2_ERR_STATE;
    }
    if (rc == SC2_OK) {
        rc = dm_roundtrip(f, &w->alice, &w->bob, w->bi.device_id, "{\"t\":\"text\",\"body\":\"after reboot\"}",
                          SC2E_DM_MESSAGE, false);
    }
    if (rc == SC2_OK) {
        /* A corrupted session record is detected, not misread. */
        memcpy(name, "dm/", 3);
        memcpy(name + 3, w->ai.device_id, SC2_DEVICE_ID_HEX_LEN + 1);
        uint8_t *rec = sc2e_memstore_record(f->b, name, &len);
        if (rec == NULL || len < 40) {
            rc = SC2_ERR_STATE;
        } else {
            rec[len / 2] ^= 0x10u;
            size_t n = 0;
            rc = sc2e_dm_encrypt(&w->bob, w->ai.device_id, NULL, TEXT_ONLY, strlen(TEXT_ONLY), w->f1, sizeof(w->f1), &n) ==
                         SC2_ERR_FORMAT
                     ? SC2_OK
                     : SC2_ERR_STATE;
            rec[len / 2] ^= 0x10u;
        }
    }
    return rc;
}

typedef struct {
    const char *name;
    int (*fn)(kat_ws_t *w);
} kat_case_t;

static const kat_case_t KATS[] = {
    {"sha256", kat_sha256},
    {"hmac-sha256", kat_hmac},
    {"hkdf", kat_hkdf},
    {"ed25519", kat_ed25519},
    {"ed25519-strict", kat_ed25519_strict},
    {"x25519", kat_x25519},
    {"xchacha20-poly1305", kat_aead},
    {"room-chain", kat_room},
    {"safety-number", kat_safety},
    {"store-record", kat_records},
};

static void tally(sc2e_report_fn report, void *ctx, const char *name, int rc, unsigned *passed, unsigned *failed)
{
    if (rc == SC2_OK) {
        (*passed)++;
    } else {
        (*failed)++;
    }
    if (report) {
        report(ctx, name, rc);
    }
}

static int flow_open(const sc2e_test_host_t *host, flow_t *f)
{
    sc2e_platform_t pf;
    memset(f, 0, sizeof(*f));
    int rc = sc2e_memstore_create(&f->a, host);
    if (rc == SC2_OK) {
        rc = sc2e_memstore_create(&f->b, host);
    }
    if (rc == SC2_OK) {
        f->w = host->alloc(host->ctx, sizeof(flow_ws_t));
        rc = f->w ? SC2_OK : SC2_ERR_BOUNDS;
    }
    if (rc == SC2_OK) {
        sc2e_memstore_platform(f->a, &pf);
        rc = sc2e_open(&f->w->alice, &pf);
    }
    if (rc == SC2_OK) {
        sc2e_memstore_platform(f->b, &pf);
        rc = sc2e_open(&f->w->bob, &pf);
    }
    return rc;
}

static void flow_close(const sc2e_test_host_t *host, flow_t *f)
{
    if (f->w) {
        sc2e_close(&f->w->alice);
        sc2e_close(&f->w->bob);
        host->release(host->ctx, f->w, sizeof(flow_ws_t));
    }
    sc2e_memstore_destroy(f->a);
    sc2e_memstore_destroy(f->b);
    memset(f, 0, sizeof(*f));
}

int sc2e_selftest(const sc2e_test_host_t *host, sc2e_report_fn report, void *ctx, unsigned *passed,
                  unsigned *failed)
{
    unsigned p = 0, q = 0;
    flow_t f;
    if (host == NULL || host->alloc == NULL || host->release == NULL || host->rng.fn == NULL) {
        return SC2_ERR_ARG;
    }
    kat_ws_t *w = host->alloc(host->ctx, sizeof(*w));
    if (w == NULL) {
        return SC2_ERR_BOUNDS;
    }
    for (size_t i = 0; i < sizeof(KATS) / sizeof(KATS[0]); i++) {
        tally(report, ctx, KATS[i].name, KATS[i].fn(w), &p, &q);
    }
    host->release(host->ctx, w, sizeof(*w));

    int rc = flow_open(host, &f);
    tally(report, ctx, "engine-open", rc, &p, &q);
    if (rc == SC2_OK) {
        rc = flow_dm(&f);
        tally(report, ctx, "dm-x3dh-ratchet", rc, &p, &q);
        if (rc == SC2_OK) {
            tally(report, ctx, "room-epoch-chains", flow_room(&f), &p, &q);
            tally(report, ctx, "public-post", flow_post(&f), &p, &q);
            tally(report, ctx, "persistence", flow_persist(&f), &p, &q);
        }
    }
    flow_close(host, &f);
    if (passed) {
        *passed = p;
    }
    if (failed) {
        *failed = q;
    }
    return q == 0 ? SC2_OK : SC2_ERR_STATE;
}

/* ---------------------------------------------------------------------- */
/* Benchmark                                                               */
/* ---------------------------------------------------------------------- */

static void bench_emit(sc2e_bench_fn report, void *ctx, const char *name, uint32_t iterations, uint64_t us, int rc)
{
    sc2e_bench_result_t r = {name, iterations, us, rc};
    if (report) {
        report(ctx, &r);
    }
}

int sc2e_bench(const sc2e_test_host_t *host, uint32_t iterations, sc2e_bench_fn report, void *ctx)
{
    uint8_t seed[SC2_KEY_LEN], pub[SC2_KEY_LEN], sig[SC2_SIG_LEN], out[SC2_KEY_LEN], key[SC2_KEY_LEN];
    uint8_t nonce[SC2_NONCE_LEN];
    flow_t f;
    if (host == NULL || host->clock_us == NULL || host->alloc == NULL || host->rng.fn == NULL) {
        return SC2_ERR_ARG;
    }
    if (iterations == 0) {
        iterations = 1;
    }
    uint8_t *buf = host->alloc(host->ctx, 2048);
    if (buf == NULL) {
        return SC2_ERR_BOUNDS;
    }
    int rc = sc2_random(&host->rng, seed, sizeof(seed));
    if (rc == SC2_OK) {
        rc = sc2_ed25519_public(pub, seed);
    }
    uint64_t t0;
    int r = rc;

    t0 = host->clock_us(host->ctx);
    for (uint32_t i = 0; i < iterations && r == SC2_OK; i++) {
        r = sc2_ed25519_sign(sig, buf, 64, seed);
    }
    bench_emit(report, ctx, "ed25519-sign", iterations, host->clock_us(host->ctx) - t0, r);

    t0 = host->clock_us(host->ctx);
    for (uint32_t i = 0; i < iterations && r == SC2_OK; i++) {
        r = sc2_ed25519_verify(sig, buf, 64, pub);
    }
    bench_emit(report, ctx, "ed25519-verify", iterations, host->clock_us(host->ctx) - t0, r);

    r = rc;
    t0 = host->clock_us(host->ctx);
    for (uint32_t i = 0; i < iterations && r == SC2_OK; i++) {
        r = sc2_x25519_public(out, seed);
    }
    bench_emit(report, ctx, "x25519", iterations, host->clock_us(host->ctx) - t0, r);

    r = sc2_random(&host->rng, key, sizeof(key));
    if (r == SC2_OK) {
        r = sc2_random(&host->rng, nonce, sizeof(nonce));
    }
    uint32_t aead_iter = iterations * 10u;
    t0 = host->clock_us(host->ctx);
    for (uint32_t i = 0; i < aead_iter && r == SC2_OK; i++) {
        r = sc2_aead_seal(buf, buf, 1024, NULL, 0, nonce, key);
    }
    bench_emit(report, ctx, "aead-seal-1k", aead_iter, host->clock_us(host->ctx) - t0, r);

    r = SC2_OK;
    t0 = host->clock_us(host->ctx);
    for (uint32_t i = 0; i < aead_iter && r == SC2_OK; i++) {
        r = sc2_hkdf(out, sizeof(out), key, sizeof(key), buf, 64, buf, 16);
    }
    bench_emit(report, ctx, "hkdf-32", aead_iter, host->clock_us(host->ctx) - t0, r);

    r = SC2_OK;
    t0 = host->clock_us(host->ctx);
    for (uint32_t i = 0; i < aead_iter && r == SC2_OK; i++) {
        r = sc2_sha256(out, buf, 1024);
    }
    bench_emit(report, ctx, "sha256-1k", aead_iter, host->clock_us(host->ctx) - t0, r);

    sc2_memzero(seed, sizeof(seed));
    sc2_memzero(key, sizeof(key));
    host->release(host->ctx, buf, 2048);

    /* Session-level timings include record (de)serialisation and the
     * in-memory store, i.e. everything except flash I/O. */
    r = flow_open(host, &f);
    if (r == SC2_OK) {
        t0 = host->clock_us(host->ctx);
        r = exchange_bundles(&f);
        bench_emit(report, ctx, "publish+add-bundle(x2)", 1, host->clock_us(host->ctx) - t0, r);
    }
    if (r == SC2_OK) {
        t0 = host->clock_us(host->ctx);
        r = dm_roundtrip(&f, &f.w->alice, &f.w->bob, f.w->bi.device_id, "{\"t\":\"text\",\"body\":\"x\"}",
                         SC2E_DM_MESSAGE, true);
        bench_emit(report, ctx, "dm-first(x3dh)", 1, host->clock_us(host->ctx) - t0, r);
    }
    if (r == SC2_OK) {
        t0 = host->clock_us(host->ctx);
        uint32_t i = 0;
        for (; i < iterations && r == SC2_OK; i++) {
            /* Alternate senders so every message performs a DH ratchet step. */
            bool ab = (i & 1u) != 0;
            r = dm_roundtrip(&f, ab ? &f.w->alice : &f.w->bob, ab ? &f.w->bob : &f.w->alice,
                             ab ? f.w->bi.device_id : f.w->ai.device_id, "{\"t\":\"text\",\"body\":\"ping\"}",
                             SC2E_DM_MESSAGE, false);
        }
        bench_emit(report, ctx, "dm-roundtrip", i, host->clock_us(host->ctx) - t0, r);
    }
    if (r == SC2_OK) {
        char room[SC2_DEVICE_ID_HEX_LEN + 1];
        uint32_t epoch = 0;
        size_t n = 0, m = 0, pl = 0;
        sc2e_dm_result_t dr;
        sc2e_room_result_t rr;
        r = sc2e_room_create(&f.w->alice, NULL, room, &epoch);
        if (r == SC2_OK) {
            r = sc2e_room_share(&f.w->alice, room, f.w->bi.device_id, NULL, f.w->f1, sizeof(f.w->f1), &n);
        }
        if (r == SC2_OK) {
            r = sc2e_relay_dm(f.w->f1, n, ++f.seq, f.w->f2, sizeof(f.w->f2), &m);
        }
        if (r == SC2_OK) {
            r = sc2e_dm_decrypt(&f.w->bob, f.w->f2, m, &dr, f.w->pt, sizeof(f.w->pt), &pl);
        }
        t0 = host->clock_us(host->ctx);
        uint32_t i = 0;
        for (; i < iterations && r == SC2_OK; i++) {
            r = sc2e_room_encrypt(&f.w->alice, room, NULL, ROOM_PING, strlen(ROOM_PING), f.w->f1,
                                  sizeof(f.w->f1), &n);
            if (r == SC2_OK) {
                r = sc2e_relay_room(f.w->f1, n, ++f.seq, f.w->f2, sizeof(f.w->f2), &m);
            }
            if (r == SC2_OK) {
                r = sc2e_room_decrypt(&f.w->bob, f.w->f2, m, &rr, f.w->pt, sizeof(f.w->pt), &pl);
            }
        }
        bench_emit(report, ctx, "room-roundtrip", i, host->clock_us(host->ctx) - t0, r);
    }
    flow_close(host, &f);
    return r;
}
