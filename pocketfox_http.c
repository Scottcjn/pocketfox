/*
 * pocketfox_http.c — HTTP client for PocketFox
 *
 * Features:
 *   - HTTP and HTTPS (via pocketfox_ssl_* for TLS)
 *   - Redirect following (301, 302, 303, 307, 308)
 *   - Chunked transfer encoding decoding
 *   - Content-Length body reading
 *   - gethostbyname DNS (Tiger-compatible, no getaddrinfo)
 *
 * Build: gcc -std=c99 -O2 -c pocketfox_http.c
 */

#include "pocketfox_http.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>    /* strncasecmp */
#include <unistd.h>
#include <errno.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netdb.h>      /* gethostbyname — Tiger-safe */
#include <arpa/inet.h>

/* SSL functions from pocketfox_ssl_tiger.c */
extern PocketFoxSSL *pocketfox_ssl_new(void);
extern void pocketfox_ssl_free(PocketFoxSSL *ctx);
extern int pocketfox_ssl_connect(PocketFoxSSL *ctx, const char *hostname, int port);
extern int pocketfox_ssl_read(PocketFoxSSL *ctx, unsigned char *buf, size_t len);
extern int pocketfox_ssl_write(PocketFoxSSL *ctx, const unsigned char *buf, size_t len);
extern void pocketfox_ssl_close(PocketFoxSSL *ctx);
extern const char *pocketfox_ssl_error(PocketFoxSSL *ctx);

/* ============================================
 * URL Parser
 * ============================================ */

int pf_parse_url(const char *url, PfURL *out) {
    memset(out, 0, sizeof(PfURL));
    strcpy(out->path, "/");
    out->port = 443;
    strcpy(out->scheme, "https");

    const char *p = url;

    if (strncmp(p, "https://", 8) == 0) {
        p += 8;
    } else if (strncmp(p, "http://", 7) == 0) {
        strcpy(out->scheme, "http");
        out->port = 80;
        p += 7;
    }

    int i = 0;
    while (*p && *p != '/' && *p != ':' && *p != '?' && i < 255) {
        out->host[i++] = *p++;
    }
    out->host[i] = '\0';

    if (*p == ':') {
        p++;
        out->port = atoi(p);
        while (*p && *p != '/' && *p != '?') p++;
    }

    if (*p == '/' || *p == '?') {
        strncpy(out->path, p, sizeof(out->path) - 1);
    }

    return strlen(out->host) > 0;
}

/* ============================================
 * Plain TCP connection (for HTTP)
 * ============================================ */

typedef struct {
    int fd;
} PlainSocket;

static PlainSocket *plain_connect(const char *hostname, int port) {
    /* Tiger-safe DNS: gethostbyname */
    struct hostent *he = gethostbyname(hostname);
    if (!he) return NULL;

    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return NULL;

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons((uint16_t)port);
    memcpy(&addr.sin_addr, he->h_addr_list[0], (size_t)he->h_length);

    if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        close(fd);
        return NULL;
    }

    PlainSocket *s = (PlainSocket *)calloc(1, sizeof(PlainSocket));
    s->fd = fd;
    return s;
}

static int plain_write(PlainSocket *s, const void *buf, size_t len) {
    return (int)send(s->fd, buf, len, 0);
}

static int plain_read(PlainSocket *s, void *buf, size_t len) {
    return (int)recv(s->fd, buf, len, 0);
}

static void plain_close(PlainSocket *s) {
    if (s) { close(s->fd); free(s); }
}

/* ============================================
 * Unified I/O (HTTP or HTTPS)
 * ============================================ */

typedef struct {
    int is_ssl;
    PocketFoxSSL *ssl;
    PlainSocket  *plain;
} Connection;

static Connection *conn_open(const char *host, int port, int use_ssl) {
    Connection *c = (Connection *)calloc(1, sizeof(Connection));
    c->is_ssl = use_ssl;

    if (use_ssl) {
        c->ssl = pocketfox_ssl_new();
        if (!c->ssl || pocketfox_ssl_connect(c->ssl, host, port) != 0) {
            if (c->ssl) pocketfox_ssl_free(c->ssl);
            free(c);
            return NULL;
        }
    } else {
        c->plain = plain_connect(host, port);
        if (!c->plain) { free(c); return NULL; }
    }
    return c;
}

static int conn_write(Connection *c, const void *buf, size_t len) {
    if (c->is_ssl)
        return pocketfox_ssl_write(c->ssl, (const unsigned char *)buf, len);
    else
        return plain_write(c->plain, buf, len);
}

static int conn_read(Connection *c, void *buf, size_t len) {
    if (c->is_ssl)
        return pocketfox_ssl_read(c->ssl, (unsigned char *)buf, len);
    else
        return plain_read(c->plain, buf, len);
}

static void conn_close(Connection *c) {
    if (!c) return;
    if (c->is_ssl) {
        pocketfox_ssl_close(c->ssl);
        pocketfox_ssl_free(c->ssl);
    } else {
        plain_close(c->plain);
    }
    free(c);
}

/* ============================================
 * Header Parsing Helpers
 * ============================================ */

/* Case-insensitive header search. Returns value start (after ": ") */
static const char *find_header(const char *headers, const char *name) {
    size_t nlen = strlen(name);
    const char *p = headers;
    while (*p) {
        if (strncasecmp(p, name, nlen) == 0 && p[nlen] == ':') {
            p += nlen + 1;
            while (*p == ' ') p++;
            return p;
        }
        /* Skip to next line */
        while (*p && *p != '\n') p++;
        if (*p) p++;
    }
    return NULL;
}

/* Extract header value into buffer (up to \r\n or \n) */
static void extract_header_value(const char *start, char *buf, size_t bufsz) {
    size_t i = 0;
    while (start[i] && start[i] != '\r' && start[i] != '\n' && i < bufsz - 1) {
        buf[i] = start[i];
        i++;
    }
    buf[i] = '\0';
}

/* ============================================
 * Chunked Transfer Decoding
 * ============================================ */

static char *decode_chunked(const char *data, size_t data_len, size_t *out_len) {
    size_t capacity = data_len;
    char *out = (char *)malloc(capacity + 1);
    size_t total = 0;
    size_t pos = 0;

    while (pos < data_len) {
        /* Read chunk size (hex) */
        unsigned long chunk_size = 0;
        while (pos < data_len) {
            char c = data[pos];
            if (c >= '0' && c <= '9') { chunk_size = chunk_size * 16 + (c - '0'); pos++; }
            else if (c >= 'a' && c <= 'f') { chunk_size = chunk_size * 16 + (c - 'a' + 10); pos++; }
            else if (c >= 'A' && c <= 'F') { chunk_size = chunk_size * 16 + (c - 'A' + 10); pos++; }
            else break;
        }

        /* Skip to end of chunk header line (\r\n) */
        while (pos < data_len && data[pos] != '\n') pos++;
        if (pos < data_len) pos++; /* skip \n */

        if (chunk_size == 0) break; /* Last chunk */

        /* Copy chunk data */
        if (total + chunk_size > capacity) {
            capacity = total + chunk_size + 4096;
            out = (char *)realloc(out, capacity + 1);
        }
        size_t to_copy = chunk_size;
        if (pos + to_copy > data_len) to_copy = data_len - pos;
        memcpy(out + total, data + pos, to_copy);
        total += to_copy;
        pos += to_copy;

        /* Skip trailing \r\n after chunk */
        if (pos < data_len && data[pos] == '\r') pos++;
        if (pos < data_len && data[pos] == '\n') pos++;
    }

    out[total] = '\0';
    *out_len = total;
    return out;
}

/* ============================================
 * Resolve relative redirect URL
 * ============================================ */

static void resolve_redirect(const char *base_url, const char *location,
                             char *resolved, size_t resolved_sz) {
    if (strncmp(location, "http://", 7) == 0 ||
        strncmp(location, "https://", 8) == 0) {
        /* Absolute URL */
        strncpy(resolved, location, resolved_sz - 1);
        resolved[resolved_sz - 1] = '\0';
        return;
    }

    /* Relative URL — combine with base */
    PfURL base;
    pf_parse_url(base_url, &base);

    if (location[0] == '/') {
        /* Absolute path */
        snprintf(resolved, resolved_sz, "%s://%s:%d%s",
                 base.scheme, base.host, base.port, location);
    } else {
        /* Relative path (append to directory of current path) */
        char dir[2048];
        strncpy(dir, base.path, sizeof(dir) - 1);
        char *last_slash = strrchr(dir, '/');
        if (last_slash) *(last_slash + 1) = '\0';
        else strcpy(dir, "/");

        snprintf(resolved, resolved_sz, "%s://%s:%d%s%s",
                 base.scheme, base.host, base.port, dir, location);
    }
}

/* ============================================
 * Main HTTP Fetch
 * ============================================ */

static HttpResponse *http_fetch_single(const char *url) {
    HttpResponse *resp = (HttpResponse *)calloc(1, sizeof(HttpResponse));
    strncpy(resp->final_url, url, sizeof(resp->final_url) - 1);

    PfURL parsed;
    if (!pf_parse_url(url, &parsed)) {
        snprintf(resp->error, sizeof(resp->error), "Invalid URL: %s", url);
        resp->status_code = -1;
        return resp;
    }

    int use_ssl = (strcmp(parsed.scheme, "https") == 0);

    fprintf(stderr, "[PocketFox] %s %s:%d%s\n",
            use_ssl ? "HTTPS" : "HTTP", parsed.host, parsed.port, parsed.path);

    Connection *conn = conn_open(parsed.host, parsed.port, use_ssl);
    if (!conn) {
        snprintf(resp->error, sizeof(resp->error),
                 "Connection failed: %s:%d", parsed.host, parsed.port);
        resp->status_code = -1;
        return resp;
    }

    /* Send HTTP request */
    char request[4096];
    snprintf(request, sizeof(request),
             "GET %s HTTP/1.1\r\n"
             "Host: %s\r\n"
             "User-Agent: PocketFox/2.0 (PowerPC; Mac OS X Tiger)\r\n"
             "Accept: text/html, application/xhtml+xml, text/plain, */*\r\n"
             "Accept-Encoding: identity\r\n"
             "Connection: close\r\n\r\n",
             parsed.path, parsed.host);

    conn_write(conn, request, strlen(request));

    /* Read full response */
    size_t total = 0;
    size_t capacity = 65536;
    char *raw = (char *)malloc(capacity);

    while (1) {
        if (total + 4096 > capacity) {
            capacity *= 2;
            raw = (char *)realloc(raw, capacity);
        }
        int n = conn_read(conn, raw + total, 4096);
        if (n <= 0) break;
        total += n;
    }
    raw[total] = '\0';
    conn_close(conn);

    if (total == 0) {
        snprintf(resp->error, sizeof(resp->error), "Empty response from server");
        resp->status_code = -1;
        free(raw);
        return resp;
    }

    /* Split headers and body */
    char *body_start = strstr(raw, "\r\n\r\n");
    size_t header_len;
    if (body_start) {
        header_len = body_start - raw;
        body_start += 4;
    } else {
        body_start = strstr(raw, "\n\n");
        if (body_start) {
            header_len = body_start - raw;
            body_start += 2;
        } else {
            header_len = total;
            body_start = raw + total;
        }
    }

    /* Copy headers */
    resp->headers = (char *)malloc(header_len + 1);
    memcpy(resp->headers, raw, header_len);
    resp->headers[header_len] = '\0';

    /* Parse status code: "HTTP/1.x NNN" */
    const char *sp = strchr(resp->headers, ' ');
    if (sp) resp->status_code = atoi(sp + 1);

    /* Parse Content-Type */
    const char *ct = find_header(resp->headers, "Content-Type");
    if (ct) extract_header_value(ct, resp->content_type, sizeof(resp->content_type));

    /* Check for chunked transfer */
    const char *te = find_header(resp->headers, "Transfer-Encoding");
    int is_chunked = 0;
    if (te) {
        char te_val[64];
        extract_header_value(te, te_val, sizeof(te_val));
        if (strncasecmp(te_val, "chunked", 7) == 0) is_chunked = 1;
    }

    /* Extract body */
    size_t raw_body_len = total - (body_start - raw);

    if (is_chunked) {
        resp->body = decode_chunked(body_start, raw_body_len, &resp->body_len);
    } else {
        resp->body = (char *)malloc(raw_body_len + 1);
        memcpy(resp->body, body_start, raw_body_len);
        resp->body[raw_body_len] = '\0';
        resp->body_len = raw_body_len;
    }

    free(raw);
    return resp;
}

/* ============================================
 * Public API: Fetch with redirect following
 * ============================================ */

HttpResponse *http_fetch(const char *url, int max_redirects) {
    char current_url[2048];
    strncpy(current_url, url, sizeof(current_url) - 1);
    current_url[sizeof(current_url) - 1] = '\0';

    int redirects = 0;

    while (1) {
        HttpResponse *resp = http_fetch_single(current_url);
        if (!resp) return NULL;

        resp->redirect_count = redirects;

        /* Check for redirect */
        if (resp->status_code >= 301 && resp->status_code <= 308 &&
            resp->status_code != 304 && resp->status_code != 305) {

            if (redirects >= max_redirects) {
                snprintf(resp->error, sizeof(resp->error),
                         "Too many redirects (%d)", redirects);
                return resp;
            }

            const char *loc = find_header(resp->headers, "Location");
            if (!loc) {
                snprintf(resp->error, sizeof(resp->error),
                         "Redirect %d but no Location header", resp->status_code);
                return resp;
            }

            char location[2048];
            extract_header_value(loc, location, sizeof(location));

            char resolved[2048];
            resolve_redirect(current_url, location, resolved, sizeof(resolved));

            fprintf(stderr, "[PocketFox] Redirect %d -> %s\n",
                    resp->status_code, resolved);

            http_response_free(resp);
            strncpy(current_url, resolved, sizeof(current_url) - 1);
            redirects++;
            continue;
        }

        strncpy(resp->final_url, current_url, sizeof(resp->final_url) - 1);
        return resp;
    }
}

void http_response_free(HttpResponse *resp) {
    if (!resp) return;
    free(resp->headers);
    free(resp->body);
    free(resp);
}
