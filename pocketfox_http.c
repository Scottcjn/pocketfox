/*
 * PocketFox HTTP client for Mac OS X Tiger / PowerPC
 *
 * Features:
 *   - HTTP over plain BSD sockets
 *   - HTTPS via PocketFoxSSL / mbedTLS
 *   - 301/302/307 redirect following
 *   - Chunked transfer decoding
 *   - Content-Length parsing
 *   - Connection: close handling
 */

#include "pocketfox_http.h"
#include "pocketfox_ssl.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>

#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netdb.h>

#define HTTP_IO_BUFFER_SIZE 4096
#define HTTP_MAX_PATH_LEN   4096

typedef struct {
    char scheme[8];
    char host[256];
    int port;
    char path[HTTP_MAX_PATH_LEN];
} ParsedUrl;

typedef struct {
    int use_ssl;
    int socket_fd;
    PocketFoxSSL* ssl;
} HttpConnection;

static char* http_strdup_local(const char* src) {
    size_t len;
    char* copy;

    if (!src) {
        return NULL;
    }

    len = strlen(src);
    copy = (char*)malloc(len + 1);
    if (!copy) {
        return NULL;
    }

    memcpy(copy, src, len);
    copy[len] = '\0';
    return copy;
}

static char* http_strndup_local(const char* src, size_t len) {
    char* copy;

    if (!src) {
        return NULL;
    }

    copy = (char*)malloc(len + 1);
    if (!copy) {
        return NULL;
    }

    if (len > 0) {
        memcpy(copy, src, len);
    }
    copy[len] = '\0';
    return copy;
}

static int buffer_reserve(char** buffer, size_t* capacity, size_t wanted_len) {
    size_t new_capacity;
    char* new_buffer;

    if (*capacity > wanted_len) {
        return 1;
    }

    new_capacity = (*capacity == 0) ? 1024 : *capacity;
    while (new_capacity <= wanted_len) {
        new_capacity *= 2;
    }

    new_buffer = (char*)realloc(*buffer, new_capacity);
    if (!new_buffer) {
        return 0;
    }

    *buffer = new_buffer;
    *capacity = new_capacity;
    return 1;
}

static int buffer_append(char** buffer, size_t* length, size_t* capacity,
                         const void* data, size_t data_len) {
    if (!buffer_reserve(buffer, capacity, *length + data_len)) {
        return 0;
    }

    if (data_len > 0) {
        memcpy(*buffer + *length, data, data_len);
    }
    *length += data_len;
    (*buffer)[*length] = '\0';
    return 1;
}

static int is_default_port(const char* scheme, int port) {
    if (strcmp(scheme, "http") == 0) {
        return port == 80;
    }
    if (strcmp(scheme, "https") == 0) {
        return port == 443;
    }
    return 0;
}

static int parse_url(const char* url, ParsedUrl* out) {
    const char* p;
    size_t host_len;
    size_t path_len;

    if (!url || !out) {
        return 0;
    }

    memset(out, 0, sizeof(ParsedUrl));
    strcpy(out->scheme, "https");
    out->port = 443;
    strcpy(out->path, "/");

    p = url;
    if (strncasecmp(p, "https://", 8) == 0) {
        p += 8;
    } else if (strncasecmp(p, "http://", 7) == 0) {
        strcpy(out->scheme, "http");
        out->port = 80;
        p += 7;
    }

    host_len = 0;
    while (*p && *p != '/' && *p != ':' && *p != '?' && *p != '#') {
        if (host_len + 1 >= sizeof(out->host)) {
            return 0;
        }
        out->host[host_len++] = *p++;
    }
    out->host[host_len] = '\0';

    if (host_len == 0) {
        return 0;
    }

    if (*p == ':') {
        char port_buf[16];
        size_t port_len;

        p++;
        port_len = 0;
        while (*p && *p != '/' && *p != '?' && *p != '#') {
            if (!isdigit((unsigned char)*p) || port_len + 1 >= sizeof(port_buf)) {
                return 0;
            }
            port_buf[port_len++] = *p++;
        }
        if (port_len == 0) {
            return 0;
        }
        port_buf[port_len] = '\0';
        out->port = atoi(port_buf);
        if (out->port <= 0 || out->port > 65535) {
            return 0;
        }
    }

    if (*p == '/') {
        path_len = 0;
        while (*p && *p != '#') {
            if (path_len + 1 >= sizeof(out->path)) {
                return 0;
            }
            out->path[path_len++] = *p++;
        }
        out->path[path_len] = '\0';
    } else if (*p == '?') {
        path_len = 1;
        out->path[0] = '/';
        while (*p && *p != '#') {
            if (path_len + 1 >= sizeof(out->path)) {
                return 0;
            }
            out->path[path_len++] = *p++;
        }
        out->path[path_len] = '\0';
    }

    if (out->path[0] == '\0') {
        strcpy(out->path, "/");
    }

    return 1;
}

static int connection_read(HttpConnection* conn, unsigned char* buffer, size_t len) {
    if (conn->use_ssl) {
        return pocketfox_ssl_read(conn->ssl, buffer, len);
    }
    return (int)recv(conn->socket_fd, buffer, len, 0);
}

static int connection_write_all(HttpConnection* conn, const unsigned char* buffer, size_t len) {
    size_t written;

    written = 0;
    while (written < len) {
        int rc;

        if (conn->use_ssl) {
            rc = pocketfox_ssl_write(conn->ssl, buffer + written, len - written);
        } else {
            rc = (int)send(conn->socket_fd, buffer + written, len - written, 0);
        }

        if (rc <= 0) {
            return -1;
        }
        written += (size_t)rc;
    }

    return 0;
}

static void connection_close(HttpConnection* conn) {
    if (!conn) {
        return;
    }

    if (conn->use_ssl) {
        if (conn->ssl) {
            pocketfox_ssl_close(conn->ssl);
            pocketfox_ssl_free(conn->ssl);
        }
    } else if (conn->socket_fd >= 0) {
        close(conn->socket_fd);
    }

    conn->ssl = NULL;
    conn->socket_fd = -1;
    conn->use_ssl = 0;
}

static int connection_open_http(HttpConnection* conn, const ParsedUrl* url) {
    struct hostent* he;
    struct sockaddr_in addr;

    he = gethostbyname(url->host);
    if (!he || !he->h_addr_list || !he->h_addr_list[0]) {
        return -1;
    }

    conn->socket_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (conn->socket_fd < 0) {
        return -1;
    }

    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons((unsigned short)url->port);
    memcpy(&addr.sin_addr, he->h_addr_list[0], (size_t)he->h_length);

    if (connect(conn->socket_fd, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        close(conn->socket_fd);
        conn->socket_fd = -1;
        return -1;
    }

    conn->use_ssl = 0;
    return 0;
}

static int connection_open_https(HttpConnection* conn, const ParsedUrl* url) {
    if (pocketfox_ssl_init() != 0) {
        return -1;
    }

    conn->ssl = pocketfox_ssl_new();
    if (!conn->ssl) {
        return -1;
    }

    if (pocketfox_ssl_connect(conn->ssl, url->host, url->port) != 0) {
        pocketfox_ssl_free(conn->ssl);
        conn->ssl = NULL;
        return -1;
    }

    conn->use_ssl = 1;
    return 0;
}

static int connection_open(HttpConnection* conn, const ParsedUrl* url) {
    memset(conn, 0, sizeof(HttpConnection));
    conn->socket_fd = -1;

    if (strcmp(url->scheme, "https") == 0) {
        return connection_open_https(conn, url);
    }
    return connection_open_http(conn, url);
}

static int build_request(const ParsedUrl* url, char** out_request) {
    char host_header[320];
    size_t request_size;
    char* request;

    if (is_default_port(url->scheme, url->port)) {
        snprintf(host_header, sizeof(host_header), "%s", url->host);
    } else {
        snprintf(host_header, sizeof(host_header), "%s:%d", url->host, url->port);
    }

    request_size = strlen(url->path) + strlen(host_header) + 160;
    request = (char*)malloc(request_size);
    if (!request) {
        return 0;
    }

    snprintf(request, request_size,
             "GET %s HTTP/1.1\r\n"
             "Host: %s\r\n"
             "User-Agent: PocketFox/1.0 (PowerPC Tiger)\r\n"
             "Accept: */*\r\n"
             "Connection: close\r\n"
             "\r\n",
             url->path, host_header);

    *out_request = request;
    return 1;
}

static int read_headers(HttpConnection* conn, char** out_headers) {
    char* headers;
    size_t length;
    size_t capacity;

    headers = NULL;
    length = 0;
    capacity = 0;

    while (1) {
        unsigned char byte;
        int rc;

        rc = connection_read(conn, &byte, 1);
        if (rc <= 0) {
            free(headers);
            return 0;
        }

        if (!buffer_append(&headers, &length, &capacity, &byte, 1)) {
            free(headers);
            return 0;
        }

        if (length >= 4 &&
            headers[length - 4] == '\r' &&
            headers[length - 3] == '\n' &&
            headers[length - 2] == '\r' &&
            headers[length - 1] == '\n') {
            *out_headers = headers;
            return 1;
        }
    }
}

static int parse_status_code(const char* headers) {
    const char* line_end;
    const char* space;

    if (!headers) {
        return 0;
    }

    line_end = strstr(headers, "\r\n");
    if (!line_end) {
        line_end = headers + strlen(headers);
    }

    space = (const char*)memchr(headers, ' ', (size_t)(line_end - headers));
    if (!space) {
        return 0;
    }

    while (space < line_end && *space == ' ') {
        space++;
    }

    if (space >= line_end) {
        return 0;
    }

    return atoi(space);
}

static char* get_header_value(const char* headers, const char* name) {
    const char* line;
    const char* name_end;
    size_t wanted_len;

    if (!headers || !name) {
        return NULL;
    }

    wanted_len = strlen(name);
    line = strstr(headers, "\r\n");
    if (line) {
        line += 2;
    } else {
        line = headers;
    }

    while (*line) {
        const char* line_end;
        const char* colon;
        const char* value_start;
        const char* value_end;

        line_end = strstr(line, "\r\n");
        if (!line_end) {
            line_end = line + strlen(line);
        }

        if (line_end == line) {
            break;
        }

        colon = (const char*)memchr(line, ':', (size_t)(line_end - line));
        if (colon) {
            name_end = colon;
            while (name_end > line &&
                   (name_end[-1] == ' ' || name_end[-1] == '\t')) {
                name_end--;
            }

            if ((size_t)(name_end - line) == wanted_len &&
                strncasecmp(line, name, wanted_len) == 0) {
                value_start = colon + 1;
                while (value_start < line_end &&
                       (*value_start == ' ' || *value_start == '\t')) {
                    value_start++;
                }

                value_end = line_end;
                while (value_end > value_start &&
                       (value_end[-1] == ' ' || value_end[-1] == '\t')) {
                    value_end--;
                }

                return http_strndup_local(value_start,
                                          (size_t)(value_end - value_start));
            }
        }

        if (*line_end == '\0') {
            break;
        }
        line = line_end + 2;
    }

    return NULL;
}

static int value_has_token(const char* value, const char* token) {
    size_t token_len;
    const char* p;

    if (!value || !token) {
        return 0;
    }

    token_len = strlen(token);
    p = value;

    while (*p) {
        const char* end;

        while (*p == ' ' || *p == '\t' || *p == ',') {
            p++;
        }

        end = p;
        while (*end && *end != ',') {
            end++;
        }
        while (end > p && (end[-1] == ' ' || end[-1] == '\t')) {
            end--;
        }

        if ((size_t)(end - p) == token_len &&
            strncasecmp(p, token, token_len) == 0) {
            return 1;
        }

        p = end;
        if (*p == ',') {
            p++;
        }
    }

    return 0;
}

static int read_exact(HttpConnection* conn, unsigned char* buffer, size_t len) {
    size_t total;

    total = 0;
    while (total < len) {
        int rc;

        rc = connection_read(conn, buffer + total, len - total);
        if (rc <= 0) {
            return -1;
        }
        total += (size_t)rc;
    }

    return 0;
}

static char* read_fixed_body(HttpConnection* conn, size_t wanted_len, size_t* out_len) {
    char* body;
    size_t total;

    body = (char*)malloc(wanted_len + 1);
    if (!body) {
        return NULL;
    }

    total = 0;
    while (total < wanted_len) {
        size_t chunk_len;
        int rc;

        chunk_len = wanted_len - total;
        if (chunk_len > HTTP_IO_BUFFER_SIZE) {
            chunk_len = HTTP_IO_BUFFER_SIZE;
        }

        rc = connection_read(conn, (unsigned char*)body + total, chunk_len);
        if (rc <= 0) {
            free(body);
            return NULL;
        }
        total += (size_t)rc;
    }

    body[total] = '\0';
    *out_len = total;
    return body;
}

static char* read_until_close(HttpConnection* conn, size_t* out_len) {
    char* body;
    size_t length;
    size_t capacity;
    int rc;

    body = NULL;
    length = 0;
    capacity = 0;

    while (1) {
        unsigned char buffer[HTTP_IO_BUFFER_SIZE];

        rc = connection_read(conn, buffer, sizeof(buffer));
        if (rc == 0) {
            break;
        }
        if (rc < 0) {
            free(body);
            return NULL;
        }

        if (!buffer_append(&body, &length, &capacity, buffer, (size_t)rc)) {
            free(body);
            return NULL;
        }
    }

    if (!body) {
        body = (char*)malloc(1);
        if (!body) {
            return NULL;
        }
        body[0] = '\0';
    }

    *out_len = length;
    return body;
}

static int read_line(HttpConnection* conn, char** out_line) {
    char* line;
    size_t length;
    size_t capacity;

    line = NULL;
    length = 0;
    capacity = 0;

    while (1) {
        unsigned char byte;
        int rc;

        rc = connection_read(conn, &byte, 1);
        if (rc <= 0) {
            free(line);
            return 0;
        }

        if (!buffer_append(&line, &length, &capacity, &byte, 1)) {
            free(line);
            return 0;
        }

        if (byte == '\n') {
            break;
        }
    }

    while (length > 0 &&
           (line[length - 1] == '\n' || line[length - 1] == '\r')) {
        line[--length] = '\0';
    }

    if (!line) {
        line = (char*)malloc(1);
        if (!line) {
            return 0;
        }
        line[0] = '\0';
    }

    *out_line = line;
    return 1;
}

static int parse_chunk_size(const char* line, size_t* out_size) {
    char* endptr;
    unsigned long value;

    if (!line || !out_size) {
        return 0;
    }

    while (*line == ' ' || *line == '\t') {
        line++;
    }

    value = strtoul(line, &endptr, 16);
    if (endptr == line) {
        return 0;
    }

    while (*endptr == ' ' || *endptr == '\t') {
        endptr++;
    }

    if (*endptr != '\0' && *endptr != ';') {
        return 0;
    }

    *out_size = (size_t)value;
    return 1;
}

static char* read_chunked_body(HttpConnection* conn, size_t* out_len) {
    char* body;
    size_t length;
    size_t capacity;

    body = NULL;
    length = 0;
    capacity = 0;

    while (1) {
        char* line;
        size_t chunk_size;

        line = NULL;
        chunk_size = 0;

        if (!read_line(conn, &line)) {
            free(body);
            return NULL;
        }

        if (!parse_chunk_size(line, &chunk_size)) {
            free(line);
            free(body);
            return NULL;
        }
        free(line);

        if (chunk_size == 0) {
            while (1) {
                if (!read_line(conn, &line)) {
                    free(body);
                    return NULL;
                }
                if (line[0] == '\0') {
                    free(line);
                    break;
                }
                free(line);
            }
            break;
        }

        if (!buffer_reserve(&body, &capacity, length + chunk_size)) {
            free(body);
            return NULL;
        }

        if (read_exact(conn, (unsigned char*)body + length, chunk_size) != 0) {
            free(body);
            return NULL;
        }
        length += chunk_size;
        body[length] = '\0';

        {
            unsigned char crlf[2];
            if (read_exact(conn, crlf, 2) != 0) {
                free(body);
                return NULL;
            }
        }
    }

    if (!body) {
        body = (char*)malloc(1);
        if (!body) {
            return NULL;
        }
        body[0] = '\0';
    }

    *out_len = length;
    return body;
}

static char* duplicate_path_without_query(const char* path) {
    const char* end;

    if (!path || path[0] == '\0') {
        return http_strdup_local("/");
    }

    end = path;
    while (*end && *end != '?' && *end != '#') {
        end++;
    }

    if (end == path) {
        return http_strdup_local("/");
    }

    return http_strndup_local(path, (size_t)(end - path));
}

static char* normalize_path_copy(const char* input) {
    char path_part[HTTP_MAX_PATH_LEN];
    char query_part[HTTP_MAX_PATH_LEN];
    char work[HTTP_MAX_PATH_LEN];
    char* segments[256];
    char* out;
    size_t path_len;
    size_t query_len;
    size_t out_len;
    int seg_count;
    int trailing_slash;
    char* cursor;

    if (!input) {
        return NULL;
    }

    path_len = 0;
    while (input[path_len] && input[path_len] != '?' && input[path_len] != '#') {
        if (path_len + 1 >= sizeof(path_part)) {
            return NULL;
        }
        path_part[path_len] = input[path_len];
        path_len++;
    }
    path_part[path_len] = '\0';

    query_len = 0;
    if (input[path_len] == '?') {
        size_t i;

        i = path_len;
        while (input[i] && input[i] != '#') {
            if (query_len + 1 >= sizeof(query_part)) {
                return NULL;
            }
            query_part[query_len++] = input[i++];
        }
    }
    query_part[query_len] = '\0';

    if (path_part[0] == '\0') {
        strcpy(path_part, "/");
    }

    if (path_part[0] != '/') {
        if (strlen(path_part) + 2 > sizeof(work)) {
            return NULL;
        }
        work[0] = '/';
        strcpy(work + 1, path_part);
    } else {
        if (strlen(path_part) + 1 > sizeof(work)) {
            return NULL;
        }
        strcpy(work, path_part);
    }

    trailing_slash = (strlen(work) > 1 && work[strlen(work) - 1] == '/');
    seg_count = 0;
    cursor = work;
    if (*cursor == '/') {
        cursor++;
    }

    while (1) {
        char* slash;

        slash = strchr(cursor, '/');
        if (slash) {
            *slash = '\0';
        }

        if (*cursor != '\0') {
            if (strcmp(cursor, ".") == 0) {
                /* Skip "." */
            } else if (strcmp(cursor, "..") == 0) {
                if (seg_count > 0) {
                    seg_count--;
                }
            } else {
                if (seg_count >= (int)(sizeof(segments) / sizeof(segments[0]))) {
                    return NULL;
                }
                segments[seg_count++] = cursor;
            }
        }

        if (!slash) {
            break;
        }
        cursor = slash + 1;
    }

    out_len = 1;
    if (seg_count > 0) {
        int i;

        for (i = 0; i < seg_count; i++) {
            out_len += strlen(segments[i]) + 1;
        }
    }
    if (trailing_slash && seg_count > 0) {
        out_len++;
    }
    out_len += query_len + 1;

    out = (char*)malloc(out_len);
    if (!out) {
        return NULL;
    }

    out[0] = '/';
    out_len = 1;

    if (seg_count > 0) {
        int i;

        for (i = 0; i < seg_count; i++) {
            size_t seg_len;

            if (out_len > 1) {
                out[out_len++] = '/';
            }
            seg_len = strlen(segments[i]);
            memcpy(out + out_len, segments[i], seg_len);
            out_len += seg_len;
        }
    }

    if (trailing_slash && out[out_len - 1] != '/') {
        out[out_len++] = '/';
    }

    if (query_len > 0) {
        memcpy(out + out_len, query_part, query_len);
        out_len += query_len;
    }

    out[out_len] = '\0';
    return out;
}

static int extract_scheme_name(const char* value, char* scheme, size_t scheme_size) {
    size_t i;

    if (!value || !isalpha((unsigned char)value[0])) {
        return 0;
    }

    i = 0;
    while (value[i]) {
        if (value[i] == ':') {
            if (i + 1 > scheme_size) {
                return 0;
            }
            memcpy(scheme, value, i);
            scheme[i] = '\0';
            return 1;
        }

        if (!isalnum((unsigned char)value[i]) &&
            value[i] != '+' &&
            value[i] != '-' &&
            value[i] != '.') {
            return 0;
        }
        i++;
    }

    return 0;
}

static char* build_url_string(const char* scheme, const char* host, int port, const char* path) {
    const char* effective_path;
    size_t size;
    char* url;

    effective_path = (path && path[0] != '\0') ? path : "/";
    size = strlen(scheme) + strlen(host) + strlen(effective_path) + 32;
    url = (char*)malloc(size);
    if (!url) {
        return NULL;
    }

    if (is_default_port(scheme, port)) {
        snprintf(url, size, "%s://%s%s", scheme, host, effective_path);
    } else {
        snprintf(url, size, "%s://%s:%d%s", scheme, host, port, effective_path);
    }

    return url;
}

static char* resolve_redirect_url(const ParsedUrl* base, const char* location) {
    char scheme[16];
    char* base_path;
    char* combined;
    char* normalized;
    char* resolved;

    if (!base || !location || location[0] == '\0') {
        return NULL;
    }

    if (strncasecmp(location, "http://", 7) == 0 ||
        strncasecmp(location, "https://", 8) == 0) {
        return http_strdup_local(location);
    }

    if (strncmp(location, "//", 2) == 0) {
        size_t size;

        size = strlen(base->scheme) + strlen(location) + 2;
        resolved = (char*)malloc(size);
        if (!resolved) {
            return NULL;
        }
        snprintf(resolved, size, "%s:%s", base->scheme, location);
        return resolved;
    }

    if (extract_scheme_name(location, scheme, sizeof(scheme))) {
        return NULL;
    }

    if (location[0] == '/') {
        normalized = normalize_path_copy(location);
        if (!normalized) {
            return NULL;
        }
        resolved = build_url_string(base->scheme, base->host, base->port, normalized);
        free(normalized);
        return resolved;
    }

    base_path = duplicate_path_without_query(base->path);
    if (!base_path) {
        return NULL;
    }

    if (location[0] == '?') {
        combined = (char*)malloc(strlen(base_path) + strlen(location) + 1);
        if (!combined) {
            free(base_path);
            return NULL;
        }
        strcpy(combined, base_path);
        strcat(combined, location);
    } else {
        size_t dir_len;
        const char* slash;

        slash = strrchr(base_path, '/');
        dir_len = slash ? (size_t)(slash - base_path) + 1 : 1;

        combined = (char*)malloc(dir_len + strlen(location) + 1);
        if (!combined) {
            free(base_path);
            return NULL;
        }
        memcpy(combined, base_path, dir_len);
        combined[dir_len] = '\0';
        strcat(combined, location);
    }
    free(base_path);

    normalized = normalize_path_copy(combined);
    free(combined);
    if (!normalized) {
        return NULL;
    }

    resolved = build_url_string(base->scheme, base->host, base->port, normalized);
    free(normalized);
    return resolved;
}

static HttpResponse* http_fetch_single(const ParsedUrl* url) {
    HttpConnection conn;
    HttpResponse* response;
    char* request;
    char* content_length_value;
    char* transfer_encoding_value;
    char* location_value;
    long content_length;
    int has_body;

    response = NULL;
    request = NULL;
    content_length_value = NULL;
    transfer_encoding_value = NULL;
    location_value = NULL;
    content_length = -1;
    has_body = 1;

    if (connection_open(&conn, url) != 0) {
        return NULL;
    }

    if (!build_request(url, &request)) {
        connection_close(&conn);
        return NULL;
    }

    if (connection_write_all(&conn, (const unsigned char*)request, strlen(request)) != 0) {
        free(request);
        connection_close(&conn);
        return NULL;
    }
    free(request);

    response = (HttpResponse*)calloc(1, sizeof(HttpResponse));
    if (!response) {
        connection_close(&conn);
        return NULL;
    }

    if (!read_headers(&conn, &response->headers)) {
        connection_close(&conn);
        http_response_free(response);
        return NULL;
    }

    response->status_code = parse_status_code(response->headers);

    transfer_encoding_value = get_header_value(response->headers, "Transfer-Encoding");
    if (value_has_token(transfer_encoding_value, "chunked")) {
        response->is_chunked = 1;
    }

    content_length_value = get_header_value(response->headers, "Content-Length");
    if (content_length_value) {
        content_length = strtol(content_length_value, NULL, 10);
        if (content_length < 0) {
            content_length = -1;
        }
    }

    location_value = get_header_value(response->headers, "Location");
    if (location_value) {
        response->redirect_url = location_value;
        location_value = NULL;
    }

    if ((response->status_code >= 100 && response->status_code < 200) ||
        response->status_code == 204 ||
        response->status_code == 304) {
        has_body = 0;
    }

    if (!has_body) {
        response->body = http_strdup_local("");
        if (!response->body) {
            free(content_length_value);
            free(transfer_encoding_value);
            connection_close(&conn);
            http_response_free(response);
            return NULL;
        }
        response->body_len = 0;
    } else if (response->is_chunked) {
        response->body = read_chunked_body(&conn, &response->body_len);
    } else if (content_length >= 0) {
        response->body = read_fixed_body(&conn, (size_t)content_length, &response->body_len);
    } else {
        response->body = read_until_close(&conn, &response->body_len);
    }

    free(content_length_value);
    free(transfer_encoding_value);
    free(location_value);
    connection_close(&conn);

    if (!response->body) {
        http_response_free(response);
        return NULL;
    }

    return response;
}

HttpResponse* http_fetch(const char* url, int max_redirects) {
    char* current_url;
    int redirects_left;

    if (!url) {
        return NULL;
    }

    current_url = http_strdup_local(url);
    if (!current_url) {
        return NULL;
    }

    redirects_left = (max_redirects < 0) ? 0 : max_redirects;

    while (1) {
        ParsedUrl parsed;
        HttpResponse* response;

        if (!parse_url(current_url, &parsed)) {
            free(current_url);
            return NULL;
        }

        response = http_fetch_single(&parsed);
        if (!response) {
            free(current_url);
            return NULL;
        }

        if ((response->status_code == 301 ||
             response->status_code == 302 ||
             response->status_code == 307) &&
            response->redirect_url &&
            redirects_left > 0) {
            char* next_url;

            next_url = resolve_redirect_url(&parsed, response->redirect_url);
            if (next_url) {
                http_response_free(response);
                free(current_url);
                current_url = next_url;
                redirects_left--;
                continue;
            }
        }

        free(current_url);
        return response;
    }
}

void http_response_free(HttpResponse* response) {
    if (!response) {
        return;
    }

    free(response->headers);
    free(response->body);
    free(response->redirect_url);
    free(response);
}
