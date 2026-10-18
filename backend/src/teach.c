/* teach.c: the teacher (teach.h): Qwen3.8-35B, through the engine, talking
 * to MechJev from outside. */
#define _GNU_SOURCE
#include "teach.h"

#include <ctype.h>
#include <time.h>
#include <fcntl.h>
#include <sys/file.h>
#include <sys/wait.h>
#include <unistd.h>
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
        sb_printf(&u, "\n\n%s\n\nWrite a new line toward the same goal, in other words than every line above.", context);
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

/* The distinct words of s, lower case (letters, digits, apostrophes), at
 * most max of them; how many. */
static size_t word_set_as(const char *s, char words[][32], size_t max, bool skip_names)
{
    size_t n = 0;
    bool start = true; /* at the start of a sentence */
    while (*s && n < max) {
        while (*s && !isalnum((unsigned char)*s)) {
            if (*s == '.' || *s == '?' || *s == '!')
                start = true;
            s++;
        }
        if (!*s)
            break;
        /* a capitalized word inside a sentence is a name: Qwen's made-up
         * names made one frame read as new lines ("Do you know the Fael?",
         * "... the Kael?", 2026-10-17), so they are set aside */
        bool name = skip_names && !start && isupper((unsigned char)*s);
        char w[32];
        size_t k = 0;
        while (*s && (isalnum((unsigned char)*s) || *s == '\'')) {
            if (k + 1 < sizeof w)
                w[k++] = (char)tolower((unsigned char)*s);
            s++;
        }
        w[k] = 0;
        start = false;
        if (name)
            continue;
        bool seen = false;
        for (size_t i = 0; i < n && !seen; i++)
            seen = !strcmp(words[i], w);
        if (!seen)
            memcpy(words[n++], w, k + 1);
    }
    return n;
}

/* The distinct words of s, lower case, names inside a sentence set aside;
 * all of them when nothing else is left. */
static size_t word_set(const char *s, char words[][32], size_t max)
{
    size_t n = word_set_as(s, words, max, true);
    return n ? n : word_set_as(s, words, max, false);
}

double teacher_word_overlap(const char *a, const char *b)
{
    char wa[64][32], wb[64][32];
    size_t na = word_set(a, wa, 64), nb = word_set(b, wb, 64), both = 0;
    for (size_t i = 0; i < na; i++)
        for (size_t j = 0; j < nb; j++)
            if (!strcmp(wa[i], wb[j])) {
                both++;
                break;
            }
    size_t either = na + nb - both;
    return either ? (double)both / (double)either : 1.0;
}

bool teacher_words_inside(const char *a, const char *b)
{
    char wa[64][32], wb[64][32];
    size_t na = word_set(a, wa, 64), nb = word_set(b, wb, 64);
    if (!na || !nb)
        return false;
    bool a_small = na <= nb;
    char (*s)[32] = a_small ? wa : wb, (*g)[32] = a_small ? wb : wa;
    size_t ns = a_small ? na : nb, ng = a_small ? nb : na;
    for (size_t i = 0; i < ns; i++) {
        bool found = false;
        for (size_t j = 0; j < ng && !found; j++)
            found = !strcmp(s[i], g[j]);
        if (!found)
            return false;
    }
    return true;
}

/* ---- typing in the TUI ------------------------------------------------- */

/* tmux with these arguments (no shell: a line is passed as it is); its
 * output into out when out is not NULL. 0 when tmux succeeded. */
static int tmux(char *const argv[], char *out, size_t outlen)
{
    int fd[2];
    if (out && pipe(fd))
        return -1;
    pid_t p = fork();
    if (p < 0)
        return -1;
    if (p == 0) {
        int nul = open("/dev/null", O_RDWR);
        if (out) {
            dup2(fd[1], 1);
            close(fd[0]);
            close(fd[1]);
        } else
            dup2(nul, 1);
        dup2(nul, 2);
        execvp("tmux", argv);
        _exit(127);
    }
    if (out) {
        close(fd[1]);
        size_t n = 0;
        ssize_t k;
        while (n + 1 < outlen && (k = read(fd[0], out + n, outlen - 1 - n)) > 0)
            n += (size_t)k;
        out[n] = 0;
        char sink[4096];
        while (read(fd[0], sink, sizeof sink) > 0)
            ;
        close(fd[0]);
    }
    int st = 0;
    waitpid(p, &st, 0);
    return WIFEXITED(st) && WEXITSTATUS(st) == 0 ? 0 : -1;
}

static int pane_screen(const char *pane, char *out, size_t n)
{
    char *argv[] = {"tmux", "capture-pane", "-p", "-t", (char *)pane, NULL};
    return tmux(argv, out, n);
}

static int pane_key(const char *pane, const char *key)
{
    char *argv[] = {"tmux", "send-keys", "-t", (char *)pane, (char *)key, NULL};
    return tmux(argv, NULL, 0);
}

static int pane_text(const char *pane, const char *text)
{
    char *argv[] = {"tmux", "send-keys", "-t", (char *)pane, "-l", (char *)text, NULL};
    return tmux(argv, NULL, 0);
}

static void sleep_ms(long ms)
{
    struct timespec ts = {ms / 1000, (ms % 1000) * 1000000L};
    nanosleep(&ts, NULL);
}

static size_t lines_in(const char *path)
{
    FILE *f = fopen(path, "r");
    if (!f)
        return 0;
    size_t n = 0;
    int ch;
    while ((ch = fgetc(f)) != EOF)
        n += ch == '\n';
    fclose(f);
    return n;
}

static char *last_line(const char *path)
{
    FILE *f = fopen(path, "r");
    if (!f)
        return NULL;
    char *line = NULL, *last = NULL;
    size_t cap = 0;
    while (getline(&line, &cap, f) > 0) {
        free(last);
        last = xstrdup(line);
    }
    free(line);
    fclose(f);
    return last;
}

#define SAY "say \xe2\x80\xba"                        /* the talk screen's input, taught: "say ›" */
#define SAY_ALONE "say (circuit alone) \xe2\x80\xba" /* the same, F2 on: the circuit alone */
#define TURN_WAIT_S 1800                           /* a turn of many words is many minutes */

int teacher_say_in_tui(const teacher_t *t, const char *line, bool alone, const char *show, const char *check,
                       json_t **rec, char *err,
                       size_t errlen)
{
    *rec = NULL;
    const char *rd = getenv("XDG_RUNTIME_DIR");
    sb_t lp;
    sb_init(&lp);
    sb_printf(&lp, "%s/mjev2-driver.lock", rd && *rd ? rd : "/tmp");
    int lk = open(lp.s, O_CREAT | O_RDWR, 0644);
    sb_free(&lp);
    if (lk < 0 || flock(lk, LOCK_EX)) { /* another driver typing: wait for it */
        snprintf(err, errlen, "cannot take the TUI driver's lock");
        if (lk >= 0)
            close(lk);
        return -1;
    }
    int rc = -1;
    char *scr = xmalloc(1 << 16);
    /* the next turn's settings */
    json_t *nx = jobj();
    jobj_set(nx, "alone", jbool(alone));
    if (check && *check && !alone) /* the stage check: a requirement of this taught turn too */
        jobj_set(nx, "check", jstr(check));
    if (show && *show && !alone)
        jobj_set(nx, "show", jstr(show));
    if (t->speaker)
        jobj_set(nx, "speaker", jstr(t->speaker));
    json_t *ok = post(t->learner, "/v1/next", nx, err, errlen);
    if (!ok)
        goto out;
    json_free(ok);
    /* out of copy mode (a person scrolling), onto the talk screen */
    char *mode[] = {"tmux", "display", "-p", "-t", (char *)t->pane, "#{pane_in_mode}", NULL};
    if (tmux(mode, scr, 64) == 0 && scr[0] == '1') {
        char *cancel[] = {"tmux", "send-keys", "-t", (char *)t->pane, "-X", "cancel", NULL};
        tmux(cancel, NULL, 0);
    }
    if (pane_screen(t->pane, scr, 1 << 16)) {
        snprintf(err, errlen, "no TUI in tmux pane %s (make watch opens it)", t->pane);
        goto out;
    }
    if (!strstr(scr, SAY) && !strstr(scr, SAY_ALONE)) {
        pane_key(t->pane, "Escape");
        sleep_ms(400);
        pane_key(t->pane, "t");
        sleep_ms(800);
        pane_screen(t->pane, scr, 1 << 16);
        if (!strstr(scr, SAY) && !strstr(scr, SAY_ALONE)) {
            snprintf(err, errlen, "the TUI in %s did not show its talk screen", t->pane);
            goto out;
        }
    }
    /* the mode the turn needs, as a person would set it: F2 */
    bool now_alone = strstr(scr, SAY_ALONE) != NULL;
    if (now_alone != alone) {
        pane_key(t->pane, "F2");
        sleep_ms(400);
    }
    size_t before = lines_in(t->turns);
    if (pane_text(t->pane, line) || pane_key(t->pane, "Enter")) {
        snprintf(err, errlen, "could not type into tmux pane %s", t->pane);
        goto out;
    }
    for (int s = 0; s < TURN_WAIT_S && lines_in(t->turns) <= before; s++)
        sleep_ms(1000);
    if (lines_in(t->turns) <= before) {
        snprintf(err, errlen, "no turn landed in %s within %d s", t->turns, TURN_WAIT_S);
        goto out;
    }
    char *last = last_line(t->turns);
    char perr[128];
    *rec = last ? json_parse(last, strlen(last), perr, sizeof perr) : NULL;
    free(last);
    if (!*rec) {
        snprintf(err, errlen, "the turn in %s could not be read", t->turns);
        goto out;
    }
    if (alone) /* back to taught, where a person left it */
        pane_key(t->pane, "F2");
    rc = 0;
out:
    free(scr);
    flock(lk, LOCK_UN);
    close(lk);
    return rc;
}
