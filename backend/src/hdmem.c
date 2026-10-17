/* hdmem.c: the fast memory (hdmem.h; Khasia, SSRN 6459123, 2026). */
#include "hdmem.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "util.h"

/* The splitmix64 finalizer: identifiers whose seeds differ by a constant
 * stride would otherwise be one stream shifted (splitmix64 steps by the
 * golden gamma), so every seed is mixed first. */
static uint64_t mix64(uint64_t z)
{
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
    return z ^ (z >> 31);
}

static void random_hv(uint64_t s, hv_t *out)
{
    for (int i = 0; i < HD_WORDS; i++)
        out->w[i] = splitmix64(&s);
}

/* Definition 1: L_0 uniformly random, then each level flips the next
 * stretch of a random permutation of the bits, never a bit twice, D/2 flips
 * in all at level L-1. So d_H(L_i, L_j) = |i - j| D / (2 (L - 1)) up to the
 * flooring of the stretch ends (Lemma 1). */
static void make_levels(hdmem_t *h)
{
    uint32_t *perm = xmalloc(HD_D * sizeof *perm);
    for (uint32_t i = 0; i < HD_D; i++)
        perm[i] = i;
    uint64_t s = mix64(h->seed ^ 0x4C4556454Cull); /* "LEVEL" */
    for (uint32_t i = HD_D - 1; i > 0; i--) {   /* Fisher-Yates */
        uint32_t j = (uint32_t)(splitmix64(&s) % (i + 1));
        uint32_t t = perm[i];
        perm[i] = perm[j];
        perm[j] = t;
    }
    random_hv(splitmix64(&s), &h->level[0]);
    size_t from = 0;
    for (int k = 1; k < HD_LEVELS; k++) {
        size_t to = (size_t)k * (HD_D / 2) / (HD_LEVELS - 1);
        h->level[k] = h->level[k - 1];
        for (size_t i = from; i < to; i++)
            h->level[k].w[perm[i] / 64] ^= 1ull << (perm[i] % 64);
        from = to;
    }
    free(perm);
}

void hd_init(hdmem_t *h, uint64_t seed)
{
    memset(h, 0, sizeof *h);
    h->seed = seed;
    make_levels(h);
}

void hd_free(hdmem_t *h)
{
    free(h->m);
    free(h->c);
    free(h->count);
    free(h->replay);
    memset(h, 0, sizeof *h);
}

void hd_reserve(hdmem_t *h, size_t n)
{
    if (n <= h->n)
        return;
    if (n > h->cap) {
        size_t cap = h->cap ? h->cap : 64;
        while (cap < n)
            cap *= 2;
        h->m = xrealloc(h->m, cap * HD_D * sizeof *h->m);
        h->c = xrealloc(h->c, cap * sizeof *h->c);
        h->count = xrealloc(h->count, cap * sizeof *h->count);
        h->cap = cap;
    }
    memset(h->m + h->n * HD_D, 0, (n - h->n) * HD_D * sizeof *h->m);
    memset(h->c + h->n, 0, (n - h->n) * sizeof *h->c);
    memset(h->count + h->n, 0, (n - h->n) * sizeof *h->count);
    h->n = n;
}

void hd_id(const hdmem_t *h, uint64_t kind, uint64_t index, hv_t *out)
{
    random_hv(mix64(h->seed ^ mix64(kind ^ 0x1D1D1D1Dull) ^ mix64(index + 0x9E3779B97F4A7C15ull)), out);
}

void hd_level(const hdmem_t *h, double x, hv_t *out)
{
    long q = lround((x < 0 ? 0 : x > 1 ? 1 : x) * (HD_LEVELS - 1));
    *out = h->level[q];
}

void hd_xor(hv_t *a, const hv_t *b)
{
    for (int i = 0; i < HD_WORDS; i++)
        a->w[i] ^= b->w[i];
}

void hd_bundle_init(hd_bundle_t *b) { memset(b, 0, sizeof *b); }

void hd_bundle_add(hd_bundle_t *b, const hv_t *v)
{
    for (int i = 0; i < HD_WORDS; i++)
        for (int k = 0; k < 64; k++)
            b->cnt[i * 64 + k] += (int16_t)((v->w[i] >> k) & 1);
    b->n++;
}

void hd_bundle_done(const hd_bundle_t *b, hv_t *out)
{
    memset(out, 0, sizeof *out);
    for (int i = 0; i < HD_D; i++)
        if (2 * b->cnt[i] > b->n)
            out->w[i / 64] |= 1ull << (i % 64);
}

unsigned hd_distance(const hv_t *a, const hv_t *b)
{
    unsigned d = 0;
    for (int i = 0; i < HD_WORDS; i++)
        d += (unsigned)__builtin_popcountll(a->w[i] ^ b->w[i]);
    return d;
}

double hd_sim(const hdmem_t *h, size_t c, const hv_t *key)
{
    if (c >= h->n || !h->count[c])
        return 0;
    return 1.0 - 2.0 * hd_distance(&h->c[c], key) / HD_D;
}

void hd_superpose(hdmem_t *h, size_t c, const hv_t *key, int sign)
{
    hd_reserve(h, c + 1);
    int16_t *m = h->m + c * HD_D;
    for (int i = 0; i < HD_D; i++) {
        int v = m[i] + ((key->w[i / 64] >> (i % 64)) & 1 ? sign : -sign);
        m[i] = (int16_t)(v > 32767 ? 32767 : v < -32767 ? -32767 : v);
    }
}

void hd_binarize(hdmem_t *h, size_t c)
{
    if (c >= h->n)
        return;
    const int16_t *m = h->m + c * HD_D;
    hv_t *o = &h->c[c];
    memset(o, 0, sizeof *o);
    for (int i = 0; i < HD_D; i++)
        if (m[i] > 0)
            o->w[i / 64] |= 1ull << (i % 64);
}

void hd_remember(hdmem_t *h, const hv_t *key, int32_t label)
{
    if (!h->replay)
        h->replay = xmalloc(HD_REPLAY_MAX * sizeof *h->replay);
    size_t at = h->nreplay < HD_REPLAY_MAX ? h->nreplay++ : h->head;
    if (h->nreplay == HD_REPLAY_MAX)
        h->head = (at + 1) % HD_REPLAY_MAX;
    h->replay[at].key = *key;
    h->replay[at].label = label;
}

/* ---- the store: the classes that hold anything, then the kept contexts.
 * The levels and identifiers are not stored: the seed makes them again. */

#define W(x) (fwrite(&(x), sizeof(x), 1, f) == 1)
#define R(x) (fread(&(x), sizeof(x), 1, f) == 1)

int hd_write(const hdmem_t *h, FILE *f)
{
    uint64_t n = h->n, nr = h->nreplay, head = h->head;
    if (!W(n))
        return -1;
    for (size_t c = 0; c < h->n; c++) {
        const int16_t *m = h->m + c * HD_D;
        uint8_t any = 0;
        for (int i = 0; i < HD_D && !any; i++)
            any = m[i] != 0;
        if (!W(h->count[c]) || !W(any) || (any && fwrite(m, sizeof *m, HD_D, f) != HD_D))
            return -1;
    }
    if (!W(nr) || !W(head))
        return -1;
    for (size_t i = 0; i < h->nreplay; i++)
        if (!W(h->replay[i].key) || !W(h->replay[i].label))
            return -1;
    return 0;
}

int hd_read(hdmem_t *h, FILE *f)
{
    uint64_t n, nr, head;
    if (!R(n))
        return -1;
    hd_reserve(h, n);
    for (size_t c = 0; c < n; c++) {
        uint8_t any;
        if (!R(h->count[c]) || !R(any) || (any && fread(h->m + c * HD_D, sizeof *h->m, HD_D, f) != HD_D))
            return -1;
        hd_binarize(h, c);
    }
    if (!R(nr) || !R(head) || nr > HD_REPLAY_MAX || (nr && head >= nr))
        return -1;
    if (nr)
        h->replay = xmalloc(HD_REPLAY_MAX * sizeof *h->replay);
    for (size_t i = 0; i < nr; i++)
        if (!R(h->replay[i].key) || !R(h->replay[i].label))
            return -1;
    h->nreplay = nr;
    h->head = head;
    return 0;
}

void hd_learn(hdmem_t *h, size_t c, const hv_t *key)
{
    hd_superpose(h, c, key, 1);
    h->count[c]++;
    hd_binarize(h, c);
    hd_remember(h, key, (int32_t)c);
}
