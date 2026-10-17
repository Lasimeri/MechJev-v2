/* teach.c: the teacher (teach.h): Qwen3.8-35B, through the engine, talking
 * to MechJev from outside. */
#define _GNU_SOURCE
#include "teach.h"

#include <ctype.h>
#include <time.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "http.h"
#include "util.h"

#define TIMEOUT_MS 900000

/* Who the teacher is, for its own lines (the system text of /v1/generate). */
static const char *WRITER =
    "You are a patient English teacher talking with MechJev, a beginner who is learning to speak English "
    "and knows only a few words. You talk with it as a kind person would: short, simple, natural English. "
    "Write only your next line to MechJev: one or two short sentences, nothing else, no quotation marks, "
    "no notes, no translation.";

/* ---- the lessons -------------------------------------------------------- */

static char *trim(char *s)
{
    while (*s && isspace((unsigned char)*s))
        s++;
    char *e = s + strlen(s);
    while (e > s && isspace((unsigned char)e[-1]))
        *--e = 0;
    return s;
}

int lessons_load(lessons_t *ls, const char *path, char *err, size_t errlen)
{
    memset(ls, 0, sizeof *ls);
    char *text = read_file(path, NULL);
    if (!text) {
        snprintf(err, errlen, "%s: cannot read the lessons", path);
        return -1;
    }
    int line_no = 0;
    char *sv = NULL;
    for (char *ln = strtok_r(text, "\n", &sv); ln; ln = strtok_r(NULL, "\n", &sv)) {
        line_no++;
        char *s = trim(ln);
        if (!*s || *s == '#')
            continue;
        char *field[5] = {0};
        int nf = 0;
        for (char *p = s; nf < 5;) {
            char *bar = strchr(p, '|');
            if (bar)
                *bar = 0;
            field[nf++] = trim(p);
            if (!bar)
                break;
            p = bar + 1;
        }
        if (nf < 3 || !*field[0] || !*field[1] || !*field[2]) {
            snprintf(err, errlen, "%s: line %d: a lesson is NAME | GOAL | CHECK [| HELD-OUT GOAL [| SHOWN WORDS]]",
                     path, line_no);
            free(text);
            lessons_free(ls);
            return -1;
        }
        ls->v = xrealloc(ls->v, (ls->n + 1) * sizeof *ls->v);
        lesson_t *l = &ls->v[ls->n++];
        l->name = xstrdup(field[0]);
        l->goal = xstrdup(field[1]);
        l->check = xstrdup(field[2]);
        l->held_goal = nf > 3 && *field[3] ? xstrdup(field[3]) : NULL;
        l->show = nf > 4 && *field[4] ? xstrdup(field[4]) : NULL;
    }
    free(text);
    if (!ls->n) {
        snprintf(err, errlen, "%s: no lessons", path);
        return -1;
    }
    return 0;
}

void lessons_free(lessons_t *ls)
{
    for (size_t i = 0; i < ls->n; i++) {
        free(ls->v[i].name);
        free(ls->v[i].goal);
        free(ls->v[i].check);
        free(ls->v[i].held_goal);
        free(ls->v[i].show);
    }
    free(ls->v);
    memset(ls, 0, sizeof *ls);
}

int lessons_find(const lessons_t *ls, const char *name)
{
    for (size_t i = 0; i < ls->n; i++)
        if (!strcmp(ls->v[i].name, name))
            return (int)i;
    return -1;
}

/* ---- over HTTP ---------------------------------------------------------- */

/* POST body to host:port path; the parsed response (owned by the caller). */
static json_t *post(const char *where, const char *path, json_t *body, char *err, size_t errlen)
{
    char *s = json_dump(body);
    json_free(body);
    char *out = NULL;
    size_t on = 0;
    int st = http_call(where, "POST", path, s, strlen(s), &out, &on, TIMEOUT_MS);
    free(s);
    if (st < 0) {
        snprintf(err, errlen, "%s does not answer (%s)", where, path);
        free(out);
        return NULL;
    }
    char perr[256];
    json_t *v = json_parse(out ? out : "", on, perr, sizeof perr);
    free(out);
    if (st != 200 || !v) {
        const json_t *m = v ? jobj_get(v, "message") : NULL;
        snprintf(err, errlen, "%s%s said %d: %s", where, path, st, m && m->t == J_STR ? m->s : v ? "?" : perr);
        json_free(v);
        return NULL;
    }
    return v;
}

/* One question to the engine about `state`; its answer (owned by *resp). */
static const json_t *decide(const teacher_t *t, const char *state, const char *id, json_t *q, json_t **resp,
                            char *err, size_t errlen)
{
    json_t *req = jobj(), *qs = jobj();
    jobj_set(req, "state", jstr(state));
    jobj_set(qs, id, q);
    jobj_set(req, "questions", qs);
    *resp = post(t->engine, "/v1/systemone", req, err, errlen);
    return *resp ? jobj_get(jobj_get(*resp, "answers"), id) : NULL;
}

/* ---- the teacher -------------------------------------------------------- */

static uint32_t clock_seed32(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    uint64_t s = (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
    return (uint32_t)(splitmix64(&s) >> 32);
}

int teacher_write(const teacher_t *t, const char *goal, const char *context, char **line, char *err,
                  size_t errlen)
{
    sb_t u;
    sb_init(&u);
    sb_printf(&u, "What your next line should do: %s", goal);
    if (context && *context)
        sb_printf(&u, "\n\n%s\n\nSay it in your own words, not as you have said it before.", context);
    json_t *req = jobj();
    jobj_set(req, "system", jstr(WRITER));
    jobj_set(req, "user", jstr(u.s));
    jobj_set(req, "max_tokens", ju64(48));
    jobj_set(req, "seed", ju64(clock_seed32()));
    sb_free(&u);
    json_t *r = post(t->engine, "/v1/generate", req, err, errlen);
    if (!r)
        return -1;
    const json_t *txt = jobj_get(r, "text");
    /* one line: the first, without quotation marks around it */
    char *s = xstrdup(txt && txt->t == J_STR ? txt->s : "");
    json_free(r);
    char *nl = strchr(s, '\n');
    if (nl)
        *nl = 0;
    char *b = trim(s);
    size_t n = strlen(b);
    if (n >= 2 && (b[0] == '"' || b[0] == '\'') && b[n - 1] == b[0]) {
        b[n - 1] = 0;
        b++;
    }
    if (!*b) {
        free(s);
        snprintf(err, errlen, "the teacher wrote nothing");
        return -1;
    }
    *line = xstrdup(b);
    free(s);
    return 0;
}

int teacher_choose(const teacher_t *t, const lessons_t *ls, const char *record, size_t *lesson, char *err,
                   size_t errlen)
{
    sb_t ins;
    sb_init(&ins);
    sb_printf(&ins, "You teach English to MechJev, a beginner. The lessons, and what it has shown in each "
                    "so far:\n%s\nWhich lesson should you teach next? Choose the one it needs most now; a lesson "
                    "that has not worked the same way twice running can wait while you teach another.",
              record);
    json_t *q = jobj(), *c = jobj();
    jobj_set(q, "type", jstr("choice"));
    jobj_set(q, "instructions", jstr(ins.s));
    sb_free(&ins);
    for (size_t i = 0; i < ls->n; i++)
        jobj_set(c, ls->v[i].name, jnull());
    jobj_set(q, "criteria", c);
    json_t *resp = NULL;
    const json_t *a = decide(t, "A teacher plans an English lesson for a beginner.", "lesson", q, &resp, err, errlen);
    const json_t *ch = a ? jobj_get(a, "choice") : NULL;
    int i = ch && ch->t == J_STR ? lessons_find(ls, ch->s) : -1;
    json_free(resp);
    if (i < 0) {
        if (a)
            snprintf(err, errlen, "the engine chose no lesson");
        return -1;
    }
    *lesson = (size_t)i;
    return 0;
}

int teacher_check(const teacher_t *t, const lesson_t *l, const char *line, const char *reply, double *p,
                  char *err, size_t errlen)
{
    sb_t st;
    sb_init(&st);
    sb_printf(&st, "A conversation in English.\nPerson: %s\n\nThe newest reply, the one to judge: ", line);
    json_quote(&st, reply, strlen(reply));
    sb_t ins;
    sb_init(&ins);
    sb_printf(&ins, "About the newest reply only: %s", l->check);
    json_t *q = jobj();
    jobj_set(q, "type", jstr("noul"));
    jobj_set(q, "instructions", jstr(ins.s));
    sb_free(&ins);
    json_t *resp = NULL;
    const json_t *a = decide(t, st.s, "check", q, &resp, err, errlen);
    sb_free(&st);
    if (!a) {
        json_free(resp);
        return -1;
    }
    const json_t *v = jobj_get(a, "noul");
    *p = v && v->t == J_NUM ? v->f : 0;
    json_free(resp);
    return 0;
}

int teacher_try(const teacher_t *t, const char *line, json_t **rec, char *err, size_t errlen)
{
    json_t *req = jobj();
    jobj_set(req, "message", jstr(line));
    *rec = post(t->learner, "/v1/probe", req, err, errlen);
    return *rec ? 0 : -1;
}

int teacher_demonstrate(const teacher_t *t, const char *line, const char *show, json_t **rec, char *err,
                        size_t errlen)
{
    json_t *req = jobj();
    jobj_set(req, "message", jstr(line));
    if (show && *show)
        jobj_set(req, "show", jstr(show));
    *rec = post(t->learner, "/v1/talk", req, err, errlen);
    return *rec ? 0 : -1;
}

bool teacher_same_text(const char *a, const char *b)
{
    for (;;) {
        while (*a && !isalnum((unsigned char)*a))
            a++;
        while (*b && !isalnum((unsigned char)*b))
            b++;
        if (!*a || !*b)
            return !*a && !*b;
        if (tolower((unsigned char)*a++) != tolower((unsigned char)*b++))
            return false;
    }
}
