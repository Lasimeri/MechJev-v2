/* ngram.h: the meaning-gated n-gram memory, Engram's design for MechJev.
 *
 * The user's ask (2026-10-17): "repurpose [the n-gram's] design in the
 * analog/logic gate part of this design, in order to provide some
 * real-time contextual coherence as those weights update in real-time",
 * with the meaning gate the user chose over plain n-grams (the no-Markov
 * rule). After Cheng et al., "Conditional Memory via Scalable Lookup"
 * (Engram, arXiv:2601.07372): suffix n-grams of the reply so far look up
 * static memory, and a context-aware gate silences what contradicts the
 * context. Here the context is the meaning (the engine's variables): each
 * entry keeps the meaning its word was said under, and counts only as much
 * as the current meaning matches it. It learns only MechJev's own passed
 * replies (never transitions copied from heard text), at once, a turn at a
 * time, while the circuit learns the same words slowly; it is made again
 * from turns.jsonl at every start (speak_ngram_rebuild), nothing stored.
 * See ngram.md. */
#ifndef MJ_NGRAM_H
#define MJ_NGRAM_H

#include <stddef.h>
#include <stdint.h>

#define NG_ORD 3 /* suffixes of 1 to 3 words */

typedef struct {
    uint8_t n;           /* the suffix's length */
    int32_t ctx[NG_ORD]; /* the suffix in the last n places, oldest first */
    int32_t next;        /* the word said after it */
    uint32_t count;
    size_t at; /* its meanings' sum: dim floats at sums + at */
} ng_entry_t;

typedef struct {
    size_t dim; /* the meaning's slots */
    ng_entry_t *e;
    size_t n, cap;
    float *sums;
    size_t nsums, capsums;
    double *mu, *sq; /* every learned meaning's sum and sum of squares, a slot each (for z-scores) */
    uint64_t steps;
} ngram_t;

void ng_init(ngram_t *g, size_t dim);
void ng_free(ngram_t *g);
/* Empty it. */
void ng_reset(ngram_t *g);

/* One word of a passed reply: `hist` the NG_ORD words before it, oldest
 * first (START where the reply had none), `next` the word, `vars` the
 * meaning it was said under (dim probabilities). */
void ng_learn(ngram_t *g, const int32_t *hist, int32_t next, const float *vars);

/* The words that may follow `hist` under the meaning `vars`: p[0..nlex)
 * a distribution (all 0: no evidence). Each entry whose suffix matches is
 * gated by max(0, cos) of the two meanings, z-scored slot by slot over
 * everything learned; each suffix length gives its own distribution, the
 * longer weighing more (1, 2, 4). Returns the evidence's gate, Engram's
 * alpha: the gated share of the matching counts, 0 when the meaning
 * contradicts all of them. */
double ng_predict(const ngram_t *g, const int32_t *hist, const float *vars, size_t nlex, double *p);

#endif
