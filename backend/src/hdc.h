/* hdc.h: the learner's arithmetic, bits only. 8192-bit binary
 * hypervectors (booleanclass, Khasia 2026): XOR binds, a cyclic rotation
 * marks order, a majority vote bundles, Hamming distance (XOR then
 * popcount) compares. No multiply anywhere, the way the circuit language
 * model of Aster's "An LLM Made from Logic Gates" has none: the only learned
 * state is one accumulator per word. No llama.cpp. See hdc.md. */
#ifndef MJ_HDC_H
#define MJ_HDC_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define HD_BITS 8192
#define HD_WORDS (HD_BITS / 64)

typedef struct {
    uint64_t w[HD_WORDS];
} hv_t;

/* A word's vector: a function of (seed, word) alone, so nothing but the
 * seed has to be stored for it (splitmix64 seeded by FNV-1a of the word). */
void hv_word(hv_t *out, uint64_t seed, const char *word);
void hv_xor(hv_t *out, const hv_t *a, const hv_t *b);
/* Cyclic rotation of all 8192 bits by k places toward the high end. */
void hv_rot(hv_t *out, const hv_t *a, unsigned k);
unsigned hv_hamming(const hv_t *a, const hv_t *b);

/* A weighted majority vote over vectors: each bit adds +w or -w; ties go to
 * a fixed vector derived from the seed. */
typedef struct {
    int32_t c[HD_BITS];
} bundle_t;

void bundle_clear(bundle_t *b);
void bundle_add(bundle_t *b, const hv_t *v, int32_t w);
void bundle_take(hv_t *out, const bundle_t *b, uint64_t seed);

/* A word's prototype: saturating int16 bipolar sums (booleanclass's
 * class_prototypes_sums), and their signs packed. */
typedef struct {
    int16_t acc[HD_BITS];
    hv_t sign;
} proto_t;

void proto_add(proto_t *p, const hv_t *v, int sgn);
void proto_pack(proto_t *p);

uint64_t splitmix64(uint64_t *s);
uint64_t fnv1a(const char *s);

#endif
