/* learner.h: the language learner, a conceptually grounded logic gate
 * circuit. Its inputs are variables whose probabilities the decision
 * engine sets (typed questions about the conversation and about the reply
 * so far), the previous word's code (Aster's fixed attention to the token
 * before) and the position. Its output bits are added or subtracted by
 * each word's ternary embedding, tied with the input code (Aster's
 * unembedding), into a score per word of its lexicon. It is taught by the
 * engine's distribution over the words it chose between, never by copying
 * what follows what in heard text. No llama.cpp: the engine is reached
 * over HTTP. The little book (Revelation 10:9-11): eaten, then spoken.
 * See learner.md. */
#ifndef MJ_LEARNER_H
#define MJ_LEARNER_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "circuit.h"

#define LEARNER_VERSION 2u
#define VAR_SLOTS 64 /* variable inputs; unused slots read 0.5 */
#define CODE_IN 32   /* bits of the previous word's code fed in */
#define POS_BITS 8   /* position thermometer: 1 2 3 5 8 12 20 30 */
#define N_IN (VAR_SLOTS + CODE_IN + POS_BITS + 2)
#define OUT_BITS 128
#define TAU 8.0f /* GroupSum temperature (the paper's tau): scores within +-16 */
#define MAX_OPTS 24
#define START "<s>"
#define END "</s>"

typedef struct {
    char *word;
    uint32_t heard;  /* times heard in the person's English */
    uint32_t picked; /* times the engine chose it */
    float e[OUT_BITS], em[OUT_BITS], ev[OUT_BITS]; /* latent embedding and its Adam moments */
} lex_t;

/* One word the engine decided: what the circuit saw, what was shown, and
 * the engine's distribution over it. */
typedef struct {
    float vars[VAR_SLOTS]; /* P of each variable, 0.5 where unknown */
    int32_t prev;          /* the word before */
    uint16_t pos;
    uint16_t n;
    int32_t opt[MAX_OPTS]; /* the words shown */
    float q[MAX_OPTS];     /* the engine's probabilities over them, summing to 1 */
    int32_t top;           /* the engine's choice */
    uint8_t failed;        /* its reply failed a requirement: teaches only the preference among the shown words */
    uint64_t id;
} example_t;

typedef struct {
    uint64_t seed;
    lex_t *v;
    size_t n, cap;
    int32_t *ix;
    size_t ixcap;
    uint32_t min_heard;
    circuit_t circ;
    char *slot[VAR_SLOTS]; /* the variable in each slot; append-only */
    example_t *replay;
    size_t nreplay, capreplay;
    uint64_t turns, steps, trained, emb_t, next_id;
} learner_t;

/* What one evaluation pass over examples measured. */
typedef struct {
    size_t n;
    double agree;       /* the circuit's first word == the engine's choice */
    double nll;         /* -log p of the engine's choice among the speakable words */
    double gap;         /* relaxed and discrete circuits picking the same word */
} evalres_t;

typedef enum { ABL_NONE, ABL_CONCEPTS, ABL_PREV } ablation_t;

learner_t *learner_new(uint64_t seed);
void learner_free(learner_t *l);
int learner_load(const char *path, learner_t **out, char *err, size_t errlen);
int learner_save(const learner_t *l, const char *path, char *err, size_t errlen);

int32_t lex_find(const learner_t *l, const char *w);
int32_t lex_add(learner_t *l, const char *w);
bool lex_speakable(const learner_t *l, int32_t i);
size_t lex_speakable_count(const learner_t *l);
bool is_punct(const char *w);
size_t split_words(const char *text, char ***out);
void free_words(char **w, size_t n);

/* Hearing: the person's words counted (that is all: what follows what is
 * never learned from hearing). */
void learner_hear(learner_t *l, char **words, size_t n);

/* The variable slot named `name`, made when new; -1 when all are taken. */
int learner_slot(learner_t *l, const char *name);

/* The circuit's input bits: each variable drawn from its probability with
 * `seed` (the clock's at inference, the example's in training), the
 * previous word's code, the position. */
void learner_inputs(const learner_t *l, const float *vars, int32_t prev, unsigned pos,
                    uint64_t seed, ablation_t abl, uint8_t *bits);
/* The k best speakable words for these inputs, by the discrete circuit,
 * and the softmax probability of the first (how sure it is). */
size_t learner_propose(const learner_t *l, const uint8_t *bits, int32_t *out, size_t k,
                       bool allow_end, double *p_first);
/* Learn a turn's examples (a failed one only the engine's preference among the
 * words shown, never that its word was right), then rehearse the replay
 * buffer for at most budget_ms. Returns the passes made. */
size_t learner_learn(learner_t *l, const example_t *ex, size_t n, double budget_ms);
/* Measure examples (prequentially: before learning them). */
evalres_t learner_eval(const learner_t *l, const example_t *ex, size_t n, ablation_t abl);

#endif
