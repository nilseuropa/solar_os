#include <assert.h>
#include <string.h>
#include "solar_os_http_policy.h"

static void resolved(const char *base, const char *location, const char *expected)
{
    char url[SOLAR_OS_HTTP_URL_MAX];
    assert(solar_os_http_redirect_url(base, location, url, sizeof(url)));
    assert(!strcmp(url, expected));
}

int main(void)
{
    solar_os_http_origin_t a, b;
    assert(solar_os_http_origin_parse("https://EXAMPLE.com/x", &a));
    assert(solar_os_http_origin_parse("https://example.com:443/y", &b));
    assert(solar_os_http_same_origin(&a, &b));
    assert(solar_os_http_origin_parse("https://example.com:444/", &b));
    assert(!solar_os_http_same_origin(&a, &b));
    assert(solar_os_http_origin_parse("http://example.com:443/", &b));
    assert(!solar_os_http_same_origin(&a, &b));
    assert(!solar_os_http_origin_parse("ftp://example.com/", &a));
    assert(!solar_os_http_origin_parse("https://example.com:0/", &a));
    assert(solar_os_http_origin_parse("http://[::1]:8080/", &a) && a.port == 8080);
    resolved("https://example.com/a/b?old", "../c", "https://example.com/c");
    resolved("https://example.com/a/b", "./c", "https://example.com/a/c");
    resolved("https://example.com/a/b", "/c/../d", "https://example.com/d");
    resolved("https://example.com/a/b", "?new", "https://example.com/a/b?new");
    resolved("https://example.com", "?new", "https://example.com/?new");
    resolved("https://example.com/a/b", "//other.com/z", "https://other.com/z");
    resolved("https://example.com/a/b", "https://other.com/z", "https://other.com/z");
    resolved("http://[::1]:8080/a", "/b", "http://[::1]:8080/b");
    char url[SOLAR_OS_HTTP_URL_MAX];
    assert(!solar_os_http_redirect_url("https://a.com/", "http://a.com/", url, sizeof(url)));
    assert(!solar_os_http_redirect_url("https://a.com/", "https://user:pass@b.com/", url, sizeof(url)));
    assert(!solar_os_http_redirect_url("https://a.com/", "/abc", url, 4));
    assert(!solar_os_http_redirect_url("https://a.com/", "https://b.com/\r\nHost:evil", url, sizeof(url)));
    for (int status = 200; status < 600; ++status)
        assert(solar_os_http_redirect_status(status) ==
            (status == 301 || status == 302 || status == 303 || status == 307 || status == 308));
}
