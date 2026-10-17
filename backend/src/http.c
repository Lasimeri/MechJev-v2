/* http.c: see http.h. */
#define _GNU_SOURCE
#include "http.h"

#include <arpa/inet.h>
#include <errno.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/socket.h>
#include <unistd.h>

#include "util.h"

#define MAX_HEAD (64 * 1024)
#define MAX_BODY (64u * 1024 * 1024)

static int write_all(int fd, const char *p, size_t n)
{
    while (n) {
        ssize_t k = send(fd, p, n, MSG_NOSIGNAL);
        if (k < 0) {
            if (errno == EINTR)
                continue;
            return -1;
        }
        p += k;
        n -= (size_t)k;
    }
    return 0;
}

/* Buffered reads over a socket. */
typedef struct {
    int fd;
    char buf[16384];
    size_t at, n;
} rd_t;

static int rd_byte(rd_t *r)
{
    if (r->at == r->n) {
        ssize_t k;
        do
            k = recv(r->fd, r->buf, sizeof r->buf, 0);
        while (k < 0 && errno == EINTR);
        if (k <= 0)
            return -1;
        r->at = 0;
        r->n = (size_t)k;
    }
    return (unsigned char)r->buf[r->at++];
}

static int rd_line(rd_t *r, sb_t *line)
{
    line->n = 0;
    if (line->s)
        line->s[0] = 0;
    for (;;) {
        int c = rd_byte(r);
        if (c < 0)
            return -1;
        if (c == '\n') {
            if (line->n && line->s[line->n - 1] == '\r')
                line->s[--line->n] = 0;
            return 0;
        }
        sb_putc(line, (char)c);
        if (line->n > MAX_HEAD)
            return -1;
    }
}

static int rd_n(rd_t *r, sb_t *out, size_t n)
{
    while (n) {
        if (r->at < r->n) {
            size_t k = r->n - r->at < n ? r->n - r->at : n;
            sb_putn(out, r->buf + r->at, k);
            r->at += k;
            n -= k;
            continue;
        }
        int c = rd_byte(r);
        if (c < 0)
            return -1;
        sb_putc(out, (char)c);
        n--;
    }
    return 0;
}

int http_read(int fd, http_req_t *q)
{
    memset(q, 0, sizeof *q);
    rd_t *r = xcalloc(1, sizeof *r);
    r->fd = fd;
    sb_t line;
    sb_init(&line);
    int rc = -1;
    size_t clen = 0;
    int chunked = 0, cont = 0;
    if (rd_line(r, &line) || !line.n)
        goto out;
    char *sp = strchr(line.s, ' ');
    if (!sp || (size_t)(sp - line.s) >= sizeof q->method)
        goto out;
    memcpy(q->method, line.s, (size_t)(sp - line.s));
    char *path = sp + 1, *sp2 = strchr(path, ' ');
    if (sp2)
        *sp2 = 0;
    char *qm = strchr(path, '?');
    if (qm)
        *qm = 0;
    q->path = xstrdup(path);
    size_t head = line.n;
    for (;;) {
        if (rd_line(r, &line))
            goto out;
        head += line.n;
        if (head > MAX_HEAD)
            goto out;
        if (!line.n)
            break;
        char *colon = strchr(line.s, ':');
        if (!colon)
            continue;
        *colon = 0;
        char *v = colon + 1;
        while (*v == ' ' || *v == '\t')
            v++;
        if (!strcasecmp(line.s, "Content-Length"))
            clen = (size_t)strtoull(v, NULL, 10);
        else if (!strcasecmp(line.s, "Transfer-Encoding") && strcasestr(v, "chunked"))
            chunked = 1;
        else if (!strcasecmp(line.s, "Authorization"))
            q->auth = xstrdup(v);
        else if (!strcasecmp(line.s, "Expect") && !strcasecmp(v, "100-continue"))
            cont = 1;
    }
    if (cont)
        write_all(fd, "HTTP/1.1 100 Continue\r\n\r\n", 25);
    sb_t body;
    sb_init(&body);
    if (chunked) {
        for (;;) {
            if (rd_line(r, &line))
                goto bad;
            size_t k = (size_t)strtoull(line.s, NULL, 16);
            if (k == 0) {
                while (!rd_line(r, &line) && line.n)
                    ;
                break;
            }
            if (body.n + k > MAX_BODY || rd_n(r, &body, k) || rd_line(r, &line))
                goto bad;
        }
    } else if (clen) {
        if (clen > MAX_BODY || rd_n(r, &body, clen))
            goto bad;
    }
    q->body_len = body.n;
    q->body = sb_take(&body);
    rc = 0;
    goto out;
bad:
    sb_free(&body);
out:
    sb_free(&line);
    free(r);
    return rc;
}

void http_req_free(http_req_t *r)
{
    free(r->path);
    free(r->auth);
    free(r->body);
}

static const char *reason(int s)
{
    switch (s) {
    case 200: return "OK";
    case 400: return "Bad Request";
    case 401: return "Unauthorized";
    case 404: return "Not Found";
    case 405: return "Method Not Allowed";
    case 422: return "Unprocessable Entity";
    case 502: return "Bad Gateway";
    default: return "Status";
    }
}

void http_respond(int fd, int status, const char *json, const char *allow)
{
    sb_t h;
    sb_init(&h);
    size_t n = strlen(json);
    sb_printf(&h, "HTTP/1.1 %d %s\r\nContent-Type: application/json\r\nContent-Length: %zu\r\n",
              status, reason(status), n);
    if (allow)
        sb_printf(&h, "Allow: %s\r\n", allow);
    sb_puts(&h, "Connection: close\r\n\r\n");
    write_all(fd, h.s, h.n);
    write_all(fd, json, n);
    sb_free(&h);
}

static int split_hostport(const char *bind, char *host, size_t hl, char *port, size_t pl)
{
    const char *c = strrchr(bind, ':');
    if (!c)
        return -1;
    size_t n = (size_t)(c - bind);
    if (n >= hl || strlen(c + 1) >= pl)
        return -1;
    memcpy(host, bind, n);
    host[n] = 0;
    strcpy(port, c + 1);
    return 0;
}

int http_listen(const char *addr)
{
    char host[256], port[16];
    if (split_hostport(addr, host, sizeof host, port, sizeof port)) {
        errno = EINVAL;
        return -1;
    }
    struct addrinfo hints = {0}, *ai;
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_flags = AI_PASSIVE;
    if (getaddrinfo(host, port, &hints, &ai)) {
        errno = EINVAL;
        return -1;
    }
    int fd = socket(ai->ai_family, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (fd < 0) {
        freeaddrinfo(ai);
        return -1;
    }
    int one = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
    if (bind(fd, ai->ai_addr, ai->ai_addrlen) || listen(fd, 64)) {
        int e = errno;
        close(fd);
        freeaddrinfo(ai);
        errno = e;
        return -1;
    }
    freeaddrinfo(ai);
    return fd;
}

int http_call(const char *hostport, const char *method, const char *path, const char *body,
              size_t body_len, char **out, size_t *out_len, int timeout_ms)
{
    *out = NULL;
    if (out_len)
        *out_len = 0;
    char host[256], port[16];
    if (split_hostport(hostport, host, sizeof host, port, sizeof port))
        return -1;
    struct addrinfo hints = {0}, *ai;
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    if (getaddrinfo(host, port, &hints, &ai))
        return -1;
    int fd = socket(ai->ai_family, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (fd < 0) {
        freeaddrinfo(ai);
        return -1;
    }
    struct timeval tv = {timeout_ms / 1000, (timeout_ms % 1000) * 1000};
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof tv);
    if (connect(fd, ai->ai_addr, ai->ai_addrlen)) {
        close(fd);
        freeaddrinfo(ai);
        return -1;
    }
    freeaddrinfo(ai);
    sb_t h;
    sb_init(&h);
    sb_printf(&h, "%s %s HTTP/1.1\r\nHost: %s\r\nConnection: close\r\n", method, path, hostport);
    if (body)
        sb_printf(&h, "Content-Type: application/json\r\nContent-Length: %zu\r\n", body_len);
    sb_puts(&h, "\r\n");
    int rc = write_all(fd, h.s, h.n);
    sb_free(&h);
    if (!rc && body)
        rc = write_all(fd, body, body_len);
    if (rc) {
        close(fd);
        return -1;
    }
    rd_t *r = xcalloc(1, sizeof *r);
    r->fd = fd;
    sb_t line;
    sb_init(&line);
    int status = -1;
    size_t clen = (size_t)-1;
    if (rd_line(r, &line) || sscanf(line.s, "HTTP/%*s %d", &status) != 1) {
        status = -1;
        goto done;
    }
    for (;;) {
        if (rd_line(r, &line)) {
            status = -1;
            goto done;
        }
        if (!line.n)
            break;
        char *colon = strchr(line.s, ':');
        if (colon && !strncasecmp(line.s, "Content-Length", (size_t)(colon - line.s)))
            clen = (size_t)strtoull(colon + 1, NULL, 10);
    }
    sb_t b;
    sb_init(&b);
    if (clen != (size_t)-1) {
        if (rd_n(r, &b, clen))
            status = -1;
    } else {
        int c;
        while ((c = rd_byte(r)) >= 0)
            sb_putc(&b, (char)c);
    }
    if (out_len)
        *out_len = b.n;
    *out = sb_take(&b);
done:
    sb_free(&line);
    free(r);
    close(fd);
    return status;
}
