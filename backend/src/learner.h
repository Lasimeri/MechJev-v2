/* learner.h: the language learner. A lexicon that grows from what it hears
 * and what the decision engine confirms, and one prototype per word over
 * contexts (the previous words, in order, and the person's words), all in
 * hdc.h's bits. It proposes next words; the engine (Jev's typed decisions,
 * over HTTP, never llama.cpp in this process) picks among them, and each
 * pick is a label. Nothing of the 35B's weights changes: the language is
 * acquired here. The little book (Revelation 10:9-11): eaten, then spoken.
 * See learner.md. */
#ifndef MJ_LEARNER_H
#define MJ_LEARNER_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "hdc.h"

#define LEARNER_VERSION 1u
#define CTX_WORDS 3
#define START "<s>"
#define END "</s>"

typedef struct {
    char *word;
    uint32_t heard;  /* times heard in the person's text */
    uint32_t picked; /* times the engine chose it */
    proto_t *proto;  /* NULL until it has been a label */
} lex_t;

typedef struct {
    uint64_t seed;
    lex_t *v;
    size_t n, cap;
    /* index: open addressing over v */
    int32_t *ix;
    size_t ixcap;
    uint32_t min_heard; /* heard this often before it may be said */
    uint64_t turns, steps, updates;
} learner_t;

typedef struct {
    int32_t word; /* lexicon index */
    int32_t sim;  /* D - 2 * Hamming, -8192..8192; 0 without a prototype */
} cand_t;

learner_t *learner_new(uint64_t seed);
void learner_free(learner_t *l);
/* The store: a header (magic, version, D, seed) refused on any mismatch. */
int learner_load(const char *path, learner_t **out, char *err, size_t errlen);
int learner_save(const learner_t *l, const char *path, char *err, size_t errlen);

int32_t lex_find(const learner_t *l, const char *w);
int32_t lex_add(learner_t *l, const char *w);
bool lex_speakable(const learner_t *l, int32_t i);
size_t lex_speakable_count(const learner_t *l);
bool is_punct(const char *w);

/* Words of a text as the learner hears them: lower-cased runs of letters,
 * digits and apostrophes, and . , ? ! ; : as words of their own. */
size_t split_words(const char *text, char ***out);
void free_words(char **w, size_t n);

/* The context before position t of `said` (n words), with `topic` (the
 * person's words) bundled in. */
void learner_context(const learner_t *l, char **said, size_t t, char **topic, size_t ntopic,
                     hv_t *out);
/* The k nearest speakable words to ctx; returns how many. */
size_t learner_predict(const learner_t *l, const hv_t *ctx, cand_t *out, size_t k, bool allow_end);
/* Learn that `label` follows ctx; `pred` (or -1) is what the learner
 * itself ranked first, pushed away when it was wrong. */
void learner_update(learner_t *l, const hv_t *ctx, int32_t label, int32_t pred);
/* Hearing a text: its words counted and every next word learned from its
 * context (observational learning, the way a child hears its parents). */
void learner_hear(learner_t *l, char **words, size_t n, char **topic, size_t ntopic);
/* -log p of label under the learner's softmax over speakable words at
 * temperature tau (in units of sim). */
double learner_nll(const learner_t *l, const hv_t *ctx, int32_t label, double tau);

#endif
