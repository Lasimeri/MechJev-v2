/* pointer.c: adaptive attention over the person's line (pointer.h). */
#include "pointer.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#define L2 1e-4f
#define L2_MEANING 1e-3f /* the meaning weights: 128 of them on a few hundred words (measured: pointer.md) */

void pt_init(pointer_t *p)
{
    memset(p, 0, sizeof *p);
    p->B0 = 2.0f;   /* trust the circuit at first */
    p->W0[0] = 1.0f; /* a new word draws the attention a little from the start */
}

void pt_line(pt_line_t *pl, const int32_t *tok, const unsigned *heard, const bool *mark, int n)
{
    memset(pl, 0, sizeof *pl);
    pl->n = n < PT_MAXT ? n : PT_MAXT;
    int last = -1, after[PT_MAXT], content = 0, novel = 0;
    for (int j = pl->n - 1, c = 0; j >= 0; j--) {
        after[j] = c;
        if (!mark[j]) {
            if (last < 0)
                last = j;
            c++;
        }
    }
    for (int j = 0; j < pl->n; j++) {
        pl->tok[j] = tok[j];
        float *f = pl->f[j];
        f[0] = tok[j] < 0 || heard[j] == 0; /* new before this turn */
        f[1] = j == last;                   /* the line's last content word */
        int d = after[j];                   /* content words after it */
        f[2] = d == 0, f[3] = d == 1, f[4] = d >= 2 && d <= 3, f[5] = d >= 4;
        f[6] = mark[j];
        f[7] = heard[j] >= 10; /* often heard: a function word, most likely */
        /* f[8], already said, is set per step */
        f[9] = 1;
        if (!mark[j]) {
            content++;
            novel += f[0] > 0;
        }
    }
    pl->novelty = content ? (float)novel / (float)content : 0;
}

/* the step's features: the line's, and whether the token was said already */
static void step_features(const pt_line_t *pl, const int32_t *said, size_t ns, float f[][PT_F])
{
    for (int j = 0; j < pl->n; j++) {
        memcpy(f[j], pl->f[j], sizeof f[j]);
        bool already = false;
        for (size_t q = 0; q < ns && !already; q++)
            already = said[q] == pl->tok[j];
        f[j][8] = already;
    }
}

/* The gate: the line, the place, what the attention landed on (fa, filled),
 * and the meaning, each slot centered (a slot not asked stays 0.5). The
 * meaning measured: without it the gate copied the word at "Do you know the
 * Zin?" where "What" comes first (pointer.md). */
static float gate_of(const pointer_t *p, const float *slots, const pt_line_t *pl, size_t t, const float f[][PT_F],
                     const float *att, float *fa)
{
    memset(fa, 0, PT_F * sizeof *fa);
    for (int j = 0; j < pl->n; j++)
        for (int k = 0; k < PT_F; k++)
            fa[k] += att[j] * f[j][k];
    float g = p->B0 + p->Cn * pl->novelty + p->TP[t == 0 ? 0 : t == 1 ? 1 : 2];
    for (int k = 0; k < PT_F; k++)
        g += p->G[k] * fa[k];
    const float *gs = p->GS[t == 0 ? 0 : 1];
    for (int k = 0; k < PT_SLOTS; k++)
        g += gs[k] * (slots[k] - 0.5f);
    return 1.0f / (1.0f + expf(-g));
}

static float attend(const pointer_t *p, const uint8_t *a, const pt_line_t *pl, const float f[][PT_F], float *att)
{
    float h[PT_F];
    for (int k = 0; k < PT_F; k++) {
        float acc = p->W0[k];
        for (int b = 0; b < PT_BITS; b++)
            acc += p->V[b][k] * (a[b] ? 1.0f : -1.0f);
        h[k] = acc;
    }
    float mx = -INFINITY, z = 0;
    for (int j = 0; j < pl->n; j++) {
        float s = 0;
        for (int k = 0; k < PT_F; k++)
            s += h[k] * f[j][k];
        att[j] = s;
        mx = fmaxf(mx, s);
    }
    for (int j = 0; j < pl->n; j++)
        z += att[j] = expf(att[j] - mx);
    for (int j = 0; j < pl->n; j++)
        att[j] /= z;
    return 0;
}

float pt_forward(const pointer_t *p, const uint8_t *a, const float *slots, const pt_line_t *pl, size_t t,
                 const int32_t *said, size_t ns, float *att)
{
    if (!pl->n)
        return 1;
    float f[PT_MAXT][PT_F], fa[PT_F];
    step_features(pl, said, ns, f);
    attend(p, a, pl, f, att);
    return gate_of(p, slots, pl, t, f, att, fa);
}

int32_t pt_attended(const pt_line_t *pl, const float *att)
{
    int best = -1;
    for (int j = 0; j < pl->n; j++)
        if (pl->tok[j] >= 0 && (best < 0 || att[j] > att[best]))
            best = j;
    return best < 0 ? -1 : pl->tok[best];
}

int32_t pt_choose(const pt_line_t *pl, const float *att, float gate)
{
    if (gate >= PT_GATE_OPEN)
        return -1;
    int best = -1;
    for (int j = 0; j < pl->n; j++)
        if (pl->tok[j] >= 0 && (best < 0 || att[j] > att[best]))
            best = j;
    if (best < 0 || pl->f[best][6] > 0 || pl->f[best][7] > 0)
        return -1;
    return pl->tok[best];
}

void pt_train(pointer_t *p, const uint8_t *a, const float *slots, const pt_line_t *pl, size_t t, const int32_t *said,
              size_t ns, int32_t label, float p_circuit, float lr)
{
    if (!pl->n)
        return;
    float f[PT_MAXT][PT_F], att[PT_MAXT], fa[PT_F];
    step_features(pl, said, ns, f);
    attend(p, a, pl, f, att);
    float beta = gate_of(p, slots, pl, t, f, att, fa);
    float A = 0;
    for (int j = 0; j < pl->n; j++)
        if (pl->tok[j] == label)
            A += att[j];
    float P = beta * p_circuit + (1 - beta) * A + 1e-6f;
    /* the gate (the circuit bits left out of it: too many for this much data) */
    float dg = -(p_circuit - A) / P * beta * (1 - beta);
    p->B0 -= lr * dg;
    p->Cn -= lr * (dg * pl->novelty + L2 * p->Cn);
    p->TP[t == 0 ? 0 : t == 1 ? 1 : 2] -= lr * dg;
    for (int k = 0; k < PT_F; k++)
        p->G[k] -= lr * (dg * fa[k] + L2 * p->G[k]);
    float *gs = p->GS[t == 0 ? 0 : 1];
    for (int k = 0; k < PT_SLOTS; k++)
        gs[k] -= lr * (dg * (slots[k] - 0.5f) + L2_MEANING * gs[k]);
    /* the attention */
    float g[PT_F] = {0};
    for (int j = 0; j < pl->n; j++) {
        float ds = -(1 - beta) / P * att[j] * ((pl->tok[j] == label ? 1.0f : 0.0f) - A);
        for (int k = 0; k < PT_F; k++)
            g[k] += ds * f[j][k];
    }
    for (int k = 0; k < PT_F; k++) {
        p->W0[k] -= lr * (g[k] + L2 * p->W0[k]);
        for (int b = 0; b < PT_BITS; b++)
            p->V[b][k] -= lr * (g[k] * (a[b] ? 1.0f : -1.0f) + L2 * p->V[b][k]);
    }
}

void pt_data_free(pt_data_t *d)
{
    free(d->lines);
    free(d->steps);
    memset(d, 0, sizeof *d);
}

void pt_fit(pointer_t *p, const pt_data_t *d, size_t skip)
{
    pt_init(p);
    for (int ep = 0; ep < PT_EPOCHS; ep++)
        for (size_t i = 0; i < d->nsteps; i++) {
            const pt_step_t *s = &d->steps[i];
            if (s->line != skip)
                pt_train(p, s->a, s->slots, &d->lines[s->line], s->t, s->said, s->ns, s->label, s->pc, PT_LR);
        }
}
