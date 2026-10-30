/*
 * SC2 host test: JSON reader/writer, line assembler and relay-protocol frames
 * (byte-exact against the Python reference), plus a mutation fuzz loop over
 * every decoder.
 * SPDX-License-Identifier: MIT
 */
#include "sc2_test_util.h"
#include "sc2_vectors.inc"

static char g_out[SC2_LINE_MAX + 1];
static uint8_t g_ct[SC2_LINE_MAX];
static char g_body[SC2_POST_BODY_MAX + 1];

static int json_ok(const char *s)
{
    return sc2_json_validate(s, strlen(s)) == SC2_OK;
}

static void test_json_reader(void)
{
    static const char *good[] = {
        "{}", "[]", "{\"a\":1}", " { \"a\" : [ 1 , 2 , {\"b\":null} ] , \"c\":true,\"d\":false } ",
        "{\"s\":\"\\u00e9\\ud83d\\ude00\\n\\/\"}", "{\"n\":-0.5e+3}", "{\"n\":0}", "\"x\"", "123",
        "{\"k\":\"\xc3\xa9\xf0\x9f\x98\x80\"}", "[[[[[[[[1]]]]]]]]",
    };
    static const char *bad[] = {
        "", " ", "{", "}", "{\"a\":1,}", "[1,]", "{\"a\" 1}", "{a:1}", "{'a':1}", "{\"a\":01}", "{\"a\":1.}",
        "{\"a\":.5}", "{\"a\":+1}", "{\"a\":NaN}", "{\"a\":tru}", "{\"a\":1}x", "{\"a\":1}{}", "\"\\x\"",
        "\"\\u12\"", "\"abc", "\"a\tb\"", "[[[[[[[[[1]]]]]]]]]", "\"\xc3\"", "\"\xc0\xaf\"",
        "\"\xed\xa0\x80\"", "\"\xf4\x90\x80\x80\"", "\"\xff\"", "{\"a\":\"\\ud800\"}", "{\"a\":\"\\udc00\\ud800\"}",
        "{\"a\":\"\\ud83d\\u0041\"}",
    };
    for (size_t i = 0; i < sizeof(good) / sizeof(good[0]); i++) {
        if (!json_ok(good[i])) {
            fprintf(stderr, "rejected good JSON #%zu: %s\n", i, good[i]);
        }
        CHECK(json_ok(good[i]));
    }
    for (size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); i++) {
        if (json_ok(bad[i])) {
            fprintf(stderr, "accepted bad JSON #%zu: %s\n", i, bad[i]);
        }
        CHECK(!json_ok(bad[i]));
    }
    /* duplicate keys: syntax is valid, but looking up a duplicated key fails */
    {
        const char *dup = "{\"a\":1,\"b\":{\"a\":3},\"a\":2}";
        sc2_json_val_t r, x;
        CHECK_RC(sc2_json_root(dup, strlen(dup), &r), SC2_OK);
        CHECK_RC(sc2_json_get(&r, "a", &x), SC2_ERR_FORMAT);
        CHECK_RC(sc2_json_get(&r, "b", &x), SC2_OK);
    }
    /* embedded NUL is never valid */
    CHECK_RC(sc2_json_validate("{\"a\":\"\0\"}", 9), SC2_ERR_FORMAT);

    const char *doc = "{\"s\":\"a\\\"b\\u00e9\\ud83d\\ude00\",\"n\":18446744073709551615,\"big\":18446744073709551616,"
                      "\"neg\":-1,\"f\":1.5,\"e\":1e3,\"q\":\"42\",\"q0\":\"042\",\"t\":true,\"arr\":[1,\"x\",{}],"
                      "\"b\":\"AAEC\",\"h\":\"00ff\"}";
    sc2_json_val_t root, v;
    CHECK_RC(sc2_json_root(doc, strlen(doc), &root), SC2_OK);
    CHECK(root.type == SC2_JSON_OBJECT);
    char s[32];
    size_t sl;
    CHECK_RC(sc2_json_get_string(&root, "s", s, sizeof(s), &sl), SC2_OK);
    CHECK(sl == 9 && memcmp(s, "a\"b\xc3\xa9\xf0\x9f\x98\x80", 9) == 0 && s[9] == '\0');
    CHECK_RC(sc2_json_get_string(&root, "s", s, 9, &sl), SC2_ERR_BOUNDS);
    CHECK_RC(sc2_json_get(&root, "s", &v), SC2_OK);
    CHECK(sc2_json_string_eq(&v, "a\"b\xc3\xa9\xf0\x9f\x98\x80"));
    CHECK(!sc2_json_string_eq(&v, "a\"b"));
    uint64_t u;
    CHECK_RC(sc2_json_get_u64(&root, "n", UINT64_MAX, &u), SC2_OK);
    CHECK(u == UINT64_MAX);
    CHECK_RC(sc2_json_get_u64(&root, "n", 100, &u), SC2_ERR_BOUNDS);
    CHECK(sc2_json_get_u64(&root, "big", UINT64_MAX, &u) != SC2_OK);
    CHECK(sc2_json_get_u64(&root, "neg", UINT64_MAX, &u) != SC2_OK);
    CHECK(sc2_json_get_u64(&root, "f", UINT64_MAX, &u) != SC2_OK);
    CHECK(sc2_json_get_u64(&root, "e", UINT64_MAX, &u) != SC2_OK);
    CHECK(sc2_json_get_u64(&root, "q", UINT64_MAX, &u) != SC2_OK);
    CHECK_RC(sc2_json_get(&root, "q", &v), SC2_OK);
    CHECK_RC(sc2_json_u64_str(&v, &u), SC2_OK);
    CHECK(u == 42);
    CHECK_RC(sc2_json_get(&root, "q0", &v), SC2_OK);
    CHECK(sc2_json_u64_str(&v, &u) != SC2_OK);
    bool bv = false;
    CHECK_RC(sc2_json_get(&root, "t", &v), SC2_OK);
    CHECK_RC(sc2_json_bool(&v, &bv), SC2_OK);
    CHECK(bv);
    CHECK_RC(sc2_json_get(&root, "missing", &v), SC2_ERR_NOT_FOUND);
    CHECK_RC(sc2_json_get(&root, "arr", &v), SC2_OK);
    size_t pos = 0, count = 0;
    sc2_json_val_t item;
    sc2_json_type_t types[3] = {SC2_JSON_NUMBER, SC2_JSON_STRING, SC2_JSON_OBJECT};
    while (sc2_json_array_next(&v, &pos, &item) == SC2_OK) {
        CHECK(count < 3 && item.type == types[count]);
        count++;
    }
    CHECK(count == 3);
    uint8_t bin[4];
    CHECK_RC(sc2_json_get_b64_exact(&root, "b", bin, 3), SC2_OK);
    CHECK(bin[0] == 0 && bin[1] == 1 && bin[2] == 2);
    CHECK(sc2_json_get_b64_exact(&root, "b", bin, 4) != SC2_OK);
    CHECK_RC(sc2_json_get_hex_exact(&root, "h", bin, 2), SC2_OK);
    CHECK(bin[0] == 0 && bin[1] == 0xff);
    CHECK(sc2_json_get_hex_exact(&root, "h", bin, 3) != SC2_OK);
    CHECK(sc2_json_get_string(&root, "n", s, sizeof(s), &sl) != SC2_OK);   /* type mismatch */

    CHECK(sc2_utf8_valid("\xe2\x82\xac", 3));
    CHECK(!sc2_utf8_valid("\xe2\x82", 2));
    CHECK(!sc2_utf8_valid("\xe0\x80\xaf", 3));   /* overlong */
    CHECK(!sc2_utf8_valid("\xed\xb0\x80", 3));   /* surrogate */
}

static void test_json_writer(void)
{
    sc2_jw_t w;
    char raw[48];
    for (int i = 0; i < 32; i++) {
        raw[i] = (char)i;
    }
    memcpy(raw + 32, "\x7f\"\\\xc3\xa9\xf0\x9f\x98\x80", 9);
    sc2_jw_init(&w, g_out, sizeof(g_out));
    sc2_jw_object_begin(&w);
    sc2_jw_key(&w, "s");
    sc2_jw_string(&w, raw, 41);
    sc2_jw_key(&w, "a");
    sc2_jw_array_begin(&w);
    sc2_jw_u64(&w, 0);
    sc2_jw_u64(&w, UINT64_MAX);
    sc2_jw_u64_str(&w, 981);
    sc2_jw_bool(&w, false);
    sc2_jw_array_end(&w);
    sc2_jw_key(&w, "b");
    sc2_jw_b64(&w, (const uint8_t *)"\x00\x01\x02\x03", 4);
    sc2_jw_key(&w, "h");
    sc2_jw_hex(&w, (const uint8_t *)"\xab\xcd", 2);
    sc2_jw_object_end(&w);
    size_t n;
    CHECK_RC(sc2_jw_finish(&w, &n), SC2_OK);
    const char *want =
        "{\"s\":\"\\u0000\\u0001\\u0002\\u0003\\u0004\\u0005\\u0006\\u0007\\b\\t\\n\\u000b\\f\\r\\u000e\\u000f"
        "\\u0010\\u0011\\u0012\\u0013\\u0014\\u0015\\u0016\\u0017\\u0018\\u0019\\u001a\\u001b\\u001c\\u001d\\u001e"
        "\\u001f\x7f\\\"\\\\\xc3\xa9\xf0\x9f\x98\x80\",\"a\":[0,18446744073709551615,\"981\",false],"
        "\"b\":\"AAECAw==\",\"h\":\"abcd\"}";
    CHECK(n == strlen(want) && strcmp(g_out, want) == 0);
    CHECK(json_ok(g_out));
    /* round trip of the string through the reader */
    sc2_json_val_t root;
    char back[64];
    size_t bl;
    CHECK_RC(sc2_json_root(g_out, n, &root), SC2_OK);
    CHECK_RC(sc2_json_get_string(&root, "s", back, sizeof(back), &bl), SC2_OK);
    CHECK(bl == 41 && memcmp(back, raw, 41) == 0);
    /* overflow is sticky and reported */
    char small[8];
    sc2_jw_init(&w, small, sizeof(small));
    sc2_jw_object_begin(&w);
    sc2_jw_key(&w, "long");
    sc2_jw_cstring(&w, "value");
    sc2_jw_object_end(&w);
    CHECK_RC(sc2_jw_finish(&w, &n), SC2_ERR_BOUNDS);
    CHECK(small[0] == '\0');
    /* misuse */
    sc2_jw_init(&w, g_out, sizeof(g_out));
    sc2_jw_object_begin(&w);
    sc2_jw_key(&w, "a");
    CHECK(sc2_jw_finish(&w, &n) != SC2_OK);
    /* invalid UTF-8 is refused by the writer */
    sc2_jw_init(&w, g_out, sizeof(g_out));
    sc2_jw_string(&w, "\xff", 1);
    CHECK(sc2_jw_finish(&w, &n) != SC2_OK);
}

static void test_lines(void)
{
    static sc2_lines_t l;
    sc2_lines_init(&l);
    const char *stream = "{\"a\":1}\r\n\n\n{\"b\"";
    const char *line;
    size_t ll, used, off = 0, total = strlen(stream);
    int rc = sc2_lines_feed(&l, (const uint8_t *)stream, total, &used, &line, &ll);
    CHECK(rc == 1 && ll == 7 && memcmp(line, "{\"a\":1}", 7) == 0 && line[7] == '\0');
    off += used;
    rc = sc2_lines_feed(&l, (const uint8_t *)stream + off, total - off, &used, &line, &ll);
    CHECK(rc == 0 && off + used == total);           /* blank lines skipped, partial kept */
    rc = sc2_lines_feed(&l, (const uint8_t *)":2}\n", 4, &used, &line, &ll);
    CHECK(rc == 1 && ll == 7 && memcmp(line, "{\"b\":2}", 7) == 0 && used == 4);

    /* byte-at-a-time delivery of a golden frame */
    const char *f = SC2V_FRAME_ROOM_PUB;
    size_t fl = strlen(f), lines = 0;
    for (size_t i = 0; i <= fl; i++) {
        uint8_t c = i < fl ? (uint8_t)f[i] : (uint8_t)'\n';
        rc = sc2_lines_feed(&l, &c, 1, &used, &line, &ll);
        if (rc == 1) {
            lines++;
            CHECK(ll == fl && memcmp(line, f, fl) == 0);
        } else {
            CHECK(rc == 0);
        }
    }
    CHECK(lines == 1);

    /* exactly SC2_LINE_MAX is fine, with or without CR; one more is not */
    static uint8_t big[SC2_LINE_MAX + 3];
    memset(big, 'x', sizeof(big));
    big[SC2_LINE_MAX] = '\n';
    rc = sc2_lines_feed(&l, big, SC2_LINE_MAX + 1u, &used, &line, &ll);
    CHECK(rc == 1 && ll == SC2_LINE_MAX);
    big[SC2_LINE_MAX] = '\r';
    big[SC2_LINE_MAX + 1] = '\n';
    rc = sc2_lines_feed(&l, big, SC2_LINE_MAX + 2u, &used, &line, &ll);
    CHECK(rc == 1 && ll == SC2_LINE_MAX);
    memset(big, 'x', sizeof(big));
    big[SC2_LINE_MAX + 1] = '\n';
    rc = sc2_lines_feed(&l, big, SC2_LINE_MAX + 2u, &used, &line, &ll);
    CHECK(rc == SC2_ERR_BOUNDS);
    /* after an overlong line the rest is discarded up to the newline, then normal again */
    size_t o2 = used;
    while (o2 < SC2_LINE_MAX + 2u) {
        rc = sc2_lines_feed(&l, big + o2, SC2_LINE_MAX + 2u - o2, &used, &line, &ll);
        CHECK(rc == 0 || rc == SC2_ERR_BOUNDS);
        o2 += used;
    }
    rc = sc2_lines_feed(&l, (const uint8_t *)"{}\n", 3, &used, &line, &ll);
    CHECK(rc == 1 && ll == 2);
    /* a huge stream without newline: one error, then silence */
    static uint8_t junk[40000];
    memset(junk, 'j', sizeof(junk));
    size_t errs = 0;
    off = 0;
    while (off < sizeof(junk)) {
        rc = sc2_lines_feed(&l, junk + off, sizeof(junk) - off, &used, &line, &ll);
        errs += rc == SC2_ERR_BOUNDS;
        CHECK(rc != 1);
        off += used;
    }
    CHECK(errs == 1);
    rc = sc2_lines_feed(&l, (const uint8_t *)"\n{}\n", 4, &used, &line, &ll);
    CHECK(rc == 1 && ll == 2);
}

static void expect_line(const char *got, size_t n, const char *want, const char *what)
{
    if (n != strlen(want) || strcmp(got, want) != 0) {
        fprintf(stderr, "%s differs:\n  got  %s\n  want %s\n", what, got, want);
    }
    CHECK(n == strlen(want) && strcmp(got, want) == 0);
}

static void test_handshake(void)
{
    size_t n;
    CHECK_RC(sc2_frame_hello(g_out, sizeof(g_out), &n, "solaros/1.x", "code-42", NULL), SC2_OK);
    expect_line(g_out, n, SC2V_FRAME_HELLO, "hello");
    uint8_t ik[32], sig[64], ch[32], rk[32];
    sc2t_hexn(ik, 32, SC2V_ID_IK_PUB);
    sc2t_hexn(sig, 64, SC2V_ID_AUTH_SIG);
    CHECK_RC(sc2_frame_auth(g_out, sizeof(g_out), &n, "a1", ik, sig, true), SC2_OK);
    expect_line(g_out, n, SC2V_FRAME_AUTH, "auth");
    CHECK_RC(sc2_frame_auth(g_out, 20, &n, "a1", ik, sig, true), SC2_ERR_BOUNDS);

    sc2_welcome_t w;
    CHECK_RC(sc2_frame_welcome_decode(SC2V_FRAME_WELCOME, strlen(SC2V_FRAME_WELCOME), &w), SC2_OK);
    sc2t_hexn(ch, 32, SC2V_ID_CHALLENGE);
    sc2t_hexn(rk, 32, SC2V_FRAME_RELAY_KEY);
    CHECK(w.suite == 1 && strcmp(w.relay_id, "relay.example") == 0 && w.has_relay_key && w.legacy_v1);
    CHECK_MEM(w.challenge, ch, 32);
    CHECK_MEM(w.relay_key, rk, 32);
    CHECK(w.limit_line == 16384 && w.limit_msg == 4096);
    /* the signature over the challenge verifies with what the welcome carried */
    CHECK_RC(sc2_auth_verify(sig, ik, w.relay_id, strlen(w.relay_id), w.challenge), SC2_OK);

    sc2_json_val_t root;
    char type[SC2_TYPE_MAX + 1], rid[SC2_RID_MAX + 1];
    CHECK_RC(sc2_frame_peek(SC2V_FRAME_AUTH, strlen(SC2V_FRAME_AUTH), &root, type, rid), SC2_OK);
    CHECK(strcmp(type, "auth") == 0 && strcmp(rid, "a1") == 0);
    CHECK_RC(sc2_frame_peek(SC2V_FRAME_DM_MSG, strlen(SC2V_FRAME_DM_MSG), &root, type, rid), SC2_OK);
    CHECK(strcmp(type, "dm.msg") == 0 && rid[0] == '\0');
    CHECK(sc2_frame_peek("{\"rid\":\"x\"}", 11, &root, type, rid) != SC2_OK);
    CHECK(sc2_frame_peek("[1]", 3, &root, type, rid) != SC2_OK);
    /* welcome with a wrong type or bad challenge length is refused */
    const char *bw = "{\"type\":\"welcome\",\"v\":2,\"proto\":\"sc2\",\"suite\":1,\"relay\":{\"id\":\"r\"},\"challenge\":\"AAAA\"}";
    CHECK(sc2_frame_welcome_decode(bw, strlen(bw), &w) != SC2_OK);
    CHECK(sc2_frame_welcome_decode(SC2V_FRAME_AUTH, strlen(SC2V_FRAME_AUTH), &w) != SC2_OK);
}

static void test_dm_frames(void)
{
    const sc2v_step_t *s0 = &SC2V_RT_STEPS[0], *s9 = &SC2V_RT_STEPS[9];
    sc2_dm_env_t env;
    uint64_t seq;
    size_t n;
    uint8_t want[32], to[16];
    CHECK_RC(sc2_frame_dm_msg_decode(SC2V_FRAME_DM_MSG, strlen(SC2V_FRAME_DM_MSG), &seq, &env, g_ct, sizeof(g_ct)),
             SC2_OK);
    CHECK(seq == 981 && env.suite == 1 && env.hdr.has_x3dh);
    CHECK(env.hdr.x3dh.spk_id == SC2V_RT_SPK_ID && env.hdr.x3dh.opk_id == SC2V_RT_OPK_ID);
    CHECK(env.hdr.pn == s0->pn && env.hdr.n == s0->n);
    sc2t_hexn(want, 32, s0->dh);
    CHECK_MEM(env.hdr.dh, want, 32);
    uint8_t nonce[24];
    sc2t_hexn(nonce, 24, s0->nonce);
    CHECK_MEM(env.nonce, nonce, 24);
    size_t cl;
    uint8_t *ct = sc2t_hex(s0->ct, &cl);
    CHECK(env.ct_len == cl && env.ct == g_ct);
    CHECK_MEM(env.ct, ct, cl);
    free(ct);
    sc2t_hexn(to, 16, "86c524f0bda0dbb06cb117072568054d");
    static char line2[SC2_LINE_MAX + 1];
    CHECK_RC(sc2_frame_dm_send(line2, sizeof(line2), &n, "d1", to, &env), SC2_OK);
    expect_line(line2, n, SC2V_FRAME_DM_SEND, "dm.send (x3dh)");

    /* plain message: header from the transcript, no x3dh object */
    memset(&env, 0, sizeof(env));
    env.suite = 1;
    sc2t_hexn(env.from, 16, "3eb04195fd8d45a87229c17fcd4441b7");
    sc2t_hexn(env.hdr.dh, 32, s9->dh);
    env.hdr.pn = s9->pn;
    env.hdr.n = s9->n;
    env.hdr.x3dh.opk_id = SC2_NO_OPK;
    sc2t_hexn(env.nonce, 24, s9->nonce);
    ct = sc2t_hex(s9->ct, &cl);
    env.ct = ct;
    env.ct_len = cl;
    CHECK_RC(sc2_frame_dm_send(line2, sizeof(line2), &n, NULL, to, &env), SC2_OK);
    expect_line(line2, n, SC2V_FRAME_DM_SEND_PLAIN, "dm.send (plain)");
    CHECK_RC(sc2_frame_dm_send(line2, 100, &n, NULL, to, &env), SC2_ERR_BOUNDS);
    free(ct);

    /* decoder strictness */
    const char *bad[] = {
        "{\"type\":\"dm.msg\",\"seq\":\"1\",\"env\":{\"s\":2,\"from\":\"3eb04195fd8d45a87229c17fcd4441b7\",\"rk\":\"AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA=\",\"pn\":0,\"n\":0,\"nonce\":\"AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA\",\"ct\":\"AAAAAAAAAAAAAAAAAAAAAA==\"}}",
        "{\"type\":\"dm.msg\",\"seq\":\"1\",\"env\":{\"s\":1,\"from\":\"3eb04195fd8d45a87229c17fcd4441b\",\"rk\":\"AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA=\",\"pn\":0,\"n\":0,\"nonce\":\"AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA\",\"ct\":\"AAAAAAAAAAAAAAAAAAAAAA==\"}}",
        "{\"type\":\"dm.msg\",\"seq\":\"1\",\"env\":{\"s\":1,\"from\":\"3eb04195fd8d45a87229c17fcd4441b7\",\"rk\":\"AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA==\",\"pn\":0,\"n\":0,\"nonce\":\"AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA\",\"ct\":\"AAAAAAAAAAAAAAAAAAAAAA==\"}}",
        "{\"type\":\"dm.msg\",\"seq\":\"1\",\"env\":{\"s\":1,\"from\":\"3eb04195fd8d45a87229c17fcd4441b7\",\"rk\":\"AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA=\",\"pn\":4294967296,\"n\":0,\"nonce\":\"AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA\",\"ct\":\"AAAAAAAAAAAAAAAAAAAAAA==\"}}",
        "{\"type\":\"dm.msg\",\"seq\":\"1\",\"env\":{\"s\":1,\"from\":\"3eb04195fd8d45a87229c17fcd4441b7\",\"rk\":\"AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA=\",\"pn\":0,\"n\":0,\"nonce\":\"AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA\"}}",
        "{\"type\":\"dm.msg\",\"seq\":1,\"env\":{\"s\":1,\"from\":\"3eb04195fd8d45a87229c17fcd4441b7\",\"rk\":\"AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA=\",\"pn\":0,\"n\":0,\"nonce\":\"AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA\",\"ct\":\"AAAAAAAAAAAAAAAAAAAAAA==\"}}",
        "{\"type\":\"room.msg\",\"seq\":\"1\",\"env\":{}}",
    };
    for (size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); i++) {
        int rc = sc2_frame_dm_msg_decode(bad[i], strlen(bad[i]), &seq, &env, g_ct, sizeof(g_ct));
        if (rc == SC2_OK) {
            fprintf(stderr, "accepted bad dm.msg #%zu\n", i);
        }
        CHECK(rc != SC2_OK);
    }
    /* the same frame with only the x3dh removed but matching otherwise decodes */
    const char *okf = "{\"type\":\"dm.msg\",\"seq\":\"1\",\"env\":{\"s\":1,\"from\":\"3eb04195fd8d45a87229c17fcd4441b7\",\"rk\":\"AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA=\",\"pn\":0,\"n\":0,\"nonce\":\"AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA\",\"ct\":\"AAAAAAAAAAAAAAAAAAAAAA==\"}}";
    CHECK_RC(sc2_frame_dm_msg_decode(okf, strlen(okf), &seq, &env, g_ct, sizeof(g_ct)), SC2_OK);
    CHECK(!env.hdr.has_x3dh && env.ct_len == 16);
    CHECK_RC(sc2_frame_dm_msg_decode(okf, strlen(okf), &seq, &env, g_ct, 15), SC2_ERR_BOUNDS);
}

static void test_room_frames(void)
{
    sc2_room_env_t env;
    uint8_t room[16], want_room[16];
    uint64_t seq;
    size_t n, cl;
    CHECK_RC(sc2_frame_room_msg_decode(SC2V_FRAME_ROOM_MSG, strlen(SC2V_FRAME_ROOM_MSG), room, &seq, &env, g_ct,
                                       sizeof(g_ct)),
             SC2_OK);
    sc2t_hexn(want_room, 16, SC2V_ROOM_ROOM);
    CHECK_MEM(room, want_room, 16);
    CHECK(seq == 4523 && env.epoch == SC2V_ROOM_EPOCH && env.n == 2 && env.suite == 1);
    uint8_t from[16], nonce[24], sig[64], ik[32], hdr[SC2_ROOM_HDR_LEN];
    sc2t_hexn(from, 16, SC2V_ROOM_SENDER);
    CHECK_MEM(env.from, from, 16);
    sc2t_hexn(nonce, 24, SC2V_ROOM_NONCE);
    CHECK_MEM(env.nonce, nonce, 24);
    sc2t_hexn(sig, 64, SC2V_ROOM_SIG);
    CHECK_MEM(env.sig, sig, 64);
    uint8_t *ct = sc2t_hex(SC2V_ROOM_CT, &cl);
    CHECK(env.ct_len == cl);
    CHECK_MEM(env.ct, ct, cl);
    free(ct);
    /* the decoded envelope verifies against the sender IK */
    sc2t_hexn(ik, 32, SC2V_ROOM_IK_PUB);
    CHECK_RC(sc2_room_header(hdr, room, env.epoch, env.from, env.n), SC2_OK);
    CHECK_RC(sc2_room_verify(env.sig, hdr, env.nonce, env.ct, env.ct_len, ik), SC2_OK);
    CHECK_RC(sc2_frame_room_send(g_out, sizeof(g_out), &n, "s1", room, &env), SC2_OK);
    expect_line(g_out, n, SC2V_FRAME_ROOM_SEND, "room.send");
    const char *bad = "{\"type\":\"room.msg\",\"room\":\"02d7c69bb45bf2bd0c96a8120e4fef4\",\"seq\":\"1\",\"env\":{}}";
    CHECK(sc2_frame_room_msg_decode(bad, strlen(bad), room, &seq, &env, g_ct, sizeof(g_ct)) != SC2_OK);
}

static void test_posts(void)
{
    sc2_post_t p, p2;
    char room[SC2_ROOM_NAME_MAX + 1];
    uint64_t seq;
    size_t n, bl;
    CHECK_RC(sc2_frame_room_pub_decode(SC2V_FRAME_ROOM_PUB, strlen(SC2V_FRAME_ROOM_PUB), room, &seq, &p, g_body,
                                       sizeof(g_body)),
             SC2_OK);
    CHECK(strcmp(room, "pub:general") == 0 && seq == 99813 && p.t == SC2_POST_T_TEXT && !p.has_ref);
    CHECK(p.ts == 1760889600ull && p.name_len == 3 && strcmp(p.name, "Ana") == 0);
    uint8_t *body = sc2t_hex(SC2V_FRAME_POST_BODY, &bl);
    CHECK(p.body_len == bl && memcmp(p.body, body, bl) == 0);
    CHECK_RC(sc2_post_verify(&p, "relay.example", "pub:general", 1024), SC2_OK);
    CHECK_RC(sc2_post_verify(&p, "relay.example2", "pub:general", 1024), SC2_ERR_AUTH);
    CHECK_RC(sc2_post_verify(&p, "relay.example", "pub:other", 1024), SC2_ERR_AUTH);
    CHECK_RC(sc2_post_verify(&p, "relay.example", "pub:general", 10), SC2_ERR_BOUNDS);
    /* signed bytes and deterministic signature */
    static uint8_t sb[SC2_POST_SIGNED_MAX];
    size_t sbl, wl;
    uint8_t *wsb = sc2t_hex(SC2V_FRAME_POST_SIGNED, &wl);
    CHECK_RC(sc2_post_signed_bytes(sb, sizeof(sb), &sbl, "relay.example", "pub:general", &p), SC2_OK);
    CHECK(sbl == wl && memcmp(sb, wsb, wl) == 0);
    CHECK_RC(sc2_post_signed_bytes(sb, wl - 1u, &sbl, "relay.example", "pub:general", &p), SC2_ERR_BOUNDS);
    free(wsb);
    uint8_t seed[32];
    sc2t_hexn(seed, 32, SC2V_FRAME_POST_SEED);
    p2 = p;
    memset(p2.sig, 0, 64);
    CHECK_RC(sc2_post_sign(&p2, "relay.example", "pub:general", seed), SC2_OK);
    CHECK_MEM(p2.sig, p.sig, 64);
    /* encoding reproduces the reference frame */
    CHECK_RC(sc2_frame_room_post(g_out, sizeof(g_out), &n, "p1", "pub:general", &p), SC2_OK);
    expect_line(g_out, n, SC2V_FRAME_ROOM_POST, "room.post");
    /* tampering */
    char tb[256];
    memcpy(tb, p.body, p.body_len);
    tb[0] = 'H';
    p2 = p;
    p2.body = tb;
    CHECK_RC(sc2_post_verify(&p2, "relay.example", "pub:general", 1024), SC2_ERR_AUTH);
    p2 = p;
    p2.ts++;
    CHECK_RC(sc2_post_verify(&p2, "relay.example", "pub:general", 1024), SC2_ERR_AUTH);
    p2 = p;
    p2.from[0] ^= 1; /* from must be the device id of ik */
    CHECK_RC(sc2_post_verify(&p2, "relay.example", "pub:general", 1024), SC2_ERR_AUTH);
    p2 = p;
    p2.name[0] = 'B';
    CHECK_RC(sc2_post_verify(&p2, "relay.example", "pub:general", 1024), SC2_ERR_AUTH);
    p2 = p;
    p2.body = "\xff";
    p2.body_len = 1;
    CHECK_RC(sc2_post_verify(&p2, "relay.example", "pub:general", 1024), SC2_ERR_FORMAT);

    /* the ASCII-escaped encoding of the same post decodes to identical fields */
    static char body2[SC2_POST_BODY_MAX + 1];
    CHECK_RC(sc2_frame_room_pub_decode(SC2V_FRAME_ROOM_PUB_ASCII, strlen(SC2V_FRAME_ROOM_PUB_ASCII), room, &seq, &p2,
                                       body2, sizeof(body2)),
             SC2_OK);
    CHECK(p2.body_len == bl && memcmp(p2.body, body, bl) == 0);
    CHECK_MEM(p2.sig, p.sig, 64);
    CHECK_RC(sc2_post_verify(&p2, "relay.example", "pub:general", 1024), SC2_OK);
    free(body);

    /* deletion post */
    sc2_json_val_t root, pv;
    CHECK_RC(sc2_json_root(SC2V_FRAME_ROOM_POST_DEL, strlen(SC2V_FRAME_ROOM_POST_DEL), &root), SC2_OK);
    CHECK_RC(sc2_json_get(&root, "post", &pv), SC2_OK);
    CHECK_RC(sc2_post_decode(&pv, &p2, body2, sizeof(body2)), SC2_OK);
    CHECK(p2.t == SC2_POST_T_DEL && p2.has_ref && p2.body_len == 0 && p2.ts == 1760889605ull);
    CHECK_MEM(p2.ref, p.id, 8);
    CHECK_RC(sc2_post_verify(&p2, "relay.example", "pub:general", 1024), SC2_OK);
    CHECK_RC(sc2_frame_room_post(g_out, sizeof(g_out), &n, NULL, "pub:general", &p2), SC2_OK);
    expect_line(g_out, n, SC2V_FRAME_ROOM_POST_DEL, "room.post (del)");
    p2.has_ref = false;
    CHECK_RC(sc2_post_verify(&p2, "relay.example", "pub:general", 1024), SC2_ERR_FORMAT);

    /* full-size body and over-size body */
    sc2_rng_t xs = {sc2t_xs_fn, NULL};
    uint8_t s2[32], pk[32];
    sc2_ed25519_generate(s2, pk, &xs);
    static char maxbody[SC2_POST_BODY_MAX + 1];
    memset(maxbody, 'z', sizeof(maxbody));
    memset(&p2, 0, sizeof(p2));
    p2.suite = 1;
    p2.t = SC2_POST_T_TEXT;
    memcpy(p2.ik, pk, 32);
    sc2_device_id(p2.from, pk);
    p2.ts = 1;
    p2.body = maxbody;
    p2.body_len = SC2_POST_BODY_MAX;
    CHECK_RC(sc2_post_sign(&p2, "r", "pub:x", s2), SC2_OK);
    CHECK_RC(sc2_post_verify(&p2, "r", "pub:x", SC2_POST_BODY_MAX), SC2_OK);
    CHECK_RC(sc2_frame_room_post(g_out, sizeof(g_out), &n, "p9", "pub:x", &p2), SC2_OK);
    sc2_post_t p3;
    CHECK_RC(sc2_json_root(g_out, n, &root), SC2_OK);
    CHECK_RC(sc2_json_get(&root, "post", &pv), SC2_OK);
    CHECK_RC(sc2_post_decode(&pv, &p3, body2, sizeof(body2)), SC2_OK);
    CHECK_RC(sc2_post_verify(&p3, "r", "pub:x", SC2_POST_BODY_MAX), SC2_OK);
    p2.body_len = SC2_POST_BODY_MAX + 1u;
    CHECK(sc2_post_sign(&p2, "r", "pub:x", s2) != SC2_OK);
}

/* Mutation fuzzing: every decoder must reject or accept without crashing
 * (run under ASan/UBSan by tests/test_sc2_protocol.py). */
static void test_fuzz(void)
{
    static const char *seeds[] = {SC2V_FRAME_WELCOME, SC2V_FRAME_DM_MSG, SC2V_FRAME_ROOM_MSG, SC2V_FRAME_ROOM_PUB,
                                  SC2V_FRAME_ROOM_PUB_ASCII, SC2V_FRAME_ROOM_POST_DEL, SC2V_FRAME_AUTH};
    static char buf[SC2_LINE_MAX + 1];
    static const char tokens[] = "{}[]\",:\\u0e+-.9tfn \x01\x80\xff";
    unsigned accepted = 0;
    for (int it = 0; it < 30000; it++) {
        const char *seed = seeds[sc2t_xs() % (sizeof(seeds) / sizeof(seeds[0]))];
        size_t len = strlen(seed);
        memcpy(buf, seed, len);
        int muts = 1 + (int)(sc2t_xs() % 4u);
        for (int m = 0; m < muts; m++) {
            size_t at = len ? (size_t)(sc2t_xs() % len) : 0;
            switch (sc2t_xs() % 5u) {
            case 0:
                buf[at] = (char)(buf[at] ^ (char)(1u << (sc2t_xs() % 8u)));
                break;
            case 1:
                buf[at] = tokens[sc2t_xs() % (sizeof(tokens) - 1u)];
                break;
            case 2:
                len = at; /* truncate */
                break;
            case 3:
                if (len + 1u < sizeof(buf)) {
                    memmove(buf + at + 1, buf + at, len - at);
                    buf[at] = tokens[sc2t_xs() % (sizeof(tokens) - 1u)];
                    len++;
                }
                break;
            default:
                if (len > 0) {
                    memmove(buf + at, buf + at + 1, len - at - 1u);
                    len--;
                }
                break;
            }
        }
        buf[len] = '\0';
        sc2_json_val_t root;
        char type[SC2_TYPE_MAX + 1], rid[SC2_RID_MAX + 1];
        sc2_welcome_t w;
        sc2_dm_env_t de;
        sc2_room_env_t re;
        sc2_post_t p;
        uint8_t room[16];
        char rn[SC2_ROOM_NAME_MAX + 1];
        uint64_t seq;
        int ok = 0;
        ok |= sc2_frame_peek(buf, len, &root, type, rid) == SC2_OK;
        ok |= sc2_frame_welcome_decode(buf, len, &w) == SC2_OK;
        ok |= sc2_frame_dm_msg_decode(buf, len, &seq, &de, g_ct, sizeof(g_ct)) == SC2_OK;
        ok |= sc2_frame_room_msg_decode(buf, len, room, &seq, &re, g_ct, sizeof(g_ct)) == SC2_OK;
        if (sc2_frame_room_pub_decode(buf, len, rn, &seq, &p, g_body, sizeof(g_body)) == SC2_OK) {
            ok = 1;
            (void)sc2_post_verify(&p, "relay.example", rn, 1024);
        }
        accepted += (unsigned)ok;
        /* anything that validates must also round-trip through the string reader */
        if (sc2_json_root(buf, len, &root) == SC2_OK && root.type == SC2_JSON_OBJECT) {
            sc2_json_val_t v;
            if (sc2_json_get(&root, "type", &v) == SC2_OK && v.type == SC2_JSON_STRING) {
                char t[64];
                size_t tl;
                int rc = sc2_json_string(&v, t, sizeof(t), &tl);
                CHECK(rc == SC2_OK || rc == SC2_ERR_BOUNDS);
            }
        }
    }
    CHECK(accepted > 0);
    printf("fuzz: 30000 mutated frames, %u accepted by some decoder\n", accepted);
}

int main(void)
{
    test_json_reader();
    test_json_writer();
    test_lines();
    test_handshake();
    test_dm_frames();
    test_room_frames();
    test_posts();
    test_fuzz();
    return sc2t_report("sc2_frame_test");
}
