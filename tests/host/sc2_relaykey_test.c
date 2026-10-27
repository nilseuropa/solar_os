/*
 * protocol/sc2 relay key: signed /.well-known descriptor (in-place parse of
 * a cut-short body), welcome proof and key rotation, against vectors made
 * by the Python reference relay (sc2proto.relaykey in the solar_os_sc2-relay
 * repository) and round trips in C.
 * SPDX-License-Identifier: MIT
 */
#include "sc2_test_util.h"

/* Generated with sc2proto/relaykey.py of the solar_os_sc2-relay repository:
 * seed 00..1f, fallback seed 20..3f, rotation to seed 07*32, nonce 00..0f,
 * challenge 100..131. */
static const char PY_BODY[] = "{\"descriptor\":\"c2MyLXJlbGF5IDEKaWQgcmVsYXkuZXhhbXBsZQprZXkgQTZFSHYvUE9FTDRkY04wWTUwdkFtV2ZrMWpDYnBRMWZIZHlHWkJKVk1iZz0KaG9zdCByZWxheS5leGFtcGxlCnBvcnQgNzc3OAp0bHMgMQpmYWxsYmFjayBjaGF0czovL2IuZXhhbXBsZTo3NDQzI3Bpbj1zaGEyNTYvQUFBQUFBQUFBQUFBQUFBQUFBQUFBQUFBQUFBQUFBQUFBQUFBQUFBQUFBQT0gS2F5NjRVRzh5dkN5TGhxVTAwMEx4elllVW0wTC9oTElsNVM4a3lLV2JkYz0KdHMgMTcwMDAwMDAwMAo=\",\"descriptor_sig\":\"kiZA6s2/nohnuuNFpmRwc2Q7torZnjXHpdnNqG8fUXFOb9TAU1zQ/YlIHC7HlEMiDPn4SthcX95zbScd8IJNBA==\",\"v\":2,\"relay_id\":\"relay.example\",\"public_rooms\":[\"pub:general\"]}";
static const char PY_WELCOME_SIG[] = "xzhykkUH+Dv29Kw3mvKLaGgatgzF621IGHi9cHlG5s/Z7RXs+qLQZ/fhFd1ZoE1oGN6/XH3ELbmZ7Bp5jFALDA==";
static const char PY_ROT_SIG[] = "+7sT/scoBpiVKMDqd0FzHyrm+9kj2yOlkOB0XTX728vR3UYrZiN37rTXgWkldd4MUxAmoo9SnkybYaDhEU9uCQ==";
static const char PY_NEW_KEY[] = "6kpsY+KcUgq+9VB7Ey7F+ZVHdq6+vnuSQh7qaRRG0iw=";
static const char PY_KEY[] = "A6EHv/POEL4dcN0Y50vAmWfk1jCbpQ1fHdyGZBJVMbg=";
static const char PY_FB_KEY[] = "Kay64UG8yvCyLhqU000LxzYeUm0L/hLIl5S8kyKWbdc=";

static char buf[2048];
static sc2_desc_t d;

static void b64(uint8_t *out, size_t n, const char *s)
{
    size_t got = 0;
    CHECK_RC(sc2_b64_decode(out, n, &got, s, strlen(s)), SC2_OK);
    CHECK(got == n);
}

static int parse_copy(const char *body, size_t len)
{
    memcpy(buf, body, len);
    return sc2_wellknown_parse(buf, len, &d);
}

static void test_python_vectors(void)
{
    uint8_t key[32], fb[32], newk[32], sig[64], nonce[16], ch[32];
    b64(key, 32, PY_KEY);
    b64(fb, 32, PY_FB_KEY);
    b64(newk, 32, PY_NEW_KEY);
    CHECK_RC(parse_copy(PY_BODY, strlen(PY_BODY)), SC2_OK);
    CHECK(strcmp(d.relay_id, "relay.example") == 0 && strcmp(d.host, "relay.example") == 0);
    CHECK(d.port == 7778 && d.tls && d.pin[0] == '\0' && d.ts == 1700000000u);
    CHECK(memcmp(d.key, key, 32) == 0);
    CHECK(d.fallback_count == 1 && memcmp(d.fallback[0].key, fb, 32) == 0);
    CHECK(strcmp(d.fallback[0].url, "chats://b.example:7443#pin=sha256/AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA=") == 0);

    /* the device reads only a prefix: cut right after descriptor_sig */
    const char *cut = strstr(PY_BODY, ",\"v\":2");
    CHECK_RC(parse_copy(PY_BODY, (size_t)(cut - PY_BODY)), SC2_OK);
    /* cut inside the signature: malformed, not a pass */
    CHECK_RC(parse_copy(PY_BODY, (size_t)(cut - PY_BODY) - 10), SC2_ERR_FORMAT);
    /* no descriptor at all (an old relay) */
    const char *old = "{\"v\":2,\"relay_id\":\"x\",\"relay_key\":\"AAAA\"}";
    CHECK_RC(parse_copy(old, strlen(old)), SC2_ERR_NOT_FOUND);

    /* any flipped bit in the descriptor or the signature fails */
    const char *dpos = strstr(PY_BODY, "\"descriptor\":\"") + 14;
    const char *spos = strstr(PY_BODY, "\"descriptor_sig\":\"") + 18;
    size_t n = strlen(PY_BODY);
    for (size_t i = 0; i < 40; i++) {
        size_t at = (size_t)(dpos - PY_BODY) + 3u * i;
        memcpy(buf, PY_BODY, n);
        buf[at] = buf[at] == 'A' ? 'B' : 'A';
        int rc = sc2_wellknown_parse(buf, n, &d);
        CHECK(rc == SC2_ERR_AUTH || rc == SC2_ERR_FORMAT);
        at = (size_t)(spos - PY_BODY) + 2u * i;
        memcpy(buf, PY_BODY, n);
        buf[at] = buf[at] == 'A' ? 'B' : 'A';
        rc = sc2_wellknown_parse(buf, n, &d);
        CHECK(rc == SC2_ERR_AUTH || rc == SC2_ERR_FORMAT);
    }

    for (size_t i = 0; i < 16; i++) nonce[i] = (uint8_t)i;
    for (size_t i = 0; i < 32; i++) ch[i] = (uint8_t)(100 + i);
    b64(sig, 64, PY_WELCOME_SIG);
    CHECK_RC(sc2_welcome_verify(sig, key, "relay.example", nonce, ch), SC2_OK);
    CHECK_RC(sc2_welcome_verify(sig, key, "relay.other", nonce, ch), SC2_ERR_AUTH);
    nonce[3] ^= 1;
    CHECK_RC(sc2_welcome_verify(sig, key, "relay.example", nonce, ch), SC2_ERR_AUTH);
    b64(sig, 64, PY_ROT_SIG);
    CHECK_RC(sc2_rotation_verify(sig, key, "relay.example", newk), SC2_OK);
    CHECK_RC(sc2_rotation_verify(sig, newk, "relay.example", key), SC2_ERR_AUTH);
    CHECK_RC(sc2_rotation_verify(sig, key, "relay.example", fb), SC2_ERR_AUTH);
}

static void test_c_round_trip(void)
{
    uint8_t seed[32], pub[32], sig[64], nonce[16] = {1}, ch[32] = {2}, newk[32];
    memset(seed, 9, sizeof(seed));
    CHECK_RC(sc2_ed25519_public(pub, seed), SC2_OK);
    memset(newk, 5, sizeof(newk));
    const char *text = "sc2-relay 1\nid r\nkey ";
    char t[512];
    char kb[45];
    size_t kn = 0;
    CHECK_RC(sc2_b64_encode(kb, sizeof(kb), &kn, pub, 32), SC2_OK);
    int tl = snprintf(t, sizeof(t), "%s%s\nport 443\ntls 1\nfuture-field anything here\n", text, kb);
    CHECK_RC(sc2_desc_sign(sig, t, (size_t)tl, seed), SC2_OK);
    CHECK_RC(sc2_desc_verify(sig, t, (size_t)tl, pub), SC2_OK);
    CHECK_RC(sc2_desc_parse(t, (size_t)tl, &d), SC2_OK);
    CHECK(d.port == 443 && d.tls && d.host[0] == '\0' && d.fallback_count == 0);
    CHECK_RC(sc2_welcome_sign(sig, seed, "r", nonce, ch), SC2_OK);
    CHECK_RC(sc2_welcome_verify(sig, pub, "r", nonce, ch), SC2_OK);
    CHECK_RC(sc2_rotation_sign(sig, seed, "r", newk), SC2_OK);
    CHECK_RC(sc2_rotation_verify(sig, pub, "r", newk), SC2_OK);

    /* parser rejects */
    static const char *bad[] = {
        "sc2-relay 2\nid r\nkey %s\nport 1\ntls 1\n",          /* version */
        "id r\nkey %s\nport 1\ntls 1\n",                       /* header */
        "sc2-relay 1\nkey %s\nport 1\ntls 1\n",                /* no id */
        "sc2-relay 1\nid r\nkey %s\ntls 1\n",                  /* no port */
        "sc2-relay 1\nid r\nkey %s\nport 0\ntls 1\n",          /* port 0 */
        "sc2-relay 1\nid r\nkey %s\nport 65536\ntls 1\n",      /* port range */
        "sc2-relay 1\nid r\nkey %s\nport 1\ntls 2\n",          /* tls */
        "sc2-relay 1\nid r\nkey %s\nport 1\ntls 1\nhost a b\n", /* host */
        "sc2-relay 1\nid r\nkey %s\nport 1\ntls 1\nhost -a\n",
        "sc2-relay 1\nid r\nkey %s\nport 1\ntls 1\npin md5/x\n",
        "sc2-relay 1\nid r\nkey %s\nport 1\ntls 1\nfallback chats://a:1\n", /* no fallback key */
        "sc2-relay 1\nid r\nkey %s\nport 01\ntls 1\n",          /* leading zero */
        "sc2-relay 1\nid r\nid s\nkey %s\nport 1\ntls 1\n",     /* duplicate */
        "sc2-relay 1\nid r\nkey %s\nport 1\ntls 1\nnoval\n",    /* no space */
    };
    for (size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); i++) {
        tl = snprintf(t, sizeof(t), bad[i], kb);
        CHECK_RC(sc2_desc_parse(t, (size_t)tl, &d), SC2_ERR_FORMAT);
    }
    char many[700];
    int m = snprintf(many, sizeof(many), "sc2-relay 1\nid r\nkey %s\nport 1\ntls 1\n", kb);
    for (int i = 0; i < 4; i++) m += snprintf(many + m, sizeof(many) - (size_t)m, "fallback chat://a:%d %s\n", i + 1, kb);
    CHECK_RC(sc2_desc_parse(many, (size_t)m, &d), SC2_ERR_FORMAT); /* four fallbacks */
}

static void test_welcome_frame(void)
{
    char line[1024];
    snprintf(line, sizeof(line),
             "{\"type\":\"welcome\",\"v\":2,\"proto\":\"sc2\",\"suite\":1,\"relay\":{\"id\":\"relay.example\",\"key\":\"%s\"},"
             "\"challenge\":\"AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA=\",\"relay_sig\":\"%s\","
             "\"rotation\":{\"from\":\"%s\",\"sig\":\"%s\"}}",
             PY_NEW_KEY, PY_WELCOME_SIG, PY_KEY, PY_ROT_SIG);
    sc2_welcome_t w;
    CHECK_RC(sc2_frame_welcome_decode(line, strlen(line), &w), SC2_OK);
    CHECK(w.has_relay_key && w.has_relay_sig && w.has_rotation);
    uint8_t key[32], sig[64];
    b64(key, 32, PY_KEY);
    b64(sig, 64, PY_ROT_SIG);
    CHECK(memcmp(w.rotation_from, key, 32) == 0 && memcmp(w.rotation_sig, sig, 64) == 0);
    CHECK_RC(sc2_rotation_verify(w.rotation_sig, w.rotation_from, w.relay_id, w.relay_key), SC2_OK);
    /* rotation without a signature is malformed, not ignored */
    snprintf(line, sizeof(line),
             "{\"type\":\"welcome\",\"v\":2,\"proto\":\"sc2\",\"suite\":1,\"relay\":{\"id\":\"r\"},"
             "\"challenge\":\"AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA=\",\"rotation\":{\"from\":\"%s\"}}", PY_KEY);
    CHECK_RC(sc2_frame_welcome_decode(line, strlen(line), &w), SC2_ERR_FORMAT);
    size_t n = 0;
    uint8_t nonce[16] = {0};
    CHECK_RC(sc2_frame_hello(line, sizeof(line), &n, "c", NULL, nonce), SC2_OK);
    CHECK(strstr(line, "\"nonce\":\"AAAAAAAAAAAAAAAAAAAAAA==\"") != NULL);
}

int main(void)
{
    test_python_vectors();
    test_c_round_trip();
    test_welcome_frame();
    return sc2t_report("sc2_relaykey_test");
}
