/* teach.h: the decision engine as the learner's teacher (the user's ask,
 * 2026-10-17: "not only decide, but also teaching this architecture"). It
 * still only decides: it picks the next lesson by Choice, composes its own
 * line to the learner a word at a time by Choice (the jev-llm way; the
 * user chose this over a bank of written lines), and judges the learner's
 * reply by a Noul. The learner's replies stay the learner's: in a
 * demonstration the engine chooses among the words the learner offers, as
 * in any taught turn. See teach.md. */
#ifndef MJ_TEACH_H
#define MJ_TEACH_H

#include <stddef.h>

#include "json.h"
#include "learner.h"
#include "speak.h"

typedef struct {
    char *name;
    char *goal;      /* what the engine's line should do */
    char *check;     /* a statement about the learner's reply, judged as a Noul */
    char *held_goal; /* the goal of the line that tests mastery (NULL: the goal) */
    char *show;      /* words a demonstration may offer besides the learner's and the line's (NULL: none) */
} lesson_t;

typedef struct {
    lesson_t *lesson;
    size_t n;
    char **words; /* the teacher's own words (teacher_words.txt) */
    size_t nwords;
} teacher_t;

/* lessons.txt (NAME | GOAL | CHECK [| HELD-OUT GOAL]) and the teacher's
 * words (whitespace-separated; # starts a comment). */
int teacher_load(teacher_t *t, const char *lessons_path, const char *words_path, char *err, size_t errlen);
void teacher_free(teacher_t *t);
int teacher_find(const teacher_t *t, const char *name);

/* The engine's pick of the next lesson; `record` is what the learner has
 * shown so far, a line a lesson. */
int teacher_choose(const teacher_t *t, const speak_cfg_t *cfg, const char *record, size_t *lesson, char *err,
                   size_t errlen);

/* The engine composes a line toward `goal`, a word at a time, from the
 * teacher's words and the words the learner has heard; `context` (may be
 * NULL) is what the teacher has said and seen lately, in the engine's state,
 * so it does not loop on one line (the user's ask). *line is rendered
 * English; *goal_p the Noul that the line does its goal, at the sentence end
 * kept (0 when none was asked); *trace (owned by the caller) holds every
 * step's options. */
int teacher_compose(const teacher_t *t, const speak_cfg_t *cfg, const learner_t *l, const char *goal, const char *context,
                    char **line, double *goal_p, json_t **trace, char *err, size_t errlen);

/* The probability that the lesson's check holds of `reply` to `line`. */
int teacher_check(const speak_cfg_t *cfg, const lesson_t *ls, const char *line, const char *reply, double *p,
                  char *err, size_t errlen);

#endif
