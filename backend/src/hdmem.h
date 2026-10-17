/* hdmem.h: the fast memory. Backpropagation-free continual learning in
 * Boolean space (Khasia, "Backpropagation-Free Continual Learning in
 * Boolean Space for Extreme Edge Intelligence", SSRN 6459123, 2026; the
 * code is booleanclass). A context is one 8192-bit hypervector: each
 * feature's value is a level hypervector (Definition 1: progressive bit
 * flips, so nearer values are nearer in Hamming distance, Lemma 1) bound by
 * XOR to the feature's identifier, and the bound features are bundled by
 * majority (eq 2-3). Each class is an integer prototype that superposes the
 * bipolar contexts it was seen in (eq 5); when the memory predicts another
 * class, the true one gains the context and the predicted one loses it (eq
 * 7-8). Prediction is the least Hamming distance to the binarized
 * prototypes (eq 6). No gradient: a class is learned from one context.
 * See hdmem.md. */
#ifndef MJ_HDMEM_H
#define MJ_HDMEM_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

#define HD_D 8192 /* the paper's dimensionality */
#define HD_WORDS (HD_D / 64)
#define HD_LEVELS 16       /* quantization levels L */
#define HD_REPLAY_MAX 4096 /* contexts kept for the correction epochs */

typedef struct {
    uint64_t w[HD_WORDS];
} hv_t;

/* A majority bundle being built: a count per bit. */
typedef struct {
    int16_t cnt[HD_D];
    int n;
} hd_bundle_t;

typedef struct {
    hv_t key;
    int32_t label;
} hd_example_t;

typedef struct {
    uint64_t seed;
    hv_t level[HD_LEVELS];
    size_t n, cap;   /* classes 0..n-1 */
    int16_t *m;      /* n x HD_D integer prototypes, saturating */
    hv_t *c;         /* their binarizations */
    uint32_t *count; /* contexts superposed into each class; 0: the class is empty */
    hd_example_t *replay; /* a ring of the last HD_REPLAY_MAX contexts */
    size_t nreplay, head;
} hdmem_t;

void hd_init(hdmem_t *h, uint64_t seed);
void hd_free(hdmem_t *h);
/* Room for classes 0..n-1 (new ones empty). */
void hd_reserve(hdmem_t *h, size_t n);

/* Feature identifiers (kind, index) and level hypervectors for x in [0, 1]. */
void hd_id(const hdmem_t *h, uint64_t kind, uint64_t index, hv_t *out);
void hd_level(const hdmem_t *h, double x, hv_t *out);
void hd_xor(hv_t *a, const hv_t *b);
void hd_bundle_init(hd_bundle_t *b);
void hd_bundle_add(hd_bundle_t *b, const hv_t *v);
/* Theta(sum - n/2): a bit is 1 where more than half the vectors have it. */
void hd_bundle_done(const hd_bundle_t *b, hv_t *out);

unsigned hd_distance(const hv_t *a, const hv_t *b);
/* 1 - 2 d_H / D: 1 the same, about 0 unrelated; 0 for an empty class. */
double hd_sim(const hdmem_t *h, size_t c, const hv_t *key);
/* M_c += sign * (2 key - 1) (eq 5, 7, 8); the binarization waits for
 * hd_binarize (the paper re-binarizes at the end of an epoch). */
void hd_superpose(hdmem_t *h, size_t c, const hv_t *key, int sign);
void hd_binarize(hdmem_t *h, size_t c);
/* A context seen with class c: superposed, counted, binarized, and kept
 * for the correction epochs (eq 5, one sample). */
void hd_learn(hdmem_t *h, size_t c, const hv_t *key);
/* Keep a context for the correction epochs. */
void hd_remember(hdmem_t *h, const hv_t *key, int32_t label);

int hd_write(const hdmem_t *h, FILE *f);
int hd_read(hdmem_t *h, FILE *f);

#endif
