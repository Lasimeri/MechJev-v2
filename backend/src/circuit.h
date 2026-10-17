/* circuit.h: a differentiable logic gate network (Petersen, Borgelt,
 * Kuehne, Deussen, "Deep Differentiable Logic Gate Networks", NeurIPS
 * 2022, arXiv:2210.08277). Every neuron has two inputs, wired once at
 * random from a seed and fixed; it learns which of the 16 two-input
 * Boolean functions it is, as a softmax over 16 weights. Training runs the
 * relaxed network (Table 1's real-valued gates, Adam at 0.01); inference
 * takes each neuron's most likely gate and runs on bits, with no multiply,
 * and `circuit_export` writes it out as C (Aster's "compiled directly into
 * C"). See circuit.md. */
#ifndef MJ_CIRCUIT_H
#define MJ_CIRCUIT_H

#include <stdint.h>
#include <stdio.h>

#define CIRCUIT_MAX_LAYERS 8

typedef struct {
    int n_in, n_layers;
    int size[CIRCUIT_MAX_LAYERS]; /* neurons per layer; the last is the output */
    uint64_t seed;                /* the wiring is a function of it */
    int32_t *wa[CIRCUIT_MAX_LAYERS], *wb[CIRCUIT_MAX_LAYERS]; /* inputs of each neuron */
    float *w[CIRCUIT_MAX_LAYERS];                             /* 16 gate weights a neuron */
    float *m[CIRCUIT_MAX_LAYERS], *v[CIRCUIT_MAX_LAYERS];     /* Adam moments */
    uint64_t t;                                               /* Adam steps */
} circuit_t;

/* Activations of one relaxed pass, kept for its backward pass. */
typedef struct {
    float *act[CIRCUIT_MAX_LAYERS + 1]; /* act[0] the input */
    float *grad[CIRCUIT_MAX_LAYERS + 1];
} trace_t;

int circuit_init(circuit_t *c, int n_in, int n_layers, const int *size, uint64_t seed);
void circuit_free(circuit_t *c);
int circuit_out(const circuit_t *c);

void trace_init(trace_t *t, const circuit_t *c);
void trace_free(trace_t *t, const circuit_t *c);

/* Relaxed forward: t->act[0] holds the input in [0,1]; the output is
 * t->act[n_layers]. */
void circuit_forward(const circuit_t *c, trace_t *t);
/* Backward from t->grad[n_layers] (dL/d output), accumulating dL/dw into
 * gw (same layout as c->w), then one Adam step at learning rate lr. */
void circuit_backward(const circuit_t *c, trace_t *t, float **gw);
void circuit_adam(circuit_t *c, float **gw, float lr);
float **circuit_grads(const circuit_t *c);
void circuit_grads_zero(const circuit_t *c, float **gw);
void circuit_grads_free(const circuit_t *c, float **gw);

/* Discrete forward on bits (0 or 1 a byte): each neuron its argmax gate. */
void circuit_run(const circuit_t *c, const uint8_t *in, uint8_t *out);
/* The discrete circuit as a C function `void NAME(const unsigned char *in,
 * unsigned char *out)`. */
int circuit_export(const circuit_t *c, FILE *f, const char *name);

int circuit_write(const circuit_t *c, FILE *f);
int circuit_read(circuit_t *c, FILE *f);

#endif
