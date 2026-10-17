/* learner.c: see learner.h and learner.md. */
#define _GNU_SOURCE
#include "learner.h"

#include <ctype.h>
#include <errno.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "util.h"

static const char MAGIC[8] = {'M', 'J', '2', 'L', 'R', 'N', 0, 0};

learner_t *learner_new(uint64_t seed)
{
    learner_t *l = xcalloc(1, sizeof *l);
    l->seed = seed;
    l->min_heard = 2;
    /* Always there: the start of an utterance (context only), its end, and
     * the punctuation a sentence needs. Everything else is acquired. */
    const char *base[] = {START, END, ".", ",", "?", "!"};
    for (size_t i = 0; i < sizeof base / sizeof *base; i++)
        lex_add(l, base[i]);
    return l;
}

void learner_free(learner_t *l)
{
    if (!l)
        return;
    for (size_t i = 0; i < l->n; i++) {
        free(l->v[i].word);
        free(l->v[i].proto);
    }
    free(l->v);
    free(l->ix);
    free(l);
}

/* ---- lexicon --------------------------------------------------------- */

static void reindex(learner_t *l, size_t cap)
{
    free(l->ix);
    l->ixcap = cap;
    l->ix = xmalloc(cap * sizeof *l->ix);
    for (size_t i = 0; i < cap; i++)
        l->ix[i] = -1;
    for (size_t i = 0; i < l->n; i++) {
        size_t j = fnv1a(l->v[i].word) & (cap - 1);
        while (l->ix[j] >= 0)
            j = (j + 1) & (cap - 1);
        l->ix[j] = (int32_t)i;
    }
}

int32_t lex_find(const learner_t *l, const char *w)
{
    if (!l->ixcap)
        return -1;
    size_t j = fnv1a(w) & (l->ixcap - 1);
    while (l->ix[j] >= 0) {
        if (!strcmp(l->v[l->ix[j]].word, w))
            return l->ix[j];
        j = (j + 1) & (l->ixcap - 1);
    }
    return -1;
}

int32_t lex_add(learner_t *l, const char *w)
{
    int32_t f = lex_find(l, w);
    if (f >= 0)
        return f;
    if (l->n == l->cap) {
        l->cap = l->cap ? l->cap * 2 : 64;
        l->v = xrealloc(l->v, l->cap * sizeof *l->v);
    }
    l->v[l->n] = (lex_t){xstrdup(w), 0, 0, NULL};
    l->n++;
    if (l->n * 2 > l->ixcap)
        reindex(l, l->ixcap ? l->ixcap * 2 : 256);
    else {
        size_t j = fnv1a(w) & (l->ixcap - 1);
        while (l->ix[j] >= 0)
            j = (j + 1) & (l->ixcap - 1);
        l->ix[j] = (int32_t)(l->n - 1);
    }
    return (int32_t)(l->n - 1);
}

bool is_punct(const char *w)
{
    return w[0] && !w[1] && strchr(".,?!;:", w[0]);
}

bool lex_speakable(const learner_t *l, int32_t i)
{
    const lex_t *x = &l->v[i];
    if (!strcmp(x->word, START))
        return false;
    if (!strcmp(x->word, END) || is_punct(x->word))
        return true;
    return x->heard >= l->min_heard || x->picked > 0;
}

size_t lex_speakable_count(const learner_t *l)
{
    size_t n = 0;
    for (size_t i = 0; i < l->n; i++)
        n += lex_speakable(l, (int32_t)i);
    return n;
}

size_t split_words(const char *text, char ***out)
{
    char **w = NULL;
    size_t n = 0, cap = 0;
    const unsigned char *s = (const unsigned char *)text;
    while (*s) {
        if (isalnum(*s) || *s == '\'' || *s >= 0x80) {
            sb_t b;
            sb_init(&b);
            while (*s && (isalnum(*s) || *s == '\'' || *s >= 0x80)) {
                sb_putc(&b, (char)(*s < 0x80 ? tolower(*s) : *s));
                s++;
            }
            if (n == cap) {
                cap = cap ? cap * 2 : 16;
                w = xrealloc(w, cap * sizeof *w);
            }
            w[n++] = sb_take(&b);
        } else if (strchr(".,?!;:", *s)) {
            if (n == cap) {
                cap = cap ? cap * 2 : 16;
                w = xrealloc(w, cap * sizeof *w);
            }
            char p[2] = {(char)*s, 0};
            w[n++] = xstrdup(p);
            s++;
        } else {
            s++;
        }
    }
    *out = w;
    return n;
}

void free_words(char **w, size_t n)
{
    for (size_t i = 0; i < n; i++)
        free(w[i]);
    free(w);
}

/* ---- context, prediction, learning ----------------------------------- */

void learner_context(const learner_t *l, char **said, size_t t, char **topic, size_t ntopic,
                     hv_t *out)
{
    hv_t w[CTX_WORDS];
    for (int k = 0; k < CTX_WORDS; k++) {
        const char *word = (size_t)(k + 1) <= t ? said[t - 1 - (size_t)k] : START;
        hv_t raw;
        hv_word(&raw, l->seed, word);
        hv_rot(&w[k], &raw, (unsigned)(k + 1)); /* the distance back, as a rotation */
    }
    /* The previous word alone, the last two bound, the last three bound:
     * near contexts share the first, the same phrase shares all three. */
    hv_t uni = w[0], bi, tri;
    hv_xor(&bi, &w[0], &w[1]);
    hv_xor(&tri, &bi, &w[2]);
    static bundle_t b; /* 32 KB: not on the stack */
    bundle_clear(&b);
    bundle_add(&b, &uni, 3);
    bundle_add(&b, &bi, 2);
    bundle_add(&b, &tri, 2);
    if (ntopic) {
        static bundle_t tb;
        bundle_clear(&tb);
        for (size_t i = 0; i < ntopic; i++) {
            if (is_punct(topic[i]))
                continue;
            hv_t v;
            hv_word(&v, l->seed, topic[i]);
            bundle_add(&tb, &v, 1);
        }
        hv_t tv;
        bundle_take(&tv, &tb, l->seed ^ 0x70971Cull);
        bundle_add(&b, &tv, 1);
    }
    bundle_take(out, &b, l->seed);
}

size_t learner_predict(const learner_t *l, const hv_t *ctx, cand_t *out, size_t k, bool allow_end)
{
    size_t m = 0;
    int32_t end = lex_find(l, END);
    for (size_t i = 0; i < l->n; i++) {
        if (!lex_speakable(l, (int32_t)i))
            continue;
        if ((int32_t)i == end && !allow_end)
            continue;
        int32_t sim = 0;
        if (l->v[i].proto)
            sim = HD_BITS - 2 * (int32_t)hv_hamming(ctx, &l->v[i].proto->sign);
        cand_t c = {(int32_t)i, sim};
        /* insertion into the top k, ties to the more often heard */
        size_t at = m;
        while (at > 0) {
            const cand_t *p = &out[at - 1];
            bool better = c.sim > p->sim ||
                          (c.sim == p->sim && l->v[c.word].heard + l->v[c.word].picked >
                                                  l->v[p->word].heard + l->v[p->word].picked);
            if (!better)
                break;
            at--;
        }
        if (at >= k)
            continue;
        size_t last = m < k ? m : k - 1;
        memmove(&out[at + 1], &out[at], (last - at) * sizeof *out);
        out[at] = c;
        if (m < k)
            m++;
    }
    return m;
}

void learner_update(learner_t *l, const hv_t *ctx, int32_t label, int32_t pred)
{
    lex_t *y = &l->v[label];
    if (!y->proto)
        y->proto = xcalloc(1, sizeof *y->proto);
    proto_add(y->proto, ctx, +1);
    proto_pack(y->proto);
    if (pred >= 0 && pred != label && l->v[pred].proto) {
        proto_add(l->v[pred].proto, ctx, -1);
        proto_pack(l->v[pred].proto);
    }
    l->updates++;
}

void learner_hear(learner_t *l, char **words, size_t n, char **topic, size_t ntopic)
{
    for (size_t t = 0; t <= n; t++) {
        const char *next = t < n ? words[t] : END;
        int32_t y = lex_add(l, next);
        if (t < n)
            l->v[y].heard++;
        hv_t ctx;
        learner_context(l, words, t, topic, ntopic, &ctx);
        cand_t c[1];
        size_t got = learner_predict(l, &ctx, c, 1, true);
        learner_update(l, &ctx, y, got ? c[0].word : -1);
    }
}

double learner_nll(const learner_t *l, const hv_t *ctx, int32_t label, double tau)
{
    double m = -INFINITY;
    double *s = xmalloc((l->n ? l->n : 1) * sizeof *s);
    for (size_t i = 0; i < l->n; i++) {
        s[i] = -INFINITY;
        if (!lex_speakable(l, (int32_t)i) && (int32_t)i != label)
            continue;
        int32_t sim = l->v[i].proto ? HD_BITS - 2 * (int32_t)hv_hamming(ctx, &l->v[i].proto->sign) : 0;
        s[i] = (double)sim / tau;
        m = fmax(m, s[i]);
    }
    double z = 0;
    for (size_t i = 0; i < l->n; i++)
        if (isfinite(s[i]))
            z += exp(s[i] - m);
    double r = -(s[label] - m - log(z));
    free(s);
    return r;
}

/* ---- the store ------------------------------------------------------- */

int learner_save(const learner_t *l, const char *path, char *err, size_t errlen)
{
    sb_t tmp;
    sb_init(&tmp);
    sb_printf(&tmp, "%s.tmp", path);
    FILE *f = fopen(tmp.s, "wb");
    if (!f) {
        snprintf(err, errlen, "%s: %s", tmp.s, strerror(errno));
        sb_free(&tmp);
        return -1;
    }
    uint32_t ver = LEARNER_VERSION, d = HD_BITS, n = (uint32_t)l->n;
    fwrite(MAGIC, 1, 8, f);
    fwrite(&ver, 4, 1, f);
    fwrite(&d, 4, 1, f);
    fwrite(&l->seed, 8, 1, f);
    fwrite(&l->min_heard, 4, 1, f);
    fwrite(&l->turns, 8, 1, f);
    fwrite(&l->steps, 8, 1, f);
    fwrite(&l->updates, 8, 1, f);
    fwrite(&n, 4, 1, f);
    for (size_t i = 0; i < l->n; i++) {
        const lex_t *x = &l->v[i];
        uint16_t len = (uint16_t)strlen(x->word);
        uint8_t has = x->proto != NULL;
        fwrite(&len, 2, 1, f);
        fwrite(x->word, 1, len, f);
        fwrite(&x->heard, 4, 1, f);
        fwrite(&x->picked, 4, 1, f);
        fwrite(&has, 1, 1, f);
        if (has)
            fwrite(x->proto->acc, sizeof x->proto->acc, 1, f);
    }
    int bad = ferror(f);
    if (fclose(f) || bad || rename(tmp.s, path)) {
        snprintf(err, errlen, "%s: %s", path, strerror(errno));
        sb_free(&tmp);
        return -1;
    }
    sb_free(&tmp);
    return 0;
}

int learner_load(const char *path, learner_t **out, char *err, size_t errlen)
{
    FILE *f = fopen(path, "rb");
    if (!f) {
        snprintf(err, errlen, "%s: %s", path, strerror(errno));
        return -1;
    }
    char magic[8];
    uint32_t ver = 0, d = 0, n = 0, mh = 0;
    uint64_t seed = 0, turns = 0, steps = 0, updates = 0;
    if (fread(magic, 1, 8, f) != 8 || memcmp(magic, MAGIC, 8) || fread(&ver, 4, 1, f) != 1 ||
        fread(&d, 4, 1, f) != 1 || fread(&seed, 8, 1, f) != 1 || fread(&mh, 4, 1, f) != 1 ||
        fread(&turns, 8, 1, f) != 1 || fread(&steps, 8, 1, f) != 1 ||
        fread(&updates, 8, 1, f) != 1 || fread(&n, 4, 1, f) != 1) {
        snprintf(err, errlen, "%s: not a learner store", path);
        fclose(f);
        return -1;
    }
    /* A store made by another version or dimension would be read with other
     * word vectors: every prototype garbage. Refused, never converted. */
    if (ver != LEARNER_VERSION || d != HD_BITS) {
        snprintf(err, errlen, "%s: store version %u, %u bits; this learner is version %u, %u bits",
                 path, ver, d, LEARNER_VERSION, HD_BITS);
        fclose(f);
        return -1;
    }
    learner_t *l = xcalloc(1, sizeof *l);
    l->seed = seed;
    l->min_heard = mh;
    l->turns = turns;
    l->steps = steps;
    l->updates = updates;
    for (uint32_t i = 0; i < n; i++) {
        uint16_t len;
        uint8_t has;
        uint32_t heard, picked;
        if (fread(&len, 2, 1, f) != 1)
            goto bad;
        char *w = xmalloc((size_t)len + 1);
        if (fread(w, 1, len, f) != len) {
            free(w);
            goto bad;
        }
        w[len] = 0;
        if (fread(&heard, 4, 1, f) != 1 || fread(&picked, 4, 1, f) != 1 || fread(&has, 1, 1, f) != 1) {
            free(w);
            goto bad;
        }
        int32_t k = lex_add(l, w);
        free(w);
        l->v[k].heard = heard;
        l->v[k].picked = picked;
        if (has) {
            l->v[k].proto = xcalloc(1, sizeof *l->v[k].proto);
            if (fread(l->v[k].proto->acc, sizeof l->v[k].proto->acc, 1, f) != 1)
                goto bad;
            proto_pack(l->v[k].proto);
        }
    }
    fclose(f);
    *out = l;
    return 0;
bad:
    snprintf(err, errlen, "%s: truncated store", path);
    learner_free(l);
    fclose(f);
    return -1;
}
