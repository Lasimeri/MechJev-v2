/* hdc.c: see hdc.h and hdc.md. */
#include "hdc.h"

#include <string.h>

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

void hv_word(hv_t *out, uint64_t seed, const char *word)
{
    uint64_t s = seed ^ fnv1a(word);
    for (int i = 0; i < HD_WORDS; i++)
        out->w[i] = splitmix64(&s);
}

void hv_xor(hv_t *out, const hv_t *a, const hv_t *b)
{
    for (int i = 0; i < HD_WORDS; i++)
        out->w[i] = a->w[i] ^ b->w[i];
}

void hv_rot(hv_t *out, const hv_t *a, unsigned k)
{
    k %= HD_BITS;
    unsigned q = k / 64, r = k % 64;
    hv_t t;
    for (int i = 0; i < HD_WORDS; i++) {
        uint64_t lo = a->w[(i - (int)q + HD_WORDS) % HD_WORDS];
        uint64_t prev = a->w[(i - (int)q - 1 + 2 * HD_WORDS) % HD_WORDS];
        t.w[i] = r ? (lo << r) | (prev >> (64 - r)) : lo;
    }
    *out = t;
}

unsigned hv_hamming(const hv_t *a, const hv_t *b)
{
    unsigned d = 0;
    for (int i = 0; i < HD_WORDS; i++)
        d += (unsigned)__builtin_popcountll(a->w[i] ^ b->w[i]);
    return d;
}

void bundle_clear(bundle_t *b) { memset(b, 0, sizeof *b); }

void bundle_add(bundle_t *b, const hv_t *v, int32_t w)
{
    for (int i = 0; i < HD_WORDS; i++) {
        uint64_t x = v->w[i];
        int32_t *c = &b->c[i * 64];
        for (int j = 0; j < 64; j++)
            c[j] += ((x >> j) & 1) ? w : -w;
    }
}

void bundle_take(hv_t *out, const bundle_t *b, uint64_t seed)
{
    hv_t tie;
    hv_word(&tie, seed, "\x01tie");
    for (int i = 0; i < HD_WORDS; i++) {
        uint64_t x = 0;
        for (int j = 0; j < 64; j++) {
            int32_t c = b->c[i * 64 + j];
            uint64_t bit = c > 0 ? 1 : c < 0 ? 0 : (tie.w[i] >> j) & 1;
            x |= bit << j;
        }
        out->w[i] = x;
    }
}

void proto_add(proto_t *p, const hv_t *v, int sgn)
{
    for (int i = 0; i < HD_WORDS; i++) {
        uint64_t x = v->w[i];
        int16_t *a = &p->acc[i * 64];
        for (int j = 0; j < 64; j++) {
            int d = ((x >> j) & 1) ? sgn : -sgn;
            int s = a[j] + d;
            a[j] = (int16_t)(s > 32767 ? 32767 : s < -32767 ? -32767 : s);
        }
    }
}

void proto_pack(proto_t *p)
{
    for (int i = 0; i < HD_WORDS; i++) {
        uint64_t x = 0;
        for (int j = 0; j < 64; j++)
            x |= (uint64_t)(p->acc[i * 64 + j] > 0) << j;
        p->sign.w[i] = x;
    }
}
