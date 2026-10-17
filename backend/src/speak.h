/* speak.h: one turn of conversation, every word a decision. The learner
 * proposes the next words; the decision engine (xks, /v1/systemone over
 * HTTP) chooses among them as English continuations of the reply, or says
 * none fits, and then a wider choice over the learner's lexicon is put to
 * it. Each choice is a label the learner learns from. The shape is
 * jev-llm's (a choice over rendered continuations, END only after a
 * sentence ends) and talktojev's (how much to say decided first, checks
 * after). See speak.md. */
#ifndef MJ_SPEAK_H
#define MJ_SPEAK_H

#include <stdbool.h>
#include <stddef.h>

#include "json.h"
#include "learner.h"

typedef struct {
    const char *engine;     /* host:port of xks, 127.0.0.1:8090 */
    size_t propose;         /* candidates the learner shows per word (8) */
    size_t window;          /* words of the reply shown in each option (8) */
    size_t group;           /* words per question when widening (25) */
    size_t max_words;       /* hard cap (60) */
    char **checks;          /* restrictions: Noul statements about the reply */
    size_t nchecks;
    bool learn;             /* update the learner from the engine's picks */
} speak_cfg_t;

typedef struct {
    char **person, **reply; /* earlier turns, oldest first */
    size_t n;
} history_t;

void speak_cfg_default(speak_cfg_t *c);
void history_add(history_t *h, const char *person, const char *reply);
void history_free(history_t *h);

/* English text from the learner's words: a space between words, none
 * before punctuation, capitals at a sentence start and for I. */
char *render_words(char **w, size_t n);

/* One turn: hear the person, plan how much to say, speak word by word.
 * The reply in *reply, and a record of the turn (the curve's row, with a
 * per-word trace) in *record. 0 on success. */
int speak_turn(learner_t *l, const speak_cfg_t *cfg, history_t *h, const char *person,
               char **reply, json_t **record, char *err, size_t errlen);

#endif
