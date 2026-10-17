/* circuit.c: see circuit.h and circuit.md. */
#include "circuit.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "util.h"

/* Table 1 of the paper: the 16 real-valued two-input gates (probabilistic
 * T-norm and T-conorm), and their partial derivatives. */
static inline float gate(int i, float a, float b)
{
    switch (i) {
    case 0: return 0;
    case 1: return a * b;
    case 2: return a - a * b;
    case 3: return a;
    case 4: return b - a * b;
    case 5: return b;
    case 6: return a + b - 2 * a * b;
    case 7: return a + b - a * b;
    case 8: return 1 - (a + b - a * b);
    case 9: return 1 - (a + b - 2 * a * b);
    case 10: return 1 - b;
    case 11: return 1 - b + a * b;
    case 12: return 1 - a;
    case 13: return 1 - a + a * b;
    case 14: return 1 - a * b;
    default: return 1;
    }
}

static inline float gate_da(int i, float b)
{
    static const float k[16] = {0, 0, 1, 1, 0, 0, 1, 1, -1, -1, 0, 0, -1, -1, 0, 0};
    static const float kb[16] = {0, 1, -1, 0, -1, 0, -2, -1, 1, 2, 0, 1, 0, 1, -1, 0};
    return k[i] + kb[i] * b; /* d/da as c + c_b * b */
}

static inline float gate_db(int i, float a)
{
    static const float k[16] = {0, 0, 0, 0, 1, 1, 1, 1, -1, -1, -1, -1, 0, 0, 0, 0};
    static const float ka[16] = {0, 1, -1, 0, -1, 0, -2, -1, 1, 2, 0, 1, 0, 1, -1, 0};
    return k[i] + ka[i] * a;
}

/* The same gates on bits. */
static inline uint8_t gate_bit(int i, uint8_t a, uint8_t b)
{
    return (uint8_t)((i >> (3 - ((a << 1) | b))) & 1); /* Table 1's truth columns 00 01 10 11 */
}

static void softmax16(const float *w, float *p)
{
    float m = w[0];
    for (int i = 1; i < 16; i++)
        m = fmaxf(m, w[i]);
    float z = 0;
    for (int i = 0; i < 16; i++) {
        p[i] = expf(w[i] - m);
        z += p[i];
    }
    for (int i = 0; i < 16; i++)
        p[i] /= z;
}

static int argmax16(const float *w)
{
    int b = 0;
    for (int i = 1; i < 16; i++)
        if (w[i] > w[b])
            b = i;
    return b;
}

/* A normal draw (Box-Muller) from a splitmix64 stream. */
static float normal(uint64_t *s)
{
    double u1 = ((double)(splitmix64(s) >> 11) + 1.0) * 0x1.0p-53;
    double u2 = (double)(splitmix64(s) >> 11) * 0x1.0p-53;
    return (float)(sqrt(-2.0 * log(u1)) * cos(6.283185307179586 * u2));
}

int circuit_out(const circuit_t *c) { return c->size[c->n_layers - 1]; }

static void wire(circuit_t *c)
{
    uint64_t s = c->seed ^ 0x6c6f676963ull; /* "logic" */
    int prev = c->n_in;
    for (int l = 0; l < c->n_layers; l++) {
        for (int k = 0; k < c->size[l]; k++) {
            int a = (int)(splitmix64(&s) % (uint64_t)prev);
            int b = (int)(splitmix64(&s) % (uint64_t)(prev - 1));
            if (b >= a)
                b++; /* two different inputs */
            c->wa[l][k] = a;
            c->wb[l][k] = b;
        }
        prev = c->size[l];
    }
}

static void alloc_layers(circuit_t *c)
{
    for (int l = 0; l < c->n_layers; l++) {
        size_t n = (size_t)c->size[l];
        c->wa[l] = xcalloc(n, sizeof(int32_t));
        c->wb[l] = xcalloc(n, sizeof(int32_t));
        c->w[l] = xcalloc(n * 16, sizeof(float));
        c->m[l] = xcalloc(n * 16, sizeof(float));
        c->v[l] = xcalloc(n * 16, sizeof(float));
    }
}

int circuit_init(circuit_t *c, int n_in, int n_layers, const int *size, uint64_t seed)
{
    if (n_layers < 1 || n_layers > CIRCUIT_MAX_LAYERS || n_in < 2)
        return -1;
    memset(c, 0, sizeof *c);
    c->n_in = n_in;
    c->n_layers = n_layers;
    c->seed = seed;
    for (int l = 0; l < n_layers; l++)
        c->size[l] = size[l];
    alloc_layers(c);
    wire(c);
    /* "we draw elements of w independently from a standard normal" */
    uint64_t s = seed ^ 0x77656967687473ull;
    for (int l = 0; l < n_layers; l++)
        for (int i = 0; i < c->size[l] * 16; i++)
            c->w[l][i] = normal(&s);
    return 0;
}

void circuit_free(circuit_t *c)
{
    for (int l = 0; l < c->n_layers; l++) {
        free(c->wa[l]);
        free(c->wb[l]);
        free(c->w[l]);
        free(c->m[l]);
        free(c->v[l]);
    }
    memset(c, 0, sizeof *c);
}

void trace_init(trace_t *t, const circuit_t *c)
{
    memset(t, 0, sizeof *t);
    t->act[0] = xcalloc((size_t)c->n_in, sizeof(float));
    t->grad[0] = xcalloc((size_t)c->n_in, sizeof(float));
    for (int l = 0; l < c->n_layers; l++) {
        t->act[l + 1] = xcalloc((size_t)c->size[l], sizeof(float));
        t->grad[l + 1] = xcalloc((size_t)c->size[l], sizeof(float));
    }
}

void trace_free(trace_t *t, const circuit_t *c)
{
    for (int l = 0; l <= c->n_layers; l++) {
        free(t->act[l]);
        free(t->grad[l]);
    }
}

void circuit_forward(const circuit_t *c, trace_t *t)
{
    float p[16];
    for (int l = 0; l < c->n_layers; l++) {
        const float *in = t->act[l];
        float *out = t->act[l + 1];
        for (int k = 0; k < c->size[l]; k++) {
            float a = in[c->wa[l][k]], b = in[c->wb[l][k]];
            softmax16(&c->w[l][k * 16], p);
            float y = 0;
            for (int i = 0; i < 16; i++)
                y += p[i] * gate(i, a, b);
            out[k] = y;
        }
    }
}

float **circuit_grads(const circuit_t *c)
{
    float **g = xcalloc(CIRCUIT_MAX_LAYERS, sizeof *g);
    for (int l = 0; l < c->n_layers; l++)
        g[l] = xcalloc((size_t)c->size[l] * 16, sizeof(float));
    return g;
}

void circuit_grads_zero(const circuit_t *c, float **gw)
{
    for (int l = 0; l < c->n_layers; l++)
        memset(gw[l], 0, (size_t)c->size[l] * 16 * sizeof(float));
}

void circuit_grads_free(const circuit_t *c, float **gw)
{
    for (int l = 0; l < c->n_layers; l++)
        free(gw[l]);
    free(gw);
}

void circuit_backward(const circuit_t *c, trace_t *t, float **gw)
{
    float p[16], f[16];
    for (int l = c->n_layers - 1; l >= 0; l--) {
        const float *in = t->act[l];
        float *gin = t->grad[l];
        memset(gin, 0, (size_t)(l ? c->size[l - 1] : c->n_in) * sizeof(float));
        for (int k = 0; k < c->size[l]; k++) {
            float g = t->grad[l + 1][k];
            if (g == 0)
                continue;
            int ia = c->wa[l][k], ib = c->wb[l][k];
            float a = in[ia], b = in[ib];
            softmax16(&c->w[l][k * 16], p);
            float y = 0;
            for (int i = 0; i < 16; i++) {
                f[i] = gate(i, a, b);
                y += p[i] * f[i];
            }
            float da = 0, db = 0;
            for (int i = 0; i < 16; i++) {
                gw[l][k * 16 + i] += g * p[i] * (f[i] - y); /* softmax: p_i (f_i - y) */
                da += p[i] * gate_da(i, b);
                db += p[i] * gate_db(i, a);
            }
            gin[ia] += g * da;
            gin[ib] += g * db;
        }
    }
}

void circuit_adam(circuit_t *c, float **gw, float lr)
{
    const float b1 = 0.9f, b2 = 0.999f, eps = 1e-8f;
    c->t++;
    float c1 = 1.0f - powf(b1, (float)c->t), c2 = 1.0f - powf(b2, (float)c->t);
    for (int l = 0; l < c->n_layers; l++)
        for (int i = 0; i < c->size[l] * 16; i++) {
            float g = gw[l][i];
            c->m[l][i] = b1 * c->m[l][i] + (1 - b1) * g;
            c->v[l][i] = b2 * c->v[l][i] + (1 - b2) * g * g;
            c->w[l][i] -= lr * (c->m[l][i] / c1) / (sqrtf(c->v[l][i] / c2) + eps);
        }
}

void circuit_run(const circuit_t *c, const uint8_t *in, uint8_t *out)
{
    int widest = c->n_in;
    for (int l = 0; l < c->n_layers; l++)
        if (c->size[l] > widest)
            widest = c->size[l];
    uint8_t *x = xmalloc((size_t)widest), *y = xmalloc((size_t)widest);
    memcpy(x, in, (size_t)c->n_in);
    for (int l = 0; l < c->n_layers; l++) {
        for (int k = 0; k < c->size[l]; k++)
            y[k] = gate_bit(argmax16(&c->w[l][k * 16]), x[c->wa[l][k]], x[c->wb[l][k]]);
        uint8_t *s = x;
        x = y;
        y = s;
    }
    memcpy(out, x, (size_t)circuit_out(c));
    free(x);
    free(y);
}

static const char *gate_c(int i)
{
    static const char *e[16] = {"0",       "a & b",        "a & !b", "a",
                                "!a & b",  "b",            "a ^ b",  "a | b",
                                "!(a | b)", "!(a ^ b)",    "!b",     "a | !b",
                                "!a",      "!a | b",       "!(a & b)", "1"};
    return e[i];
}

int circuit_export(const circuit_t *c, FILE *f, const char *name)
{
    fprintf(f, "/* A logic gate circuit exported by MechJev-v2's learner: %d inputs,", c->n_in);
    for (int l = 0; l < c->n_layers; l++)
        fprintf(f, " %d", c->size[l]);
    fprintf(f, " gates a layer, wiring seed %llu. Bits are 0 or 1. */\n",
            (unsigned long long)c->seed);
    fprintf(f, "void %s(const unsigned char *in, unsigned char *out)\n{\n", name);
    for (int l = 0; l < c->n_layers; l++)
        fprintf(f, "    unsigned char l%d[%d];\n", l, c->size[l]);
    for (int l = 0; l < c->n_layers; l++) {
        for (int k = 0; k < c->size[l]; k++) {
            int g = argmax16(&c->w[l][k * 16]);
            char a[32], b[32];
            if (l == 0) {
                snprintf(a, sizeof a, "in[%d]", c->wa[l][k]);
                snprintf(b, sizeof b, "in[%d]", c->wb[l][k]);
            } else {
                snprintf(a, sizeof a, "l%d[%d]", l - 1, c->wa[l][k]);
                snprintf(b, sizeof b, "l%d[%d]", l - 1, c->wb[l][k]);
            }
            fprintf(f, "    { unsigned char a = %s, b = %s; (void)a; (void)b; l%d[%d] = (%s) & 1; }\n",
                    a, b, l, k, gate_c(g));
        }
    }
    fprintf(f, "    for (int k = 0; k < %d; k++)\n        out[k] = l%d[k];\n}\n", circuit_out(c),
            c->n_layers - 1);
    return ferror(f) ? -1 : 0;
}

int circuit_write(const circuit_t *c, FILE *f)
{
    int32_t hdr[2 + CIRCUIT_MAX_LAYERS] = {c->n_in, c->n_layers};
    for (int l = 0; l < c->n_layers; l++)
        hdr[2 + l] = c->size[l];
    fwrite(hdr, sizeof hdr, 1, f);
    fwrite(&c->seed, 8, 1, f);
    fwrite(&c->t, 8, 1, f);
    for (int l = 0; l < c->n_layers; l++) {
        size_t n = (size_t)c->size[l] * 16;
        fwrite(c->w[l], sizeof(float), n, f);
        fwrite(c->m[l], sizeof(float), n, f);
        fwrite(c->v[l], sizeof(float), n, f);
    }
    return ferror(f) ? -1 : 0;
}

int circuit_read(circuit_t *c, FILE *f)
{
    int32_t hdr[2 + CIRCUIT_MAX_LAYERS];
    memset(c, 0, sizeof *c);
    if (fread(hdr, sizeof hdr, 1, f) != 1 || hdr[1] < 1 || hdr[1] > CIRCUIT_MAX_LAYERS)
        return -1;
    c->n_in = hdr[0];
    c->n_layers = hdr[1];
    for (int l = 0; l < c->n_layers; l++)
        c->size[l] = hdr[2 + l];
    if (fread(&c->seed, 8, 1, f) != 1 || fread(&c->t, 8, 1, f) != 1)
        return -1;
    alloc_layers(c);
    wire(c); /* a function of the seed: never stored */
    for (int l = 0; l < c->n_layers; l++) {
        size_t n = (size_t)c->size[l] * 16;
        if (fread(c->w[l], sizeof(float), n, f) != n || fread(c->m[l], sizeof(float), n, f) != n ||
            fread(c->v[l], sizeof(float), n, f) != n)
            return -1;
    }
    return 0;
}
