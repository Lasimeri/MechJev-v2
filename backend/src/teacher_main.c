/* teacher: Qwen3.8-35B teaching MechJev from outside (teach.h, teach.md),
 * a client of the decision engine (xks) and of MechJev's server (learner
 * serve), as a person at the TUI is.
 *
 *   teacher lines [LESSON...]  a line Qwen writes for each lesson (a look)
 *   teacher freeze [K]         K test lines a lesson, kept for measuring
 *   teacher measure [R]        MechJev alone on every test line, R times
 *   teacher teach [N]          N lessons (the engine chooses them)
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
#include "json.h"
#include "teach.h"
#include "util.h"

#define SHOTS 3          /* demonstrations a lesson at most */
#define RECENT_LINES 8   /* the teacher's own lines it is shown, so it does not loop */
#define RECENT_LESSONS 6 /* its last lessons, when it chooses the next */
#define REWRITES 3       /* a line that is a test line is written again */

typedef struct {
    teacher_t t;
    lessons_t ls;
    char *dir, *lessons_log, *tests;
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
 * (0.5 or more). A row {line, reply, requirements, check, coherence, passed}. */
static int judged_try(teach_ctx_t *c, const lesson_t *l, const char *line, json_t **row, char *err, size_t errlen)
{
    json_t *rec = NULL;
    if (teacher_try(&c->t, line, &rec, err, errlen))
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
                if (judged_try(c, l, str(tests->items[k], "line"), &row, err, sizeof err)) {
                    fprintf(stderr, "error: %s\n", err);
                    json_free(tests);
                    return 1;
                }
                jobj_set(row, "time", jstr(when));
                jobj_set(row, "lesson", jstr(l->name));
                jobj_set(row, "mode", jstr("measure"));
                append(c->lessons_log, row);
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

/* ---- the teacher's context ---------------------------------------------- */

/* What it reads to choose: each lesson's lessons taught, passes on the test
 * lines, the last result; then its last lessons in order. */
static char *record(teach_ctx_t *c)
{
    json_t *rows = read_rows(c->lessons_log);
    sb_t b;
    sb_init(&b);
    for (size_t i = 0; i < c->ls.n; i++) {
        const char *name = c->ls.v[i].name;
        size_t taught = 0, meas = 0, meas_ok = 0;
        const char *last = "not taught yet";
        for (size_t k = 0; k < rows->n; k++) {
            const json_t *r = rows->items[k];
            if (strcmp(str(r, "lesson"), name))
                continue;
            if (!strcmp(str(r, "mode"), "teach")) {
                taught++;
                last = boolean(r, "passed") ? "passed" : "did not pass";
            } else if (!strcmp(str(r, "mode"), "measure")) {
                meas++;
                meas_ok += boolean(r, "passed");
            }
        }
        sb_printf(&b, "- %s (%s): taught %zu times; on the test lines it passed %zu of %zu; last lesson: %s\n",
                  name, c->ls.v[i].goal, taught, meas_ok, meas, last);
    }
    size_t shown = 0;
    for (size_t k = rows->n; k-- > 0 && shown < RECENT_LESSONS;)
        shown += !strcmp(str(rows->items[k], "mode"), "teach");
    if (shown) {
        sb_puts(&b, "Your last lessons, oldest first:\n");
        for (size_t k = 0; k < rows->n; k++) {
            const json_t *r = rows->items[k];
            if (strcmp(str(r, "mode"), "teach"))
                continue;
            size_t after = 0; /* lessons after this one */
            for (size_t j = k + 1; j < rows->n; j++)
                after += !strcmp(str(rows->items[j], "mode"), "teach");
            if (after >= RECENT_LESSONS)
                continue;
            sb_printf(&b, "- %s: you said \"%s\"; %s after %u demonstrations\n", str(r, "lesson"), str(r, "line"),
                      boolean(r, "passed") ? "it passed" : "it did not pass",
                      (unsigned)(jobj_get(r, "shots") ? jobj_get(r, "shots")->u : 0));
        }
    }
    json_free(rows);
    return sb_take(&b);
}

/* The lines it said lately, newest first, once each. */
static char *recent_lines(teach_ctx_t *c)
{
    json_t *rows = read_rows(c->lessons_log);
    sb_t b;
    sb_init(&b);
    const char *seen[RECENT_LINES];
    size_t n = 0;
    for (size_t k = rows->n; k-- > 0 && n < RECENT_LINES;) {
        const json_t *r = rows->items[k];
        if (strcmp(str(r, "mode"), "teach"))
            continue;
        const char *l = str(r, "line");
        bool again = false;
        for (size_t s = 0; s < n && !again; s++)
            again = teacher_same_text(seen[s], l);
        if (again || !*l)
            continue;
        seen[n++] = l;
        sb_printf(&b, "%s\"%s\"", n > 1 ? "; " : "Lines you have said to MechJev lately: ", l);
    }
    char *out = n ? xstrdup(b.s) : xstrdup("");
    sb_free(&b);
    json_free(rows);
    return out;
}

/* ---- a lesson ------------------------------------------------------------ */

static int lesson(teach_ctx_t *c, int fixed, char *err, size_t errlen)
{
    size_t li;
    if (fixed >= 0)
        li = (size_t)fixed;
    else {
        char *rec = record(c);
        int rc = teacher_choose(&c->t, &c->ls, rec, &li, err, errlen);
        free(rec);
        if (rc)
            return -1;
    }
    const lesson_t *l = &c->ls.v[li];
    json_t *tests = tests_load(c);
    char *ctx = recent_lines(c);
    char *line = NULL;
    for (int k = 0; k < REWRITES; k++) { /* never teach a test line */
        free(line);
        line = NULL;
        if (teacher_write(&c->t, l->goal, ctx, &line, err, errlen)) {
            free(ctx);
            json_free(tests);
            return -1;
        }
        if (!is_test_line(tests, line))
            break;
    }
    free(ctx);
    if (is_test_line(tests, line)) {
        snprintf(err, errlen, "every line the teacher wrote for %s is a test line", l->name);
        free(line);
        json_free(tests);
        return -1;
    }
    json_free(tests);
    char when[32];
    stamp(when, sizeof when);
    json_t *row = jobj(), *steps = jarr();
    jobj_set(row, "time", jstr(when));
    jobj_set(row, "lesson", jstr(l->name));
    jobj_set(row, "mode", jstr("teach"));
    jobj_set(row, "chosen_by", jstr(fixed >= 0 ? "asked" : "teacher"));
    jobj_set(row, "line", jstr(line));
    printf("lesson %s: Qwen says \"%s\"\n", l->name, line);
    fflush(stdout);
    json_t *a = NULL;
    int rc = judged_try(c, l, line, &a, err, errlen);
    bool passed = !rc && boolean(a, "passed");
    if (!rc) {
        printf("  MechJev alone: \"%s\" %s (check %.2f)\n", str(a, "reply"), passed ? "PASS" : "fail",
               jobj_get(a, "check")->f);
        jarr_push(steps, a);
    }
    size_t shots = 0;
    while (!rc && !passed && shots < SHOTS) {
        json_t *rec = NULL;
        rc = teacher_demonstrate(&c->t, line, l->show, &rec, err, errlen);
        if (rc)
            break;
        json_t *d = jobj();
        jobj_set(d, "demonstration", ju64(++shots));
        jobj_set(d, "reply", jstr(str(rec, "reply")));
        jobj_set(d, "requirements", jbool(!boolean(rec, "failed_requirement")));
        jobj_set(d, "turn", jclone(jobj_get(rec, "turn")));
        printf("  demonstration %zu (taught turn, the engine choosing among MechJev's words): \"%s\"\n", shots,
               str(rec, "reply"));
        json_free(rec);
        jarr_push(steps, d);
        rc = judged_try(c, l, line, &a, err, errlen);
        if (!rc) {
            passed = boolean(a, "passed");
            printf("  MechJev alone: \"%s\" %s (check %.2f)\n", str(a, "reply"), passed ? "PASS" : "fail",
                   jobj_get(a, "check")->f);
            jarr_push(steps, a);
        }
    }
    free(line);
    fflush(stdout);
    if (rc) {
        json_free(row);
        json_free(steps);
        return -1;
    }
    jobj_set(row, "steps", steps);
    jobj_set(row, "shots", ju64(shots));
    jobj_set(row, "passed", jbool(passed));
    append(c->lessons_log, row);
    json_free(row);
    return 0;
}

static int cmd_teach(teach_ctx_t *c, int n, const char *only)
{
    int fixed = only ? lessons_find(&c->ls, only) : -1;
    if (only && fixed < 0) {
        fprintf(stderr, "no lesson %s in the lessons\n", only);
        return 2;
    }
    char err[512];
    int in_a_row = 0, failed = 0;
    for (int k = 0; k < n && in_a_row < 2; k++) {
        if (lesson(c, fixed, err, sizeof err)) {
            fprintf(stderr, "error: lesson %d: %s\n", k + 1, err);
            in_a_row++;
            failed++;
        } else
            in_a_row = 0;
    }
    return failed ? 1 : 0;
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
          "  teacher teach [N] [--lesson NAME]  N lessons (1); the engine chooses them\n"
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
    c.dir = data_dir();
    c.lessons_log = join(c.dir, "lessons.jsonl");
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
    else if (!strcmp(cmd, "teach")) {
        int n = 1;
        const char *only = NULL;
        for (int i = 2; i < argc; i++) {
            if (!strcmp(argv[i], "--lesson") && i + 1 < argc)
                only = argv[++i];
            else
                n = atoi(argv[i]);
        }
        rc = cmd_teach(&c, n, only);
    } else {
        usage();
        rc = 2;
    }
    lessons_free(&c.ls);
    free(c.dir);
    free(c.lessons_log);
    free(c.tests);
    return rc;
}
