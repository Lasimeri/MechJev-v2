/* util.h: growable buffers and small helpers shared by the backend. */
#ifndef MJ_UTIL_H
#define MJ_UTIL_H

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

void *xmalloc(size_t n);
void *xcalloc(size_t n, size_t m);
void *xrealloc(void *p, size_t n);
char *xstrdup(const char *s);
char *xstrndup(const char *s, size_t n);

/* A growable byte string, always NUL-terminated. */
typedef struct {
    char *s;
    size_t n, cap;
} sb_t;

void sb_init(sb_t *b);
void sb_free(sb_t *b);
void sb_putn(sb_t *b, const char *s, size_t n);
void sb_puts(sb_t *b, const char *s);
void sb_putc(sb_t *b, char c);
void sb_printf(sb_t *b, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
char *sb_take(sb_t *b); /* the string, the buffer reset */

/* A growable array of int32 (tokens). */
typedef struct {
    int32_t *v;
    size_t n, cap;
} ivec_t;

void iv_push(ivec_t *a, int32_t x);
void iv_free(ivec_t *a);
void iv_append(ivec_t *a, const int32_t *x, size_t n);

/* Whole file into a NUL-terminated buffer; NULL when unreadable. */
char *read_file(const char *path, size_t *len);
double now_ms(void);
/* `~/x` and a leading `$HOME` expanded; a new string. */
char *expand_home(const char *p);
/* Directory for the server's pid file and log: $XDG_RUNTIME_DIR/xks. */
char *run_dir(void);
int mkdir_p(const char *path);

/* splitmix64 (Steele, Lea, Flood 2014) and FNV-1a: seeded streams and
 * string hashes, shared by the circuit, the learner and the speaker. */
uint64_t splitmix64(uint64_t *s);
uint64_t fnv1a(const char *s);

#endif
