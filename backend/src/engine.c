/* engine.c: see engine.h and engine.md. Line for line the Rust ARTICHOKE
 * (Intel Phi Jev src/artichoke/mod.rs at 26866cb): the session prefilled
 * once on sequence 0 (the rolling buffer), each fingerprint forked from it
 * with llama_memory_seq_cp (the Hologram: every fork holds the whole
 * session), every suffix in one batch, an output row only at each
 * suffix's last token. */
#define _GNU_SOURCE
#include "engine.h"

#include <math.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "llama.h"
#include "ggml-backend.h"

/* ---- logging --------------------------------------------------------- */

static bool g_verbose;
static enum ggml_log_level g_last = GGML_LOG_LEVEL_NONE;

static void log_cb(enum ggml_log_level level, const char *text, void *ud)
{
    (void)ud;
    if (level == GGML_LOG_LEVEL_CONT)
        level = g_last;
    else
        g_last = level;
    if ((level >= GGML_LOG_LEVEL_WARN || g_verbose) && text)
        fputs(text, stderr);
}

/* ---- label cache ----------------------------------------------------- */

typedef struct {
    char *key;
    ivec_t *toks; /* on the heap: a rehash moves the entry, never the tokens */
} lab_t;

typedef struct {
    lab_t *v;
    size_t cap, n;
} labmap_t;

static uint64_t fnv(const char *s)
{
    uint64_t h = 1469598103934665603ull;
    for (; *s; s++)
        h = (h ^ (unsigned char)*s) * 1099511628211ull;
    return h;
}

static lab_t *lab_find(labmap_t *m, const char *k, bool make)
{
    if (make && (m->n + 1) * 2 > m->cap) {
        size_t nc = m->cap ? m->cap * 2 : 256;
        lab_t *nv = xcalloc(nc, sizeof *nv);
        for (size_t i = 0; i < m->cap; i++) {
            if (!m->v[i].key)
                continue;
            size_t j = fnv(m->v[i].key) & (nc - 1);
            while (nv[j].key)
                j = (j + 1) & (nc - 1);
            nv[j] = m->v[i];
        }
        free(m->v);
        m->v = nv;
        m->cap = nc;
    }
    if (!m->cap)
        return NULL;
    size_t j = fnv(k) & (m->cap - 1);
    while (m->v[j].key) {
        if (!strcmp(m->v[j].key, k))
            return &m->v[j];
        j = (j + 1) & (m->cap - 1);
    }
    if (!make)
        return NULL;
    m->v[j].key = xstrdup(k);
    m->n++;
    return &m->v[j];
}

/* ---- the engine ------------------------------------------------------ */

struct engine {
    struct llama_model *model;
    const struct llama_vocab *vocab;
    struct llama_context *ctx;
    size_t n_vocab;
    ivec_t cached; /* tokens sequence 0 holds: the last session prefilled */
    labmap_t labels;
    pthread_mutex_t lock;
    char *name;
    size_t forks, n_ctx, n_batch;
    bool recurrent;
    llama_token pad_tok; /* a newline: what pad_equal pads with */
};

void engine_opts_default(engine_opts_t *o, const char *gguf)
{
    o->gguf = gguf;
    o->n_ctx = 16384;
    o->forks = 15;
    o->n_batch = 2048;
    o->n_ubatch = 512;
    o->threads = 16;
    o->verbose = false;
    o->repack = false;
    o->flash_attn = true;
}

static void seterr(char *err, size_t n, const char *fmt, ...)
{
    if (!err || !n)
        return;
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(err, n, fmt, ap);
    va_end(ap);
}

/* XKS_GPU_LAYERS: a count, or a share of the layers ("50%") read from the
 * file's `<arch>.block_count` by a vocabulary-only load (which leaves
 * llama_model_n_layer at 0). Unset: none. */
static int gpu_layers(const char *path, int *out, char *err, size_t errlen)
{
    const char *v = getenv("XKS_GPU_LAYERS");
    *out = 0;
    if (!v)
        return 0;
    while (*v == ' ' || *v == '\t')
        v++;
    size_t len = strlen(v);
    while (len && (v[len - 1] == ' ' || v[len - 1] == '\t' || v[len - 1] == '\n'))
        len--;
    if (!len)
        return 0;
    char *s = xstrndup(v, len);
    if (s[len - 1] == '%') {
        s[len - 1] = 0;
        char *end;
        double share = strtod(s, &end);
        if (end == s || *end) {
            seterr(err, errlen, "XKS_GPU_LAYERS=%s%%: a count or a share like 50%%", s);
            free(s);
            return -1;
        }
        if (share < 0 || share > 100) {
            seterr(err, errlen, "XKS_GPU_LAYERS=%s%%: a share from 0%% to 100%%", s);
            free(s);
            return -1;
        }
        struct llama_model_params vp = llama_model_default_params();
        vp.vocab_only = true;
        vp.n_gpu_layers = 0;
        struct llama_model *m = llama_model_load_from_file(path, vp);
        if (!m) {
            seterr(err, errlen, "XKS_GPU_LAYERS: could not read the layer count");
            free(s);
            return -1;
        }
        char arch[256], key[300], cnt[64];
        int n = 0;
        if (llama_model_meta_val_str(m, "general.architecture", arch, sizeof arch) > 0) {
            snprintf(key, sizeof key, "%s.block_count", arch);
            if (llama_model_meta_val_str(m, key, cnt, sizeof cnt) > 0)
                n = atoi(cnt);
        }
        llama_model_free(m);
        if (n <= 0) {
            seterr(err, errlen, "XKS_GPU_LAYERS: the file names no block_count");
            free(s);
            return -1;
        }
        int k = (int)round(share / 100.0 * (double)n);
        fprintf(stderr, "xks: GPU layers: %d of %d (%g%%)\n", k, n, share);
        *out = k;
        free(s);
        return 0;
    }
    char *end;
    long k = strtol(s, &end, 10);
    if (end == s || *end) {
        seterr(err, errlen, "XKS_GPU_LAYERS=%s: a count or a share like 50%%", s);
        free(s);
        return -1;
    }
    fprintf(stderr, "xks: GPU layers: %ld\n", k);
    *out = k < 0 ? 0 : (int)k;
    free(s);
    return 0;
}

static char *file_stem(const char *path)
{
    const char *b = strrchr(path, '/');
    b = b ? b + 1 : path;
    const char *dot = strrchr(b, '.');
    if (!dot || dot == b)
        return xstrdup(b);
    return xstrndup(b, (size_t)(dot - b));
}

static int tokenize_with(engine_t *e, const char *text, size_t len, bool add_special,
                         bool parse_special, ivec_t *out);

engine_t *engine_open(const engine_opts_t *o, char *err, size_t errlen)
{
    static bool init;
    g_verbose = o->verbose;
    if (!init) {
        llama_log_set(log_cb, NULL);
        llama_backend_init();
        /* A build without dynamic backends (~/llama.cpp/build:
         * GGML_BACKEND_DL=OFF) has CPU and CUDA linked in. */
        size_t nd = ggml_backend_dev_count();
        sb_t b;
        sb_init(&b);
        for (size_t i = 0; i < nd; i++) {
            if (i)
                sb_puts(&b, ", ");
            sb_puts(&b, ggml_backend_dev_name(ggml_backend_dev_get(i)));
        }
        fprintf(stderr, "xks: devices: %s\n", b.s ? b.s : "");
        sb_free(&b);
        init = true;
    }
    struct stat st;
    double gb = stat(o->gguf, &st) == 0 ? (double)st.st_size / 1e9 : 0.0;
    const char *fn = strrchr(o->gguf, '/');
    fprintf(stderr, "xks: loading %s (%.1f GB)\n", fn ? fn + 1 : o->gguf, gb);
    int ngl;
    if (gpu_layers(o->gguf, &ngl, err, errlen))
        return NULL;
    struct llama_model_params mp = llama_model_default_params();
    mp.use_extra_bufts = o->repack;
    mp.n_gpu_layers = ngl; /* always: the default puts every layer on a GPU */
    struct llama_model *model = llama_model_load_from_file(o->gguf, mp);
    if (!model) {
        seterr(err, errlen, "could not load %s", o->gguf);
        return NULL;
    }
    struct llama_context_params cp = llama_context_default_params();
    cp.n_ctx = o->n_ctx;
    cp.n_batch = o->n_batch;
    cp.n_ubatch = o->n_ubatch < o->n_batch ? o->n_ubatch : o->n_batch;
    cp.n_seq_max = o->forks + 1;
    cp.n_outputs_max = o->forks + 1;
    cp.n_threads = o->threads;
    cp.n_threads_batch = o->threads;
    cp.kv_unified = true;
    cp.no_perf = true;
    if (!o->flash_attn)
        cp.flash_attn_type = LLAMA_FLASH_ATTN_TYPE_DISABLED;
    struct llama_context *ctx = llama_init_from_model(model, cp);
    if (!ctx) {
        llama_model_free(model);
        seterr(err, errlen, "could not create a context (memory for the forks?)");
        return NULL;
    }
    engine_t *e = xcalloc(1, sizeof *e);
    e->model = model;
    e->ctx = ctx;
    e->vocab = llama_model_get_vocab(model);
    e->n_vocab = (size_t)llama_vocab_n_tokens(e->vocab);
    e->recurrent = llama_model_is_recurrent(model) || llama_model_is_hybrid(model);
    {
        ivec_t nl = {0};
        tokenize_with(e, "\n", 1, false, false, &nl);
        e->pad_tok = nl.n ? nl.v[nl.n - 1] : llama_vocab_nl(e->vocab);
        iv_free(&nl);
    }
    e->forks = o->forks;
    e->n_ctx = o->n_ctx;
    e->n_batch = o->n_batch;
    char *stem = file_stem(o->gguf);
    sb_t nb;
    sb_init(&nb);
    sb_printf(&nb, "artichoke/%s", stem);
    free(stem);
    e->name = sb_take(&nb);
    pthread_mutex_init(&e->lock, NULL);
    return e;
}

void engine_close(engine_t *e)
{
    if (!e)
        return;
    llama_free(e->ctx);
    llama_model_free(e->model);
    for (size_t i = 0; i < e->labels.cap; i++) {
        if (e->labels.v[i].key) {
            free(e->labels.v[i].key);
            iv_free(e->labels.v[i].toks);
            free(e->labels.v[i].toks);
        }
    }
    free(e->labels.v);
    iv_free(&e->cached);
    free(e->name);
    pthread_mutex_destroy(&e->lock);
    free(e);
}

const char *engine_name(const engine_t *e) { return e->name; }
bool engine_recurrent(const engine_t *e) { return e->recurrent; }

void scored_free(scored_t *s)
{
    free(s->logprobs);
    s->logprobs = NULL;
}

/* ---- tokens ---------------------------------------------------------- */

static int tokenize_with(engine_t *e, const char *text, size_t len, bool add_special,
                         bool parse_special, ivec_t *out)
{
    int32_t cap = (int32_t)len + 8;
    int32_t *buf = xmalloc((size_t)cap * sizeof *buf);
    int32_t n = llama_tokenize(e->vocab, text, (int32_t)len, buf, cap, add_special, parse_special);
    if (n < 0) {
        cap = -n;
        buf = xrealloc(buf, (size_t)cap * sizeof *buf);
        n = llama_tokenize(e->vocab, text, (int32_t)len, buf, cap, add_special, parse_special);
    }
    if (n < 0) {
        free(buf);
        return -1;
    }
    iv_append(out, buf, (size_t)n);
    free(buf);
    return 0;
}

int engine_tokenize_plain(engine_t *e, const char *text, ivec_t *out)
{
    return tokenize_with(e, text, strlen(text), false, false, out);
}

/* A rendered prompt segment by segment: template text with special tokens
 * parsed, user text without, the start token before the first. */
static int tokenize_segs(engine_t *e, const segs_t *s, ivec_t *out)
{
    out->n = 0;
    for (size_t i = 0; i < s->n; i++)
        if (tokenize_with(e, s->v[i].text, s->v[i].len, i == 0, !s->v[i].user, out))
            return -1;
    llama_token bos = llama_vocab_bos(e->vocab);
    if (out->n >= 2 && out->v[0] == bos && out->v[1] == bos) {
        memmove(out->v, out->v + 1, (out->n - 1) * sizeof *out->v);
        out->n--;
    }
    return 0;
}

static int label_tokens(engine_t *e, const char *label, const ivec_t **out, char *err, size_t errlen)
{
    lab_t *l = lab_find(&e->labels, label, false);
    if (l) {
        *out = l->toks;
        return E_OK;
    }
    ivec_t t = {0};
    if (engine_tokenize_plain(e, label, &t)) {
        seterr(err, errlen, "tokenizer failed");
        return E_FAILED;
    }
    if (t.n == 0) {
        sb_t q;
        sb_init(&q);
        json_quote(&q, label, strlen(label));
        seterr(err, errlen, "label %s has no tokens", q.s);
        sb_free(&q);
        iv_free(&t);
        return E_REJECTED;
    }
    l = lab_find(&e->labels, label, true);
    l->toks = xmalloc(sizeof *l->toks);
    *l->toks = t;
    *out = l->toks;
    return E_OK;
}

/* ---- decode and read ------------------------------------------------- */

typedef struct {
    llama_token tok;
    int32_t pos, seq;
    long slot; /* -1: no output */
} step_t;

typedef struct {
    step_t *v;
    size_t n, cap;
} plan_t;

static void plan_push(plan_t *p, llama_token t, int32_t pos, int32_t seq, long slot)
{
    if (p->n == p->cap) {
        p->cap = p->cap ? p->cap * 2 : 256;
        p->v = xrealloc(p->v, p->cap * sizeof *p->v);
    }
    p->v[p->n++] = (step_t){t, pos, seq, slot};
}

typedef void (*out_fn)(void *ud, long slot, int32_t row);

/* Decode in windows of n_batch, calling out(slot, row) for every output
 * row of each window after it is decoded. */
static int decode(engine_t *e, const plan_t *p, out_fn out, void *ud, char *err, size_t errlen)
{
    for (size_t at = 0; at < p->n; at += e->n_batch) {
        size_t w = p->n - at < e->n_batch ? p->n - at : e->n_batch;
        struct llama_batch b = llama_batch_init((int32_t)w, 0, 1);
        for (size_t k = 0; k < w; k++) {
            const step_t *s = &p->v[at + k];
            b.token[k] = s->tok;
            b.pos[k] = s->pos;
            b.n_seq_id[k] = 1;
            b.seq_id[k][0] = s->seq;
            b.logits[k] = s->slot >= 0;
        }
        b.n_tokens = (int32_t)w;
        int32_t rc = llama_decode(e->ctx, b);
        if (rc != 0) {
            llama_batch_free(b);
            seterr(err, errlen, "llama_decode returned %d", rc);
            return E_FAILED;
        }
        if (out)
            for (size_t k = 0; k < w; k++)
                if (p->v[at + k].slot >= 0)
                    out(ud, p->v[at + k].slot, (int32_t)k);
        llama_batch_free(b);
    }
    return E_OK;
}

/* Full-vocabulary log-probabilities of ids from output row i: the max as
 * f32 (f32::max), the sum of exp in f64 left to right, as the Rust. */
static void read_row(engine_t *e, int32_t i, const llama_token *ids, size_t n, double *out)
{
    const float *row = llama_get_logits_ith(e->ctx, i);
    float m = -INFINITY;
    for (size_t k = 0; k < e->n_vocab; k++)
        m = fmaxf(m, row[k]);
    double max = (double)m;
    double sum = 0.0;
    for (size_t k = 0; k < e->n_vocab; k++)
        sum += exp((double)row[k] - max);
    double lse = max + log(sum);
    for (size_t k = 0; k < n; k++)
        out[k] = (double)row[ids[k]] - lse;
}

/* ---- limits (REBAL) -------------------------------------------------- */

static int check_limits(const ivec_t *prompts, const ivec_t **const *cands, const size_t *ncand,
                        size_t np, size_t shared, size_t n_ctx, char *err, size_t errlen)
{
    size_t longest = 0, request = shared;
    for (size_t i = 0; i < np; i++) {
        size_t extra = 0;
        for (size_t c = 0; c < ncand[i]; c++) {
            size_t l = cands[i][c]->n ? cands[i][c]->n - 1 : 0;
            if (l > extra)
                extra = l;
        }
        if (prompts[i].n + extra > longest)
            longest = prompts[i].n + extra;
        request += prompts[i].n - shared;
    }
    if (longest > BRANCH_LIMIT) {
        seterr(err, errlen, "the state and a question come to %zu tokens, past the %d limit",
               longest, BRANCH_LIMIT);
        return E_REJECTED;
    }
    if (request > REQUEST_LIMIT) {
        seterr(err, errlen, "the request comes to %zu tokens, past the %d limit", request,
               REQUEST_LIMIT);
        return E_REJECTED;
    }
    if (longest > n_ctx) {
        seterr(err, errlen,
               "the state and a question come to %zu tokens, past this server's context of %zu "
               "(xks --ctx)",
               longest, n_ctx);
        return E_REJECTED;
    }
    return E_OK;
}

static size_t lcp(const ivec_t *a, const ivec_t *b)
{
    size_t k = 0;
    while (k < a->n && k < b->n && a->v[k] == b->v[k])
        k++;
    return k;
}

/* ---- the trie -------------------------------------------------------- */

/* A node of the label trie: a proper prefix of some label, and the next
 * tokens of the labels through it, in the order they were met. */
typedef struct {
    const llama_token *pre;
    size_t len;
    ivec_t next;
    double *read; /* logprobs of next, once read */
} node_t;

/* Ord for Vec<i32>: element-wise, then shorter first. */
static int node_cmp(const void *a, const void *b)
{
    const node_t *x = a, *y = b;
    size_t n = x->len < y->len ? x->len : y->len;
    for (size_t i = 0; i < n; i++)
        if (x->pre[i] != y->pre[i])
            return x->pre[i] < y->pre[i] ? -1 : 1;
    return x->len < y->len ? -1 : x->len > y->len ? 1 : 0;
}

static node_t *node_find(node_t *v, size_t n, const llama_token *pre, size_t len)
{
    for (size_t i = 0; i < n; i++)
        if (v[i].len == len && (len == 0 || !memcmp(v[i].pre, pre, len * sizeof *pre)))
            return &v[i];
    return NULL;
}

typedef struct {
    engine_t *e;
    double **rows;
    node_t *const *round;
} trie_ud_t;

static void trie_out(void *ud, long slot, int32_t row)
{
    trie_ud_t *t = ud;
    node_t *nd = t->round[slot];
    nd->read = xmalloc((nd->next.n ? nd->next.n : 1) * sizeof(double));
    read_row(t->e, row, nd->next.v, nd->next.n, nd->read);
}

static int read_trie(engine_t *e, const ivec_t *prompt, size_t shared, const ivec_t *const *cands,
                     size_t nc, double *out, size_t *evaluated, char *err, size_t errlen)
{
    struct llama_memory_i *mem = llama_get_memory(e->ctx);
    node_t *nodes = NULL;
    size_t nn = 0, ncap = 0;
    for (size_t c = 0; c < nc; c++) {
        for (size_t d = 0; d < cands[c]->n; d++) {
            node_t *nd = node_find(nodes, nn, cands[c]->v, d);
            if (!nd) {
                if (nn == ncap) {
                    ncap = ncap ? ncap * 2 : 16;
                    nodes = xrealloc(nodes, ncap * sizeof *nodes);
                }
                nodes[nn] = (node_t){cands[c]->v, d, {0}, NULL};
                nd = &nodes[nn++];
            }
            bool have = false;
            for (size_t k = 0; k < nd->next.n; k++)
                have |= nd->next.v[k] == cands[c]->v[d];
            if (!have)
                iv_push(&nd->next, cands[c]->v[d]);
        }
    }
    qsort(nodes, nn, sizeof *nodes, node_cmp); /* BTreeMap order */
    size_t len = prompt->n;
    *evaluated = len - shared;
    int rc = E_OK;

    /* The root on sequence 1. */
    llama_memory_seq_cp(mem, 0, 1, -1, -1);
    node_t *root = &nodes[0]; /* the empty prefix sorts first */
    plan_t p = {0};
    for (size_t m = shared; m < len; m++)
        plan_push(&p, prompt->v[m], (int32_t)m, 1, m + 1 == len ? 0 : -1);
    node_t *rr[1] = {root};
    trie_ud_t ud = {e, NULL, rr};
    rc = decode(e, &p, trie_out, &ud, err, errlen);
    free(p.v);
    if (rc) {
        llama_memory_seq_rm(mem, 1, -1, -1);
        goto done;
    }
    if (!root->read) {
        seterr(err, errlen, "no root row");
        rc = E_FAILED;
        llama_memory_seq_rm(mem, 1, -1, -1);
        goto done;
    }

    /* The probes, forked from sequence 1 in rounds of the free sequences. */
    size_t nprobe = nn - 1;
    if (e->forks < 2 && nprobe > 0) {
        llama_memory_seq_rm(mem, 1, -1, -1);
        seterr(err, errlen,
               "labels of several tokens need --forks 2 or more (a sequence for the probes)");
        rc = E_REJECTED;
        goto done;
    }
    size_t free_seqs = e->forks - 1;
    for (size_t at = 0; at < nprobe; at += free_seqs) {
        size_t w = nprobe - at < free_seqs ? nprobe - at : free_seqs;
        node_t **round = xmalloc(w * sizeof *round);
        plan_t q = {0};
        for (size_t k = 0; k < w; k++) {
            node_t *nd = &nodes[1 + at + k];
            round[k] = nd;
            int32_t seq = (int32_t)(k + 2);
            llama_memory_seq_cp(mem, 1, seq, -1, -1);
            for (size_t m = 0; m < nd->len; m++)
                plan_push(&q, nd->pre[m], (int32_t)(len + m), seq, m + 1 == nd->len ? (long)k : -1);
            *evaluated += nd->len;
        }
        trie_ud_t u2 = {e, NULL, round};
        rc = decode(e, &q, trie_out, &u2, err, errlen);
        free(q.v);
        for (size_t k = 0; k < w; k++)
            llama_memory_seq_rm(mem, (int32_t)(k + 2), -1, -1);
        free(round);
        if (rc)
            break;
    }
    llama_memory_seq_rm(mem, 1, -1, -1);
    if (rc)
        goto done;
    for (size_t c = 0; c < nc; c++) {
        double sum = 0.0;
        for (size_t d = 0; d < cands[c]->n; d++) {
            node_t *nd = node_find(nodes, nn, cands[c]->v, d);
            size_t at = 0;
            while (nd->next.v[at] != cands[c]->v[d])
                at++;
            sum += nd->read[at];
        }
        out[c] = sum;
    }
done:
    for (size_t i = 0; i < nn; i++) {
        iv_free(&nodes[i].next);
        free(nodes[i].read);
    }
    free(nodes);
    return rc;
}

/* ---- fork and read --------------------------------------------------- */

typedef struct {
    engine_t *e;
    llama_token **firsts;
    size_t *nfirst;
    double **lp;
    size_t *evaluated;
    const ivec_t *prompts;
    size_t shared;
} round_ud_t;

static void round_out(void *ud, long slot, int32_t row)
{
    round_ud_t *r = ud;
    r->lp[slot] = xmalloc((r->nfirst[slot] ? r->nfirst[slot] : 1) * sizeof(double));
    read_row(r->e, row, r->firsts[slot], r->nfirst[slot], r->lp[slot]);
    r->evaluated[slot] = r->prompts[slot].n - r->shared;
}

static int fork_and_read(engine_t *e, bool pad_equal, const ivec_t *prompts, const ivec_t **const *cands,
                         const size_t *ncand, size_t np, size_t shared, scored_t *out, char *err,
                         size_t errlen)
{
    double t0 = now_ms();
    int rc = check_limits(prompts, cands, ncand, np, shared, e->n_ctx, err, errlen);
    if (rc)
        return rc;
    struct llama_memory_i *mem = llama_get_memory(e->ctx);
    const ivec_t *session = &prompts[0];

    /* Reuse what sequence 0 holds. A recurrent state cannot be cut back, so
     * a partial match there means starting over. */
    size_t reuse = 0;
    while (reuse < e->cached.n && reuse < shared && e->cached.v[reuse] == session->v[reuse])
        reuse++;
    if (reuse < e->cached.n) {
        if (llama_memory_seq_rm(mem, 0, (int32_t)reuse, -1)) {
            e->cached.n = reuse;
        } else {
            llama_memory_clear(mem, true);
            e->cached.n = 0;
        }
    }
    size_t start = e->cached.n;
    if (shared > start) {
        plan_t p = {0};
        for (size_t i = start; i < shared; i++)
            plan_push(&p, session->v[i], (int32_t)i, 0, -1);
        rc = decode(e, &p, NULL, NULL, err, errlen);
        free(p.v);
        if (rc) {
            llama_memory_clear(mem, true);
            e->cached.n = 0;
            return rc;
        }
    }
    e->cached.n = 0;
    iv_append(&e->cached, session->v, shared);

    double **lp = xcalloc(np, sizeof *lp);
    size_t *evaluated = xcalloc(np, sizeof *evaluated);
    llama_token **firsts = xcalloc(np, sizeof *firsts);
    size_t *nfirst = xcalloc(np, sizeof *nfirst);
    size_t *single = xcalloc(np, sizeof *single);
    size_t ns = 0;
    for (size_t n = 0; n < np; n++) {
        bool one = true;
        firsts[n] = xmalloc((ncand[n] ? ncand[n] : 1) * sizeof **firsts);
        nfirst[n] = ncand[n];
        for (size_t c = 0; c < ncand[n]; c++) {
            one &= cands[n][c]->n == 1;
            firsts[n][c] = cands[n][c]->v[0];
        }
        if (one)
            single[ns++] = n;
    }
    round_ud_t ud = {e, firsts, nfirst, lp, evaluated, prompts, shared};
    size_t i = 0;
    while (i < ns) {
        /* With pad_equal (an xks extension a caller asks for), every suffix
         * of the round is left-padded with newlines to the longest: a hybrid
         * model splits a batch into ubatches of equal tokens per sequence
         * (llama-batch.cpp split_equal), so suffixes of different lengths
         * cost a forward pass each (measured 2026-10-17: 6 Nouls of one
         * length 1.1 s, of six lengths 4.4 to 6.5 s). */
        size_t j = i, cells = shared, longest = 0;
        while (j < ns && j - i < e->forks) {
            size_t len = prompts[single[j]].n - shared;
            size_t lg = len > longest ? len : longest;
            size_t need = pad_equal ? shared + (j - i + 1) * lg : cells + len;
            if (need > e->n_ctx)
                break;
            cells += len;
            longest = lg;
            j++;
        }
        if (j == i) {
            seterr(err, errlen, "a prompt of %zu tokens does not fit a context of %zu",
                   prompts[single[i]].n, e->n_ctx);
            rc = E_REJECTED;
            goto done;
        }
        plan_t p = {0};
        for (size_t k = 0; k < j - i; k++) {
            size_t n = single[i + k];
            int32_t seq = (int32_t)(k + 1);
            llama_memory_seq_cp(mem, 0, seq, -1, -1);
            size_t pad = pad_equal ? longest - (prompts[n].n - shared) : 0;
            for (size_t q = 0; q < pad; q++)
                plan_push(&p, e->pad_tok, (int32_t)(shared + q), seq, -1);
            for (size_t m = shared; m < prompts[n].n; m++)
                plan_push(&p, prompts[n].v[m], (int32_t)(m + pad), seq, m + 1 == prompts[n].n ? (long)n : -1);
        }
        rc = decode(e, &p, round_out, &ud, err, errlen);
        free(p.v);
        for (size_t k = 0; k < j - i; k++)
            llama_memory_seq_rm(mem, (int32_t)(k + 1), -1, -1);
        if (rc)
            goto done;
        i = j;
    }
    for (size_t n = 0; n < np; n++) {
        if (lp[n])
            continue;
        lp[n] = xmalloc((ncand[n] ? ncand[n] : 1) * sizeof(double));
        rc = read_trie(e, &prompts[n], shared, cands[n], ncand[n], lp[n], &evaluated[n], err, errlen);
        if (rc)
            goto done;
    }
    double each = (now_ms() - t0) / (double)np;
    for (size_t n = 0; n < np; n++) {
        out[n].logprobs = lp[n];
        lp[n] = NULL;
        out[n].prompt_evaluated = evaluated[n] + (n == 0 ? shared - start : 0);
        out[n].prompt_cached = n == 0 ? start : shared;
        out[n].latency_ms = each;
    }
done:
    for (size_t n = 0; n < np; n++) {
        free(lp[n]);
        free(firsts[n]);
    }
    free(lp);
    free(evaluated);
    free(firsts);
    free(nfirst);
    free(single);
    return rc;
}

/* Every fingerprint's whole prompt, and where they fork: where their tokens
 * part, never past the end of the session itself. */
static int prepare(engine_t *e, const segs_t *prefix, const item_t *items, size_t n,
                   ivec_t *prompts, size_t *shared, char *err, size_t errlen)
{
    for (size_t i = 0; i < n; i++) {
        segs_t whole = segs_concat(prefix, &items[i].suffix);
        int rc = tokenize_segs(e, &whole, &prompts[i]);
        segs_free(&whole);
        if (rc) {
            seterr(err, errlen, "tokenizer failed");
            return E_FAILED;
        }
        if (prompts[i].n == 0) {
            seterr(err, errlen, "empty prompt");
            return E_REJECTED;
        }
    }
    ivec_t own = {0};
    if (tokenize_segs(e, prefix, &own)) {
        seterr(err, errlen, "tokenizer failed");
        return E_FAILED;
    }
    size_t session_end = lcp(&own, &prompts[0]);
    iv_free(&own);
    size_t fp = prompts[0].n - 1;
    for (size_t i = 1; i < n; i++) {
        size_t l = lcp(&prompts[0], &prompts[i]);
        if (l < fp)
            fp = l;
        if (prompts[i].n - 1 < fp)
            fp = prompts[i].n - 1;
    }
    *shared = fp < session_end ? fp : session_end;
    return E_OK;
}

int engine_session_tokens(engine_t *e, const segs_t *prefix, const item_t *items, size_t n,
                          size_t *shared, char *err, size_t errlen)
{
    pthread_mutex_lock(&e->lock);
    ivec_t *prompts = xcalloc(n, sizeof *prompts);
    int rc = prepare(e, prefix, items, n, prompts, shared, err, errlen);
    for (size_t i = 0; i < n; i++)
        iv_free(&prompts[i]);
    free(prompts);
    pthread_mutex_unlock(&e->lock);
    return rc;
}

int engine_score_many(engine_t *e, bool pad_equal, const segs_t *prefix, const item_t *items, size_t n,
                      scored_t *out, char *err, size_t errlen)
{
    if (n == 0)
        return E_OK;
    pthread_mutex_lock(&e->lock);
    int rc = E_OK;
    const ivec_t ***cands = xcalloc(n, sizeof *cands);
    size_t *ncand = xcalloc(n, sizeof *ncand);
    ivec_t *prompts = xcalloc(n, sizeof *prompts);
    for (size_t i = 0; i < n && !rc; i++) {
        cands[i] = xcalloc(items[i].n ? items[i].n : 1, sizeof **cands);
        ncand[i] = items[i].n;
        for (size_t c = 0; c < items[i].n && !rc; c++)
            rc = label_tokens(e, items[i].labels[c], &cands[i][c], err, errlen);
    }
    size_t shared = 0;
    if (!rc)
        rc = prepare(e, prefix, items, n, prompts, &shared, err, errlen);
    if (!rc)
        rc = fork_and_read(e, pad_equal, prompts, (const ivec_t **const *)cands, ncand, n, shared, out, err,
                           errlen);
    for (size_t i = 0; i < n; i++) {
        free(cands[i]);
        iv_free(&prompts[i]);
    }
    free(cands);
    free(ncand);
    free(prompts);
    pthread_mutex_unlock(&e->lock);
    return rc;
}

typedef struct {
    engine_t *e;
    const llama_token *ids;
    size_t n;
    double *out;
    bool got;
} split_ud_t;

static void split_out(void *ud, long slot, int32_t row)
{
    (void)slot;
    split_ud_t *s = ud;
    read_row(s->e, row, s->ids, s->n, s->out);
    s->got = true;
}

int engine_read_split(engine_t *e, const segs_t *prompt, char **labels, size_t n, size_t at,
                      scored_t *out, char *err, size_t errlen)
{
    double t0 = now_ms();
    pthread_mutex_lock(&e->lock);
    int rc = E_OK;
    llama_token *ids = xcalloc(n ? n : 1, sizeof *ids);
    for (size_t i = 0; i < n && !rc; i++) {
        const ivec_t *t;
        rc = label_tokens(e, labels[i], &t, err, errlen);
        if (!rc && t->n != 1) {
            seterr(err, errlen, "label is %zu tokens; the polygraph reads one-token labels", t->n);
            rc = E_REJECTED;
        }
        if (!rc)
            ids[i] = t->v[0];
    }
    ivec_t toks = {0};
    if (!rc && tokenize_segs(e, prompt, &toks)) {
        seterr(err, errlen, "tokenizer failed");
        rc = E_FAILED;
    }
    if (!rc) {
        struct llama_memory_i *mem = llama_get_memory(e->ctx);
        if (at > toks.n - 1)
            at = toks.n - 1;
        llama_memory_clear(mem, true);
        e->cached.n = 0;
        plan_t head = {0}, tail = {0};
        for (size_t i = 0; i < at; i++)
            plan_push(&head, toks.v[i], (int32_t)i, 0, -1);
        for (size_t i = at; i < toks.n; i++)
            plan_push(&tail, toks.v[i], (int32_t)i, 0, i + 1 == toks.n ? 0 : -1);
        out->logprobs = xmalloc((n ? n : 1) * sizeof(double));
        split_ud_t ud = {e, ids, n, out->logprobs, false};
        rc = decode(e, &head, NULL, NULL, err, errlen);
        if (!rc)
            rc = decode(e, &tail, split_out, &ud, err, errlen);
        llama_memory_clear(mem, true);
        free(head.v);
        free(tail.v);
        if (!rc && !ud.got) {
            seterr(err, errlen, "no output row");
            rc = E_FAILED;
        }
        out->prompt_evaluated = toks.n;
        out->prompt_cached = 0;
        out->latency_ms = now_ms() - t0;
    }
    iv_free(&toks);
    free(ids);
    pthread_mutex_unlock(&e->lock);
    return rc;
}
