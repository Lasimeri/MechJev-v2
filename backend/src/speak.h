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
    bool ties_only; /* the most likely word, the seed breaking exact ties (the default) */
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
    int slot;   /* the first slot */
    bool local; /* computed here, not asked of the engine (the novelty: speak.c) */
} var_t;

typedef struct {
    const char *engine; /* host:port of xks, 127.0.0.1:8090 */
    size_t propose;     /* words the circuit proposes per step (8) */
    size_t window;      /* words of the reply shown in each option (8) */
    size_t group;       /* words per question when widening (25) */
    size_t max_words;   /* hard cap (60) */
    double train_ms;    /* rehearsal budget after a turn */
    char **checks;      /* requirements: a Noul each about the finished reply */
    size_t nchecks;
    var_t *vars;
    size_t nvars;
    bool learn;
    bool alone; /* the circuit speaks with no engine choice (it still perceives and judges) */
    bool probe; /* a measurement: alone, and the learner hears, learns and counts nothing */
    size_t lookahead; /* the rerank's best scored for coherence; 0: the rerank's top is said */
    double keep_drop; /* the coherence keeper: end a reply this far below its best sentence end; < 0: off */
    const char *show; /* words a taught turn offers besides the person's (the teacher's demonstration: the answer's words); NULL: none */
    bool parent; /* the person speaking is the parent (no other speaker named): heard PARENT_HEARD times over */
    bool truth;  /* judge the truth: no fact it was not told, no experience it could not have had (speak.md) */
    bool grammar; /* judge the grammar: a whole sentence, no word missing, or a bare short reply (speak.md) */
    bool phrases; /* the lookahead judges two-word phrases, not single words (speak.md; MJ2_PHRASES) */
    bool persona; /* the conversation says who MechJev is (PERSONA; MJ2_PERSONA) */
    const char *extra_check; /* the teacher's stage check for this turn, judged as one more requirement; NULL: none */
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

/* Shared with the teacher (teach.c): one request to the engine (the
 * answers object, owned by *resp), a Choice question, an option's
 * probability in an answer, whether a word ends a sentence, a clock seed,
 * and the conversation as the engine's state. */
json_t *speak_ask(const speak_cfg_t *cfg, const char *state, json_t *questions, json_t **resp, char *err,
                  size_t errlen);
json_t *speak_choice_q(const char *instructions, char **keys, size_t n);
json_t *speak_ask_rotations(const speak_cfg_t *cfg, const char *state, json_t *questions, size_t rotations,
                            json_t **resp, char *err, size_t errlen);
double speak_prob(const json_t *answer, const char *key);
bool speak_sentence_end(const char *w);
uint64_t speak_clock_seed(void);
char *speak_conversation(const history_t *h, const char *person);

/* One turn: the reply in *reply, the turn's record (a row of the curve and
 * the per-word trace) in *record. 0 on success. */
/* The fast memory made again from the record (turns.jsonl), in order: each
 * taught turn that passed every requirement, each step's context as it was
 * (the turn's variables, the reply variables the step before answered, the
 * position, the person's words) and the word decided. Before a turn is
 * learned the memory says what it would have chosen: *report holds that
 * prequential agreement, by tens of turns, and the margins its weight
 * scales. */
int speak_memory_rebuild(learner_t *l, const speak_cfg_t *cfg, const char *turns_path, json_t **report,
                         char *err, size_t errlen);
/* The n-gram memory made again from turns.jsonl (ngram.h): each passed
 * taught turn's words, up to the keeper's cut, with the meaning each was
 * chosen under (as speak_memory_rebuild rebuilds the slots). At every start:
 * nothing of it is stored. */
int speak_ngram_rebuild(learner_t *l, const speak_cfg_t *cfg, const char *turns_path, size_t *turns,
                        size_t *steps, char *err, size_t errlen);
/* Every passed taught turn of turns.jsonl judged again by today's
 * requirements (the user's choice, 2026-10-17: the caveman replies had
 * passed the old grammar). A reply that fails now is marked so in its record
 * ("rejudged") and in the replay (each run of examples teaching exactly
 * it), so it is not rehearsed as right again. turns.jsonl is rewritten. */
int speak_rejudge(learner_t *l, const speak_cfg_t *cfg, const char *turns_path, size_t *judged, size_t *now_failed,
                  size_t *examples, char *err, size_t errlen);

int speak_turn(learner_t *l, const speak_cfg_t *cfg, history_t *h, const char *person,
               char **reply, json_t **record, char *err, size_t errlen);

#endif
