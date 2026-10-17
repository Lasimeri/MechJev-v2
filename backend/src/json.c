/* json.c: see json.h and json.md. */
#define _GNU_SOURCE
#include "json.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ---- constructors ---------------------------------------------------- */

static json_t *jnew(jtype_t t)
{
    json_t *v = xcalloc(1, sizeof *v);
    v->t = t;
    return v;
}

json_t *jnull(void) { return jnew(J_NULL); }

json_t *jbool(bool b)
{
    json_t *v = jnew(J_BOOL);
    v->b = b;
    return v;
}

json_t *jf64(double f)
{
    json_t *v = jnew(J_NUM);
    v->nk = N_F64;
    v->f = f;
    return v;
}

json_t *ju64(uint64_t u)
{
    json_t *v = jnew(J_NUM);
    v->nk = N_U64;
    v->u = u;
    v->f = (double)u;
    return v;
}

json_t *jstrn(const char *s, size_t n)
{
    json_t *v = jnew(J_STR);
    v->s = xstrndup(s, n);
    v->slen = n;
    return v;
}

json_t *jstr(const char *s) { return jstrn(s, strlen(s)); }
json_t *jarr(void) { return jnew(J_ARR); }
json_t *jobj(void) { return jnew(J_OBJ); }

static void grow(json_t *c)
{
    if (c->n < c->cap)
        return;
    c->cap = c->cap ? c->cap * 2 : 4;
    c->items = xrealloc(c->items, c->cap * sizeof *c->items);
    if (c->t == J_OBJ) {
        c->keys = xrealloc(c->keys, c->cap * sizeof *c->keys);
        c->klens = xrealloc(c->klens, c->cap * sizeof *c->klens);
    }
}

void jarr_push(json_t *a, json_t *v)
{
    grow(a);
    a->items[a->n++] = v;
}

static void jobj_setn(json_t *o, const char *k, size_t kn, json_t *v)
{
    for (size_t i = 0; i < o->n; i++) {
        if (o->klens[i] == kn && memcmp(o->keys[i], k, kn) == 0) {
            json_free(o->items[i]);
            o->items[i] = v;
            return;
        }
    }
    grow(o);
    o->keys[o->n] = xstrndup(k, kn);
    o->klens[o->n] = kn;
    o->items[o->n++] = v;
}

void jobj_set(json_t *o, const char *k, json_t *v) { jobj_setn(o, k, strlen(k), v); }

json_t *jobj_get(const json_t *o, const char *k)
{
    if (!o || o->t != J_OBJ)
        return NULL;
    size_t kn = strlen(k);
    for (size_t i = 0; i < o->n; i++)
        if (o->klens[i] == kn && memcmp(o->keys[i], k, kn) == 0)
            return o->items[i];
    return NULL;
}

json_t *jclone(const json_t *v)
{
    json_t *c = jnew(v->t);
    c->b = v->b;
    c->nk = v->nk;
    c->u = v->u;
    c->i = v->i;
    c->f = v->f;
    if (v->t == J_STR) {
        c->s = xstrndup(v->s, v->slen);
        c->slen = v->slen;
    }
    if (v->t == J_ARR)
        for (size_t i = 0; i < v->n; i++)
            jarr_push(c, jclone(v->items[i]));
    if (v->t == J_OBJ)
        for (size_t i = 0; i < v->n; i++)
            jobj_setn(c, v->keys[i], v->klens[i], jclone(v->items[i]));
    return c;
}

void json_free(json_t *v)
{
    if (!v)
        return;
    for (size_t i = 0; i < v->n; i++) {
        json_free(v->items[i]);
        if (v->keys)
            free(v->keys[i]);
    }
    free(v->items);
    free(v->keys);
    free(v->klens);
    free(v->s);
    free(v);
}

/* ---- parser ---------------------------------------------------------- */

typedef struct {
    const char *p, *end, *start;
    char *err;
    size_t errlen;
    int depth;
} parser_t;

static void perr(parser_t *ps, const char *msg)
{
    if (ps->err[0])
        return;
    int line = 1, col = 1;
    for (const char *q = ps->start; q < ps->p && q < ps->end; q++) {
        if (*q == '\n') {
            line++;
            col = 1;
        } else {
            col++;
        }
    }
    snprintf(ps->err, ps->errlen, "%s at line %d column %d", msg, line, col);
}

static void ws(parser_t *ps)
{
    while (ps->p < ps->end && (*ps->p == ' ' || *ps->p == '\t' || *ps->p == '\n' || *ps->p == '\r'))
        ps->p++;
}

static json_t *pvalue(parser_t *ps);

static int hex4(parser_t *ps, unsigned *out)
{
    if (ps->end - ps->p < 4) {
        perr(ps, "EOF while parsing a string");
        return -1;
    }
    unsigned v = 0;
    for (int i = 0; i < 4; i++) {
        char c = ps->p[i];
        v <<= 4;
        if (c >= '0' && c <= '9')
            v |= (unsigned)(c - '0');
        else if (c >= 'a' && c <= 'f')
            v |= (unsigned)(c - 'a' + 10);
        else if (c >= 'A' && c <= 'F')
            v |= (unsigned)(c - 'A' + 10);
        else {
            perr(ps, "invalid escape");
            return -1;
        }
    }
    ps->p += 4;
    *out = v;
    return 0;
}

static void put_utf8(sb_t *b, unsigned c)
{
    char o[4];
    if (c < 0x80) {
        o[0] = (char)c;
        sb_putn(b, o, 1);
    } else if (c < 0x800) {
        o[0] = (char)(0xC0 | (c >> 6));
        o[1] = (char)(0x80 | (c & 0x3F));
        sb_putn(b, o, 2);
    } else if (c < 0x10000) {
        o[0] = (char)(0xE0 | (c >> 12));
        o[1] = (char)(0x80 | ((c >> 6) & 0x3F));
        o[2] = (char)(0x80 | (c & 0x3F));
        sb_putn(b, o, 3);
    } else {
        o[0] = (char)(0xF0 | (c >> 18));
        o[1] = (char)(0x80 | ((c >> 12) & 0x3F));
        o[2] = (char)(0x80 | ((c >> 6) & 0x3F));
        o[3] = (char)(0x80 | (c & 0x3F));
        sb_putn(b, o, 4);
    }
}

/* A string body after its opening quote, into b. */
static int pstring(parser_t *ps, sb_t *b)
{
    for (;;) {
        if (ps->p >= ps->end) {
            perr(ps, "EOF while parsing a string");
            return -1;
        }
        unsigned char c = (unsigned char)*ps->p;
        if (c == '"') {
            ps->p++;
            return 0;
        }
        if (c < 0x20) {
            perr(ps, "control character (\\u0000-\\u001F) found while parsing a string");
            return -1;
        }
        if (c != '\\') {
            const char *q = ps->p;
            while (q < ps->end && *q != '"' && *q != '\\' && (unsigned char)*q >= 0x20)
                q++;
            sb_putn(b, ps->p, (size_t)(q - ps->p));
            ps->p = q;
            continue;
        }
        ps->p++;
        if (ps->p >= ps->end) {
            perr(ps, "EOF while parsing a string");
            return -1;
        }
        char e = *ps->p++;
        switch (e) {
        case '"': sb_putc(b, '"'); break;
        case '\\': sb_putc(b, '\\'); break;
        case '/': sb_putc(b, '/'); break;
        case 'b': sb_putc(b, '\b'); break;
        case 'f': sb_putc(b, '\f'); break;
        case 'n': sb_putc(b, '\n'); break;
        case 'r': sb_putc(b, '\r'); break;
        case 't': sb_putc(b, '\t'); break;
        case 'u': {
            unsigned u;
            if (hex4(ps, &u))
                return -1;
            if (u >= 0xD800 && u <= 0xDBFF) {
                unsigned lo;
                if (ps->end - ps->p < 2 || ps->p[0] != '\\' || ps->p[1] != 'u') {
                    perr(ps, "lone leading surrogate in hex escape");
                    return -1;
                }
                ps->p += 2;
                if (hex4(ps, &lo))
                    return -1;
                if (lo < 0xDC00 || lo > 0xDFFF) {
                    perr(ps, "lone leading surrogate in hex escape");
                    return -1;
                }
                u = 0x10000 + ((u - 0xD800) << 10) + (lo - 0xDC00);
            } else if (u >= 0xDC00 && u <= 0xDFFF) {
                perr(ps, "lone trailing surrogate in hex escape");
                return -1;
            }
            put_utf8(b, u);
            break;
        }
        default:
            perr(ps, "invalid escape");
            return -1;
        }
    }
}

/* Numbers exactly as serde_json 1.0.151 reads them without its
 * `float_roundtrip` feature (de.rs: parse_integer, parse_number,
 * parse_decimal, parse_exponent, f64_from_parts): the digits gathered into
 * a u64 significand, digits past its overflow dropped (integer digits
 * counted into the exponent), then significand * or / 10^|exp| from a
 * table, 1e308 at a time beyond it. Not always the nearest double for 17
 * or more significant digits, which is the point: the same double the
 * Rust xks reads. */
static double pow10_tab[309];
static bool pow10_ready;

static void pow10_init(void)
{
    if (pow10_ready)
        return;
    char b[16];
    for (int i = 0; i <= 308; i++) {
        snprintf(b, sizeof b, "1e%d", i);
        pow10_tab[i] = strtod(b, NULL); /* the literal 1eN, correctly rounded */
    }
    pow10_ready = true;
}

#define OVF(a, d, max) ((a) >= (max) / 10 && ((a) > (max) / 10 || (d) > (max) % 10))

static bool isdig(parser_t *ps)
{
    return ps->p < ps->end && *ps->p >= '0' && *ps->p <= '9';
}

static int from_parts(parser_t *ps, bool pos, uint64_t sig, int32_t exp, double *out)
{
    pow10_init();
    double f = (double)sig;
    for (;;) {
        int64_t a = exp < 0 ? -(int64_t)exp : exp;
        if (a <= 308) {
            if (exp >= 0) {
                f *= pow10_tab[a];
                if (isinf(f)) {
                    perr(ps, "number out of range");
                    return -1;
                }
            } else {
                f /= pow10_tab[a];
            }
            break;
        }
        if (f == 0.0)
            break;
        if (exp >= 0) {
            perr(ps, "number out of range");
            return -1;
        }
        f /= 1e308;
        exp += 308;
    }
    *out = pos ? f : -f;
    return 0;
}

static int pexponent(parser_t *ps, bool pos, uint64_t sig, int32_t start, double *out)
{
    ps->p++; /* e or E */
    bool pexp = true;
    if (ps->p < ps->end && (*ps->p == '+' || *ps->p == '-')) {
        pexp = *ps->p == '+';
        ps->p++;
    }
    if (!isdig(ps)) {
        perr(ps, ps->p >= ps->end ? "EOF while parsing a value" : "invalid number");
        return -1;
    }
    int32_t e = *ps->p++ - '0';
    while (isdig(ps)) {
        int32_t d = *ps->p - '0';
        ps->p++;
        if (OVF(e, d, INT32_MAX)) {
            if (sig != 0 && pexp) {
                perr(ps, "number out of range");
                return -1;
            }
            while (isdig(ps))
                ps->p++;
            *out = pos ? 0.0 : -0.0;
            return 0;
        }
        e = e * 10 + d;
    }
    int64_t fin = pexp ? (int64_t)start + e : (int64_t)start - e;
    if (fin > INT32_MAX)
        fin = INT32_MAX;
    if (fin < INT32_MIN)
        fin = INT32_MIN;
    return from_parts(ps, pos, sig, (int32_t)fin, out);
}

static int pdecimal(parser_t *ps, bool pos, uint64_t sig, int32_t before, double *out)
{
    ps->p++; /* the point */
    int32_t after = 0;
    while (isdig(ps)) {
        unsigned d = (unsigned)(*ps->p - '0');
        if (OVF(sig, d, UINT64_MAX)) {
            while (isdig(ps))
                ps->p++;
            if (ps->p < ps->end && (*ps->p == 'e' || *ps->p == 'E'))
                return pexponent(ps, pos, sig, before + after, out);
            return from_parts(ps, pos, sig, before + after, out);
        }
        ps->p++;
        sig = sig * 10 + d;
        after--;
    }
    if (after == 0) {
        perr(ps, ps->p >= ps->end ? "EOF while parsing a value" : "invalid number");
        return -1;
    }
    if (ps->p < ps->end && (*ps->p == 'e' || *ps->p == 'E'))
        return pexponent(ps, pos, sig, before + after, out);
    return from_parts(ps, pos, sig, before + after, out);
}

static json_t *pnumber(parser_t *ps)
{
    bool pos = true;
    if (*ps->p == '-') {
        pos = false;
        ps->p++;
    }
    if (!isdig(ps)) {
        perr(ps, ps->p >= ps->end ? "EOF while parsing a value" : "invalid number");
        return NULL;
    }
    uint64_t sig = 0;
    double f;
    if (*ps->p == '0') {
        ps->p++;
        if (isdig(ps)) {
            perr(ps, "invalid number");
            return NULL;
        }
    } else {
        sig = (uint64_t)(*ps->p++ - '0');
        while (isdig(ps)) {
            unsigned d = (unsigned)(*ps->p - '0');
            if (OVF(sig, d, UINT64_MAX)) {
                /* parse_long_integer: the rest of the integer digits only
                 * move the exponent. */
                int32_t exp = 0;
                while (isdig(ps)) {
                    ps->p++;
                    exp++;
                }
                int rc;
                if (ps->p < ps->end && *ps->p == '.')
                    rc = pdecimal(ps, pos, sig, exp, &f);
                else if (ps->p < ps->end && (*ps->p == 'e' || *ps->p == 'E'))
                    rc = pexponent(ps, pos, sig, exp, &f);
                else
                    rc = from_parts(ps, pos, sig, exp, &f);
                return rc ? NULL : jf64(f);
            }
            ps->p++;
            sig = sig * 10 + d;
        }
    }
    if (ps->p < ps->end && *ps->p == '.')
        return pdecimal(ps, pos, sig, 0, &f) ? NULL : jf64(f);
    if (ps->p < ps->end && (*ps->p == 'e' || *ps->p == 'E'))
        return pexponent(ps, pos, sig, 0, &f) ? NULL : jf64(f);
    json_t *v = jnew(J_NUM);
    if (pos) {
        v->nk = N_U64;
        v->u = sig;
        v->f = (double)sig;
    } else {
        int64_t ng = (int64_t)(0 - sig);
        if (ng >= 0) { /* -0, or past i64: a float, as serde_json */
            v->nk = N_F64;
            v->f = -(double)sig;
        } else {
            v->nk = N_I64;
            v->i = ng;
            v->f = (double)ng;
        }
    }
    return v;
}

static bool lit(parser_t *ps, const char *w)
{
    size_t n = strlen(w);
    if ((size_t)(ps->end - ps->p) >= n && memcmp(ps->p, w, n) == 0) {
        ps->p += n;
        return true;
    }
    return false;
}

static json_t *pvalue(parser_t *ps)
{
    ws(ps);
    if (ps->p >= ps->end) {
        perr(ps, "EOF while parsing a value");
        return NULL;
    }
    char c = *ps->p;
    if (c == 'n') {
        if (lit(ps, "null"))
            return jnull();
        perr(ps, "expected ident");
        return NULL;
    }
    if (c == 't') {
        if (lit(ps, "true"))
            return jbool(true);
        perr(ps, "expected ident");
        return NULL;
    }
    if (c == 'f') {
        if (lit(ps, "false"))
            return jbool(false);
        perr(ps, "expected ident");
        return NULL;
    }
    if (c == '-' || (c >= '0' && c <= '9'))
        return pnumber(ps);
    if (c == '"') {
        ps->p++;
        sb_t b;
        sb_init(&b);
        if (pstring(ps, &b)) {
            sb_free(&b);
            return NULL;
        }
        json_t *v = jnew(J_STR);
        v->slen = b.n;
        v->s = sb_take(&b);
        return v;
    }
    if (c == '[' || c == '{') {
        if (++ps->depth > 128) {
            perr(ps, "recursion limit exceeded");
            return NULL;
        }
        ps->p++;
        bool obj = c == '{';
        json_t *v = obj ? jobj() : jarr();
        ws(ps);
        if (ps->p < ps->end && *ps->p == (obj ? '}' : ']')) {
            ps->p++;
            ps->depth--;
            return v;
        }
        for (;;) {
            sb_t key;
            sb_init(&key);
            if (obj) {
                ws(ps);
                if (ps->p >= ps->end || *ps->p != '"') {
                    perr(ps, ps->p >= ps->end ? "EOF while parsing an object" : "key must be a string");
                    json_free(v);
                    return NULL;
                }
                ps->p++;
                if (pstring(ps, &key)) {
                    sb_free(&key);
                    json_free(v);
                    return NULL;
                }
                ws(ps);
                if (ps->p >= ps->end || *ps->p != ':') {
                    perr(ps, "expected `:`");
                    sb_free(&key);
                    json_free(v);
                    return NULL;
                }
                ps->p++;
            }
            json_t *item = pvalue(ps);
            if (!item) {
                sb_free(&key);
                json_free(v);
                return NULL;
            }
            if (obj)
                jobj_setn(v, key.s ? key.s : "", key.n, item);
            else
                jarr_push(v, item);
            sb_free(&key);
            ws(ps);
            if (ps->p < ps->end && *ps->p == ',') {
                ps->p++;
                continue;
            }
            if (ps->p < ps->end && *ps->p == (obj ? '}' : ']')) {
                ps->p++;
                ps->depth--;
                return v;
            }
            perr(ps, obj ? "expected `,` or `}`" : "expected `,` or `]`");
            json_free(v);
            return NULL;
        }
    }
    perr(ps, "expected value");
    return NULL;
}

json_t *json_parse(const char *text, size_t len, char *err, size_t errlen)
{
    char dummy[8];
    if (!err) {
        err = dummy;
        errlen = sizeof dummy;
    }
    err[0] = 0;
    parser_t ps = {text, text + len, text, err, errlen, 0};
    json_t *v = pvalue(&ps);
    if (!v)
        return NULL;
    ws(&ps);
    if (ps.p != ps.end) {
        perr(&ps, "trailing characters");
        json_free(v);
        return NULL;
    }
    return v;
}

/* ---- writer ---------------------------------------------------------- */

void json_quote(sb_t *b, const char *s, size_t n)
{
    static const char hex[] = "0123456789abcdef";
    sb_putc(b, '"');
    size_t run = 0;
    for (size_t i = 0; i < n; i++) {
        unsigned char c = (unsigned char)s[i];
        const char *esc = NULL;
        char u[7];
        if (c == '"')
            esc = "\\\"";
        else if (c == '\\')
            esc = "\\\\";
        else if (c < 0x20) {
            switch (c) {
            case '\b': esc = "\\b"; break;
            case '\t': esc = "\\t"; break;
            case '\n': esc = "\\n"; break;
            case '\f': esc = "\\f"; break;
            case '\r': esc = "\\r"; break;
            default:
                u[0] = '\\', u[1] = 'u', u[2] = '0', u[3] = '0';
                u[4] = hex[c >> 4], u[5] = hex[c & 15], u[6] = 0;
                esc = u;
            }
        }
        if (esc) {
            sb_putn(b, s + i - run, run);
            run = 0;
            sb_puts(b, esc);
        } else {
            run++;
        }
    }
    sb_putn(b, s + n - run, run);
    sb_putc(b, '"');
}

/* serde_json 1.0.151 prints with zmij 1.0.23 (lib.rs, write): the shortest
 * digits that read back as f, fixed notation for decimal exponents -5..=15
 * (ryu's range too), else d.ddde+X / e-X unpadded. The digits
 * come from the shortest %.*e that round-trips. */
void json_f64(sb_t *b, double f)
{
    if (!isfinite(f)) {
        sb_puts(b, "null");
        return;
    }
    if (f == 0) {
        sb_puts(b, signbit(f) ? "-0.0" : "0.0");
        return;
    }
    char buf[40];
    for (int p = 0; p < 17; p++) {
        snprintf(buf, sizeof buf, "%.*e", p, f);
        if (strtod(buf, NULL) == f)
            break;
    }
    char *q = buf;
    if (*q == '-') {
        sb_putc(b, '-');
        q++;
    }
    char digits[24];
    int len = 0;
    char *e = strchr(q, 'e');
    for (char *r = q; r < e; r++)
        if (*r != '.')
            digits[len++] = *r;
    while (len > 1 && digits[len - 1] == '0')
        len--;
    digits[len] = 0;
    int exp10 = atoi(e + 1);
    int kk = exp10 + 1;      /* 10^(kk-1) <= v < 10^kk */
    int k = kk - len;        /* exponent of the last digit */
    if (k >= 0 && kk <= 16) {
        sb_putn(b, digits, (size_t)len);
        for (int i = len; i < kk; i++)
            sb_putc(b, '0');
        sb_puts(b, ".0");
    } else if (kk > 0 && kk <= 16) {
        sb_putn(b, digits, (size_t)kk);
        sb_putc(b, '.');
        sb_putn(b, digits + kk, (size_t)(len - kk));
    } else if (kk > -5 && kk <= 0) {
        sb_puts(b, "0.");
        for (int i = 0; i < -kk; i++)
            sb_putc(b, '0');
        sb_putn(b, digits, (size_t)len);
    } else if (len == 1) {
        sb_putc(b, digits[0]);
        sb_printf(b, "e%c%d", kk - 1 < 0 ? '-' : '+', abs(kk - 1));
    } else {
        sb_putc(b, digits[0]);
        sb_putc(b, '.');
        sb_putn(b, digits + 1, (size_t)(len - 1));
        sb_printf(b, "e%c%d", kk - 1 < 0 ? '-' : '+', abs(kk - 1));
    }
}

static void wnum(sb_t *b, const json_t *v)
{
    if (v->nk == N_U64)
        sb_printf(b, "%llu", (unsigned long long)v->u);
    else if (v->nk == N_I64)
        sb_printf(b, "%lld", (long long)v->i);
    else
        json_f64(b, v->f);
}

static void indent(sb_t *b, int lvl)
{
    for (int i = 0; i < lvl; i++)
        sb_puts(b, "  ");
}

static void wval(sb_t *b, const json_t *v, bool pretty, int lvl)
{
    switch (v->t) {
    case J_NULL: sb_puts(b, "null"); return;
    case J_BOOL: sb_puts(b, v->b ? "true" : "false"); return;
    case J_NUM: wnum(b, v); return;
    case J_STR: json_quote(b, v->s, v->slen); return;
    case J_ARR:
    case J_OBJ: {
        bool obj = v->t == J_OBJ;
        sb_putc(b, obj ? '{' : '[');
        if (v->n == 0) {
            sb_putc(b, obj ? '}' : ']');
            return;
        }
        for (size_t i = 0; i < v->n; i++) {
            if (i)
                sb_putc(b, ',');
            if (pretty) {
                sb_putc(b, '\n');
                indent(b, lvl + 1);
            }
            if (obj) {
                json_quote(b, v->keys[i], v->klens[i]);
                sb_puts(b, pretty ? ": " : ":");
            }
            wval(b, v->items[i], pretty, lvl + 1);
        }
        if (pretty) {
            sb_putc(b, '\n');
            indent(b, lvl);
        }
        sb_putc(b, obj ? '}' : ']');
        return;
    }
    }
}

void json_write(sb_t *b, const json_t *v) { wval(b, v, false, 0); }
void json_write_pretty(sb_t *b, const json_t *v) { wval(b, v, true, 0); }

char *json_dump(const json_t *v)
{
    sb_t b;
    sb_init(&b);
    json_write(&b, v);
    return sb_take(&b);
}
