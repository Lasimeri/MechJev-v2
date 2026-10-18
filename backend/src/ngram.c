/* ngram.c: the meaning-gated n-gram memory (ngram.h). */
#include "ngram.h"

#include <math.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

#include "util.h"

#define SD_MIN 1e-3 /* a slot that hardly varies carries no meaning: left out of the cosine */

void ng_init(ngram_t *g, size_t dim)
{
    memset(g, 0, sizeof *g);
    g->dim = dim;
    g->mu = xcalloc(dim, sizeof *g->mu);
    g->sq = xcalloc(dim, sizeof *g->sq);
}

void ng_free(ngram_t *g)
{
    free(g->e);
    free(g->sums);
    free(g->mu);
    free(g->sq);
    memset(g, 0, sizeof *g);
}

void ng_reset(ngram_t *g)
{
    size_t dim = g->dim;
    ng_free(g);
    ng_init(g, dim);
}

static bool same_suffix(const int32_t *a, const int32_t *b, int n)
{
    for (int k = NG_ORD - n; k < NG_ORD; k++)
        if (a[k] != b[k])
            return false;
    return true;
}

void ng_learn(ngram_t *g, const int32_t *hist, int32_t next, const float *vars)
{
    for (size_t i = 0; i < g->dim; i++) {
        g->mu[i] += vars[i];
        g->sq[i] += (double)vars[i] * vars[i];
    }
    g->steps++;
    for (int n = 1; n <= NG_ORD; n++) {
        ng_entry_t *e = NULL;
        for (size_t i = 0; i < g->n && !e; i++)
            if (g->e[i].n == n && g->e[i].next == next && same_suffix(g->e[i].ctx, hist, n))
                e = &g->e[i];
        if (!e) {
            if (g->n == g->cap) {
                g->cap = g->cap ? g->cap * 2 : 256;
                g->e = xrealloc(g->e, g->cap * sizeof *g->e);
            }
            if (g->nsums + g->dim > g->capsums) {
                g->capsums = g->capsums ? g->capsums * 2 : 256 * g->dim;
                while (g->nsums + g->dim > g->capsums)
                    g->capsums *= 2;
                g->sums = xrealloc(g->sums, g->capsums * sizeof *g->sums);
            }
            e = &g->e[g->n++];
            memset(e, 0, sizeof *e);
            e->n = (uint8_t)n;
            memcpy(e->ctx, hist, sizeof e->ctx);
            for (int k = 0; k < NG_ORD - n; k++)
                e->ctx[k] = -1; /* outside the suffix: not compared */
            e->next = next;
            e->at = g->nsums;
            memset(g->sums + e->at, 0, g->dim * sizeof *g->sums);
            g->nsums += g->dim;
        }
        e->count++;
        float *s = g->sums + e->at;
        for (size_t i = 0; i < g->dim; i++)
            s[i] += vars[i];
    }
}

double ng_predict(const ngram_t *g, const int32_t *hist, const float *vars, size_t nlex, double *p)
{
    memset(p, 0, nlex * sizeof *p);
    if (!g->steps)
        return 0;
    /* z-scores over everything learned */
    double *mu = xmalloc(g->dim * sizeof *mu), *sd = xmalloc(g->dim * sizeof *sd);
    double *zq = xmalloc(g->dim * sizeof *zq), *ze = xmalloc(g->dim * sizeof *ze);
    double qq = 0;
    for (size_t i = 0; i < g->dim; i++) {
        mu[i] = g->mu[i] / (double)g->steps;
        double v = g->sq[i] / (double)g->steps - mu[i] * mu[i];
        sd[i] = v > 0 ? sqrt(v) : 0;
        zq[i] = sd[i] >= SD_MIN ? (vars[i] - mu[i]) / sd[i] : 0;
        qq += zq[i] * zq[i];
    }
    double *w = xcalloc(nlex, sizeof *w);
    double all = 0, gated = 0, norm = 0;
    for (int n = 1; n <= NG_ORD; n++) {
        memset(w, 0, nlex * sizeof *w);
        double z = 0;
        for (size_t k = 0; k < g->n; k++) {
            const ng_entry_t *e = &g->e[k];
            if (e->n != n || e->next < 0 || (size_t)e->next >= nlex || !same_suffix(e->ctx, hist, n))
                continue;
            const float *s = g->sums + e->at;
            double ee = 0, qe = 0;
            for (size_t i = 0; i < g->dim; i++) {
                ze[i] = sd[i] >= SD_MIN ? (s[i] / e->count - mu[i]) / sd[i] : 0;
                ee += ze[i] * ze[i];
                qe += zq[i] * ze[i];
            }
            double gate = qq > 0 && ee > 0 ? fmax(0, qe / sqrt(qq * ee)) : 0;
            w[e->next] += e->count * gate;
            z += e->count * gate;
            all += e->count;
            gated += e->count * gate;
        }
        if (z > 0) {
            double beta = (double)(1 << (n - 1));
            for (size_t c = 0; c < nlex; c++)
                p[c] += beta * w[c] / z;
            norm += beta;
        }
    }
    if (norm > 0)
        for (size_t c = 0; c < nlex; c++)
            p[c] /= norm;
    free(w);
    free(mu);
    free(sd);
    free(zq);
    free(ze);
    return all > 0 ? gated / all : 0;
}
