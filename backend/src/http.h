/* http.h: just enough HTTP/1.1 for a local JSON service: a request read
 * from a socket (Content-Length or chunked, Expect: 100-continue
 * answered), a response written with Connection: close, and a client for
 * GET and POST to 127.0.0.1. See http.md. */
#ifndef MJ_HTTP_H
#define MJ_HTTP_H

#include <stddef.h>

typedef struct {
    char method[16];
    char *path;   /* without the query */
    char *auth;   /* Authorization header value, or NULL */
    char *body;
    size_t body_len;
} http_req_t;

/* Read one request from fd; 0 on success. */
int http_read(int fd, http_req_t *r);
void http_req_free(http_req_t *r);
/* Write a JSON response; allow may be NULL. */
void http_respond(int fd, int status, const char *json, const char *allow);

/* Listen on "host:port"; the socket, or -1 with errno. */
int http_listen(const char *bind);

/* A request to http://host:port/path; the status (or -1), body malloc'd. */
int http_call(const char *hostport, const char *method, const char *path, const char *body,
              size_t body_len, char **out, size_t *out_len, int timeout_ms);

#endif
