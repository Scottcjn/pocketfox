#include "pocketfox_http.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

struct PocketFoxSSL {};

PocketFoxSSL *pocketfox_ssl_new(void) { return NULL; }
void pocketfox_ssl_free(PocketFoxSSL *ctx) { (void)ctx; }
int pocketfox_ssl_connect(PocketFoxSSL *ctx, const char *hostname, int port) {
    (void)ctx;
    (void)hostname;
    (void)port;
    return -1;
}
int pocketfox_ssl_read(PocketFoxSSL *ctx, unsigned char *buf, size_t len) {
    (void)ctx;
    (void)buf;
    (void)len;
    return -1;
}
int pocketfox_ssl_write(PocketFoxSSL *ctx, const unsigned char *buf, size_t len) {
    (void)ctx;
    (void)buf;
    (void)len;
    return -1;
}
void pocketfox_ssl_close(PocketFoxSSL *ctx) { (void)ctx; }
const char *pocketfox_ssl_error(PocketFoxSSL *ctx) {
    (void)ctx;
    return "stub";
}

static void assert_url(const char *input,
                       const char *scheme,
                       const char *host,
                       int port,
                       const char *path) {
    PfURL parsed;
    int ok = pf_parse_url(input, &parsed);

    assert(ok == 1);
    assert(strcmp(parsed.scheme, scheme) == 0);
    assert(strcmp(parsed.host, host) == 0);
    assert(parsed.port == port);
    assert(strcmp(parsed.path, path) == 0);
}

int main(void) {
    PfURL parsed;

    assert_url("https://example.com/index.html",
               "https", "example.com", 443, "/index.html");
    assert_url("http://example.com:8080/path?q=1",
               "http", "example.com", 8080, "/path?q=1");
    assert_url("example.com",
               "https", "example.com", 443, "/");
    assert_url("https://example.com?agent=pocketfox",
               "https", "example.com", 443, "?agent=pocketfox");

    memset(&parsed, 0x7f, sizeof(parsed));
    assert(pf_parse_url("https://", &parsed) == 0);
    assert(strcmp(parsed.path, "/") == 0);
    assert(strcmp(parsed.scheme, "https") == 0);

    printf("pocketfox_http URL parser tests passed\n");
    return 0;
}
