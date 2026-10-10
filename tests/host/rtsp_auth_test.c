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
    assert(solar_os_rtsp_auth_set("cam", 554, "user", "stored") == ESP_OK);
    char user[65], password[65];
    assert(solar_os_rtsp_auth_lookup("CAM", 554, user, sizeof(user), password, sizeof(password)) == ESP_OK);
    assert(!strcmp(user, "user") && !strcmp(password, "stored"));
    assert(solar_os_rtsp_auth_lookup("cam", 8554, user, sizeof(user), password, sizeof(password)) == ESP_ERR_NOT_FOUND);
    solar_os_rtsp_auth_account_t listed[4];
    assert(solar_os_rtsp_auth_list(listed, 4) == 1 && !strcmp(listed[0].user, "user"));
    assert(solar_os_rtsp_auth_clear("cam", 554) == ESP_OK);
    assert(solar_os_rtsp_auth_lookup("cam", 554, user, sizeof(user), password, sizeof(password)) == ESP_ERR_NOT_FOUND);
    const char *text = "RTSP/1.0 401 Unauthorized\r\nCSeq: 1\r\nWWW-Authenticate: Digest realm=\"cam\", nonce=\"n\", qop=\"auth\"\r\n\r\n";
    solar_os_rtsp_response_t r;
    assert(solar_os_rtsp_response_parse((const uint8_t *)text, strlen(text), &r) == ESP_OK);
    assert(strstr(r.www_authenticate, "nonce=\"n\""));
}

int main(void)
{
    test_digest_vector();
    test_basic_and_store();
    printf("rtsp_auth_test: OK\n");
    return 0;
}
