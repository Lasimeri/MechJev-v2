/* teacher: Qwen3.8-35B teaching MechJev from outside (teach.h, teach.md),
 * a client of the decision engine (xks) and of MechJev's server (learner
 * serve), as a person at the TUI is.
 *
 *   teacher lines [LESSON...]  a line Qwen writes for each lesson (a look)
 *   teacher freeze [K]         K test lines a lesson, kept for measuring
 *   teacher measure [R]        MechJev alone on every test line, R times
 *   teacher teach [N]          N turns on the ladder (lessons.txt in order)
 *   teacher status             where MechJev is on the ladder
 *   teacher stage NAME         move the ladder to a stage
 *
 * Its rows go to lessons.jsonl in the learner's data directory. */
#define _GNU_SOURCE
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>

#include "conf.h"
#include "activity.h"
#include "json.h"
#include "teach.h"
#include "util.h"

#define RECENT_LINES 8   /* the teacher's own lines it is shown, so it does not loop */
#define REWRITES 4       /* a line that is a test line or a repeat is written again */
#define NEAR 0.75        /* a line sharing this share of its words with one said is a near-repeat (teach.h) */
#define LINE_FIT_MIN 0.8 /* a line fits its stage from here (teacher_line_fits: fitting .93-.98, drifted .54-.68) */

typedef struct {
    teacher_t t;
    lessons_t ls;
    char *dir, *lessons_log, *tests;
    char *name; /* the learner taught, in the harness, when it is not the harness's own (activity.h) */
} teach_ctx_t;

static char *join(const char *a, const char *b)
{
    sb_t s;
    sb_init(&s);
    sb_printf(&s, "%s/%s", a, b);
    return sb_take(&s);
}

static char *data_dir(void)
{
    const char *d = getenv("MJ2_DATA");
    if (d && *d)
        return expand_home(d);
    const char *x = getenv("XDG_DATA_HOME");
    sb_t b;
    sb_init(&b);
    if (x && *x)
        sb_printf(&b, "%s/mechjev-v2", x);
    else
        sb_printf(&b, "%s/.local/share/mechjev-v2", getenv("HOME") ? getenv("HOME") : ".");
    return sb_take(&b);
}

static void stamp(char *out, size_t n)
{
    time_t now = time(NULL);
    strftime(out, n, "%Y-%m-%dT%H:%M:%S", localtime(&now));
}

static void append(const char *path, const json_t *row)
{
    char *s = json_dump(row);
    FILE *f = fopen(path, "a");
    if (f) {
        fprintf(f, "%s\n", s);
        fclose(f);
    }
    free(s);
}

/* A row onto lessons.jsonl, and where the harness shows it (activity.h):
 * every verdict, measure, promotion and stage move, labelled with the
 * learner taught when it is not the harness's own (a test, an A/B). */
static void log_row(const teach_ctx_t *c, const json_t *row)
{
    append(c->lessons_log, row);
    json_t *e = jclone(row);
    jobj_set(e, "kind", jstr("lesson"));
    jobj_set(e, "instance", c->name ? jstr(c->name) : jnull());
    activity_append(e);
    json_free(e);
}

/* Every row of a .jsonl file. */
static json_t *read_rows(const char *path)
{
    json_t *rows = jarr();
    FILE *f = fopen(path, "r");
    char *line = NULL;
    size_t cap = 0;
    ssize_t len;
    while (f && (len = getline(&line, &cap, f)) > 0) {
        char perr[64];
        json_t *r = json_parse(line, (size_t)len, perr, sizeof perr);
        if (r)
            jarr_push(rows, r);
    }
    free(line);
    if (f)
        fclose(f);
    return rows;
}

static const char *str(const json_t *o, const char *k)
{
    const json_t *v = jobj_get(o, k);
    return v && v->t == J_STR ? v->s : "";
}

static bool boolean(const json_t *o, const char *k)
{
    const json_t *v = jobj_get(o, k);
    return v && v->t == J_BOOL && v->b;
}

/* ---- judging a try -------------------------------------------------------- */

/* MechJev alone on `line`, judged: every requirement and the lesson's check
 * (0.5 or more). A row {line, reply, requirements, check, coherence, passed}.
 * in_tui: typed in the TUI, an alone turn the person watches (the user's ask:
 * "The local teaching model needs to show up in the harness just like when
 * you type for me"); else the probe path (measuring: nothing heard). */
static int judged_try(teach_ctx_t *c, const lesson_t *l, const char *line, bool in_tui, json_t **row, char *err,
                      size_t errlen)
{
    json_t *rec = NULL;
    if (in_tui && c->t.pane ? teacher_say_in_tui(&c->t, line, true, NULL, NULL, &rec, err, errlen)
                            : teacher_try(&c->t, line, &rec, err, errlen))
        return -1;
    const char *reply = str(rec, "reply");
    bool req_ok = !boolean(rec, "failed_requirement");
    double p = 0;
    if (*reply && teacher_check(&c->t, l, line, reply, &p, err, errlen)) {
        json_free(rec);
        return -1;
    }
    json_t *o = jobj();
    jobj_set(o, "line", jstr(line));
    jobj_set(o, "reply", jstr(reply));
    jobj_set(o, "requirements", jbool(req_ok));
    jobj_set(o, "check", jf64(round(p * 1e4) / 1e4));
    jobj_set(o, "coherence", jclone(jobj_get(rec, "coherence_final")));
    jobj_set(o, "passed", jbool(req_ok && p >= 0.5));
    json_free(rec);
    *row = o;
    return 0;
}

/* ---- the frozen test lines --------------------------------------------- */

static json_t *tests_load(teach_ctx_t *c) { return read_rows(c->tests); }

static bool is_test_line(const json_t *tests, const char *line)
{
    for (size_t i = 0; i < tests->n; i++)
        if (teacher_same_text(str(tests->items[i], "line"), line))
            return true;
    return false;
}

static int cmd_freeze(teach_ctx_t *c, int k)
{
    struct stat st;
    if (stat(c->tests, &st) == 0) {
        fprintf(stderr, "%s exists: the test lines are frozen (remove it to write new ones; measurements "
                        "before and after must use the same lines)\n",
                c->tests);
        return 2;
    }
    char err[512];
    json_t *tests = jarr();
    for (size_t i = 0; i < c->ls.n; i++) {
        const lesson_t *l = &c->ls.v[i];
        for (int j = 0; j < k; j++) {
            char *line = NULL;
            /* a new line each time: the ones already written are its context */
            sb_t ctx;
            sb_init(&ctx);
            for (size_t m = 0; m < tests->n; m++)
                if (!strcmp(str(tests->items[m], "lesson"), l->name))
                    sb_printf(&ctx, "%s\"%s\"", ctx.n ? "; " : "Lines already written for this: ",
                              str(tests->items[m], "line"));
            int rc = teacher_write(&c->t, l->held_goal ? l->held_goal : l->goal, ctx.s, &line, err, sizeof err);
            sb_free(&ctx);
            if (rc) {
                fprintf(stderr, "error: %s\n", err);
                json_free(tests);
                return 1;
            }
            if (is_test_line(tests, line)) {
                free(line);
                j--; /* the same line again: write another */
                continue;
            }
            json_t *row = jobj();
            jobj_set(row, "lesson", jstr(l->name));
            jobj_set(row, "line", jstr(line));
            printf("%s: %s\n", l->name, line);
            fflush(stdout);
            jarr_push(tests, row);
            free(line);
        }
    }
    for (size_t i = 0; i < tests->n; i++)
        append(c->tests, tests->items[i]);
    json_free(tests);
    return 0;
}

static int cmd_measure(teach_ctx_t *c, int repeats)
{
    json_t *tests = tests_load(c);
    if (!tests->n) {
        fprintf(stderr, "no test lines in %s: teacher freeze first\n", c->tests);
        json_free(tests);
        return 2;
    }
    char err[512], when[32];
    stamp(when, sizeof when);
    size_t passed_all = 0, tried_all = 0;
    for (size_t i = 0; i < c->ls.n; i++) {
        const lesson_t *l = &c->ls.v[i];
        size_t passed = 0, tried = 0;
        for (size_t k = 0; k < tests->n; k++) {
            if (strcmp(str(tests->items[k], "lesson"), l->name))
                continue;
            for (int r = 0; r < repeats; r++) {
                json_t *row = NULL;
                if (judged_try(c, l, str(tests->items[k], "line"), false, &row, err, sizeof err)) {
                    fprintf(stderr, "error: %s\n", err);
                    json_free(tests);
                    return 1;
                }
                jobj_set(row, "time", jstr(when));
                jobj_set(row, "lesson", jstr(l->name));
                jobj_set(row, "mode", jstr("measure"));
                log_row(c, row);
                printf("measured %s: \"%s\" -> \"%s\" %s (check %.2f)\n", l->name, str(row, "line"),
                       str(row, "reply"), boolean(row, "passed") ? "PASS" : "fail", jobj_get(row, "check")->f);
                fflush(stdout);
                passed += boolean(row, "passed");
                tried++;
                json_free(row);
            }
        }
        printf("lesson %s: passed %zu of %zu\n", l->name, passed, tried);
        passed_all += passed;
        tried_all += tried;
    }
    printf("measured: passed %zu of %zu test tries\n", passed_all, tried_all);
    json_free(tests);
    return 0;
}

/* The lines it said in its last lessons, newest first, once each, every
 * line (every turn types a new one): a string
 * for its prompt, and each line pushed to `avoid`. */
static char *recent_lines(teach_ctx_t *c, json_t *avoid)
{
    json_t *rows = read_rows(c->lessons_log);
    sb_t b;
    sb_init(&b);
    size_t n = 0;
    for (size_t k = rows->n; k-- > 0 && n < RECENT_LINES;) {
        const json_t *r = rows->items[k];
        if (!strcmp(str(r, "mode"), "promote") || !strcmp(str(r, "mode"), "stage"))
            continue;
        const json_t *st = jobj_get(r, "steps");
        size_t ns = st && st->t == J_ARR ? st->n : 0;
        for (size_t j = ns + 1; j-- > 0 && n < RECENT_LINES;) {
            const char *l = j == ns ? str(r, "line") : str(st->items[j], "line");
            bool again = !*l;
            for (size_t s = 0; s < avoid->n && !again; s++)
                again = teacher_same_text(avoid->items[s]->s, l);
            if (again)
                continue;
            jarr_push(avoid, jstr(l));
            n++;
            sb_printf(&b, "%s\"%s\"", n > 1 ? "; " : "Lines you have said to MechJev in your last lessons: ", l);
        }
    }
    json_free(rows);
    return sb_take(&b);
}

/* Qwen's next line at a stage, toward `goal`: shown the stage's last
 * exchanges (its lines and MechJev's replies) and its lines of the last
 * turns, and written again while it is a test line or one in `avoid`, the
 * lines it rejected shown to it. The user's ask (2026-10-17): "Stop the
 * Qwen model from looping when teaching MechJev": a lesson had typed its one
 * line for every try and demonstration, 4 times in the greet lesson. The
 * line is pushed to `avoid`. */
static int next_line(teach_ctx_t *c, const lesson_t *l, const char *goal, const json_t *tests, const char *recent,
                     json_t *avoid,
                     const char *exchanges, json_t *refused, char **line, char *err, size_t errlen)
{
    sb_t ctx;
    sb_init(&ctx);
    if (exchanges && *exchanges)
        sb_printf(&ctx, "This lesson so far:\n%s", exchanges);
    if (recent && *recent)
        sb_printf(&ctx, "%s%s", ctx.n ? "\n\n" : "", recent);
    *line = NULL;
    char *near = NULL; /* the last near-repeat: said only when every rewrite was one */
    char *best = NULL; /* the best-fitting new line that did not fit, said when nothing better comes */
    double best_fit = -1;
    for (int k = 0; k < REWRITES; k++) {
        free(*line);
        *line = NULL;
        if (teacher_write(&c->t, goal, ctx.s, line, err, errlen)) {
            free(near);
            sb_free(&ctx);
            return -1;
        }
        bool again = is_test_line(tests, *line);
        for (size_t s = 0; s < avoid->n && !again; s++)
            again = teacher_same_text(avoid->items[s]->s, *line);
        double most = 0;
        const char *like = NULL;
        for (size_t s = 0; s < avoid->n && !again; s++) {
            double o = teacher_word_overlap(avoid->items[s]->s, *line);
            if (teacher_words_inside(avoid->items[s]->s, *line))
                o = 1; /* a line within a line said, or one said within it */
            if (o > most) {
                most = o;
                like = avoid->items[s]->s;
            }
        }
        if (!again && most < NEAR) {
            /* new: does it do what the stage asks? (Qwen drifted: "The sun is
             * hot. Is the moon cold?" for a stage that asks back the fact) */
            double fit = 0;
            if (teacher_line_fits(&c->t, goal, *line, &fit, err, errlen)) {
                free(near);
                free(best);
                sb_free(&ctx);
                return -1;
            }
            if (fit >= LINE_FIT_MIN) {
                free(near);
                free(best);
                jarr_push(avoid, jstr(*line));
                sb_free(&ctx);
                return 0;
            }
            if (fit > best_fit) { /* the best of the lines that do not fit, said if none does */
                free(best);
                best = xstrdup(*line);
                best_fit = fit;
            }
            if (refused) {
                json_t *x = jobj();
                jobj_set(x, "line", jstr(*line));
                jobj_set(x, "why", jstr("does not do what the stage asks"));
                jobj_set(x, "fit", jf64(round(fit * 1e4) / 1e4));
                jarr_push(refused, x);
            }
            sb_printf(&ctx, "%sNot \"%s\": it does not do what your line should do: %s", ctx.n ? "\n\n" : "",
                      *line, goal);
            continue;
        }
        if (refused) { /* for the row: what Qwen wrote and why it was not said */
            json_t *x = jobj();
            jobj_set(x, "line", jstr(*line));
            jobj_set(x, "why", jstr(again ? "said already, or a test line" : "a near-repeat"));
            if (!again)
                jobj_set(x, "of", jstr(like));
            jarr_push(refused, x);
        }
        if (again)
            sb_printf(&ctx, "%sNot \"%s\": you have said that already.", ctx.n ? "\n\n" : "", *line);
        else {
            free(near);
            near = xstrdup(*line);
            sb_printf(&ctx, "%sNot \"%s\": it is nearly \"%s\", which you have said. Say something new.",
                      ctx.n ? "\n\n" : "", *line, like);
        }
    }
    sb_free(&ctx);
    if (best) { /* nothing that fits: the new line that came closest, not a failed turn */
        free(near);
        free(*line);
        *line = best;
        jarr_push(avoid, jstr(*line));
        return 0;
    }
    if (near) { /* nothing new but near-repeats: the last of them, not a failed turn */
        free(*line);
        *line = near;
        jarr_push(avoid, jstr(*line));
        return 0;
    }
    snprintf(err, errlen, "the teacher wrote %d lines for %s and each was a repeat or a test line (last: \"%s\")",
             REWRITES, l->name, *line);
    free(*line);
    *line = NULL;
    return -1;
}

/* ---- the ladder ------------------------------------------------------------ */

/* The user's asks (2026-10-17): "Qwen needs to prompt increasingly complex
 * prompts", "MechJev needs to learn to communicate first", and "the
 * repetitions aren't needed". The stages are lessons.txt in order, each
 * harder than the one before; every turn Qwen writes a new line. A stage
 * opens with a check of MechJev alone (what it has mastered is climbed past
 * at once), then taught turns, a check after every CHECK_EVERY of them; it
 * moves up when PROMOTE of its last 3 checks passed. */
#define CHECK_EVERY 2 /* taught turns between checks of MechJev alone */
#define PROMOTE 2     /* passed checks, of the last 3, that move it up */
#define EXCHANGES 6   /* the stage's last exchanges Qwen is shown */

typedef struct {
    size_t stage;     /* its index in the lessons */
    size_t taught;    /* taught turns at it since it was reached */
    size_t since;     /* taught turns since its last check */
    size_t nchecks;   /* checks at it */
    bool last[3];     /* the last checks, newest last */
    size_t nlast;
    char *exchanges;  /* its last exchanges, for Qwen's next line */
} ladder_t;

/* Where MechJev is, from lessons.jsonl: the stage the last promotion (or
 * `teacher stage`) reached, else the first; its turns and checks since. */
static void ladder_read(teach_ctx_t *c, ladder_t *ld)
{
    memset(ld, 0, sizeof *ld);
    json_t *rows = read_rows(c->lessons_log);
    size_t from = 0;
    for (size_t k = rows->n; k-- > 0;) {
        const json_t *r = rows->items[k];
        const char *m = str(r, "mode");
        int i = !strcmp(m, "promote") || !strcmp(m, "stage") ? lessons_find(&c->ls, str(r, "to")) : -1;
        if (i >= 0) {
            ld->stage = (size_t)i;
            from = k + 1;
            break;
        }
    }
    const char *name = c->ls.v[ld->stage].name;
    size_t ex[EXCHANGES], nex = 0;
    for (size_t k = from; k < rows->n; k++) {
        const json_t *r = rows->items[k];
        const char *m = str(r, "mode");
        if (strcmp(str(r, "lesson"), name) || (strcmp(m, "taught") && strcmp(m, "check")))
            continue;
        if (!strcmp(m, "taught")) {
            ld->taught++;
            ld->since++;
        } else {
            ld->nchecks++;
            ld->since = 0;
            if (ld->nlast == 3) {
                memmove(ld->last, ld->last + 1, 2 * sizeof *ld->last);
                ld->nlast = 2;
            }
            ld->last[ld->nlast++] = boolean(r, "passed");
        }
        if (nex == EXCHANGES) {
            memmove(ex, ex + 1, (EXCHANGES - 1) * sizeof *ex);
            nex--;
        }
        ex[nex++] = k;
    }
    sb_t b;
    sb_init(&b);
    for (size_t i = 0; i < nex; i++) {
        const json_t *r = rows->items[ex[i]];
        sb_printf(&b, "%sYou: %s\nMechJev: %s", i ? "\n" : "", str(r, "line"), str(r, "reply"));
    }
    ld->exchanges = sb_take(&b);
    json_free(rows);
}

static size_t passed_of_last(const ladder_t *ld)
{
    size_t n = 0;
    for (size_t i = 0; i < ld->nlast; i++)
        n += ld->last[i];
    return n;
}

/* One turn on the ladder: a check of MechJev alone when one is due, else a
 * taught turn; both typed in the TUI, a new line Qwen writes each time. */
static int ladder_step(teach_ctx_t *c, char *err, size_t errlen)
{
    ladder_t ld;
    ladder_read(c, &ld);
    const lesson_t *l = &c->ls.v[ld.stage];
    bool check = ld.nchecks == 0 || ld.since >= CHECK_EVERY;
    const char *goal = check && l->held_goal ? l->held_goal : l->goal;
    json_t *tests = tests_load(c), *avoid = jarr();
    char *recent = recent_lines(c, avoid);
    char *line = NULL;
    json_t *refused = jarr(); /* the lines Qwen wrote that were not said */
    int rc = next_line(c, l, goal, tests, recent, avoid, ld.exchanges, refused, &line, err, errlen);
    free(recent);
    json_free(avoid);
    json_free(tests);
    if (rc) {
        free(ld.exchanges);
        json_free(refused);
        return -1;
    }
    printf("stage %zu/%zu %s, %s: Qwen says \"%s\"\n", ld.stage + 1, c->ls.n, l->name,
           check ? "a check of MechJev alone" : "taught", line);
    for (size_t i = 0; i < refused->n; i++)
        printf("  (refused first: \"%s\", %s)\n", str(refused->items[i], "line"), str(refused->items[i], "why"));
    fflush(stdout);
    json_t *row = NULL;
    if (check)
        rc = judged_try(c, l, line, true, &row, err, errlen);
    else {
        json_t *rec = NULL;
        rc = c->t.pane ? teacher_say_in_tui(&c->t, line, false, l->show, l->check, &rec, err, errlen)
                       : teacher_demonstrate(&c->t, line, l->show, &rec, err, errlen);
        double p = 0;
        const char *reply = rc ? "" : str(rec, "reply");
        if (!rc && *reply)
            rc = teacher_check(&c->t, l, line, reply, &p, err, errlen);
        if (!rc) {
            bool req_ok = !boolean(rec, "failed_requirement");
            row = jobj();
            jobj_set(row, "line", jstr(line));
            jobj_set(row, "reply", jstr(reply));
            jobj_set(row, "requirements", jbool(req_ok));
            jobj_set(row, "check", jf64(round(p * 1e4) / 1e4));
            jobj_set(row, "coherence", jclone(jobj_get(rec, "coherence_final")));
            jobj_set(row, "passed", jbool(req_ok && p >= 0.5));
            jobj_set(row, "turn", jclone(jobj_get(rec, "turn")));
        }
        json_free(rec);
    }
    free(line);
    free(ld.exchanges);
    if (rc) {
        json_free(refused);
        return -1;
    }
    char when[32];
    stamp(when, sizeof when);
    jobj_set(row, "time", jstr(when));
    jobj_set(row, "lesson", jstr(l->name));
    jobj_set(row, "stage", ju64(ld.stage + 1));
    jobj_set(row, "mode", jstr(check ? "check" : "taught"));
    jobj_set(row, "refused", refused); /* the row owns it now */
    bool passed = boolean(row, "passed");
    printf("  MechJev%s: \"%s\" check %.2f, requirements %s: %s\n", check ? " alone" : "", str(row, "reply"),
           jobj_get(row, "check")->f, boolean(row, "requirements") ? "met" : "failed", passed ? "PASS" : "fail");
    log_row(c, row);
    json_free(row);
    if (check) {
        if (ld.nlast == 3) {
            memmove(ld.last, ld.last + 1, 2 * sizeof *ld.last);
            ld.nlast = 2;
        }
        ld.last[ld.nlast++] = passed;
        if (passed_of_last(&ld) >= PROMOTE) {
            if (ld.stage + 1 < c->ls.n) {
                json_t *p = jobj();
                jobj_set(p, "time", jstr(when));
                jobj_set(p, "mode", jstr("promote"));
                jobj_set(p, "from", jstr(l->name));
                jobj_set(p, "to", jstr(c->ls.v[ld.stage + 1].name));
                jobj_set(p, "taught", ju64(ld.taught));
                jobj_set(p, "checks", ju64(ld.nchecks + 1));
                log_row(c, p);
                json_free(p);
                printf("  promoted: %s -> %s (after %zu taught turns, %zu checks)\n", l->name,
                       c->ls.v[ld.stage + 1].name, ld.taught, ld.nchecks + 1);
            } else
                printf("  the top of the ladder: %s passed\n", l->name);
        }
    }
    fflush(stdout);
    return 0;
}

static int cmd_teach(teach_ctx_t *c, int n)
{
    char err[512];
    int in_a_row = 0, failed = 0;
    for (int k = 0; k < n && in_a_row < 2; k++) {
        if (ladder_step(c, err, sizeof err)) {
            fprintf(stderr, "error: turn %d: %s\n", k + 1, err);
            in_a_row++;
            failed++;
        } else
            in_a_row = 0;
    }
    return failed ? 1 : 0;
}

/* `teacher stage NAME`: the ladder moved to a stage, as a row. */
static int cmd_stage(teach_ctx_t *c, const char *name)
{
    if (lessons_find(&c->ls, name) < 0) {
        fprintf(stderr, "no stage %s in the lessons\n", name);
        return 2;
    }
    char when[32];
    stamp(when, sizeof when);
    json_t *r = jobj();
    jobj_set(r, "time", jstr(when));
    jobj_set(r, "mode", jstr("stage"));
    jobj_set(r, "to", jstr(name));
    log_row(c, r);
    json_free(r);
    printf("the ladder is at %s\n", name);
    return 0;
}

static int cmd_status(teach_ctx_t *c)
{
    ladder_t ld;
    ladder_read(c, &ld);
    printf("stage %zu/%zu %s: %zu taught turns, %zu checks, %zu of the last %zu passed (%d move it up)\n",
           ld.stage + 1, c->ls.n, c->ls.v[ld.stage].name, ld.taught, ld.nchecks, passed_of_last(&ld), ld.nlast,
           PROMOTE);
    if (*ld.exchanges)
        printf("%s\n", ld.exchanges);
    free(ld.exchanges);
    return 0;
}

static int cmd_lines(teach_ctx_t *c, int argc, char **argv)
{
    char err[512];
    for (size_t i = 0; i < c->ls.n; i++) {
        bool want = !argc;
        for (int a = 0; a < argc && !want; a++)
            want = !strcmp(argv[a], c->ls.v[i].name);
        if (!want)
            continue;
        char *line = NULL;
        if (teacher_write(&c->t, c->ls.v[i].goal, NULL, &line, err, sizeof err)) {
            fprintf(stderr, "error: %s\n", err);
            return 1;
        }
        printf("%s: %s\n", c->ls.v[i].name, line);
        fflush(stdout);
        free(line);
    }
    return 0;
}

static void usage(void)
{
    fputs("teacher: Qwen3.8-35B teaching MechJev from outside (teach.md)\n"
          "  teacher lines [LESSON...]  a line Qwen writes for each lesson (a look; nothing is sent)\n"
          "  teacher freeze [K]         K test lines a lesson (3), kept in teacher_tests.jsonl\n"
          "  teacher measure [R]        MechJev alone on every test line, R times (1)\n"
          "  teacher teach [N]          N turns on the ladder (1): lessons.txt in order, each stage harder\n"
          "  teacher status             where MechJev is on the ladder, and its last exchanges\n"
          "  teacher stage NAME         move the ladder to a stage\n"
          "The engine (make serve) and MechJev's server (learner serve) must be up.\n",
          stderr);
}

int main(int argc, char **argv)
{
    conf_load();
    if (argc < 2) {
        usage();
        return 2;
    }
    teach_ctx_t c;
    memset(&c, 0, sizeof c);
    c.t.engine = conf_get("MJ2_ENGINE", conf_get("XKS_BIND", "127.0.0.1:8090"));
    c.t.learner = conf_get("MJ2_BIND", "127.0.0.1:8092");
    {
        char *h = harness_dir();
        const char *nm = getenv("MJ2_NAME"), *base = strrchr(c.dir, '/');
        if (nm && *nm)
            c.name = xstrdup(nm);
        else if (strcmp(h, c.dir)) {
            sb_t b;
            sb_init(&b);
            sb_printf(&b, "%s %s", base ? base + 1 : c.dir, c.t.learner);
            c.name = sb_take(&b);
        }
        free(h);
    }
    c.dir = data_dir();
    c.lessons_log = join(c.dir, "lessons.jsonl");
    /* the TUI to type into, as a person would: MJEV2_PANE (jev2.0); empty:
     * the API, nothing shown */
    const char *pane = getenv("MJEV2_PANE");
    c.t.pane = pane ? (*pane ? pane : NULL) : "jev2.0";
    char *turns = join(c.dir, "turns.jsonl");
    c.t.turns = turns;
    c.t.speaker = "Qwen";
    c.tests = join(c.dir, "teacher_tests.jsonl");
    const char *lp = getenv("MJ2_LESSONS");
    char *root = conf_repo_root();
    char *lessons = lp && *lp ? expand_home(lp) : root ? join(root, "lessons.txt") : xstrdup("lessons.txt");
    free(root);
    char err[512];
    if (lessons_load(&c.ls, lessons, err, sizeof err)) {
        fprintf(stderr, "error: %s\n", err);
        free(lessons);
        return 1;
    }
    free(lessons);
    const char *cmd = argv[1];
    int rc;
    if (!strcmp(cmd, "lines"))
        rc = cmd_lines(&c, argc - 2, argv + 2);
    else if (!strcmp(cmd, "freeze"))
        rc = cmd_freeze(&c, argc > 2 ? atoi(argv[2]) : 3);
    else if (!strcmp(cmd, "measure"))
        rc = cmd_measure(&c, argc > 2 ? atoi(argv[2]) : 1);
    else if (!strcmp(cmd, "teach"))
        rc = cmd_teach(&c, argc > 2 ? atoi(argv[2]) : 1);
    else if (!strcmp(cmd, "status"))
        rc = cmd_status(&c);
    else if (!strcmp(cmd, "stage") && argc > 2)
        rc = cmd_stage(&c, argv[2]);
    else {
        usage();
        rc = 2;
    }
    lessons_free(&c.ls);
    free(c.dir);
    free(c.lessons_log);
    free(c.name);
    free(c.tests);
    free(turns);
    return rc;
}
