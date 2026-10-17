/* speak.h: one turn of conversation. The decision engine (xks, over HTTP)
 * sets the probability of every variable (variables.txt: typed questions
 * about the conversation, and about the reply so far), the learner's
 * circuit proposes words from those variables, and the engine chooses
 * among the words as English continuations of the reply. Each choice, with
 * the engine's distribution over the words shown, is an example the
 * circuit learns from, once the finished reply has passed every
 * requirement (restrictions.txt). The shape of a word step is jev-llm's
 * rerank; the variables and requirements follow Aster's judge. See
 * speak.md. */
#ifndef MJ_SPEAK_H
#define MJ_SPEAK_H

#include <stdbool.h>
#include <stddef.h>

#include "json.h"
#include "learner.h"

/* How a word is drawn from the engine's distribution over the shown
 * options, seeded per word from the clock. The defaults are the user's
 * sampling settings (memory model-sampling-settings, 2026-10-09): temp
 * 0.5, top-k 75, top-p 0.95, min-p 0.05, repeat penalty 1.2. temp 0 is the
 * engine's own choice, always. */
typedef struct {
    double temp, top_p, min_p, repeat_penalty;
    int top_k;
} sampling_t;

/* A variable: a Noul (one slot, P true) or a Choice (a slot per option,
 * the distribution), about the conversation (`turn`) or about the reply so
 * far (`step`). */
typedef struct {
    bool step;
    bool choice;
    char *text;
    char **opts;
    size_t nopts;
    int slot; /* the first slot */
} var_t;

typedef struct {
    const char *engine; /* host:port of xks, 127.0.0.1:8090 */
    size_t propose;     /* words the circuit proposes per step (8) */
    size_t window;      /* words of the reply shown in each option (8) */
    size_t group;       /* words per question when widening (25) */
    size_t max_words;   /* hard cap (60) */
    double sure_p;      /* below this probability of its first word the circuit is unsure: widen */
    double train_ms;    /* rehearsal budget after a turn */
    char **checks;      /* requirements: a Noul each about the finished reply */
    size_t nchecks;
    var_t *vars;
    size_t nvars;
    bool learn;
    sampling_t sampling;
    /* Called with the reply so far after every word (a terminal shows it live). */
    void (*on_word)(void *ud, const char *reply_so_far);
    void *on_word_ud;
} speak_cfg_t;

typedef struct {
    char **person, **reply; /* earlier turns, oldest first */
    size_t n;
} history_t;

void speak_cfg_default(speak_cfg_t *c);
/* variables.txt: `turn noul TEXT`, `step noul TEXT`, `turn choice TEXT |
 * opt | opt ...`; slots taken in the learner, append-only. */
int vars_load(speak_cfg_t *c, learner_t *l, const char *path, char *err, size_t errlen);
void vars_free(speak_cfg_t *c);
void history_add(history_t *h, const char *person, const char *reply);
void history_free(history_t *h);

/* English text from words: a space between words, none before
 * punctuation, capitals at a sentence start and for I. */
char *render_words(char **w, size_t n);

/* One turn: the reply in *reply, the turn's record (a row of the curve and
 * the per-word trace) in *record. 0 on success. */
int speak_turn(learner_t *l, const speak_cfg_t *cfg, history_t *h, const char *person,
               char **reply, json_t **record, char *err, size_t errlen);

#endif
