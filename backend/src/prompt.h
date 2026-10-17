/* prompt.h: turn (session, fingerprint) into a prompt whose next token is
 * the answer, exactly as Intel Phi Jev's src/prompt.rs does (the C port).
 * A prompt is a list of segments, template text (special tokens parsed) or
 * user text (never parsed: the Energy Conversion Box). See prompt.md. */
#ifndef MJ_PROMPT_H
#define MJ_PROMPT_H

#include <stdbool.h>
#include <stddef.h>

#include "json.h"
#include "util.h"

typedef enum { T_CHATML, T_GEMMA, T_LLAMA3, T_RAW } template_t;
typedef enum { L_LETTERS, L_JEV } layout_t;
typedef enum { Q_NOUL, Q_CHOICE, Q_SCORE } qkind_t;

typedef struct {
    char *text;
    size_t len;
    bool user;
} seg_t;

typedef struct {
    seg_t *v;
    size_t n, cap;
} segs_t;

void segs_t_(segs_t *s, const char *text);  /* template text */
void segs_u(segs_t *s, const char *text);   /* user text */
void segs_un(segs_t *s, const char *text, size_t n);
segs_t segs_concat(const segs_t *a, const segs_t *b);
segs_t segs_clone(const segs_t *a);
void segs_free(segs_t *s);
char *segs_raw(const segs_t *s);

/* A parsed question. */
typedef struct {
    char *id;
    qkind_t kind;
    const json_t *q;            /* the question object, owned by the request */
    const json_t *instructions;
    const json_t *criteria;     /* choice: object; score: array; noul: object or NULL */
    size_t n;                   /* options: 2 for noul */
} question_t;

/* A rendered fingerprint: the suffix after the session, and its labels and
 * keys in rendered order. */
typedef struct {
    segs_t suffix;
    char **labels;
    char **keys;
    size_t n;
} rendered_t;

void rendered_free(rendered_t *r);

int template_parse(const char *s, template_t *out);
int layout_parse(const char *s, layout_t *out);

/* Parse `questions` (an object) into qs; an error message on failure. */
int parse_questions(const json_t *questions, question_t **qs, size_t *nq, char *err, size_t errlen);
void questions_free(question_t *qs, size_t nq);

/* render_state and text_of from protocol.rs. */
char *render_state(const json_t *state);
char *text_of(const json_t *v);

/* The shared prefix for a session (prefix_for). */
segs_t prompt_prefix(layout_t layout, template_t tpl, const char *session);
/* One fingerprint; order is a permutation of 0..n (NULL: identity). */
rendered_t prompt_render(layout_t layout, template_t tpl, const question_t *q, const size_t *order);

/* The label of option i of n: " A".." Z", past 26 " 001".." 255". */
char *prompt_label(size_t i, size_t n);

#define LETTERS 26
#define MAX_OPTIONS 255

#endif
