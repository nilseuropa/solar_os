#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "solar_os_rtsp_auth.h"
#include "solar_os_rtsp_receiver.h"

static void test_digest_vector(void)
{
    solar_os_rtsp_auth_challenge_t c = {0};
    const char *www = "Digest realm=\"testrealm@host.com\", nonce=\"dcd98b7102dd2f0e8b11d0f600bfb0c093\", "
                      "qop=\"auth\", algorithm=MD5";
    assert(solar_os_rtsp_auth_challenge(www, &c) == ESP_OK);
    assert(c.digest && c.qop_auth);
    snprintf(c.cnonce, sizeof(c.cnonce), "0a4f113b");
    c.nc = 1;
    char header[384];
    assert(solar_os_rtsp_authorization(&c, "GET", "/dir/index.html", "Mufasa", "Circle Of Life",
                                      header, sizeof(header)) == ESP_OK);
    assert(strstr(header, "response=\"6629fae49393a05397450978507c4ef1\""));
}

static void test_basic_and_store(void)
{
    solar_os_rtsp_auth_challenge_t c = {0};
    assert(solar_os_rtsp_auth_challenge("Basic realm=\"cam\"", &c) == ESP_OK && !c.digest);
    char header[128];
    assert(solar_os_rtsp_authorization(&c, "DESCRIBE", "rtsp://cam/media", "user", "pass",
                                      header, sizeof(header)) == ESP_OK);
    assert(!strcmp(header, "Basic dXNlcjpwYXNz"));
    const size_t needed = strlen(header) + 1U;
    assert(solar_os_rtsp_authorization(&c, "DESCRIBE", "rtsp://cam/media", "user", "pass",
                                      header, needed) == ESP_OK);
    assert(solar_os_rtsp_authorization(&c, "DESCRIBE", "rtsp://cam/media", "user", "pass",
                                      header, needed - 1U) == ESP_ERR_INVALID_SIZE);
    assert(!header[0]);
    assert(solar_os_rtsp_auth_set("cam", 554, "user", "stored") == ESP_OK);
    char user[65], password[65];
    assert(solar_os_rtsp_auth_lookup("CAM", 554, user, sizeof(user), password, sizeof(password)) == ESP_OK);
    assert(!strcmp(user, "user") && !strcmp(password, "stored"));
    assert(solar_os_rtsp_auth_lookup("cam", 8554, user, sizeof(user), password, sizeof(password)) == ESP_ERR_NOT_FOUND);
    solar_os_rtsp_auth_account_t listed[4];
    size_t count;
    assert(solar_os_rtsp_auth_list(listed, 4, &count) == ESP_OK);
    assert(count == 1 && !strcmp(listed[0].user, "user"));
    assert(solar_os_rtsp_auth_clear("cam", 554) == ESP_OK);
    assert(solar_os_rtsp_auth_lookup("cam", 554, user, sizeof(user), password, sizeof(password)) == ESP_ERR_NOT_FOUND);
    const char *text = "RTSP/1.0 401 Unauthorized\r\nCSeq: 1\r\nWWW-Authenticate: Digest realm=\"cam\", nonce=\"n\", qop=\"auth\"\r\n\r\n";
    solar_os_rtsp_response_t r;
    assert(solar_os_rtsp_response_parse((const uint8_t *)text, strlen(text), &r) == ESP_OK);
    assert(strstr(r.www_authenticate, "nonce=\"n\""));
}

static void test_challenge_boundaries(void)
{
    solar_os_rtsp_auth_challenge_t c;
    assert(solar_os_rtsp_auth_challenge("Basic realm=\"Digest Camera, nonce=fake\"", &c) == ESP_OK);
    assert(!c.digest);
    assert(solar_os_rtsp_auth_challenge("NotBasic realm=\"cam\"", &c) == ESP_ERR_NOT_SUPPORTED);
    assert(solar_os_rtsp_auth_challenge("DigestExtra realm=\"cam\", nonce=\"n\"", &c) == ESP_ERR_NOT_SUPPORTED);
    const char *www = "Basic realm=\"Digest Camera\", dIgEsT realm=\"cam,nonce=fake\\\", qop=bogus\", "
                      "nonce\t=\t\"real-nonce\", qop=\"auth-int, auth\", opaque=\"Basic, Digest\", "
                      "Basic realm=\"other\"";
    assert(solar_os_rtsp_auth_challenge(www, &c) == ESP_OK);
    assert(c.digest && c.qop_auth);
    assert(!strcmp(c.realm, "cam,nonce=fake\", qop=bogus"));
    assert(!strcmp(c.nonce, "real-nonce"));
    assert(!strcmp(c.opaque, "Basic, Digest"));
    assert(solar_os_rtsp_auth_challenge("Digest realm=\"cam\", Basic realm=\"other\", nonce=\"n\"", &c)
           == ESP_ERR_INVALID_RESPONSE);
    assert(solar_os_rtsp_auth_challenge("Digest realm=\"cam\", nonce=\"unterminated", &c) != ESP_OK);
    assert(solar_os_rtsp_auth_challenge("Digest realm=\"cam\", nonce=\"n\", qop=\"auth-int\"", &c)
           == ESP_ERR_NOT_SUPPORTED);
    assert(solar_os_rtsp_auth_challenge("Digest realm=\"cam\", nonce=\"n\", algorithm=SHA-256", &c)
           == ESP_ERR_NOT_SUPPORTED);
}

static void test_escaped_authorization(void)
{
    solar_os_rtsp_auth_challenge_t c;
    assert(solar_os_rtsp_auth_challenge(
        "Digest realm=\"cam\\\"era\\\\room\", nonce=\"n\\\"\\\\\", opaque=\"o\\\"\\\\\", qop=\"auth\"", &c) == ESP_OK);
    assert(!strcmp(c.realm, "cam\"era\\room") && !strcmp(c.nonce, "n\"\\"));
    strcpy(c.cnonce, "c\"\\"); c.nc = 1;
    char header[SOLAR_OS_RTSP_AUTH_HEADER_MAX];
    assert(solar_os_rtsp_authorization(&c, "DESCRIBE", "rtsp://cam/media", "user", "secret",
                                      header, sizeof(header)) == ESP_OK);
    assert(strstr(header, "realm=\"cam\\\"era\\\\room\""));
    assert(strstr(header, "nonce=\"n\\\"\\\\\""));
    assert(strstr(header, "opaque=\"o\\\"\\\\\""));
    const char *prefix = "Digest username=\"domain\\\\user\\\"\", realm=\"cam\\\"era\\\\room\", "
                         "nonce=\"n\\\"\\\\\", uri=\"rtsp://cam/me\\\\dia\\\"\", algorithm=MD5, ";
    for (unsigned qop = 0; qop <= 1; qop++) {
        c.qop_auth = qop;
        assert(solar_os_rtsp_authorization(&c, "DESCRIBE", "rtsp://cam/me\\dia\"", "domain\\user\"", "secret",
                                          header, sizeof(header)) == ESP_OK);
        char expected[SOLAR_OS_RTSP_AUTH_HEADER_MAX];
        snprintf(expected, sizeof(expected), "%sresponse=\"%s\"%s, opaque=\"o\\\"\\\\\"", prefix,
                 qop ? "510faf205d9147ad42b465a2a5b06f1d" : "18efccb0d316b82b39842becd68b5148",
                 qop ? ", qop=auth, nc=00000001, cnonce=\"c\\\"\\\\\"" : "");
        assert(!strcmp(header, expected));
        size_t length = strlen(header);
        assert(solar_os_rtsp_authorization(&c, "DESCRIBE", "rtsp://cam/me\\dia\"", "domain\\user\"", "secret",
                                          header, length + 1) == ESP_OK);
        assert(solar_os_rtsp_authorization(&c, "DESCRIBE", "rtsp://cam/me\\dia\"", "domain\\user\"", "secret",
                                          header, length) == ESP_ERR_INVALID_SIZE);
        assert(!header[0]);
    }
    c.digest = false;
    assert(solar_os_rtsp_authorization(&c, "DESCRIBE", "rtsp://cam/media", "u\"ser", "pass",
                                      header, sizeof(header)) == ESP_OK);
    assert(!strcmp(header, "Basic dSJzZXI6cGFzcw=="));
}

static void test_escaped_capacity(void)
{
    solar_os_rtsp_auth_challenge_t c = {.digest = true, .qop_auth = true, .nc = 1};
    memset(c.realm, '\\', sizeof(c.realm) - 1);
    memset(c.nonce, '\\', sizeof(c.nonce) - 1);
    memset(c.opaque, '\\', sizeof(c.opaque) - 1);
    memset(c.cnonce, '\\', sizeof(c.cnonce) - 1);
    char user[SOLAR_OS_RTSP_AUTH_USER_MAX + 1] = {0}, uri[SOLAR_OS_RTSP_URI_MAX] = {0};
    memset(user, '\\', sizeof(user) - 1);
    memset(uri, '\\', sizeof(uri) - 1);
    char header[SOLAR_OS_RTSP_AUTH_HEADER_MAX];
    assert(solar_os_rtsp_authorization(&c, "DESCRIBE", uri, user, "secret", header, sizeof(header)) == ESP_OK);
    assert(strlen(header) > 768);
    char challenge[256];
    snprintf(challenge, sizeof(challenge), "Digest realm=\"%063d\", nonce=\"n\"", 0);
    assert(solar_os_rtsp_auth_challenge(challenge, &c) == ESP_OK && strlen(c.realm) == 63);
}

int main(void)
{
    test_digest_vector();
    test_basic_and_store();
    test_challenge_boundaries();
    test_escaped_authorization();
    test_escaped_capacity();
    printf("rtsp_auth_test: OK\n");
    return 0;
}
