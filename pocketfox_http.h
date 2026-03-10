/*
 * pocketfox_http.h — HTTP client with redirect following and chunked decoding
 * For Mac OS X Tiger PowerPC (gcc-4.0, C99)
 */

#ifndef POCKETFOX_HTTP_H
#define POCKETFOX_HTTP_H

#include <stddef.h>

/* Forward declare SSL type */
typedef struct PocketFoxSSL PocketFoxSSL;

/* HTTP response structure */
typedef struct {
    int   status_code;       /* 200, 301, 404, etc. */
    char *headers;           /* Raw HTTP headers (null-terminated) */
    char *body;              /* Decoded body (null-terminated) */
    size_t body_len;         /* Body length in bytes */
    char  final_url[2048];   /* URL after all redirects */
    char  content_type[128]; /* Content-Type header value */
    int   redirect_count;    /* How many redirects followed */
    char  error[512];        /* Error message if failed */
} HttpResponse;

/* Fetch a URL, following up to max_redirects redirects.
 * Handles both HTTP and HTTPS, chunked transfer encoding,
 * and Content-Length.
 * Caller must free the response with http_response_free(). */
HttpResponse *http_fetch(const char *url, int max_redirects);

/* Free an HttpResponse */
void http_response_free(HttpResponse *resp);

/* URL parser (shared) */
typedef struct {
    char scheme[16];
    char host[256];
    int  port;
    char path[2048];
} PfURL;

int pf_parse_url(const char *url, PfURL *out);

#endif /* POCKETFOX_HTTP_H */
