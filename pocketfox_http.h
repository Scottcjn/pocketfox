#ifndef POCKETFOX_HTTP_H
#define POCKETFOX_HTTP_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct HttpResponse {
    int status_code;
    char* headers;
    char* body;
    size_t body_len;
    char* redirect_url;
    int is_chunked;
} HttpResponse;

HttpResponse* http_fetch(const char* url, int max_redirects);
void http_response_free(HttpResponse* response);

#ifdef __cplusplus
}
#endif

#endif /* POCKETFOX_HTTP_H */
