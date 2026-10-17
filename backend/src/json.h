/* json.h: a JSON tree that reads and writes the way serde_json does with
 * `preserve_order` (the Rust xks's build): objects keep insertion order, a
 * repeated key replaces the value in the first key's place, integers stay
 * integers (u64, else i64, else f64), floats print as zmij prints them (serde_json 1.0.151). The
 * byte-for-byte match matters: a state given as JSON is rendered as pretty
 * JSON into the prompt, and its tokens decide the answer. See json.md. */
#ifndef MJ_JSON_H
#define MJ_JSON_H

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

#include "util.h"

typedef enum { J_NULL, J_BOOL, J_NUM, J_STR, J_ARR, J_OBJ } jtype_t;
typedef enum { N_U64, N_I64, N_F64 } jnum_t;

typedef struct json json_t;

struct json {
    jtype_t t;
    bool b;
    jnum_t nk;
    uint64_t u;
    int64_t i;
    double f;
    char *s; /* J_STR: UTF-8, may hold NULs */
    size_t slen;
    size_t n, cap; /* J_ARR, J_OBJ */
    json_t **items;
    char **keys; /* J_OBJ */
    size_t *klens;
};

json_t *json_parse(const char *text, size_t len, char *err, size_t errlen);
void json_free(json_t *v);

json_t *jnull(void);
json_t *jbool(bool b);
json_t *jf64(double f);
json_t *ju64(uint64_t u);
json_t *jstr(const char *s);
json_t *jstrn(const char *s, size_t n);
json_t *jarr(void);
json_t *jobj(void);
json_t *jclone(const json_t *v);
void jarr_push(json_t *a, json_t *v);
/* Takes ownership of v; a key already there keeps its place. */
void jobj_set(json_t *o, const char *k, json_t *v);
json_t *jobj_get(const json_t *o, const char *k);

/* serde_json::to_string and to_string_pretty. */
void json_write(sb_t *b, const json_t *v);
void json_write_pretty(sb_t *b, const json_t *v);
char *json_dump(const json_t *v);
/* A string as serde_json quotes it, quotes included. */
void json_quote(sb_t *b, const char *s, size_t n);
/* An f64 the way ryu (and so serde_json) prints it. */
void json_f64(sb_t *b, double f);

#endif
