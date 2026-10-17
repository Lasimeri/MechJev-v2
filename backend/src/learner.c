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

static const char MAGIC[8] = {'M', 'J', '2', 'C', 'I', 'R', 'C', 0};
static const char MAGIC_V1[8] = {'M', 'J', '2', 'L', 'R', 'N', 0, 0};
static const int LAYERS[] = {256, 256, OUT_BITS};
#define N_LAYERS 3
#define REPLAY_MAX 20000
#define LR 0.01f       /* Adam, constant, as the paper trains */
#define CE_WEIGHT 0.5f /* the engine's choice against the whole speakable lexicon */
#define RL_WEIGHT 1.0f /* the per-token reward term (RED), against the KL term's 1 */
#define PREV_DROPOUT 0.5 /* passes trained with the word before blanked */
#define TERN 0.33f     /* latent embedding to ternary: above +TERN 1, below -TERN -1 */

static const unsigned POS_AT[POS_BITS] = {1, 2, 3, 5, 8, 12, 20, 30};

static inline float tern(float x) { return x > TERN ? 1.0f : x < -TERN ? -1.0f : 0.0f; }

static double rand01(uint64_t *s) { return (double)(splitmix64(s) >> 11) * 0x1.0p-53; }

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

static int32_t lex_push(learner_t *l, const char *w)
{
    if (l->n == l->cap) {
        l->cap = l->cap ? l->cap * 2 : 64;
        l->v = xrealloc(l->v, l->cap * sizeof *l->v);
    }
    lex_t *x = &l->v[l->n];
    memset(x, 0, sizeof *x);
    x->word = xstrdup(w);
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

int32_t lex_add(learner_t *l, const char *w)
{
    int32_t f = lex_find(l, w);
    if (f >= 0)
        return f;
    int32_t i = lex_push(l, w);
    /* A new word's code: uniform in [-1, 1] from (seed, word), so about two
     * thirds of its ternary entries are nonzero. */
    uint64_t s = l->seed ^ fnv1a(w);
    for (int j = 0; j < OUT_BITS; j++)
        l->v[i].e[j] = (float)(2.0 * rand01(&s) - 1.0);
    return i;
}

bool is_punct(const char *w) { return w[0] && !w[1] && strchr(".,?!;:", w[0]); }

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
        bool word = isalnum(*s) || *s == '\'' || *s >= 0x80;
        bool mark = !word && strchr(".,?!;:", *s);
        if (!word && !mark) {
            s++;
            continue;
        }
        sb_t b;
        sb_init(&b);
        if (word)
            while (*s && (isalnum(*s) || *s == '\'' || *s >= 0x80)) {
                sb_putc(&b, (char)(*s < 0x80 ? tolower(*s) : *s));
                s++;
            }
        else
            sb_putc(&b, (char)*s++);
        if (n == cap) {
            cap = cap ? cap * 2 : 16;
            w = xrealloc(w, cap * sizeof *w);
        }
        w[n++] = sb_take(&b);
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

void learner_hear(learner_t *l, char **words, size_t n)
{
    for (size_t i = 0; i < n; i++)
        l->v[lex_add(l, words[i])].heard++;
}

int learner_slot(learner_t *l, const char *name)
{
    for (int i = 0; i < VAR_SLOTS; i++)
        if (l->slot[i] && !strcmp(l->slot[i], name))
            return i;
    for (int i = 0; i < VAR_SLOTS; i++)
        if (!l->slot[i]) {
            l->slot[i] = xstrdup(name);
            return i;
        }
    return -1;
}

/* ---- new, free ------------------------------------------------------- */

learner_t *learner_new(uint64_t seed)
{
    learner_t *l = xcalloc(1, sizeof *l);
    l->seed = seed;
    l->min_heard = 2;
    circuit_init(&l->circ, N_IN, N_LAYERS, LAYERS, seed);
    /* Always there: the start (an input only), the end, the punctuation a
     * sentence needs. Every other word is acquired by hearing it. */
    const char *base[] = {START, END, ".", ",", "?", "!"};
    for (size_t i = 0; i < sizeof base / sizeof *base; i++)
        lex_add(l, base[i]);
    return l;
}

void learner_free(learner_t *l)
{
    if (!l)
        return;
    for (size_t i = 0; i < l->n; i++)
        free(l->v[i].word);
    for (int i = 0; i < VAR_SLOTS; i++)
        free(l->slot[i]);
    free(l->v);
    free(l->ix);
    free(l->replay);
    circuit_free(&l->circ);
    free(l);
}

/* ---- the circuit's inputs and outputs -------------------------------- */

void learner_inputs(const learner_t *l, const float *vars, int32_t prev, unsigned pos,
                    uint64_t seed, ablation_t abl, uint8_t *bits)
{
    uint64_t s = seed;
    for (int i = 0; i < VAR_SLOTS; i++) {
        double p = abl == ABL_CONCEPTS ? 0.5 : vars[i];
        bits[i] = rand01(&s) < p;
    }
    for (int j = 0; j < CODE_IN; j++)
        bits[VAR_SLOTS + j] = abl != ABL_PREV && prev >= 0 && l->v[prev].e[j] > 0;
    for (int j = 0; j < POS_BITS; j++)
        bits[VAR_SLOTS + CODE_IN + j] = pos >= POS_AT[j];
    bits[N_IN - 2] = 0;
    bits[N_IN - 1] = 1;
}

/* A word's score from output bits a (0 or 1): its ternary code added where
 * a bit is on and subtracted where it is off (popcount arithmetic). */
static float score_bits(const lex_t *x, const uint8_t *a)
{
    int s = 0;
    for (int j = 0; j < OUT_BITS; j++) {
        int t = (int)tern(x->e[j]);
        s += a[j] ? t : -t;
    }
    return (float)s / TAU;
}

static float score_soft(const lex_t *x, const float *a)
{
    float s = 0;
    for (int j = 0; j < OUT_BITS; j++)
        s += tern(x->e[j]) * (2.0f * a[j] - 1.0f);
    return s / TAU;
}

static bool candidate(const learner_t *l, int32_t i, bool allow_end)
{
    if (!lex_speakable(l, i))
        return false;
    return allow_end || strcmp(l->v[i].word, END);
}

size_t learner_propose(const learner_t *l, const uint8_t *bits, int32_t *out, size_t k,
                       bool allow_end, double *p_first)
{
    uint8_t a[OUT_BITS];
    circuit_run(&l->circ, bits, a);
    float *sc = xmalloc((l->n ? l->n : 1) * sizeof *sc);
    size_t m = 0;
    float mx = -INFINITY;
    for (size_t i = 0; i < l->n; i++) {
        if (!candidate(l, (int32_t)i, allow_end))
            continue;
        sc[i] = score_bits(&l->v[i], a);
        mx = fmaxf(mx, sc[i]);
        size_t at = m;
        while (at > 0 && sc[out[at - 1]] < sc[i])
            at--;
        if (at >= k)
            continue;
        size_t last = m < k ? m : k - 1;
        memmove(&out[at + 1], &out[at], (last - at) * sizeof *out);
        out[at] = (int32_t)i;
        if (m < k)
            m++;
    }
    if (p_first) {
        double z = 0;
        for (size_t i = 0; i < l->n; i++)
            if (candidate(l, (int32_t)i, allow_end))
                z += exp((double)(sc[i] - mx));
        *p_first = m ? exp((double)(sc[out[0]] - mx)) / z : 0;
    }
    free(sc);
    return m;
}

/* ---- training -------------------------------------------------------- */

static void emb_adam(learner_t *l, lex_t *x, const float *g)
{
    const float b1 = 0.9f, b2 = 0.999f, eps = 1e-8f;
    float c1 = 1.0f - powf(b1, (float)l->emb_t), c2 = 1.0f - powf(b2, (float)l->emb_t);
    for (int j = 0; j < OUT_BITS; j++) {
        x->em[j] = b1 * x->em[j] + (1 - b1) * g[j];
        x->ev[j] = b2 * x->ev[j] + (1 - b2) * g[j] * g[j];
        float e = x->e[j] - LR * (x->em[j] / c1) / (sqrtf(x->ev[j] / c2) + eps);
        x->e[j] = e > 1.5f ? 1.5f : e < -1.5f ? -1.5f : e;
    }
}

/* One example, one gradient step. The loss: KL from the engine's
 * distribution over the shown words to the circuit's over the same words,
 * plus CE_WEIGHT x the engine's choice against every speakable word (replies
 * that passed), plus RL_WEIGHT x the per-token reward on the word said (RED). */
static void train_one(learner_t *l, const example_t *ex, uint64_t seed, trace_t *t, float **gw,
                      float *gs, float *ge)
{
    uint8_t bits[N_IN];
    /* The word before blanked for PREV_DROPOUT of the passes (input dropout,
     * drawn from the pass's seed): the circuit must carry the reply through
     * the concepts, not only the previous word (measured 2026-10-17 before
     * this: agreement 0.86, 0.71 with the variables blanked, 0.14 with the
     * word before blanked, the user's no-Markov rule at risk). */
    uint64_t ds = seed ^ 0xD20F0u;
    ablation_t drop = (double)(splitmix64(&ds) >> 11) * 0x1.0p-53 < PREV_DROPOUT ? ABL_PREV : ABL_NONE;
    learner_inputs(l, ex->vars, ex->prev, ex->pos, seed, drop, bits);
    for (int i = 0; i < N_IN; i++)
        t->act[0][i] = bits[i];
    circuit_forward(&l->circ, t);
    const float *a = t->act[N_LAYERS];
    size_t n = l->n;
    for (size_t i = 0; i < n; i++)
        gs[i] = 0;
    /* KL over the shown words */
    float s_opt[MAX_OPTS], mx = -INFINITY, z = 0;
    for (int k = 0; k < ex->n; k++) {
        s_opt[k] = score_soft(&l->v[ex->opt[k]], a);
        mx = fmaxf(mx, s_opt[k]);
    }
    for (int k = 0; k < ex->n; k++)
        z += expf(s_opt[k] - mx);
    for (int k = 0; k < ex->n; k++)
        gs[ex->opt[k]] += expf(s_opt[k] - mx) / z - ex->q[k];
    /* The engine's choice against the whole speakable lexicon, when its reply
     * passed every requirement: this is what pushes down words that were
     * never shown. A failed reply teaches only the preference among the
     * shown words (the KL above) and its per-token rewards (below), never
     * that its word was right. */
    if (!ex->failed) {
        float *sall = xmalloc((n ? n : 1) * sizeof *sall);
        mx = -INFINITY;
        for (size_t i = 0; i < n; i++)
            if (lex_speakable(l, (int32_t)i) || (int32_t)i == ex->top) {
                sall[i] = score_soft(&l->v[i], a);
                mx = fmaxf(mx, sall[i]);
            }
        z = 0;
        for (size_t i = 0; i < n; i++)
            if (lex_speakable(l, (int32_t)i) || (int32_t)i == ex->top)
                z += expf(sall[i] - mx);
        for (size_t i = 0; i < n; i++)
            if (lex_speakable(l, (int32_t)i) || (int32_t)i == ex->top)
                gs[i] += CE_WEIGHT * (expf(sall[i] - mx) / z - ((int32_t)i == ex->top));
        free(sall);
    }
    /* The word said, credited by its per-token reward (RED, Li et al.,
     * arXiv:2411.08302: r_t = R(y<=t) - R(y<=t-1), the engine's coherence of
     * the reply after the word minus before it). The loss -r log p(said) over
     * the words shown: a word that made the reply more coherent is
     * pushed up, one that broke it pushed down. */
    if (ex->said >= 0 && ex->reward != 0.0f) {
        /* over the words shown, the alternatives the engine weighed: a
         * negative reward moves probability to them, never to words nobody
         * offered (over the whole lexicon it raised punctuation never shown:
         * measured 2026-10-17, a reward-only toy fell to 0.275) */
        float s_r[MAX_OPTS], mr = -INFINITY, zr = 0;
        int said_at = -1;
        for (int k = 0; k < ex->n; k++) {
            s_r[k] = s_opt[k];
            mr = fmaxf(mr, s_r[k]);
            if (ex->opt[k] == ex->said)
                said_at = k;
        }
        if (said_at >= 0) {
            for (int k = 0; k < ex->n; k++)
                zr += expf(s_r[k] - mr);
            for (int k = 0; k < ex->n; k++)
                gs[ex->opt[k]] += RL_WEIGHT * ex->reward * (expf(s_r[k] - mr) / zr - (k == said_at));
        }
    }
    /* back through the scores: to the output bits and (straight through the
     * ternary rounding) to the embeddings */
    float *ga = t->grad[N_LAYERS];
    for (int j = 0; j < OUT_BITS; j++)
        ga[j] = 0;
    l->emb_t++;
    for (size_t i = 0; i < n; i++) {
        if (gs[i] == 0)
            continue;
        lex_t *x = &l->v[i];
        for (int j = 0; j < OUT_BITS; j++) {
            ga[j] += gs[i] * tern(x->e[j]) * 2.0f / TAU;
            ge[j] = gs[i] * (2.0f * a[j] - 1.0f) / TAU;
        }
        emb_adam(l, x, ge);
    }
    circuit_grads_zero(&l->circ, gw);
    circuit_backward(&l->circ, t, gw);
    circuit_adam(&l->circ, gw, LR);
}

static void replay_push(learner_t *l, const example_t *ex)
{
    if (l->nreplay == REPLAY_MAX) {
        memmove(l->replay, l->replay + 1, (REPLAY_MAX - 1) * sizeof *l->replay);
        l->nreplay--;
    }
    if (l->nreplay == l->capreplay) {
        l->capreplay = l->capreplay ? l->capreplay * 2 : 256;
        l->replay = xrealloc(l->replay, l->capreplay * sizeof *l->replay);
    }
    l->replay[l->nreplay] = *ex;
    l->replay[l->nreplay].id = l->next_id++;
    l->nreplay++;
}

size_t learner_learn(learner_t *l, const example_t *ex, size_t n, double budget_ms, size_t max_passes)
{
    double t0 = now_ms();
    trace_t t;
    trace_init(&t, &l->circ);
    float **gw = circuit_grads(&l->circ);
    float *gs = xmalloc((l->n ? l->n : 1) * sizeof *gs), ge[OUT_BITS];
    for (size_t i = 0; i < n; i++)
        replay_push(l, &ex[i]);
    size_t passes = 0;
    /* the new examples once, in order: the last n of the buffer (a failed
     * reply's examples teach only the engine's preference among the shown
     * words: train_one) */
    for (size_t i = l->nreplay - (n < l->nreplay ? n : l->nreplay); i < l->nreplay; i++) {
        train_one(l, &l->replay[i], l->replay[i].id * 0x9E3779B97F4A7C15ull, &t, gw, gs, ge);
        passes++;
    }
    /* then rehearsal over everything kept, until the budget is spent */
    uint64_t s = l->seed ^ l->trained;
    while (l->nreplay && now_ms() - t0 < budget_ms && (!max_passes || passes < max_passes)) {
        const example_t *e = &l->replay[splitmix64(&s) % l->nreplay];
        train_one(l, e, e->id * 0x9E3779B97F4A7C15ull + splitmix64(&s), &t, gw, gs, ge);
        passes++;
    }
    l->trained += passes;
    free(gs);
    circuit_grads_free(&l->circ, gw);
    trace_free(&t, &l->circ);
    return passes;
}

evalres_t learner_eval(const learner_t *l, const example_t *ex, size_t n, ablation_t abl)
{
    evalres_t r = {0};
    trace_t t;
    trace_init(&t, &l->circ);
    uint8_t bits[N_IN], a[OUT_BITS];
    for (size_t k = 0; k < n; k++) {
        const example_t *e = &ex[k];
        learner_inputs(l, e->vars, e->prev, e->pos, e->id * 0x9E3779B97F4A7C15ull, abl, bits);
        circuit_run(&l->circ, bits, a);
        for (int i = 0; i < N_IN; i++)
            t.act[0][i] = bits[i];
        circuit_forward(&l->circ, &t);
        int32_t best = -1, best_soft = -1;
        float bs = -INFINITY, bss = -INFINITY, mx = -INFINITY;
        for (size_t i = 0; i < l->n; i++) {
            if (!lex_speakable(l, (int32_t)i) && (int32_t)i != e->top)
                continue;
            float s = score_bits(&l->v[i], a), ss = score_soft(&l->v[i], t.act[N_LAYERS]);
            if (s > bs)
                bs = s, best = (int32_t)i;
            if (ss > bss)
                bss = ss, best_soft = (int32_t)i;
            mx = fmaxf(mx, s);
        }
        double z = 0;
        for (size_t i = 0; i < l->n; i++)
            if (lex_speakable(l, (int32_t)i) || (int32_t)i == e->top)
                z += exp((double)(score_bits(&l->v[i], a) - mx));
        r.agree += best == e->top;
        r.gap += best == best_soft;
        r.nll += -((double)(score_bits(&l->v[e->top], a) - mx) - log(z));
        r.n++;
    }
    if (r.n) {
        r.agree /= (double)r.n;
        r.gap /= (double)r.n;
        r.nll /= (double)r.n;
    }
    trace_free(&t, &l->circ);
    return r;
}

/* ---- the store ------------------------------------------------------- */

#define W(x) fwrite(&(x), sizeof(x), 1, f)
#define R(x) (fread(&(x), sizeof(x), 1, f) == 1)

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
    uint32_t ver = LEARNER_VERSION, nin = N_IN, out = OUT_BITS, slots = VAR_SLOTS;
    uint64_t n = l->n, nr = l->nreplay;
    fwrite(MAGIC, 1, 8, f);
    W(ver), W(nin), W(out), W(slots), W(l->seed), W(l->min_heard);
    W(l->turns), W(l->steps), W(l->trained), W(l->emb_t), W(l->next_id), W(n);
    for (size_t i = 0; i < l->n; i++) {
        const lex_t *x = &l->v[i];
        uint16_t len = (uint16_t)strlen(x->word);
        W(len);
        fwrite(x->word, 1, len, f);
        W(x->heard), W(x->picked);
        fwrite(x->e, sizeof x->e, 1, f);
        fwrite(x->em, sizeof x->em, 1, f);
        fwrite(x->ev, sizeof x->ev, 1, f);
    }
    for (int i = 0; i < VAR_SLOTS; i++) {
        uint16_t len = l->slot[i] ? (uint16_t)strlen(l->slot[i]) : 0;
        W(len);
        if (len)
            fwrite(l->slot[i], 1, len, f);
    }
    circuit_write(&l->circ, f);
    W(nr);
    for (size_t i = 0; i < l->nreplay; i++) {
        const example_t *e = &l->replay[i];
        fwrite(e->vars, sizeof e->vars, 1, f);
        W(e->prev), W(e->pos), W(e->n);
        fwrite(e->opt, sizeof e->opt, 1, f);
        fwrite(e->q, sizeof e->q, 1, f);
        W(e->top), W(e->said), W(e->reward), W(e->failed), W(e->id);
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
    uint32_t ver = 0, nin = 0, outb = 0, slots = 0;
    if (fread(magic, 1, 8, f) != 8) {
        snprintf(err, errlen, "%s: not a learner store", path);
        fclose(f);
        return -1;
    }
    if (!memcmp(magic, MAGIC_V1, 8)) {
        snprintf(err, errlen,
                 "%s: a version 1 store (the word-window learner, retired); this learner is version "
                 "%u, a circuit. learner reset --yes starts over",
                 path, LEARNER_VERSION);
        fclose(f);
        return -1;
    }
    if (memcmp(magic, MAGIC, 8) || !R(ver) || !R(nin) || !R(outb) || !R(slots)) {
        snprintf(err, errlen, "%s: not a learner store", path);
        fclose(f);
        return -1;
    }
    /* Another shape would be read with other wiring and other meanings for its
     * inputs: refused, never converted. Version 2 is read as 3 with no
     * rewards (the fields are additions; no meaning changed). */
    if ((ver != LEARNER_VERSION && ver != 2) || nin != N_IN || outb != OUT_BITS || slots != VAR_SLOTS) {
        snprintf(err, errlen,
                 "%s: store version %u (%u inputs, %u outputs); this learner is version %u (%u, %u)",
                 path, ver, nin, outb, LEARNER_VERSION, (unsigned)N_IN, (unsigned)OUT_BITS);
        fclose(f);
        return -1;
    }
    learner_t *l = xcalloc(1, sizeof *l);
    uint64_t n, nr;
    if (!R(l->seed) || !R(l->min_heard) || !R(l->turns) || !R(l->steps) || !R(l->trained) ||
        !R(l->emb_t) || !R(l->next_id) || !R(n))
        goto bad;
    for (uint64_t i = 0; i < n; i++) {
        uint16_t len;
        if (!R(len))
            goto bad;
        char *w = xmalloc((size_t)len + 1);
        if (fread(w, 1, len, f) != len) {
            free(w);
            goto bad;
        }
        w[len] = 0;
        int32_t k = lex_push(l, w);
        free(w);
        lex_t *x = &l->v[k];
        if (!R(x->heard) || !R(x->picked) || !R(x->e) || !R(x->em) || !R(x->ev))
            goto bad;
    }
    for (int i = 0; i < VAR_SLOTS; i++) {
        uint16_t len;
        if (!R(len))
            goto bad;
        if (len) {
            l->slot[i] = xmalloc((size_t)len + 1);
            if (fread(l->slot[i], 1, len, f) != len)
                goto bad;
            l->slot[i][len] = 0;
        }
    }
    if (circuit_read(&l->circ, f) || !R(nr))
        goto bad;
    for (uint64_t i = 0; i < nr; i++) {
        example_t e;
        memset(&e, 0, sizeof e);
        if (!R(e.vars) || !R(e.prev) || !R(e.pos) || !R(e.n) || !R(e.opt) || !R(e.q) || !R(e.top) ||
            (ver >= 3 && (!R(e.said) || !R(e.reward))) || !R(e.failed) || !R(e.id))
            goto bad;
        if (ver < 3) { /* version 2 had no per-token reward: the said word is the
                        * engine's choice, credited nothing */
            e.said = e.top;
            e.reward = 0;
        }
        uint64_t keep = l->next_id;
        replay_push(l, &e);
        l->replay[l->nreplay - 1].id = e.id;
        l->next_id = keep;
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
