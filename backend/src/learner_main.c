/* learner_main.c: the `learner` command, the language learner beside the
 * decision engine. It does not link llama.cpp: every decision it needs
 * comes from xks over HTTP. See learner_main.md.
 *
 *   learner talk [MESSAGE]     one turn, or a conversation on the terminal
 *   learner hear FILE...       read English text (observational learning)
 *   learner stats              what it knows
 *   learner serve [--bind B]   the HTTP API the harness uses (127.0.0.1:8092)
 *   learner reset --yes        forget everything (removes the store)
 */
#define _GNU_SOURCE
#include <ctype.h>
#include <errno.h>
#include <math.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <fcntl.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include "conf.h"
#include "http.h"
#include "json.h"
#include "learner.h"
#include "speak.h"
#include "util.h"

#define DEFAULT_SEED 20261017ull
/* The fast memory's weight in a word's score (MJ2_MEMORY; 0: it learns but
 * does not speak). Set from the rebuild's margins: see learner_main.md. */
#define DEFAULT_MEM_WEIGHT 0.0
/* the n-gram memory's weight (MJ2_NGRAM): 1, the probe A/B's choice (ngram.md:
 * 18 of 24 against 15 at 0; at 3, "I am." to "Do you like music?") */
#define DEFAULT_NG_WEIGHT 1.0

typedef struct {
    char *dir, *store, *curves, *history, *turns; /* turns: whole records, the per-word trace kept */
    learner_t *l;
    speak_cfg_t cfg;
    history_t h;
    pthread_mutex_t mu;
    /* the next turn's settings (POST /v1/next, used once by the next
     * /v1/talk): the teacher typing in the TUI like a person, its turn
     * labelled, its answer words shown, or MechJev alone */
    char *next_show, *next_speaker;
    bool next_alone;
} ctx_t;

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
        sb_printf(&b, "%s/.local/share/mechjev-v2", getenv("HOME") ? getenv("HOME") : "");
    return sb_take(&b);
}

static char *join(const char *a, const char *b)
{
    sb_t s;
    sb_init(&s);
    sb_printf(&s, "%s/%s", a, b);
    return sb_take(&s);
}

/* The restrictions: one Noul statement per line, `#` comments. */
static void load_checks(speak_cfg_t *cfg)
{
    char *path = NULL;
    const char *p = getenv("MJ2_CHECKS");
    if (p && *p)
        path = expand_home(p);
    else {
        char *root = conf_repo_root();
        if (root)
            path = join(root, "restrictions.txt");
        free(root);
    }
    char *t = path ? read_file(path, NULL) : NULL;
    free(path);
    if (!t)
        return;
    char *sv = NULL;
    for (char *line = strtok_r(t, "\n", &sv); line; line = strtok_r(NULL, "\n", &sv)) {
        while (*line == ' ' || *line == '\t')
            line++;
        if (!*line || *line == '#')
            continue;
        cfg->checks = xrealloc(cfg->checks, (cfg->nchecks + 1) * sizeof *cfg->checks);
        cfg->checks[cfg->nchecks++] = xstrdup(line);
    }
    free(t);
}

/* The last turns, kept so a conversation goes on across commands. */
static void load_history(ctx_t *c)
{
    size_t n;
    char *t = read_file(c->history, &n);
    if (!t)
        return;
    json_t *v = json_parse(t, n, NULL, 0);
    free(t);
    for (size_t i = 0; v && v->t == J_ARR && i < v->n; i++) {
        json_t *p = jobj_get(v->items[i], "person"), *r = jobj_get(v->items[i], "reply");
        if (p && r && p->t == J_STR && r->t == J_STR)
            history_add(&c->h, p->s, r->s);
    }
    json_free(v);
}

static void save_history(ctx_t *c)
{
    json_t *a = jarr();
    size_t from = c->h.n > 8 ? c->h.n - 8 : 0;
    for (size_t i = from; i < c->h.n; i++) {
        json_t *o = jobj();
        jobj_set(o, "person", jstr(c->h.person[i]));
        jobj_set(o, "reply", jstr(c->h.reply[i]));
        jarr_push(a, o);
    }
    char *s = json_dump(a);
    json_free(a);
    FILE *f = fopen(c->history, "w");
    if (f) {
        fputs(s, f);
        fclose(f);
    }
    free(s);
}

/* One process at a time writes the learner's data: `talk`, `hear` and
 * `serve` hold an exclusive lock on its directory (flock on `lock`, the
 * holder's pid written in it). A second writer would interleave saves and
 * lose turns, so it is refused with the holder's pid. */
static int take_lock(const char *dir, char *err, size_t errlen)
{
    char *path = join(dir, "lock");
    int fd = open(path, O_RDWR | O_CREAT | O_CLOEXEC, 0600);
    if (fd < 0) {
        snprintf(err, errlen, "%s: %s", path, strerror(errno));
        free(path);
        return -1;
    }
    if (flock(fd, LOCK_EX | LOCK_NB)) {
        char pid[32] = "";
        ssize_t k = pread(fd, pid, sizeof pid - 1, 0);
        pid[k > 0 ? k : 0] = 0;
        snprintf(err, errlen,
                 "the learner is in use by pid %s (a learner talk or serve); stop it first, or talk "
                 "through it",
                 *pid ? pid : "?");
        close(fd);
        free(path);
        return -1;
    }
    char pid[32];
    int n = snprintf(pid, sizeof pid, "%d", (int)getpid());
    if (ftruncate(fd, 0) == 0 && pwrite(fd, pid, (size_t)n, 0) < 0)
        n = 0;
    free(path);
    return fd; /* held until the process exits */
}

static int open_ctx(ctx_t *c, bool write, char *err, size_t errlen)
{
    memset(c, 0, sizeof *c);
    c->dir = data_dir();
    if (mkdir_p(c->dir)) {
        snprintf(err, errlen, "%s: %s", c->dir, strerror(errno));
        return -1;
    }
    if (write && take_lock(c->dir, err, errlen) < 0)
        return -1;
    c->store = join(c->dir, "learner.store");
    c->curves = join(c->dir, "curves.jsonl");
    c->history = join(c->dir, "history.json");
    c->turns = join(c->dir, "turns.jsonl");
    struct stat st;
    if (stat(c->store, &st) == 0) {
        if (learner_load(c->store, &c->l, err, errlen))
            return -1;
    } else {
        const char *s = getenv("MJ2_SEED");
        c->l = learner_new(s && *s ? strtoull(s, NULL, 10) : DEFAULT_SEED);
    }
    if (c->l->circuit_fresh)
        fprintf(stderr, "warning: %s was learned with another input layout: its circuit is new and knows "
                        "nothing yet (learner circuit-rebuild retrains it from the replay)\n",
                c->store);
    long mh = conf_long("MJ2_MIN_HEARD", 0);
    if (mh > 0)
        c->l->min_heard = (uint32_t)mh;
    speak_cfg_default(&c->cfg);
    c->cfg.engine = conf_get("MJ2_ENGINE", conf_get("XKS_BIND", "127.0.0.1:8090"));
    sampling_t *s = &c->cfg.sampling;
    const char *v;
    if ((v = getenv("MJ2_TEMP")) && *v)
        s->temp = strtod(v, NULL);
    if ((v = getenv("MJ2_TOP_P")) && *v)
        s->top_p = strtod(v, NULL);
    if ((v = getenv("MJ2_MIN_P")) && *v)
        s->min_p = strtod(v, NULL);
    if ((v = getenv("MJ2_REPEAT_PENALTY")) && *v)
        s->repeat_penalty = strtod(v, NULL);
    if ((v = getenv("MJ2_TRUTH")) && *v)
        c->cfg.truth = strcmp(v, "0") != 0;
    if ((v = getenv("MJ2_KEEP_DROP")) && *v)
        c->cfg.keep_drop = strtod(v, NULL);
    if ((v = getenv("MJ2_LOOKAHEAD")) && *v)
        c->cfg.lookahead = (size_t)strtoul(v, NULL, 10);
    if ((v = getenv("MJ2_DRAW")) && *v)
        s->ties_only = strcmp(v, "sample") != 0; /* sample: drawn from the distribution */
    c->l->mem_weight = (float)DEFAULT_MEM_WEIGHT;
    if ((v = getenv("MJ2_RECENT_SHARE")) && *v)
        c->l->recent_share = strtof(v, NULL);
    c->l->ng_weight = (float)DEFAULT_NG_WEIGHT;
    if ((v = getenv("MJ2_NGRAM")) && *v)
        c->l->ng_weight = strtof(v, NULL);
    if ((v = getenv("MJ2_MEMORY")) && *v)
        c->l->mem_weight = strtof(v, NULL);
    if ((v = getenv("MJ2_TOP_K")) && *v)
        s->top_k = atoi(v);
    load_checks(&c->cfg);
    if (!c->cfg.nchecks) /* found nothing: every reply would pass unjudged (2026-10-17: a copied binary) */
        fprintf(stderr, "warning: no requirements loaded (restrictions.txt not found beside the checkout; "
                        "set MJ2_CHECKS): every reply passes unjudged\n");
    char *vars = getenv("MJ2_VARS") && *getenv("MJ2_VARS") ? expand_home(getenv("MJ2_VARS")) : NULL;
    if (!vars) {
        char *root = conf_repo_root();
        vars = root ? join(root, "variables.txt") : xstrdup("variables.txt");
        free(root);
    }
    int vr = vars_load(&c->cfg, c->l, vars, err, errlen);
    free(vars);
    if (vr)
        return -1;
    /* the n-gram memory, made from what MechJev has said (nothing stored) */
    size_t ng_turns, ng_steps;
    if (speak_ngram_rebuild(c->l, &c->cfg, c->turns, &ng_turns, &ng_steps, err, errlen))
        return -1;
    load_history(c);
    pthread_mutex_init(&c->mu, NULL);
    return 0;
}

static void close_ctx(ctx_t *c)
{
    learner_free(c->l);
    history_free(&c->h);
    for (size_t i = 0; i < c->cfg.nchecks; i++)
        free(c->cfg.checks[i]);
    free(c->cfg.checks);
    vars_free(&c->cfg);
    free(c->dir);
    free(c->store);
    free(c->curves);
    free(c->history);
    free(c->turns);
}

static int persist(ctx_t *c, const json_t *rec, char *err, size_t errlen)
{
    if (learner_save(c->l, c->store, err, errlen))
        return -1;
    save_history(c);
    if (!rec)
        return 0;
    /* curves.jsonl keeps the numbers, one row a turn; turns.jsonl the whole
     * record with every word's options, draw and seed. */
    FILE *f = fopen(c->curves, "a"), *g = fopen(c->turns, "a");
    if (!f || !g) {
        snprintf(err, errlen, "%s: %s", !f ? c->curves : c->turns, strerror(errno));
        if (f)
            fclose(f);
        if (g)
            fclose(g);
        return -1;
    }
    json_t *slim = jobj();
    for (size_t i = 0; i < rec->n; i++)
        if (strcmp(rec->keys[i], "trace"))
            jobj_set(slim, rec->keys[i], jclone(rec->items[i]));
    char *s = json_dump(slim), *w = json_dump(rec);
    fprintf(f, "%s\n", s);
    fprintf(g, "%s\n", w);
    free(s);
    free(w);
    json_free(slim);
    fclose(f);
    fclose(g);
    return 0;
}

/* The turn being answered, for the TUI whoever asked (the user's question,
 * 2026-10-17: "Why isn't the harness wired in?": probes and API turns had
 * shown nothing in it): `turn.json` {line, speaker, mode} in the data
 * directory while it runs, written before the first word, removed with the
 * reply so far when it ends. Under c->mu. */
static void turn_begin(const ctx_t *c, const char *line, const char *speaker, const char *mode)
{
    json_t *o = jobj();
    jobj_set(o, "line", jstr(line));
    jobj_set(o, "speaker", speaker && *speaker ? jstr(speaker) : jnull());
    jobj_set(o, "mode", jstr(mode));
    char *s = json_dump(o);
    json_free(o);
    char *tmp = join(c->dir, "turn.json.tmp"), *path = join(c->dir, "turn.json");
    FILE *f = fopen(tmp, "w");
    if (f) {
        fputs(s, f);
        if (fclose(f) == 0)
            rename(tmp, path);
    }
    free(s);
    free(tmp);
    free(path);
}

static void turn_end(const ctx_t *c)
{
    const char *names[] = {"speaking.txt", "speaker.txt", "turn.json"};
    for (size_t i = 0; i < sizeof names / sizeof *names; i++) {
        char *p = join(c->dir, names[i]);
        unlink(p);
        free(p);
    }
}

/* One turn; alone: the circuit speaks by itself this turn (the engine
 * perceives and judges, chooses no word, nothing is learned). */
static int one_turn(ctx_t *c, const char *message, bool alone, const char *show, const char *speaker, char **reply,
                    json_t **rec, char *err, size_t errlen)
{
    pthread_mutex_lock(&c->mu);
    speak_cfg_t cfg = c->cfg;
    cfg.alone = cfg.alone || alone;
    cfg.show = show; /* a teacher's demonstration: the answer's words offered too */
    cfg.parent = !(speaker && *speaker); /* no one else named: the person at the TUI, the parent */
    if (show && *show && !cfg.alone) { /* shown is heard: the words enter its lexicon, so they can be offered */
        char **w;
        size_t n = split_words(show, &w);
        learner_hear(c->l, w, n);
        free_words(w, n);
    }
    turn_begin(c, message, speaker, cfg.alone ? "alone" : "taught");
    int rc = speak_turn(c->l, &cfg, &c->h, message, reply, rec, err, errlen);
    turn_end(c); /* the turn is over: nothing is being said, no one waits */
    if (!rc && speaker && *speaker) /* who said it, for the TUI (the teacher: "Qwen") */
        jobj_set(*rec, "speaker", jstr(speaker));
    if (!rc)
        rc = persist(c, *rec, err, errlen);
    pthread_mutex_unlock(&c->mu);
    return rc;
}

/* The reply as it grows: written to `speaking.txt` in the data directory
 * after every word (the TUI shows it live, whoever asked), and on a
 * terminal, on one line. */
static bool g_terminal;

static void live_word(void *ud, const char *so_far)
{
    const ctx_t *c = ud;
    char *tmp = join(c->dir, "speaking.txt.tmp"), *path = join(c->dir, "speaking.txt");
    FILE *f = fopen(tmp, "w");
    if (f) {
        fputs(so_far, f);
        if (fclose(f) == 0)
            rename(tmp, path);
    }
    free(tmp);
    free(path);
    if (g_terminal) {
        printf("\r\033[Klearner: %s", so_far);
        fflush(stdout);
    }
}

static double num(const json_t *o, const char *k)
{
    const json_t *v = jobj_get(o, k);
    return v && v->t == J_NUM ? v->f : 0;
}

static void say_turn(const char *reply, const json_t *rec)
{
    printf("\r\033[Klearner: %s\n", reply && *reply ? reply : "(no words yet)");
    const json_t *ev = jobj_get(rec, "eval"), *nv = jobj_get(rec, "eval_no_variables"),
                 *np = jobj_get(rec, "eval_no_prev_word");
    fprintf(stderr,
            "  %.0f words (%s), %.0f known, %.0f it can say; the circuit's first word was the engine's "
            "on %.0f%% (%.0f%% without the variables, %.0f%% without the word before); %.1f s a word%s\n",
            num(rec, "words"), jobj_get(rec, "length_planned")->s, num(rec, "lexicon"), num(rec, "speakable"),
            100 * num(ev, "agree"), 100 * num(nv, "agree"), 100 * num(np, "agree"),
            num(rec, "ms_per_word") / 1e3,
            jobj_get(rec, "failed_requirement")->b ? "; failed a requirement (learned from the scores, not as a right answer)" : "");
    /* each word with its per-token score: the change it made in the coherence */
    const json_t *tr = jobj_get(rec, "trace");
    sb_t s;
    sb_init(&s);
    for (size_t i = 0; tr && i < tr->n; i++) {
        const json_t *p = jobj_get(tr->items[i], "pick"), *r = jobj_get(tr->items[i], "reward");
        if (p && p->t == J_STR && r && r->t == J_NUM)
            sb_printf(&s, " %s %+.2f", p->s, r->f);
    }
    if (s.n)
        fprintf(stderr, "  per word:%s; coherence %.2f\n", s.s, num(rec, "coherence_final"));
    sb_free(&s);
}

static int cmd_export(ctx_t *c, int argc, char **argv)
{
    if (argc < 1) {
        fprintf(stderr, "error: learner export FILE.c (the discrete circuit as C)\n");
        return 2;
    }
    FILE *f = fopen(argv[0], "w");
    if (!f || circuit_export(&c->l->circ, f, "mechjev_circuit") || fclose(f)) {
        fprintf(stderr, "error: %s: %s\n", argv[0], strerror(errno));
        return 1;
    }
    fprintf(stderr, "learner: the circuit (%d inputs, %d layers) written to %s\n", c->l->circ.n_in,
            c->l->circ.n_layers, argv[0]);
    return 0;
}

/* The acquisition curve's instrument: fixed questions (`probes.txt`,
 * `MJ2_PROBES`) put to the circuit alone, each from an empty conversation.
 * The engine perceives and judges as in any turn; the learner hears
 * nothing, learns nothing and counts no turn, so probing changes nothing it
 * measures. One row per probe in `probes.jsonl`, stamped with the turns the
 * learner had had. */
/* One message to the circuit alone, from an empty conversation, the learner
 * hearing, learning and counting nothing (the probe path): the outside
 * teacher's try (teach.md). *out is the turn record. */
static int probe_one(ctx_t *c, const char *msg, json_t **out, char *err, size_t errlen)
{
    history_t empty = {0};
    speak_cfg_t cfg = c->cfg;
    cfg.alone = cfg.probe = true;
    char *reply = NULL;
    pthread_mutex_lock(&c->mu);
    turn_begin(c, msg, NULL, "probe");
    int rc = speak_turn(c->l, &cfg, &empty, msg, &reply, out, err, errlen);
    turn_end(c);
    pthread_mutex_unlock(&c->mu);
    history_free(&empty);
    free(reply);
    return rc;
}

/* ng_weight >= 0: the n-gram memory at that weight for these probes only
 * (POST /v1/probe {"ngram_weight"}: the weights A/B against the live
 * learner, its own weight kept for every other turn). */
static int run_probes(ctx_t *c, double ng_weight, json_t **out, char *err, size_t errlen)
{
    char *path = NULL;
    const char *p = getenv("MJ2_PROBES");
    if (p && *p)
        path = expand_home(p);
    else {
        char *root = conf_repo_root();
        path = root ? join(root, "probes.txt") : xstrdup("probes.txt");
        free(root);
    }
    char *text = read_file(path, NULL);
    if (!text) {
        snprintf(err, errlen, "%s: unreadable (the probe questions, one a line)", path);
        free(path);
        return -1;
    }
    free(path);
    char *file = join(c->dir, "probes.jsonl");
    json_t *rows = jarr();
    int rc = 0;
    time_t now = time(NULL);
    char stamp[32];
    strftime(stamp, sizeof stamp, "%Y-%m-%dT%H:%M:%SZ", gmtime(&now));
    char *sv = NULL;
    /* Each question several times: the variable bits are drawn with a new
     * clock seed every time, and one answer a question could not tell
     * learning from luck (2026-10-17: 3 of 4 passed after 49 turns, 1 of 4
     * after 59, one sample each). */
    long repeats = conf_long("MJ2_PROBE_REPEATS", 3);
    for (char *q = strtok_r(text, "\n", &sv); q && !rc; q = strtok_r(NULL, "\n", &sv)) {
        while (*q == ' ' || *q == '\t')
            q++;
        if (!*q || *q == '#')
            continue;
        for (long rep = 0; rep < repeats && !rc; rep++) {
            history_t empty = {0};
            speak_cfg_t cfg = c->cfg;
            cfg.alone = cfg.probe = true;
            char *reply = NULL;
            json_t *rec = NULL;
            pthread_mutex_lock(&c->mu);
            float keep = c->l->ng_weight;
            if (ng_weight >= 0)
                c->l->ng_weight = (float)ng_weight;
            float used_ng = c->l->ng_weight;
            turn_begin(c, q, NULL, "probe");
            rc = speak_turn(c->l, &cfg, &empty, q, &reply, &rec, err, errlen);
            turn_end(c);
            c->l->ng_weight = keep;
            size_t turns = c->l->turns, lexicon = c->l->n, say = lex_speakable_count(c->l);
            pthread_mutex_unlock(&c->mu);
            char *sp = join(c->dir, "speaking.txt");
            unlink(sp);
            free(sp);
            history_free(&empty);
            if (rc)
                break;
            json_t *row = jobj();
            jobj_set(row, "time", jstr(stamp));
            jobj_set(row, "turns", ju64(turns));
            jobj_set(row, "lexicon", ju64(lexicon));
            jobj_set(row, "speakable", ju64(say));
            jobj_set(row, "probe", jstr(q));
            jobj_set(row, "repeat", ju64((uint64_t)rep));
            jobj_set(row, "memory_weight", jf64(c->l->mem_weight));
            jobj_set(row, "ngram_weight", jf64(used_ng));
            jobj_set(row, "reply", jstr(reply));
            jobj_set(row, "words", jclone(jobj_get(rec, "words")));
            jobj_set(row, "coherence", jclone(jobj_get(rec, "coherence_final")));
            jobj_set(row, "failed_requirement", jclone(jobj_get(rec, "failed_requirement")));
            jobj_set(row, "checks", jclone(jobj_get(rec, "checks")));
            char *line = json_dump(row);
            FILE *f = fopen(file, "a");
            if (f) {
                fprintf(f, "%s\n", line);
                fclose(f);
            }
            free(line);
            jarr_push(rows, row);
            free(reply);
            json_free(rec);
        }
    }
    free(text);
    free(file);
    if (rc) {
        json_free(rows);
        return -1;
    }
    *out = rows;
    return 0;
}

static void print_record(const json_t *rec)
{
    sb_t b;
    sb_init(&b);
    json_write_pretty(&b, rec);
    puts(b.s);
    sb_free(&b);
}

static int cmd_talk(ctx_t *c, int argc, char **argv)
{
    char err[1024];
    bool trace = false; /* the whole record after each turn */
    while (argc > 0 && argv[0][0] == '-' && argv[0][1] == '-') {
        if (!strcmp(argv[0], "--trace"))
            trace = true;
        else if (!strcmp(argv[0], "--alone"))
            c->cfg.alone = true; /* the circuit speaks by itself */
        else {
            fprintf(stderr, "error: talk takes --trace and --alone, not %s\n", argv[0]);
            return 2;
        }
        argc--;
        argv++;
    }
    if (argc > 0) {
        sb_t m;
        sb_init(&m);
        for (int i = 0; i < argc; i++) {
            if (i)
                sb_putc(&m, ' ');
            sb_puts(&m, argv[i]);
        }
        char *reply;
        json_t *rec;
        if (one_turn(c, m.s, false, NULL, NULL, &reply, &rec, err, sizeof err)) {
            fprintf(stderr, "error: %s\n", err);
            return 1;
        }
        say_turn(reply, rec);
        if (trace)
            print_record(rec);
        free(reply);
        json_free(rec);
        sb_free(&m);
        return 0;
    }
    char line[8192];
    for (;;) {
        printf("you> ");
        fflush(stdout);
        if (!fgets(line, sizeof line, stdin))
            break;
        size_t n = strlen(line);
        while (n && (line[n - 1] == '\n' || line[n - 1] == '\r'))
            line[--n] = 0;
        if (!n)
            continue;
        char *reply;
        json_t *rec;
        if (one_turn(c, line, false, NULL, NULL, &reply, &rec, err, sizeof err)) {
            fprintf(stderr, "error: %s\n", err);
            continue;
        }
        say_turn(reply, rec);
        if (trace)
            print_record(rec);
        free(reply);
        json_free(rec);
    }
    return 0;
}

/* Sentences of a text: split at . ? ! and blank lines. */
static size_t hear_text(learner_t *l, const char *text)
{
    size_t sentences = 0;
    const char *s = text;
    while (*s) {
        const char *e = s;
        while (*e && !strchr(".?!", *e) && !(e[0] == '\n' && e[1] == '\n'))
            e++;
        if (*e && strchr(".?!", *e))
            e++;
        char *one = xstrndup(s, (size_t)(e - s));
        char **w;
        size_t n = split_words(one, &w);
        if (n) {
            learner_hear(l, w, n);
            sentences++;
        }
        free_words(w, n);
        free(one);
        s = *e ? e + (e[0] == '\n') : e;
    }
    return sentences;
}

static int cmd_hear(ctx_t *c, int argc, char **argv)
{
    char err[1024];
    if (!argc) {
        fprintf(stderr, "error: learner hear FILE... (English text to read)\n");
        return 2;
    }
    for (int i = 0; i < argc; i++) {
        size_t n;
        char *t = read_file(argv[i], &n);
        if (!t) {
            fprintf(stderr, "error: %s: unreadable\n", argv[i]);
            return 1;
        }
        size_t before = c->l->n;
        double t0 = now_ms();
        size_t s = hear_text(c->l, t);
        free(t);
        fprintf(stderr, "learner: heard %zu sentences of %s in %.1f s; %zu new words, %zu known, %zu it can say\n",
                s, argv[i], (now_ms() - t0) / 1e3, c->l->n - before, c->l->n, lex_speakable_count(c->l));
    }
    if (persist(c, NULL, err, sizeof err)) {
        fprintf(stderr, "error: %s\n", err);
        return 1;
    }
    return 0;
}

static json_t *stats_json(ctx_t *c)
{
    json_t *o = jobj();
    jobj_set(o, "lexicon", ju64(c->l->n));
    jobj_set(o, "speakable", ju64(lex_speakable_count(c->l)));
    size_t used = 0;
    for (int i = 0; i < VAR_SLOTS; i++)
        used += c->l->slot[i] != NULL;
    jobj_set(o, "variable_slots", ju64(used));
    jobj_set(o, "ngram_entries", ju64(c->l->ng.n));
    jobj_set(o, "ngram_weight", jf64(c->l->ng_weight));
    json_t *layers = jarr();
    for (int i = 0; i < c->l->circ.n_layers; i++)
        jarr_push(layers, ju64((uint64_t)c->l->circ.size[i]));
    jobj_set(o, "circuit_inputs", ju64((uint64_t)c->l->circ.n_in));
    jobj_set(o, "circuit_layers", layers);
    jobj_set(o, "turns", ju64(c->l->turns));
    jobj_set(o, "steps", ju64(c->l->steps));
    jobj_set(o, "replay", ju64(c->l->nreplay));
    jobj_set(o, "trained_passes", ju64(c->l->trained));
    jobj_set(o, "min_heard", ju64(c->l->min_heard));
    jobj_set(o, "seed", ju64(c->l->seed));
    jobj_set(o, "engine", jstr(c->cfg.engine));
    jobj_set(o, "store", jstr(c->store));
    jobj_set(o, "curves", jstr(c->curves));
    /* the most used words, the way a person reads what it knows */
    size_t k = c->l->n < 20 ? c->l->n : 20;
    int32_t *top = xmalloc((k ? k : 1) * sizeof *top);
    size_t m = 0;
    for (size_t i = 0; i < c->l->n; i++) {
        const lex_t *x = &c->l->v[i];
        if (!strcmp(x->word, START) || !strcmp(x->word, END) || is_punct(x->word))
            continue;
        uint32_t use = x->heard + x->picked;
        size_t at = m;
        while (at > 0 && c->l->v[top[at - 1]].heard + c->l->v[top[at - 1]].picked < use)
            at--;
        if (at >= k)
            continue;
        size_t last = m < k ? m : k - 1;
        memmove(&top[at + 1], &top[at], (last - at) * sizeof *top);
        top[at] = (int32_t)i;
        if (m < k)
            m++;
    }
    json_t *tw = jarr();
    for (size_t i = 0; i < m; i++) {
        json_t *w = jobj();
        jobj_set(w, "word", jstr(c->l->v[top[i]].word));
        jobj_set(w, "heard", ju64(c->l->v[top[i]].heard));
        jobj_set(w, "said", ju64(c->l->v[top[i]].picked));
        jarr_push(tw, w);
    }
    free(top);
    jobj_set(o, "top_words", tw);
    return o;
}

/* ---- the HTTP API ---------------------------------------------------- */

typedef struct {
    ctx_t *c;
    int fd;
} conn_t;

static void reply_json(int fd, int status, json_t *v)
{
    char *s = json_dump(v);
    http_respond(fd, status, s, NULL);
    free(s);
    json_free(v);
}

static json_t *message(const char *m)
{
    json_t *o = jobj();
    jobj_set(o, "message", jstr(m));
    return o;
}

static void *serve_conn(void *arg)
{
    conn_t *k = arg;
    ctx_t *c = k->c;
    http_req_t r;
    if (http_read(k->fd, &r)) {
        http_respond(k->fd, 400, "{\"message\":\"unreadable request\"}", NULL);
        close(k->fd);
        free(k);
        return NULL;
    }
    char err[1024];
    bool get = !strcmp(r.method, "GET"), post = !strcmp(r.method, "POST");
    if (get && !strcmp(r.path, "/health")) {
        json_t *o = jobj();
        jobj_set(o, "status", jstr("ok"));
        jobj_set(o, "engine", jstr(c->cfg.engine));
        reply_json(k->fd, 200, o);
    } else if (get && !strcmp(r.path, "/v1/stats")) {
        pthread_mutex_lock(&c->mu);
        json_t *o = stats_json(c);
        pthread_mutex_unlock(&c->mu);
        reply_json(k->fd, 200, o);
    } else if (post && !strcmp(r.path, "/v1/next")) {
        /* {"show", "alone", "speaker"}: the next /v1/talk turn's settings, used
         * once: the teacher types its line into the TUI as a person would,
         * and its turn is labelled, shows its answer words, or is MechJev's
         * alone */
        json_t *v = json_parse(r.body ? r.body : "", r.body_len, err, sizeof err);
        if (!v || v->t != J_OBJ)
            reply_json(k->fd, 422, message("POST {\"show\": ..., \"alone\": true, \"speaker\": ...}"));
        else {
            const json_t *sh = jobj_get(v, "show"), *al = jobj_get(v, "alone"), *sp = jobj_get(v, "speaker");
            pthread_mutex_lock(&c->mu);
            free(c->next_show);
            free(c->next_speaker);
            c->next_show = sh && sh->t == J_STR ? xstrdup(sh->s) : NULL;
            c->next_speaker = sp && sp->t == J_STR ? xstrdup(sp->s) : NULL;
            c->next_alone = al && al->t == J_BOOL && al->b;
            /* who will speak, for the TUI from the first frame of the turn */
            char *spf = join(c->dir, "speaker.txt");
            if (c->next_speaker) {
                FILE *f = fopen(spf, "w");
                if (f) {
                    fputs(c->next_speaker, f);
                    fclose(f);
                }
            } else
                unlink(spf);
            free(spf);
            pthread_mutex_unlock(&c->mu);
            json_t *o = jobj();
            jobj_set(o, "ok", jbool(true));
            reply_json(k->fd, 200, o);
        }
        json_free(v);
    } else if (post && !strcmp(r.path, "/v1/probe")) {
        /* {"message": ...}: that message alone (the teacher's try); else the
         * fixed probes (probes.txt); {"ngram_weight": W}: the n-gram memory
         * at W for them only (tools/ngram_ab.sh) */
        json_t *v = r.body_len ? json_parse(r.body, r.body_len, err, sizeof err) : NULL;
        const json_t *m = v ? jobj_get(v, "message") : NULL;
        json_t *rows = NULL;
        const json_t *nw = v ? jobj_get(v, "ngram_weight") : NULL;
        double ngw = nw && nw->t == J_NUM ? nw->f : -1;
        int rc = m && m->t == J_STR ? probe_one(c, m->s, &rows, err, sizeof err)
                                     : run_probes(c, ngw, &rows, err, sizeof err);
        if (rc)
            reply_json(k->fd, 502, message(err));
        else
            reply_json(k->fd, 200, rows);
        json_free(v);
    } else if (post && (!strcmp(r.path, "/v1/talk") || !strcmp(r.path, "/v1/hear"))) {
        json_t *v = json_parse(r.body ? r.body : "", r.body_len, err, sizeof err);
        const json_t *m = v ? jobj_get(v, !strcmp(r.path, "/v1/talk") ? "message" : "text") : NULL;
        if (!m || m->t != J_STR) {
            reply_json(k->fd, 422, message(!strcmp(r.path, "/v1/talk") ? "POST {\"message\": \"...\"}"
                                                                         : "POST {\"text\": \"...\"}"));
        } else if (!strcmp(r.path, "/v1/talk")) {
            char *reply;
            json_t *rec;
            const json_t *al = jobj_get(v, "alone");
            const json_t *sh = jobj_get(v, "show");
            /* the pending settings from POST /v1/next, used by this turn once */
            pthread_mutex_lock(&c->mu);
            char *n_show = c->next_show, *n_speaker = c->next_speaker;
            bool n_alone = c->next_alone;
            c->next_show = c->next_speaker = NULL;
            c->next_alone = false;
            pthread_mutex_unlock(&c->mu);
            const char *show = sh && sh->t == J_STR ? sh->s : n_show;
            int trc = one_turn(c, m->s, (al && al->t == J_BOOL && al->b) || n_alone, show, n_speaker, &reply, &rec,
                               err, sizeof err);
            free(n_show);
            free(n_speaker);
            if (trc)
                reply_json(k->fd, 502, message(err));
            else {
                free(reply);
                reply_json(k->fd, 200, rec);
            }
        } else {
            pthread_mutex_lock(&c->mu);
            size_t before = c->l->n;
            size_t s = hear_text(c->l, m->s);
            int rc = persist(c, NULL, err, sizeof err);
            json_t *o = jobj();
            jobj_set(o, "sentences", ju64(s));
            jobj_set(o, "new_words", ju64(c->l->n - before));
            jobj_set(o, "lexicon", ju64(c->l->n));
            jobj_set(o, "speakable", ju64(lex_speakable_count(c->l)));
            pthread_mutex_unlock(&c->mu);
            if (rc)
                reply_json(k->fd, 502, message(err)), json_free(o);
            else
                reply_json(k->fd, 200, o);
        }
        json_free(v);
    } else {
        reply_json(k->fd, 404, message("routes: GET /health, GET /v1/stats, POST /v1/talk {message}, "
                                       "POST /v1/hear {text}, POST /v1/probe; talk takes \"alone\": true for the circuit by itself"));
    }
    http_req_free(&r);
    close(k->fd);
    free(k);
    return NULL;
}

static int cmd_serve(ctx_t *c, const char *bind)
{
    int lfd = http_listen(bind);
    if (lfd < 0) {
        fprintf(stderr, "error: bind %s: %s\n", bind, strerror(errno));
        return 1;
    }
    signal(SIGPIPE, SIG_IGN);
    fprintf(stderr, "learner listening on http://%s  (engine %s, store %s)\n", bind, c->cfg.engine, c->store);
    for (;;) {
        int fd = accept4(lfd, NULL, NULL, SOCK_CLOEXEC);
        if (fd < 0) {
            if (errno == EINTR)
                continue;
            fprintf(stderr, "error: accept: %s\n", strerror(errno));
            return 1;
        }
        conn_t *k = xmalloc(sizeof *k);
        k->c = c;
        k->fd = fd;
        pthread_t t;
        if (pthread_create(&t, NULL, serve_conn, k)) {
            close(fd);
            free(k);
            continue;
        }
        pthread_detach(t);
    }
}

static void usage(void)
{
    fputs("learner (MechJev-v2): a language learner taught by the decision engine's choices\n"
          "  learner talk [--trace] [--alone] [MESSAGE]  one turn, or a conversation; --alone:\n"
          "                              the circuit speaks with no engine choice\n"
          "  learner hear FILE...        read English text\n"
          "  learner stats               what it knows\n"
          "  learner export FILE.c       the discrete circuit as C\n"
          "  learner probe               the fixed questions to the circuit alone (probes.jsonl)\n"
          "  learner memory-rebuild      the fast memory made again from turns.jsonl (prints its agreement)\n"
          "  learner circuit-rebuild [P] the circuit made new and retrained from the replay (P passes)\n"
          "  learner serve [--bind B]    the HTTP API (default 127.0.0.1:8092)\n"
          "  learner reset --yes         forget everything\n"
          "The engine (xks) must be up: make serve.\n",
          stderr);
}

/* learner circuit-rebuild [PASSES]: the circuit made new and retrained from
 * the replay (after the input layout changes, or to start the circuit over
 * without forgetting the examples). The examples of the last HELD_TURNS
 * taught turns are held out first: trained on the rest for PASSES (default:
 * as many as the old circuit had), the new circuit is measured on them with
 * every input, the variables blanked and the word before blanked, beside
 * what the old circuit measured on the same turns before learning them
 * (turns.jsonl). Then a quarter more passes over everything, and saved. */
#define HELD_TURNS 10

static void eval_line(const char *who, double agree, double no_vars, double no_prev, size_t n)
{
    printf("  %-34s agree %.3f, variables blanked %.3f, word before blanked %.3f (%zu words)\n", who, agree,
           no_vars, no_prev, n);
}

static int cmd_circuit_rebuild(ctx_t *c, int argc, char **argv)
{
    learner_t *l = c->l;
    size_t passes = argc > 0 ? (size_t)strtoull(argv[0], NULL, 10) : (size_t)l->trained;
    if (!passes)
        passes = 200000;
    /* the last HELD_TURNS taught turns: their examples are the replay's last */
    FILE *f = fopen(c->turns, "r");
    size_t steps[HELD_TURNS] = {0}, seen = 0;
    double old[HELD_TURNS][3] = {{0}};
    char *line = NULL;
    size_t cap = 0;
    ssize_t len;
    while (f && (len = getline(&line, &cap, f)) > 0) {
        char perr[64];
        json_t *r = json_parse(line, (size_t)len, perr, sizeof perr);
        const json_t *mode = r ? jobj_get(r, "mode") : NULL;
        if (mode && mode->t == J_STR && !strcmp(mode->s, "taught")) {
            size_t k = seen++ % HELD_TURNS;
            steps[k] = (size_t)num(r, "steps");
            const char *ev[3] = {"eval", "eval_no_variables", "eval_no_prev_word"};
            for (int e = 0; e < 3; e++)
                old[k][e] = num(jobj_get(r, ev[e]), "agree");
        }
        json_free(r);
    }
    free(line);
    if (f)
        fclose(f);
    size_t hold = 0;
    double sum[3] = {0};
    for (size_t k = 0; k < HELD_TURNS && k < seen; k++) {
        hold += steps[k];
        for (int e = 0; e < 3; e++)
            sum[e] += old[k][e] * (double)steps[k];
    }
    if (hold >= l->nreplay)
        hold = 0;
    size_t keep = l->nreplay;
    double t0 = now_ms();
    learner_circuit_reset(l);
    l->nreplay = keep - hold;
    size_t done = learner_learn(l, NULL, 0, 1e15, passes);
    l->nreplay = keep;
    printf("circuit-rebuild: %d input bits a variable; %zu passes over %zu examples (%.0f s)\n", VAR_BITS, done,
           keep - hold, (now_ms() - t0) / 1000);
    if (hold) {
        const example_t *held = &l->replay[keep - hold];
        evalres_t a = learner_eval(l, held, hold, ABL_NONE), b = learner_eval(l, held, hold, ABL_CONCEPTS),
                  p = learner_eval(l, held, hold, ABL_PREV);
        printf("held out: the last %zu taught turns (%zu words)\n", seen < HELD_TURNS ? seen : (size_t)HELD_TURNS,
               hold);
        eval_line("the new circuit, before them", a.agree, b.agree, p.agree, hold);
        eval_line("the old circuit, before each (record)", sum[0] / (double)hold, sum[1] / (double)hold,
                  sum[2] / (double)hold, hold);
    }
    done += learner_learn(l, NULL, 0, 1e15, passes / 4);
    l->circuit_fresh = false;
    char err[256];
    if (learner_save(l, c->store, err, sizeof err)) {
        fprintf(stderr, "error: %s\n", err);
        return 1;
    }
    printf("saved %s (%zu passes in all, %.0f s)\n", c->store, done, (now_ms() - t0) / 1000);
    return 0;
}

int main(int argc, char **argv)
{
    conf_load();
    if (argc < 2) {
        usage();
        return 2;
    }
    ctx_t c;
    char err[1024];
    const char *cmd = argv[1];
    if (!strcmp(cmd, "reset")) {
        if (argc < 3 || strcmp(argv[2], "--yes")) {
            fprintf(stderr, "learner reset removes everything it has learned; run it with --yes\n");
            return 2;
        }
        char *dir = data_dir();
        mkdir_p(dir);
        if (take_lock(dir, err, sizeof err) < 0) {
            fprintf(stderr, "error: %s\n", err);
            free(dir);
            return 1;
        }
        const char *files[] = {"learner.store", "history.json"};
        for (size_t i = 0; i < 2; i++) {
            char *p = join(dir, files[i]);
            if (!unlink(p))
                fprintf(stderr, "removed %s\n", p);
            free(p);
        }
        free(dir);
        return 0;
    }
    /* stats and export only read (the store is replaced by rename, never
     * half written); the rest write and take the lock */
    bool write = strcmp(cmd, "stats") && strcmp(cmd, "export");
    if (open_ctx(&c, write, err, sizeof err)) {
        fprintf(stderr, "error: %s\n", err);
        return 1;
    }
    int rc;
    c.cfg.on_word = live_word;
    c.cfg.on_word_ud = &c;
    g_terminal = !strcmp(cmd, "talk");
    if (!strcmp(cmd, "talk"))
        rc = cmd_talk(&c, argc - 2, argv + 2);
    else if (!strcmp(cmd, "export"))
        rc = cmd_export(&c, argc - 2, argv + 2);
    else if (!strcmp(cmd, "probe")) {
        json_t *rows;
        rc = run_probes(&c, -1, &rows, err, sizeof err) ? 1 : 0;
        if (rc)
            fprintf(stderr, "error: %s\n", err);
        for (size_t i = 0; !rc && i < rows->n; i++) {
            const json_t *r = rows->items[i];
            printf("probe: %s\nalone: %s\n", jobj_get(r, "probe")->s, jobj_get(r, "reply")->s);
            const json_t *co = jobj_get(r, "coherence"), *fr = jobj_get(r, "failed_requirement");
            if (co && co->t == J_NUM)
                fprintf(stderr, "  coherence %.2f, requirements %s\n", co->f,
                        fr && fr->t == J_BOOL && fr->b ? "failed" : "passed");
            else
                fprintf(stderr, "  (no words)\n");
        }
        if (!rc)
            json_free(rows);
    } else if (!strcmp(cmd, "circuit-rebuild"))
        rc = cmd_circuit_rebuild(&c, argc - 2, argv + 2);
    else if (!strcmp(cmd, "memory-rebuild")) {
        json_t *rep = NULL;
        rc = speak_memory_rebuild(c.l, &c.cfg, c.turns, &rep, err, sizeof err) || learner_save(c.l, c.store, err, sizeof err) ? 1 : 0;
        if (rc)
            fprintf(stderr, "error: %s\n", err);
        if (rep) {
            sb_t b;
            sb_init(&b);
            json_write_pretty(&b, rep);
            puts(b.s);
            sb_free(&b);
            json_free(rep);
        }
    } else if (!strcmp(cmd, "hear"))
        rc = cmd_hear(&c, argc - 2, argv + 2);
    else if (!strcmp(cmd, "stats")) {
        json_t *o = stats_json(&c);
        sb_t b;
        sb_init(&b);
        json_write_pretty(&b, o);
        puts(b.s);
        sb_free(&b);
        json_free(o);
        rc = 0;
    } else if (!strcmp(cmd, "serve")) {
        const char *bind = conf_get("MJ2_BIND", "127.0.0.1:8092");
        if (argc >= 4 && !strcmp(argv[2], "--bind"))
            bind = argv[3];
        rc = cmd_serve(&c, bind);
    } else {
        usage();
        rc = 2;
    }
    close_ctx(&c);
    return rc;
}
