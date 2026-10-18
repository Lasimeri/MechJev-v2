/* teach.h: the teacher. Qwen3.8-35B, through the decision engine (xks),
 * talks to MechJev from outside, as a person would: the user's ask
 * (2026-10-17), "MechJev shouldn't question/teach itself, Qwen3.8-35B
 * should do that for MechJev". It writes its own lines in plain English
 * (POST /v1/generate) and judges MechJev's replies (Nouls); teacher_main.c
 * climbs its ladder of stages (lessons.txt). MechJev answers in the TUI a
 * person watches, the teacher typing there (teacher_say_in_tui), or
 * through its own server (learner serve): a check alone is its probe
 * (nothing heard, learned or counted); a taught turn is a turn in its
 * conversation, where the engine chooses each of MechJev's words among
 * MechJev's own. Qwen never writes MechJev's replies, and MechJev never
 * learns from a reply Qwen wrote. See teach.md. */
#ifndef MJ_TEACH_H
#define MJ_TEACH_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "json.h"

typedef struct {
    char *name;
    char *goal;      /* what the teacher's line should do */
    char *check;     /* a statement about MechJev's reply, judged as a Noul */
    char *held_goal; /* the goal of the lines that test mastery (NULL: the goal) */
    char *show;      /* words a taught turn may offer besides MechJev's and the line's (NULL: none) */
} lesson_t;

typedef struct {
    lesson_t *v;
    size_t n;
} lessons_t;

/* lessons.txt: NAME | GOAL | CHECK [| HELD-OUT GOAL [| SHOWN WORDS]]. */
int lessons_load(lessons_t *ls, const char *path, char *err, size_t errlen);
void lessons_free(lessons_t *ls);
int lessons_find(const lessons_t *ls, const char *name);

typedef struct {
    const char *engine;  /* xks, host:port */
    const char *learner; /* learner serve, host:port */
    const char *pane;    /* the TUI's tmux pane to type into (NULL: the API, no TUI) */
    const char *turns;   /* MechJev's turns.jsonl, where a typed turn lands */
    const char *speaker; /* how its lines are labelled in the TUI ("Qwen") */
} teacher_t;

/* Qwen writes the teacher's next line toward `goal`; `context` (may be
 * NULL) is what it has said lately and how MechJev answered, so it does not
 * loop. *line is one or two short sentences of plain English. */
int teacher_write(const teacher_t *t, const char *goal, const char *context, char **line, char *err,
                  size_t errlen);

/* The probability that the lesson's check holds of `reply` to `line`. */
int teacher_check(const teacher_t *t, const lesson_t *l, const char *line, const char *reply, double *p,
                  char *err, size_t errlen);

/* MechJev alone on `line`: its probe (from an empty conversation; nothing
 * heard, learned or counted). *rec is its turn record. */
int teacher_try(const teacher_t *t, const char *line, json_t **rec, char *err, size_t errlen);

/* A demonstration: a taught turn on `line` in MechJev's conversation (the
 * engine chooses MechJev's words among its own, the line's and `show`).
 * *rec is the turn record. */
int teacher_demonstrate(const teacher_t *t, const char *line, const char *show, json_t **rec, char *err,
                        size_t errlen);

/* MechJev answering `line` in the TUI a person watches, the teacher typing
 * it there as a person would (as tools/tui_say.sh does): the next turn's
 * settings first (POST /v1/next: alone, `show`, the speaker), the TUI put
 * on its talk screen and in the mode the turn needs (F2: alone or taught,
 * seen on screen), the line typed and Enter, then the turn read from
 * turns.jsonl. Under the driver lock ($XDG_RUNTIME_DIR/mjev2-driver.lock):
 * no two drivers type at once. *rec is the turn record. */
int teacher_say_in_tui(const teacher_t *t, const char *line, bool alone, const char *show, json_t **rec, char *err,
                       size_t errlen);

/* Two lines the same but for marks, spaces and case. */
bool teacher_same_text(const char *a, const char *b);

#endif
