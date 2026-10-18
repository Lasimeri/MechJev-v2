/* pointer.h: adaptive attention over the person's line, a sentinel-gated
 * pointer (the user's ask, 2026-10-18: "the model isn't adaptive enough",
 * "Adaptive attention"). After Lu et al., "Knowing When to Look"
 * (arXiv:1612.01887): a sentinel gate decides at each word whether to look
 * at the input or rely on what the model knows; and See et al.'s
 * pointer-generator (arXiv:1704.04368): the attention over the input names
 * a word to copy, one MechJev may never have said. It attends only over the
 * person's current line, never over MechJev's own reply (the no-Markov
 * rule). Keys are a few features of each token of the line (a word heard
 * once has no trained embedding, so content could never find it); the query
 * is MechJev's circuit output. The gate reads the meaning the engine gave
 * the turn (the slots): the meaning decides whether to look at the line,
 * the learner learns where. It decides outright (the circuit's
 * distribution is flat, so a soft mix let a little copy mass outvote it).
 * See pointer.md. */
#ifndef MJ_POINTER_H
#define MJ_POINTER_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define PT_BITS 128 /* the circuit's output bits (learner.h OUT_BITS) */
#define PT_F 10     /* features a token */
#define PT_MAXT 24  /* tokens of a line attended over */
#define PT_SLOTS 64 /* the meaning slots (learner.h VAR_SLOTS) */
/* The gate opens under this. 0.5 copied rare words of lines the gate had
 * not learned ("Good job, MechJev." -> "Job are you.", gate 0.26; "Call!",
 * 0.21) while the right copies came at 0.05 to 0.17; held out, 0.2 costs 1
 * copy of 122 and no other step (pointer.md). */
#define PT_GATE_OPEN 0.2f

typedef struct {
    float V[PT_BITS][PT_F], W0[PT_F]; /* the attention: query h = V^T a' + W0, score = h . f */
    float B0, Cn, TP[3], G[PT_F];     /* the sentinel gate: bias, novelty, reply position, attended features, */
    float GS[2][PT_SLOTS];            /* and the meaning (the first word apart from the later ones) */
} pointer_t;

/* The person's line as the pointer sees it: its tokens (lexicon ids, -1
 * for none) and the features that do not change during the reply. */
typedef struct {
    int n;
    int32_t tok[PT_MAXT];
    float f[PT_MAXT][PT_F];
    float novelty;
} pt_line_t;

void pt_init(pointer_t *p);

/* The line's features, `heard` each token's hearings before this turn
 * (from the lexicon, or the replay of the past), `mark` whether a token is
 * a mark. */
void pt_line(pt_line_t *pl, const int32_t *tok, const unsigned *heard, const bool *mark, int n);

/* The attention over the line at reply position t, with MechJev's output
 * bits a (0 or 1), the meaning slots now and the words said so far
 * (feature: already said): att[]
 * filled, the gate returned (under 0.5: copy). */
float pt_forward(const pointer_t *p, const uint8_t *a, const float *slots, const pt_line_t *pl, size_t t,
                 const int32_t *said, size_t ns, float *att);

/* The token the attention names (its lexicon id), -1 when there is none. */
int32_t pt_attended(const pt_line_t *pl, const float *att);

/* The word the pointer says, -1 for the circuit's: the gate open (under
 * PT_GATE_OPEN) and the attention on a word of the line that is neither a mark nor
 * often heard. Those the circuit says itself; copying them made parrots
 * ("Thank you!" -> "You!"; measured: pointer.md). */
int32_t pt_choose(const pt_line_t *pl, const float *att, float gate);

/* One gradient step on the word learned at t: -log P(label), P = gate x
 * the circuit's P(label) + (1 - gate) x the attention on the line's tokens
 * that are the label. */
void pt_train(pointer_t *p, const uint8_t *a, const float *slots, const pt_line_t *pl, size_t t, const int32_t *said,
              size_t ns, int32_t label, float p_circuit, float lr);

/* ---- made again at every start (speak_pointer_rebuild) ---------------- */

#define PT_SAID 64     /* words of the reply so far kept for a step */
#define PT_EPOCHS 25   /* passes over the past at a start (measured: pointer.md) */
#define PT_LR 0.05f

/* One learned word of a past reply: the circuit's output bits then (as the
 * circuit is now), its line, its place, the words said before it. */
typedef struct {
    uint8_t a[PT_BITS];
    float slots[PT_SLOTS]; /* the meaning the circuit saw */
    size_t line; /* into pt_data_t lines */
    uint32_t t, ns;
    int32_t said[PT_SAID];
    int32_t label;
    float pc;          /* the circuit's probability of the label, as it was before it learned the turn */
    int32_t ctop_then; /* the circuit's first word then (the trace's circuit_top1), -1: none */
} pt_step_t;

typedef struct {
    pt_line_t *lines; /* one a passed taught turn */
    size_t nlines, nsteps;
    pt_step_t *steps;
} pt_data_t;

void pt_data_free(pt_data_t *d);

/* The pointer fitted to the past: pt_init, then PT_EPOCHS passes in order,
 * the line `skip` left out ((size_t)-1: none; a held-out measure). */
void pt_fit(pointer_t *p, const pt_data_t *d, size_t skip);

#endif
