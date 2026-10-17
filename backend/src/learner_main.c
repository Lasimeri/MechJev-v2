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
#include <errno.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>

#include "conf.h"
#include "http.h"
#include "json.h"
#include "learner.h"
#include "speak.h"
#include "util.h"

#define DEFAULT_SEED 20261017ull

typedef struct {
    char *dir, *store, *curves, *history;
    learner_t *l;
    speak_cfg_t cfg;
    history_t h;
    pthread_mutex_t mu;
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

static int open_ctx(ctx_t *c, char *err, size_t errlen)
{
    memset(c, 0, sizeof *c);
    c->dir = data_dir();
    if (mkdir_p(c->dir)) {
        snprintf(err, errlen, "%s: %s", c->dir, strerror(errno));
        return -1;
    }
    c->store = join(c->dir, "learner.store");
    c->curves = join(c->dir, "curves.jsonl");
    c->history = join(c->dir, "history.json");
    struct stat st;
    if (stat(c->store, &st) == 0) {
        if (learner_load(c->store, &c->l, err, errlen))
            return -1;
    } else {
        const char *s = getenv("MJ2_SEED");
        c->l = learner_new(s && *s ? strtoull(s, NULL, 10) : DEFAULT_SEED);
    }
    long mh = conf_long("MJ2_MIN_HEARD", 0);
    if (mh > 0)
        c->l->min_heard = (uint32_t)mh;
    speak_cfg_default(&c->cfg);
    c->cfg.engine = conf_get("MJ2_ENGINE", conf_get("XKS_BIND", "127.0.0.1:8090"));
    load_checks(&c->cfg);
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
    free(c->dir);
    free(c->store);
    free(c->curves);
    free(c->history);
}

static int persist(ctx_t *c, const json_t *rec, char *err, size_t errlen)
{
    if (learner_save(c->l, c->store, err, errlen))
        return -1;
    save_history(c);
    if (rec) {
        FILE *f = fopen(c->curves, "a");
        if (!f) {
            snprintf(err, errlen, "%s: %s", c->curves, strerror(errno));
            return -1;
        }
        json_t *row = jclone(rec);
        /* the curve keeps the numbers; the per-word trace stays in the reply */
        json_t *slim = jobj();
        for (size_t i = 0; i < row->n; i++)
            if (strcmp(row->keys[i], "trace"))
                jobj_set(slim, row->keys[i], jclone(row->items[i]));
        char *s = json_dump(slim);
        fprintf(f, "%s\n", s);
        free(s);
        json_free(slim);
        json_free(row);
        fclose(f);
    }
    return 0;
}

static int one_turn(ctx_t *c, const char *message, char **reply, json_t **rec, char *err, size_t errlen)
{
    pthread_mutex_lock(&c->mu);
    int rc = speak_turn(c->l, &c->cfg, &c->h, message, reply, rec, err, errlen);
    if (!rc)
        rc = persist(c, *rec, err, errlen);
    pthread_mutex_unlock(&c->mu);
    return rc;
}

static void say_turn(const char *reply, const json_t *rec)
{
    const json_t *w = jobj_get(rec, "words");
    if (reply && *reply)
        printf("learner: %s\n", reply);
    else
        printf("learner: (no words yet)\n");
    fprintf(stderr, "  %llu words, %llu known, %llu it can say; agreed with the engine on %.0f%% of words; "
                    "%.1f s a word\n",
            (unsigned long long)(w ? w->u : 0), (unsigned long long)jobj_get(rec, "lexicon")->u,
            (unsigned long long)jobj_get(rec, "speakable")->u, 100 * jobj_get(rec, "agree_top1")->f,
            jobj_get(rec, "ms_per_word")->f / 1e3);
}

static int cmd_talk(ctx_t *c, int argc, char **argv)
{
    char err[1024];
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
        if (one_turn(c, m.s, &reply, &rec, err, sizeof err)) {
            fprintf(stderr, "error: %s\n", err);
            return 1;
        }
        say_turn(reply, rec);
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
        if (one_turn(c, line, &reply, &rec, err, sizeof err)) {
            fprintf(stderr, "error: %s\n", err);
            continue;
        }
        say_turn(reply, rec);
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
            learner_hear(l, w, n, w, n);
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
    size_t protos = 0;
    for (size_t i = 0; i < c->l->n; i++)
        protos += c->l->v[i].proto != NULL;
    jobj_set(o, "with_prototype", ju64(protos));
    jobj_set(o, "turns", ju64(c->l->turns));
    jobj_set(o, "steps", ju64(c->l->steps));
    jobj_set(o, "updates", ju64(c->l->updates));
    jobj_set(o, "min_heard", ju64(c->l->min_heard));
    jobj_set(o, "seed", ju64(c->l->seed));
    jobj_set(o, "bits", ju64(HD_BITS));
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
    } else if (post && (!strcmp(r.path, "/v1/talk") || !strcmp(r.path, "/v1/hear"))) {
        json_t *v = json_parse(r.body ? r.body : "", r.body_len, err, sizeof err);
        const json_t *m = v ? jobj_get(v, !strcmp(r.path, "/v1/talk") ? "message" : "text") : NULL;
        if (!m || m->t != J_STR) {
            reply_json(k->fd, 422, message(!strcmp(r.path, "/v1/talk") ? "POST {\"message\": \"...\"}"
                                                                         : "POST {\"text\": \"...\"}"));
        } else if (!strcmp(r.path, "/v1/talk")) {
            char *reply;
            json_t *rec;
            if (one_turn(c, m->s, &reply, &rec, err, sizeof err))
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
                                       "POST /v1/hear {text}"));
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
          "  learner talk [MESSAGE]      one turn, or a conversation on the terminal\n"
          "  learner hear FILE...        read English text\n"
          "  learner stats               what it knows\n"
          "  learner serve [--bind B]    the HTTP API (default 127.0.0.1:8092)\n"
          "  learner reset --yes         forget everything\n"
          "The engine (xks) must be up: make serve.\n",
          stderr);
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
    if (open_ctx(&c, err, sizeof err)) {
        fprintf(stderr, "error: %s\n", err);
        return 1;
    }
    int rc;
    if (!strcmp(cmd, "talk"))
        rc = cmd_talk(&c, argc - 2, argv + 2);
    else if (!strcmp(cmd, "hear"))
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
