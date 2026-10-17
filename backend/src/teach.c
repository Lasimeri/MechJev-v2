/* teach.c: the decision engine as the learner's teacher (teach.h). */
#define _GNU_SOURCE
#include "teach.h"

#include <ctype.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "util.h"

#define LINE_TOKENS 16 /* words and marks in a line, at most */
#define GROUP 25       /* words in one Choice (the engine's comfortable size, as speak.c's widen) */
#define FINALISTS 12   /* group candidates put to the final Choice */
#define GROUP_MIN_P 0.05 /* a group passes on every word it gives at least this ("do" fell under 0.10) */
#define LINE_SENTENCES 2 /* a teacher's line: one sentence, or two (tell, then ask) */
#define NOT_LISTED "(not listed)"

static const char *TEACHER = "You are teaching English to a beginner, who learns to speak only from what you say "
                             "to it and how you judge its replies.";
static const char *MARKS[] = {".", "?", "!", ","};

/* ---- loading ---------------------------------------------------------- */

static char *trim(char *s)
{
    while (*s && isspace((unsigned char)*s))
        s++;
    char *e = s + strlen(s);
    while (e > s && isspace((unsigned char)e[-1]))
        *--e = 0;
    return s;
}

int teacher_load(teacher_t *t, const char *lessons_path, const char *words_path, char *err, size_t errlen)
{
    memset(t, 0, sizeof *t);
    char *text = read_file(lessons_path, NULL);
    if (!text) {
        snprintf(err, errlen, "%s: cannot read the lessons", lessons_path);
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
            snprintf(err, errlen, "%s: line %d: a lesson is NAME | GOAL | CHECK [| HELD-OUT GOAL [| SHOWN WORDS]]", lessons_path,
                     line_no);
            free(text);
            teacher_free(t);
            return -1;
        }
        t->lesson = xrealloc(t->lesson, (t->n + 1) * sizeof *t->lesson);
        lesson_t *ls = &t->lesson[t->n++];
        ls->name = xstrdup(field[0]);
        ls->goal = xstrdup(field[1]);
        ls->check = xstrdup(field[2]);
        ls->held_goal = nf > 3 && *field[3] ? xstrdup(field[3]) : NULL;
        ls->show = nf > 4 && *field[4] ? xstrdup(field[4]) : NULL;
    }
    free(text);
    if (!t->n) {
        snprintf(err, errlen, "%s: no lessons", lessons_path);
        return -1;
    }
    text = read_file(words_path, NULL);
    if (!text) {
        snprintf(err, errlen, "%s: cannot read the teacher's words", words_path);
        teacher_free(t);
        return -1;
    }
    sv = NULL;
    for (char *ln = strtok_r(text, "\n", &sv); ln; ln = strtok_r(NULL, "\n", &sv)) {
        char *hash = strchr(ln, '#');
        if (hash)
            *hash = 0;
        char *wv = NULL;
        for (char *w = strtok_r(ln, " \t\r", &wv); w; w = strtok_r(NULL, " \t\r", &wv)) {
            t->words = xrealloc(t->words, (t->nwords + 1) * sizeof *t->words);
            t->words[t->nwords++] = xstrdup(w);
        }
    }
    free(text);
    return 0;
}

void teacher_free(teacher_t *t)
{
    for (size_t i = 0; i < t->n; i++) {
        free(t->lesson[i].name);
        free(t->lesson[i].goal);
        free(t->lesson[i].check);
        free(t->lesson[i].held_goal);
        free(t->lesson[i].show);
    }
    free(t->lesson);
    for (size_t i = 0; i < t->nwords; i++)
        free(t->words[i]);
    free(t->words);
    memset(t, 0, sizeof *t);
}

int teacher_find(const teacher_t *t, const char *name)
{
    for (size_t i = 0; i < t->n; i++)
        if (!strcmp(t->lesson[i].name, name))
            return (int)i;
    return -1;
}

/* ---- choosing the lesson ---------------------------------------------- */

int teacher_choose(const teacher_t *t, const speak_cfg_t *cfg, const char *record, size_t *lesson, char *err,
                   size_t errlen)
{
    sb_t ins;
    sb_init(&ins);
    sb_printf(&ins, "%s The lessons, and what the learner has shown in each so far:\n%s\nWhich lesson should "
                    "you teach next? Choose the one it needs most now; a lesson that has not worked the same "
                    "way twice running can wait while you teach another.",
              TEACHER, record);
    char **keys = xmalloc(t->n * sizeof *keys);
    for (size_t i = 0; i < t->n; i++)
        keys[i] = t->lesson[i].name;
    json_t *qs = jobj();
    jobj_set(qs, "lesson", speak_choice_q(ins.s, keys, t->n));
    sb_free(&ins);
    free(keys);
    json_t *resp = NULL;
    json_t *ans = speak_ask(cfg, "A teacher plans an English lesson for a beginner.", qs, &resp, err, errlen);
    if (!ans)
        return -1;
    const json_t *c = jobj_get(jobj_get(ans, "lesson"), "choice");
    int i = c && c->t == J_STR ? teacher_find(t, c->s) : -1;
    json_free(resp);
    if (i < 0) {
        snprintf(err, errlen, "the engine chose no lesson");
        return -1;
    }
    *lesson = (size_t)i;
    return 0;
}

/* ---- composing a line ------------------------------------------------- */

static bool has_word(char **v, size_t n, const char *w)
{
    for (size_t i = 0; i < n; i++)
        if (!strcasecmp(v[i], w))
            return true;
    return false;
}

/* The teacher's words and the learner's, once each (the teacher's spelling
 * first: "MechJev", not "mechjev"); no marks, no start or end. */
static char **vocabulary(const teacher_t *t, const learner_t *l, size_t *n)
{
    char **v = xmalloc((t->nwords + l->n + 1) * sizeof *v);
    size_t k = 0;
    for (size_t i = 0; i < t->nwords; i++)
        if (!has_word(v, k, t->words[i]))
            v[k++] = t->words[i];
    for (size_t i = 0; i < l->n; i++) {
        const char *w = l->v[i].word;
        if (!strcmp(w, START) || !strcmp(w, END) || is_punct(w) || has_word(v, k, w))
            continue;
        v[k++] = l->v[i].word;
    }
    *n = k;
    return v;
}

/* What the engine is asked at each word. The groups: which of these words
 * comes next. The finalists: which goes on best, the line framed as
 * unfinished (asked "the best way to go on", a line that looked finished
 * won: "Hello how?", "MechJev?", 2026-10-17; the same myopia the learner's
 * own word choice had, speak.md). */
static char *instructions(const char *goal, char **said, size_t t, bool groups, bool vary)
{
    sb_t b;
    sb_init(&b);
    sb_printf(&b, "%s What this line of yours should do: %s Use short, simple words.%s ", TEACHER, goal,
              vary ? " Say it in a new way, not as in your lines so far." : "");
    char *so_far = t ? render_words(said, t) : NULL;
    if (!t)
        sb_puts(&b, "Your line is empty so far. ");
    else {
        sb_puts(&b, "Your line so far: ");
        json_quote(&b, so_far, strlen(so_far));
        sb_puts(&b, ". ");
    }
    free(so_far);
    if (groups)
        sb_puts(&b, "Which of these should come next in it? Choose " NOT_LISTED " if none of these should.");
    else
        sb_puts(&b, "It is unfinished: more words will follow. Which of these is the best start of clear, "
                    "correct English that does what the line should do?");
    return sb_take(&b);
}

/* Once a sentence of the line has ended: does it do all it should? (The
 * end is decided by this Noul, as the learner's is, never as one more
 * option among words.) */
static int line_does_goal(const speak_cfg_t *cfg, const char *state, const char *goal, char **said, size_t t,
                          double *p, char *err, size_t errlen)
{
    char *so_far = render_words(said, t);
    sb_t ins;
    sb_init(&ins);
    sb_printf(&ins, "%s Your line so far is ", TEACHER);
    json_quote(&ins, so_far, strlen(so_far));
    sb_printf(&ins, ". It is a complete, well-formed English line, and it does all that this line should do: %s",
              goal);
    free(so_far);
    json_t *q = jobj();
    jobj_set(q, "type", jstr("noul"));
    jobj_set(q, "instructions", jstr(ins.s));
    sb_free(&ins);
    json_t *qs = jobj();
    jobj_set(qs, "done", q);
    json_t *resp = NULL;
    json_t *ans = speak_ask(cfg, state, qs, &resp, err, errlen);
    if (!ans)
        return -1;
    const json_t *v = jobj_get(jobj_get(ans, "done"), "noul");
    *p = v && v->t == J_NUM ? v->f : 0;
    json_free(resp);
    return 0;
}

int teacher_compose(const teacher_t *t, const speak_cfg_t *cfg, const learner_t *l, const char *goal, const char *context,
                    char **line, double *goal_p, json_t **trace, char *err, size_t errlen)
{
    size_t nv;
    char **vocab = vocabulary(t, l, &nv);
    char *said[LINE_TOKENS + 1];
    size_t n = 0;
    json_t *tr = jarr();
    sb_t sst;
    sb_init(&sst);
    sb_puts(&sst, "A conversation in English between a teacher and a beginner it teaches.");
    if (context && *context)
        sb_printf(&sst, "\n%s", context);
    char *state = sb_take(&sst);
    int rc = 0;
    size_t *idx = xmalloc((nv + 4) * sizeof *idx);
    /* the keeper, for the teacher: the sentence end where the line did its
     * goal best (as the learner's replies, its lines ran on and decayed) */
    size_t best_n = 0, sentences = 0;
    double best_p = -1;
    while (n < LINE_TOKENS) {
        uint64_t seed = speak_clock_seed(), s = seed;
        bool after_word = n > 0 && !is_punct(said[n - 1]);
        /* the candidates: every word but the one before, the marks after a
         * word; shuffled by the clock seed, so no group order is favoured
         * (and one option rotation is enough for the groups) */
        size_t np = 0;
        for (size_t i = 0; i < nv; i++)
            if (!n || strcasecmp(vocab[i], said[n - 1]))
                idx[np++] = i;
        for (size_t m = 0; after_word && m < sizeof MARKS / sizeof *MARKS; m++)
            idx[np++] = nv + m;
        for (size_t i = np; i > 1; i--) {
            size_t j = (size_t)(splitmix64(&s) % i), tmp = idx[i - 1];
            idx[i - 1] = idx[j];
            idx[j] = tmp;
        }
        char *gi = instructions(goal, said, n, true, context && *context);
        json_t *qs = jobj();
        size_t ng = 0;
        for (size_t at = 0; at < np; at += GROUP, ng++) {
            size_t m = np - at < GROUP ? np - at : GROUP;
            char **keys = xmalloc((m + 1) * sizeof *keys);
            for (size_t i = 0; i < m; i++)
                keys[i] = idx[at + i] < nv ? vocab[idx[at + i]] : (char *)MARKS[idx[at + i] - nv];
            keys[m] = NOT_LISTED;
            char id[24];
            snprintf(id, sizeof id, "g%zu", ng);
            jobj_set(qs, id, speak_choice_q(gi, keys, m + 1));
            free(keys);
        }
        free(gi);
        json_t *resp = NULL;
        json_t *ans = speak_ask_rotations(cfg, state, qs, 1, &resp, err, errlen);
        if (!ans) {
            rc = -1;
            break;
        }
        /* from each group, every word it gives GROUP_MIN_P or more, best of
         * all first (only each group's top lost the right word: after
         * "Hello how are" the finalists were "is", "do" and "?", no "you") */
        char *fin[FINALISTS + 1];
        double fs[FINALISTS + 1];
        size_t nf = 0;
        for (size_t g = 0; g < ng; g++) {
            char id[24];
            snprintf(id, sizeof id, "g%zu", g);
            const json_t *pr = jobj_get(jobj_get(ans, id), "probabilities");
            for (size_t k = 0; pr && pr->t == J_OBJ && k < pr->n; k++) {
                const json_t *v = pr->items[k];
                double sc = v && v->t == J_NUM ? v->f : 0;
                if (sc < GROUP_MIN_P || !strcmp(pr->keys[k], NOT_LISTED))
                    continue;
                size_t at = nf;
                while (at > 0 && fs[at - 1] < sc)
                    at--;
                if (at >= FINALISTS)
                    continue;
                memmove(&fin[at + 1], &fin[at], (nf - at) * sizeof *fin);
                memmove(&fs[at + 1], &fs[at], (nf - at) * sizeof *fs);
                fin[at] = xstrdup(pr->keys[k]);
                fs[at] = sc;
                if (nf < FINALISTS)
                    nf++;
                else
                    free(fin[FINALISTS]);
            }
        }
        json_free(resp);
        if (!nf)
            break; /* nothing may come next: the mark is chosen below */
        /* the final Choice: the line as it would go on with each finalist */
        char **keys = xmalloc(nf * sizeof *keys);
        for (size_t i = 0; i < nf; i++) {
            said[n] = fin[i];
            keys[i] = render_words(said, n + 1);
        }
        char *fi = instructions(goal, said, n, false, context && *context);
        json_t *q2 = jobj();
        jobj_set(q2, "next", speak_choice_q(fi, keys, nf));
        free(fi);
        json_t *r2 = NULL;
        json_t *a2 = speak_ask(cfg, state, q2, &r2, err, errlen);
        const json_t *c2 = a2 ? jobj_get(jobj_get(a2, "next"), "choice") : NULL;
        int pick = -1;
        for (size_t i = 0; c2 && c2->t == J_STR && i < nf; i++)
            if (!strcmp(keys[i], c2->s))
                pick = (int)i;
        json_t *step = jobj();
        jobj_set(step, "t", ju64(n));
        jobj_set(step, "seed", ju64(seed));
        json_t *opts = jobj();
        for (size_t i = 0; i < nf; i++)
            jobj_set(opts, keys[i], jf64(round(speak_prob(jobj_get(a2, "next"), keys[i]) * 1e4) / 1e4));
        jobj_set(step, "options", opts);
        for (size_t i = 0; i < nf; i++)
            free(keys[i]);
        free(keys);
        json_free(r2);
        if (!a2 || pick < 0) {
            for (size_t i = 0; i < nf; i++)
                free(fin[i]);
            json_free(step);
            if (a2)
                snprintf(err, errlen, "the engine chose nothing for the line");
            rc = -1;
            break;
        }
        jobj_set(step, "pick", jstr(fin[pick]));
        said[n++] = fin[pick];
        for (size_t i = 0; i < nf; i++)
            if ((int)i != pick)
                free(fin[i]);
        /* a sentence has ended: does the line do all it should? */
        bool done = false;
        if (speak_sentence_end(said[n - 1])) {
            double p = 0;
            if (line_does_goal(cfg, state, goal, said, n, &p, err, errlen)) {
                json_free(step);
                rc = -1;
                break;
            }
            jobj_set(step, "does_its_goal", jf64(round(p * 1e4) / 1e4));
            if (p > best_p) {
                best_p = p;
                best_n = n;
            }
            sentences++;
            done = p >= 0.5 || sentences >= LINE_SENTENCES;
        }
        jarr_push(tr, step);
        if (done)
            break;
    }
    free(idx);
    /* cut back to the best sentence end (the words after it go) */
    if (!rc && best_n && best_n < n) {
        for (size_t i = best_n; i < n; i++)
            free(said[i]);
        n = best_n;
    }
    if (!rc && n && !speak_sentence_end(said[n - 1])) {
        /* cut at its length or with nothing to go on: the engine ends it
         * with the mark that fits (a comma left at the end goes first) */
        if (is_punct(said[n - 1]))
            free(said[--n]);
        static const char *ends[] = {".", "?", "!"};
        char *keys[3];
        for (int m = 0; m < 3; m++) {
            said[n] = (char *)ends[m];
            keys[m] = render_words(said, n + 1);
        }
        char *fi = instructions(goal, said, n, false, context && *context);
        json_t *q = jobj();
        jobj_set(q, "end", speak_choice_q(fi, keys, 3));
        free(fi);
        json_t *r = NULL;
        json_t *a = speak_ask(cfg, state, q, &r, err, errlen);
        const json_t *c = a ? jobj_get(jobj_get(a, "end"), "choice") : NULL;
        int pick = 0;
        for (int m = 0; c && c->t == J_STR && m < 3; m++)
            if (!strcmp(keys[m], c->s))
                pick = m;
        for (int m = 0; m < 3; m++)
            free(keys[m]);
        json_free(r);
        said[n++] = xstrdup(ends[pick]);
    }
    if (!rc && !n) {
        snprintf(err, errlen, "the engine composed no line");
        rc = -1;
    }
    if (!rc)
        *line = render_words(said, n);
    for (size_t i = 0; i < n; i++)
        free(said[i]);
    free(vocab);
    free(state);
    if (rc) {
        json_free(tr);
        return -1;
    }
    *goal_p = best_p < 0 ? 0 : best_p;
    *trace = tr;
    return 0;
}

/* ---- judging the learner's reply -------------------------------------- */

int teacher_check(const speak_cfg_t *cfg, const lesson_t *ls, const char *line, const char *reply, double *p,
                  char *err, size_t errlen)
{
    sb_t st;
    sb_init(&st);
    sb_printf(&st, "A conversation in English.\nPerson: %s\n\nThe newest reply, the one to judge: ", line);
    json_quote(&st, reply, strlen(reply));
    json_t *q = jobj();
    jobj_set(q, "type", jstr("noul"));
    sb_t ins;
    sb_init(&ins);
    sb_printf(&ins, "About the newest reply only: %s", ls->check);
    jobj_set(q, "instructions", jstr(ins.s));
    sb_free(&ins);
    json_t *qs = jobj();
    jobj_set(qs, "check", q);
    json_t *resp = NULL;
    json_t *ans = speak_ask(cfg, st.s, qs, &resp, err, errlen);
    sb_free(&st);
    if (!ans)
        return -1;
    const json_t *v = jobj_get(jobj_get(ans, "check"), "noul");
    *p = v && v->t == J_NUM ? v->f : 0;
    json_free(resp);
    return 0;
}
