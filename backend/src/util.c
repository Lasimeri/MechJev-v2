/* util.c: see util.h. */
#define _GNU_SOURCE
#include "util.h"

#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>

static void oom(void)
{
    fputs("xks: out of memory\n", stderr);
    abort();
}

void *xmalloc(size_t n)
{
    void *p = malloc(n ? n : 1);
    if (!p)
        oom();
    return p;
}

void *xcalloc(size_t n, size_t m)
{
    void *p = calloc(n ? n : 1, m ? m : 1);
    if (!p)
        oom();
    return p;
}

void *xrealloc(void *p, size_t n)
{
    p = realloc(p, n ? n : 1);
    if (!p)
        oom();
    return p;
}

char *xstrdup(const char *s)
{
    return xstrndup(s, strlen(s));
}

char *xstrndup(const char *s, size_t n)
{
    char *d = xmalloc(n + 1);
    memcpy(d, s, n);
    d[n] = 0;
    return d;
}

void sb_init(sb_t *b)
{
    b->s = NULL;
    b->n = b->cap = 0;
}

void sb_free(sb_t *b)
{
    free(b->s);
    sb_init(b);
}

static void sb_grow(sb_t *b, size_t add)
{
    if (b->n + add + 1 <= b->cap)
        return;
    size_t c = b->cap ? b->cap : 64;
    while (c < b->n + add + 1)
        c *= 2;
    b->s = xrealloc(b->s, c);
    b->cap = c;
}

void sb_putn(sb_t *b, const char *s, size_t n)
{
    sb_grow(b, n);
    memcpy(b->s + b->n, s, n);
    b->n += n;
    b->s[b->n] = 0;
}

void sb_puts(sb_t *b, const char *s)
{
    sb_putn(b, s, strlen(s));
}

void sb_putc(sb_t *b, char c)
{
    sb_putn(b, &c, 1);
}

void sb_printf(sb_t *b, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    char tmp[256];
    int k = vsnprintf(tmp, sizeof tmp, fmt, ap);
    va_end(ap);
    if (k < 0)
        return;
    if ((size_t)k < sizeof tmp) {
        sb_putn(b, tmp, (size_t)k);
        return;
    }
    sb_grow(b, (size_t)k);
    va_start(ap, fmt);
    vsnprintf(b->s + b->n, (size_t)k + 1, fmt, ap);
    va_end(ap);
    b->n += (size_t)k;
}

char *sb_take(sb_t *b)
{
    char *s = b->s ? b->s : xstrdup("");
    sb_init(b);
    return s;
}

void iv_push(ivec_t *a, int32_t x)
{
    if (a->n == a->cap) {
        a->cap = a->cap ? a->cap * 2 : 16;
        a->v = xrealloc(a->v, a->cap * sizeof *a->v);
    }
    a->v[a->n++] = x;
}

void iv_append(ivec_t *a, const int32_t *x, size_t n)
{
    for (size_t i = 0; i < n; i++)
        iv_push(a, x[i]);
}

void iv_free(ivec_t *a)
{
    free(a->v);
    a->v = NULL;
    a->n = a->cap = 0;
}

char *read_file(const char *path, size_t *len)
{
    FILE *f = fopen(path, "rb");
    if (!f)
        return NULL;
    sb_t b;
    sb_init(&b);
    char buf[65536];
    size_t k;
    while ((k = fread(buf, 1, sizeof buf, f)) > 0)
        sb_putn(&b, buf, k);
    fclose(f);
    if (len)
        *len = b.n;
    return sb_take(&b);
}

double now_ms(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (double)t.tv_sec * 1e3 + (double)t.tv_nsec / 1e6;
}

char *expand_home(const char *p)
{
    const char *home = getenv("HOME");
    if (!home)
        home = "";
    sb_t b;
    sb_init(&b);
    if (p[0] == '~' && p[1] == '/') {
        sb_puts(&b, home);
        sb_puts(&b, p + 1);
    } else {
        sb_puts(&b, p);
    }
    return sb_take(&b);
}

char *run_dir(void)
{
    const char *x = getenv("XDG_RUNTIME_DIR");
    sb_t b;
    sb_init(&b);
    sb_puts(&b, x && *x ? x : "/tmp");
    sb_puts(&b, "/xks");
    return sb_take(&b);
}

int mkdir_p(const char *path)
{
    char *p = xstrdup(path);
    for (char *s = p + 1; *s; s++) {
        if (*s == '/') {
            *s = 0;
            mkdir(p, 0700);
            *s = '/';
        }
    }
    int rc = mkdir(p, 0700);
    free(p);
    return (rc == 0 || errno == EEXIST) ? 0 : -1;
}

uint64_t splitmix64(uint64_t *s)
{
    uint64_t z = (*s += 0x9E3779B97F4A7C15ull);
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
    return z ^ (z >> 31);
}

uint64_t fnv1a(const char *s)
{
    uint64_t h = 1469598103934665603ull;
    for (; *s; s++)
        h = (h ^ (unsigned char)*s) * 1099511628211ull;
    return h;
}
