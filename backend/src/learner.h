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
#include "hdmem.h"
#include "ngram.h"
#include "pointer.h"

#define LEARNER_VERSION 4u
#define VAR_SLOTS 64 /* variable inputs; unused slots read 0.5 */
#ifndef VAR_BITS
#define VAR_BITS 1 /* input bits a variable, each drawn from its probability; 4 measured (learner.md) */
#endif
#define VAR_IN (VAR_SLOTS * VAR_BITS)
#define CODE_IN 32   /* bits of the previous word's code fed in */
#define POS_BITS 8   /* position thermometer: 1 2 3 5 8 12 20 30 */
#define N_IN (VAR_IN + CODE_IN + POS_BITS + 2)
#define N_IN_ONE_BIT (VAR_SLOTS + CODE_IN + POS_BITS + 2) /* the input width with one bit a variable */
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
    /* its casing as heard: mid-sentence with capitals ("MechJev", "Zin") or
     * without, and the capitalised form; shown so when capitals won (not
     * stored: made again from turns.jsonl at a start, learner_hear_casing) */
    uint32_t cap_mid, low_mid, cased_votes; /* cased_votes: its form's lead (a majority vote) */
    char *cased;
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
    int32_t said;          /* the word drawn and said (-1: none) */
    float reward;          /* its per-token reward: the change in the engine's coherence of the reply (RED) */
    uint8_t failed;        /* its reply failed a requirement (recorded; the reward carries the credit) */
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
    hdmem_t mem;      /* the fast memory: contexts to words, no gradient (hdmem.h) */
    float mem_weight; /* its share of a word's score; 0: off (configuration, not stored) */
    float recent_share; /* share of rehearsal passes drawn from the last RECENT_EXAMPLES (configuration) */
    ngram_t ng;        /* the meaning-gated n-gram memory, made from turns.jsonl at every start (ngram.h) */
    float ng_weight;   /* its share of a word's probability, times its gate; 0: off (configuration) */
    pointer_t pt;      /* adaptive attention over the person's line, made from turns.jsonl at every start (pointer.h) */
    bool circuit_fresh; /* the circuit was made new at load (another input layout): learner circuit-rebuild */
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
/* The casing of the words of a line as heard (its original text): a word
 * with a capital inside it, or one capitalised away from a sentence's start,
 * counts for that form; one all in small letters counts against. A word
 * not in the lexicon is passed over. */
void learner_hear_casing(learner_t *l, const char *text);
/* Word i as MechJev writes it: the form it heard with capitals when that
 * won, else as it is kept (small letters). */
const char *lex_shown(const learner_t *l, int32_t i);
bool is_punct(const char *w);
size_t split_words(const char *text, char ***out);
void free_words(char **w, size_t n);

/* Hearing: the person's words counted (that is all: what follows what is
 * never learned from hearing). */
void learner_hear(learner_t *l, char **words, size_t n);
/* Heard `times` over: the parent's words (speak.c PARENT_HEARD), a child's
 * first teacher, count more than anyone else's. */
void learner_hear_as(learner_t *l, char **words, size_t n, uint32_t times);

/* The variable slot named `name`, made when new; -1 when all are taken. */
int learner_slot(learner_t *l, const char *name);

/* The circuit's input bits: each variable drawn from its probability with
 * `seed` (the clock's at inference, the example's in training), the
 * previous word's code, the position. */
void learner_inputs(const learner_t *l, const float *vars, int32_t prev, unsigned pos,
                    uint64_t seed, ablation_t abl, uint8_t *bits);
/* The n-gram memory's question at a step (ngram.h): the NG_ORD words said
 * before it (START where there were none) and the meaning (the slots the
 * circuit sees); its own first word and its gate come back. */
typedef struct {
    const int32_t *hist;
    const float *vars;
    int32_t top;
    double gate;
} ng_query_t;
/* The k best speakable words for these inputs, by the discrete circuit and
 * (with a context and a mem_weight) the fast memory, and (with a query and
 * an ng_weight) the n-gram memory; and the softmax probability of the first
 * (how sure it is). */
/* The circuit's output bits for these inputs (the pointer's query), and its
 * probability of word w over the speakable words from them. */
void learner_out_bits(const learner_t *l, const uint8_t *bits, uint8_t *a);
float learner_circuit_prob(const learner_t *l, const uint8_t *a, int32_t w);
size_t learner_propose(const learner_t *l, const uint8_t *bits, const hv_t *context, ng_query_t *ngq,
                       int32_t *out, size_t k, bool allow_end, double *p_first);
/* Learn a turn's examples (a failed one only the engine's preference among the
 * words shown, never that its word was right), then rehearse the replay
 * buffer for at most budget_ms, and at most max_passes passes in all (0: no
 * cap; a test caps it, so a seed gives the same circuit every run). Returns
 * the passes made. */
size_t learner_learn(learner_t *l, const example_t *ex, size_t n, double budget_ms, size_t max_passes);
/* Measure examples (prequentially: before learning them). */
evalres_t learner_eval(const learner_t *l, const example_t *ex, size_t n, ablation_t abl);

/* The fast memory's context for a step: the variables, the position and
 * the person's words, bundled (hdmem.h). Never the reply's own words: what
 * follows what is not learned. */
void learner_context(const learner_t *l, const float *vars, unsigned pos, char **heard, size_t nheard,
                     hv_t *out);
/* What one turn did to the memory. */
typedef struct {
    size_t learned; /* contexts superposed */
    size_t epochs;  /* correction epochs run */
    double fit;     /* share of the kept contexts it predicted right, in the last epoch */
} memfit_t;
/* The paper's Algorithm 1 for new data: each context superposed into its
 * word's prototype, then correction epochs over the kept contexts (eq
 * 7-8) until 95% are right or 3 epochs have run. */
memfit_t learner_memorize(learner_t *l, const hv_t *contexts, const int32_t *labels, size_t n);
/* The memory's own word for each context (-1: it holds nothing yet), and
 * the share it got right, before learning them; NAN when it holds nothing. */
int32_t learner_recall(const learner_t *l, const hv_t *context, bool allow_end);
double learner_recall_agree(const learner_t *l, const hv_t *contexts, const int32_t *labels, size_t n);
/* How far the memory's first word is ahead of its second, in similarity
 * (NAN while it holds fewer than two words): what mem_weight scales. */
double learner_recall_margin(const learner_t *l, const hv_t *context);
/* The circuit made new (its wiring from the seed) and its training count
 * zeroed: learner circuit-rebuild retrains it from the replay. */
void learner_circuit_reset(learner_t *l);
/* The memory emptied (the levels and identifiers stay: they are the seed's). */
void learner_memory_reset(learner_t *l);

#endif
