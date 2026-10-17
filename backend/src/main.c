/* main.c: the `xks` command of MechJev-v2, the decision engine in C. The
 * same command line Mechanical Jev consumes from Intel Phi Jev's xks:
 * `serve [--detach] [--bind B] [--kill-date S]`, `stop`, `/health`,
 * `doctor`; and `query`, `polygraph` for the checks. See main.md. */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#include "conf.h"
#include "engine.h"
#include "http.h"
#include "json.h"
#include "judge.h"
#include "util.h"

/* ---- options --------------------------------------------------------- */

typedef struct {
    const char *subject;
    engine_opts_t eng;
    judge_cfg_t judge;
} setup_t;

static int setup(setup_t *s, char *err, size_t errlen)
{
    s->subject = conf_get("XKS_SUBJECT", NULL);
    if (!s->subject || !*s->subject) {
        snprintf(err, errlen,
                 "no subject: set XKS_SUBJECT=/path/to/model.gguf in xks.local.conf");
        return -1;
    }
    struct stat st;
    if (stat(s->subject, &st)) {
        snprintf(err, errlen, "the subject %s is missing", s->subject);
        return -1;
    }
    engine_opts_default(&s->eng, s->subject);
    s->eng.n_ctx = (uint32_t)conf_long("XKS_CTX", 16384);
    long f = conf_long("XKS_FORKS", 15);
    s->eng.forks = (uint32_t)(f < 1 ? 1 : f);
    s->eng.n_batch = (uint32_t)conf_long("XKS_BATCH", 2048);
    s->eng.n_ubatch = (uint32_t)conf_long("XKS_UBATCH", 512);
    s->eng.threads = (int32_t)conf_long("XKS_THREADS", 16);
    const char *rp = conf_get("XKS_REPACK", "");
    s->eng.repack = !strcmp(rp, "1") || !strcmp(rp, "true");
    s->eng.verbose = conf_long("XKS_VERBOSE", 0) != 0;
    judge_cfg_default(&s->judge);
    if (template_parse(conf_get("XKS_TEMPLATE", "chatml"), &s->judge.tpl)) {
        snprintf(err, errlen, "unknown template `%s` (chatml|gemma|llama3|raw)",
                 conf_get("XKS_TEMPLATE", ""));
        return -1;
    }
    if (layout_parse(conf_get("XKS_LAYOUT", "letters"), &s->judge.layout)) {
        snprintf(err, errlen, "unknown layout `%s` (letters|jev)", conf_get("XKS_LAYOUT", ""));
        return -1;
    }
    long p = conf_long("XKS_PERMUTATIONS", 1);
    s->judge.permutations = (size_t)(p < 1 ? 1 : p);
    const char *cond = getenv("XKS_CONDITIONING");
    if (cond && *cond && calibration_load(cond, &s->judge.cal, err, errlen))
        return -1;
    return 0;
}

static const char *site_name(void)
{
    const char *g = getenv("XKS_GPU_LAYERS");
    return g && *g && strcmp(g, "0") && strcmp(g, "0%") ? "cuda" : "x86";
}

/* ---- server ---------------------------------------------------------- */

typedef struct {
    engine_t *eng;
    judge_cfg_t *judge;
    char **keys;
    size_t nkeys;
    long kill_date;
    pthread_mutex_t mu;
    double last;
    atomic_int busy;
} server_t;

typedef struct {
    server_t *s;
    int fd;
} conn_t;

static json_t *msg(const char *m)
{
    json_t *o = jobj();
    jobj_set(o, "message", jstr(m));
    return o;
}

static bool authorized(server_t *s, const char *auth)
{
    if (!s->nkeys)
        return true;
    if (!auth)
        return false;
    if (!strncmp(auth, "Bearer ", 7))
        auth += 7;
    while (*auth == ' ')
        auth++;
    size_t n = strlen(auth);
    while (n && auth[n - 1] == ' ')
        n--;
    for (size_t i = 0; i < s->nkeys; i++)
        if (strlen(s->keys[i]) == n && !strncmp(s->keys[i], auth, n))
            return true;
    return false;
}

/* A request object as the Rust Request deserializes it. */
static int check_request(const json_t *v, char *err, size_t errlen)
{
    if (v->t != J_OBJ) {
        snprintf(err, errlen, "invalid request: a request is an object with `state` and `questions`");
        return -1;
    }
    if (!jobj_get(v, "state")) {
        snprintf(err, errlen, "invalid request: missing field `state`");
        return -1;
    }
    const json_t *q = jobj_get(v, "questions");
    if (!q) {
        snprintf(err, errlen, "invalid request: missing field `questions`");
        return -1;
    }
    if (q->t != J_OBJ) {
        snprintf(err, errlen, "invalid request: `questions` is an object of id -> question");
        return -1;
    }
    const json_t *m = jobj_get(v, "model");
    if (m && m->t != J_STR && m->t != J_NULL) {
        snprintf(err, errlen, "invalid request: `model` is a string");
        return -1;
    }
    return 0;
}

static void *serve_conn(void *arg)
{
    conn_t *c = arg;
    server_t *s = c->s;
    http_req_t r;
    if (http_read(c->fd, &r)) {
        http_respond(c->fd, 400, "{\"message\":\"unreadable request\"}", NULL);
        goto out;
    }
    char *path = xstrdup(r.path);
    size_t pl = strlen(path);
    while (pl && path[pl - 1] == '/')
        path[--pl] = 0;
    if (!pl) {
        free(path);
        path = xstrdup("/");
    }
    const char *want = NULL;
    int route = 0; /* 1 index, 2 health, 3 models, 4 systemone */
    if (!strcmp(path, "/"))
        want = "GET", route = 1;
    else if (!strcmp(path, "/health"))
        want = "GET", route = 2;
    else if (!strcmp(path, "/v1/models"))
        want = "GET", route = 3;
    else if (!strcmp(path, "/v1/generate"))
        want = "POST", route = 5;
    else if (!strcmp(path, "/v1/systemone"))
        want = "POST", route = 4;
    int status = 200;
    json_t *body = NULL;
    const char *allow = NULL;
    bool question = false;
    const char *subject = engine_name(s->eng);
    if (!route) {
        sb_t m;
        sb_init(&m);
        sb_printf(&m, "no route %s %s (POST /v1/systemone; GET / lists the routes)", r.method, path);
        status = 404;
        body = msg(m.s);
        sb_free(&m);
    } else if (strcmp(r.method, want)) {
        sb_t m;
        sb_init(&m);
        sb_printf(&m, "%s takes %s, not %s", path, want, r.method);
        status = 405;
        allow = want;
        body = msg(m.s);
        sb_free(&m);
    } else if (route == 1) {
        body = jobj();
        jobj_set(body, "service", jstr("xks, a local Jev (TypeSafe System One), in C (MechJev-v2)"));
        jobj_set(body, "subject", jstr(subject));
        jobj_set(body, "site", jstr(site_name()));
        json_t *routes = jobj();
        jobj_set(routes, "POST /v1/systemone", jstr("a request: {\"state\": ..., \"questions\": {...}}"));
        jobj_set(routes, "POST /v1/generate",
                 jstr("the teacher's lines only: {\"system\": ..., \"user\": ...} -> {\"text\"}"));
        jobj_set(routes, "GET /health", jstr("the subject and where it runs"));
        jobj_set(routes, "GET /v1/models", jstr("the model ids this server answers to"));
        jobj_set(body, "routes", routes);
        jobj_set(body, "docs", jstr("https://github.com/Lasimeri/MechJev-v2"));
    } else if (route == 2) {
        pthread_mutex_lock(&s->mu);
        double idle = (now_ms() - s->last) / 1e3;
        pthread_mutex_unlock(&s->mu);
        body = jobj();
        jobj_set(body, "status", jstr("ok"));
        jobj_set(body, "subject", jstr(subject));
        jobj_set(body, "site", jstr(site_name()));
        jobj_set(body, "cards", jarr());
        jobj_set(body, "kill_date_s", ju64((uint64_t)(s->kill_date > 0 ? s->kill_date : 0)));
        jobj_set(body, "idle_s", ju64((uint64_t)floor(idle)));
    } else if (route == 3) {
        body = jobj();
        jobj_set(body, "object", jstr("list"));
        json_t *data = jarr();
        json_t *a = jobj();
        jobj_set(a, "id", jstr(subject));
        jobj_set(a, "object", jstr("model"));
        jobj_set(a, "owned_by", jstr("xks"));
        jarr_push(data, a);
        json_t *b = jobj();
        jobj_set(b, "id", jstr("jev-latest"));
        jobj_set(b, "object", jstr("model"));
        jobj_set(b, "owned_by", jstr("xks"));
        jobj_set(b, "alias_of", jstr(subject));
        jarr_push(data, b);
        jobj_set(body, "data", data);
    } else if (route == 5) {
        /* the teacher's lines (teach.md): a chat in, its text out. Never
         * MechJev's replies: those are its own words, chosen by decisions */
        question = true;
        char err[1024] = "";
        char perr[512];
        json_t *req = authorized(s, r.auth) ? json_parse(r.body ? r.body : "", r.body_len, perr, sizeof perr) : NULL;
        const json_t *sys = req ? jobj_get(req, "system") : NULL, *usr = req ? jobj_get(req, "user") : NULL;
        if (!authorized(s, r.auth)) {
            status = 401;
            body = msg("invalid or missing API key (Authorization: Bearer KEY)");
        } else if (!usr || usr->t != J_STR || (sys && sys->t != J_STR)) {
            status = 422;
            body = msg("POST {\"system\": \"...\", \"user\": \"...\"} (and optionally max_tokens, temperature, "
                       "top_k, top_p, min_p, repeat_penalty, seed)");
        } else if (s->judge->tpl != T_CHATML) {
            status = 422;
            body = msg("generation renders ChatML; this subject's template is another");
        } else {
            gen_opts_t o;
            gen_opts_default(&o);
            const json_t *v;
            if ((v = jobj_get(req, "max_tokens")) && v->t == J_NUM && v->f >= 1 && v->f <= 512)
                o.max_tokens = (int32_t)v->f;
            if ((v = jobj_get(req, "temperature")) && v->t == J_NUM)
                o.temp = (float)v->f;
            if ((v = jobj_get(req, "top_k")) && v->t == J_NUM)
                o.top_k = (int32_t)v->f;
            if ((v = jobj_get(req, "top_p")) && v->t == J_NUM)
                o.top_p = (float)v->f;
            if ((v = jobj_get(req, "min_p")) && v->t == J_NUM)
                o.min_p = (float)v->f;
            if ((v = jobj_get(req, "repeat_penalty")) && v->t == J_NUM)
                o.repeat = (float)v->f;
            if ((v = jobj_get(req, "seed")) && v->t == J_NUM)
                o.seed = (uint32_t)v->u;
            sb_t p;
            sb_init(&p);
            sb_printf(&p, "<|im_start|>system\n%s<|im_end|>\n<|im_start|>user\n%s<|im_end|>\n"
                          "<|im_start|>assistant\n<think>\n\n</think>\n\n",
                      sys ? sys->s : "", usr->s);
            char *text = NULL;
            size_t ntok = 0;
            double t0 = now_ms();
            int rc = engine_generate(s->eng, p.s, &o, &text, &ntok, err, sizeof err);
            sb_free(&p);
            if (rc == E_OK) {
                body = jobj();
                jobj_set(body, "text", jstr(text));
                jobj_set(body, "tokens", ju64(ntok));
                jobj_set(body, "ms", jf64(round(now_ms() - t0)));
                free(text);
            } else {
                status = rc == E_REJECTED ? 422 : 502;
                body = msg(err);
            }
        }
        json_free(req);
    } else {
        question = true;
        char err[1024] = "";
        if (!authorized(s, r.auth)) {
            status = 401;
            body = msg("invalid or missing API key (Authorization: Bearer KEY)");
        } else {
            size_t a = 0;
            while (a < r.body_len && (r.body[a] == ' ' || r.body[a] == '\n' || r.body[a] == '\r' ||
                                      r.body[a] == '\t'))
                a++;
            if (a == r.body_len) {
                status = 422;
                body = msg("empty body: POST a JSON request, {\"state\": ..., \"questions\": {...}}");
            } else {
                char perr[512];
                json_t *req = json_parse(r.body, r.body_len, perr, sizeof perr);
                if (!req) {
                    snprintf(err, sizeof err, "invalid request: %s", perr);
                    status = 422;
                    body = msg(err);
                } else if (check_request(req, err, sizeof err)) {
                    status = 422;
                    body = msg(err);
                } else {
                    json_t *ev;
                    int rc = judge_evaluate(s->eng, s->judge, req, &ev, err, sizeof err);
                    if (rc == E_OK) {
                        body = ev;
                    } else {
                        status = rc == E_REJECTED ? 422 : 502;
                        if (rc != E_REJECTED) {
                            char e2[1100];
                            snprintf(e2, sizeof e2, "backend http: %s", err);
                            body = msg(e2);
                        } else {
                            body = msg(err);
                        }
                    }
                }
                json_free(req);
            }
        }
    }
    char *out = json_dump(body);
    http_respond(c->fd, status, out, allow);
    free(out);
    json_free(body);
    free(path);
    http_req_free(&r);
    if (question) {
        pthread_mutex_lock(&s->mu);
        s->last = now_ms();
        pthread_mutex_unlock(&s->mu);
    }
out:
    close(c->fd);
    atomic_fetch_sub(&s->busy, 1);
    free(c);
    return NULL;
}

static void span(char *b, size_t n, long s)
{
    if (s >= 60 && s % 60 == 0)
        snprintf(b, n, "%ld min", s / 60);
    else
        snprintf(b, n, "%ld s", s);
}

static int serve(const char *bind, long kill_date, bool debug)
{
    char err[1024];
    setup_t su;
    if (setup(&su, err, sizeof err)) {
        fprintf(stderr, "error: %s\n", err);
        return 1;
    }
    su.judge.debug = debug;
    fprintf(stderr, "xks: site %s\n", site_name());
    int lfd = http_listen(bind);
    if (lfd < 0) {
        fprintf(stderr, "error: bind %s: %s\n", bind, strerror(errno));
        return 1;
    }
    engine_t *eng = engine_open(&su.eng, err, sizeof err);
    if (!eng) {
        fprintf(stderr, "error: %s\n", err);
        return 1;
    }
    server_t s = {0};
    s.eng = eng;
    s.judge = &su.judge;
    s.kill_date = kill_date;
    pthread_mutex_init(&s.mu, NULL);
    s.last = now_ms();
    const char *keys = conf_get("XKS_API_KEYS", "");
    if (*keys) {
        char *dup = xstrdup(keys), *sv = NULL;
        for (char *k = strtok_r(dup, ",", &sv); k; k = strtok_r(NULL, ",", &sv)) {
            while (*k == ' ')
                k++;
            if (!*k)
                continue;
            s.keys = xrealloc(s.keys, (s.nkeys + 1) * sizeof *s.keys);
            s.keys[s.nkeys++] = xstrdup(k);
        }
        free(dup);
    }
    char kd[160] = "";
    if (kill_date > 0) {
        char sp[32];
        span(sp, sizeof sp, kill_date);
        snprintf(kd, sizeof kd, "; stops itself after %s without a question (kill date)", sp);
    }
    fprintf(stderr, "xks listening on http://%s  (subject %s)%s\n", bind, engine_name(eng), kd);
    signal(SIGPIPE, SIG_IGN);
    for (;;) {
        struct pollfd pf = {lfd, POLLIN, 0};
        int k = poll(&pf, 1, 1000);
        if (k == 0) {
            if (kill_date > 0 && atomic_load(&s.busy) == 0) {
                pthread_mutex_lock(&s.mu);
                double idle = (now_ms() - s.last) / 1e3;
                pthread_mutex_unlock(&s.mu);
                if (idle >= (double)kill_date) {
                    char sp[32];
                    span(sp, sizeof sp, (long)idle);
                    fprintf(stderr, "xks: kill date reached: %s without a question, exiting\n", sp);
                    engine_close(eng);
                    return 0;
                }
            }
            continue;
        }
        if (k < 0) {
            if (errno == EINTR)
                continue;
            fprintf(stderr, "error: accept: %s\n", strerror(errno));
            return 1;
        }
        int fd = accept4(lfd, NULL, NULL, SOCK_CLOEXEC);
        if (fd < 0)
            continue;
        conn_t *c = xmalloc(sizeof *c);
        c->s = &s;
        c->fd = fd;
        atomic_fetch_add(&s.busy, 1);
        pthread_t t;
        pthread_attr_t at;
        pthread_attr_init(&at);
        pthread_attr_setdetachstate(&at, PTHREAD_CREATE_DETACHED);
        if (pthread_create(&t, &at, serve_conn, c)) {
            atomic_fetch_sub(&s.busy, 1);
            close(fd);
            free(c);
        }
        pthread_attr_destroy(&at);
    }
}

/* ---- detach and stop ------------------------------------------------- */

static bool answers(const char *bind)
{
    char *b = NULL;
    int st = http_call(bind, "GET", "/health", NULL, 0, &b, NULL, 2000);
    free(b);
    return st == 200;
}

/* A NUL-separated command line that runs `...xks ... serve`. */
static bool is_server_cmdline(const char *c, size_t n)
{
    bool seen = false;
    for (size_t i = 0; i < n;) {
        const char *a = c + i;
        size_t l = strnlen(a, n - i);
        if (seen && l == 5 && !memcmp(a, "serve", 5))
            return true;
        if (l >= 3 && !memcmp(a + l - 3, "xks", 3))
            seen = true;
        i += l + 1;
    }
    return false;
}

static long detached_server(const char *pidfile)
{
    char *t = read_file(pidfile, NULL);
    if (!t)
        return 0;
    long pid = strtol(t, NULL, 10);
    free(t);
    if (pid <= 0)
        return 0;
    char p[64];
    snprintf(p, sizeof p, "/proc/%ld/cmdline", pid);
    size_t n;
    char *c = read_file(p, &n);
    if (!c)
        return 0;
    bool ok = is_server_cmdline(c, n);
    free(c);
    return ok ? pid : 0;
}

static void relay(const char *log, size_t *seen, bool all)
{
    size_t n;
    char *b = read_file(log, &n);
    if (!b)
        return;
    if (*seen > n)
        *seen = n;
    size_t end = n;
    if (!all) {
        end = *seen;
        for (size_t i = n; i > *seen; i--)
            if (b[i - 1] == '\n') {
                end = i;
                break;
            }
    }
    char *s = b + *seen, *e = b + end;
    while (s < e) {
        char *nl = memchr(s, '\n', (size_t)(e - s));
        char *le = nl ? nl : e;
        size_t l = (size_t)(le - s);
        if ((l >= 4 && !strncmp(s, "xks:", 4)) || (l >= 13 && !strncmp(s, "xks listening", 13)) ||
            (l >= 6 && !strncmp(s, "error:", 6)))
            fprintf(stderr, "%.*s\n", (int)l, s);
        s = nl ? nl + 1 : e;
    }
    *seen = end;
    free(b);
}

static int detach(int argc, char **argv, const char *bind)
{
    char *dir = run_dir();
    mkdir_p(dir);
    sb_t pf, lg;
    sb_init(&pf);
    sb_init(&lg);
    sb_printf(&pf, "%s/serve.pid", dir);
    sb_printf(&lg, "%s/serve.log", dir);
    free(dir);
    long pid = detached_server(pf.s);
    if (pid) {
        fprintf(stderr, "error: a server is already running (pid %ld); xks stop\n", pid);
        return 1;
    }
    if (answers(bind)) {
        fprintf(stderr, "error: something already answers on %s (a server not started with --detach?)\n",
                bind);
        return 1;
    }
    int lfd = open(lg.s, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);
    if (lfd < 0) {
        fprintf(stderr, "error: %s: %s\n", lg.s, strerror(errno));
        return 1;
    }
    char **args = xcalloc((size_t)argc + 1, sizeof *args);
    int na = 0;
    args[na++] = "/proc/self/exe";
    for (int i = 1; i < argc; i++)
        if (strcmp(argv[i], "--detach"))
            args[na++] = argv[i];
    args[na] = NULL;
    char exe[4096];
    ssize_t el = readlink("/proc/self/exe", exe, sizeof exe - 1);
    exe[el > 0 ? el : 0] = 0;
    args[0] = argv[0];
    pid_t child = fork();
    if (child == 0) {
        setpgid(0, 0);
        int nul = open("/dev/null", O_RDONLY);
        dup2(nul, 0);
        dup2(lfd, 1);
        dup2(lfd, 2);
        execv(exe, args);
        _exit(127);
    }
    close(lfd);
    FILE *f = fopen(pf.s, "w");
    if (f) {
        fprintf(f, "%d", child);
        fclose(f);
    }
    fprintf(stderr, "xks: server pid %d starting, log %s\n", child, lg.s);
    size_t seen = 0;
    double t0 = now_ms();
    while (now_ms() - t0 < 900e3) {
        relay(lg.s, &seen, false);
        int st;
        if (waitpid(child, &st, WNOHANG) == child) {
            relay(lg.s, &seen, true);
            unlink(pf.s);
            fprintf(stderr, "error: the server exited (status %d); log %s\n",
                    WIFEXITED(st) ? WEXITSTATUS(st) : -1, lg.s);
            return 1;
        }
        if (answers(bind)) {
            relay(lg.s, &seen, true);
            printf("http://%s/v1/systemone\n", bind);
            return 0;
        }
        usleep(500000);
    }
    fprintf(stderr, "error: the server did not answer in 900 s (still running, pid %d); log %s\n",
            child, lg.s);
    return 1;
}

static int stop(void)
{
    char *dir = run_dir();
    sb_t pf;
    sb_init(&pf);
    sb_printf(&pf, "%s/serve.pid", dir);
    free(dir);
    long pid = detached_server(pf.s);
    if (pid) {
        kill((pid_t)-pid, SIGTERM);
        char p[64];
        snprintf(p, sizeof p, "/proc/%ld", pid);
        double t0 = now_ms();
        struct stat st;
        while (stat(p, &st) == 0 && now_ms() - t0 < 30e3)
            usleep(200000);
        if (stat(p, &st) == 0) {
            kill((pid_t)-pid, SIGKILL);
            usleep(500000);
        }
        if (stat(p, &st) == 0) {
            fprintf(stderr, "error: server %ld did not stop\n", pid);
            return 1;
        }
        fprintf(stderr, "xks: server %ld stopped\n", pid);
    } else {
        fprintf(stderr, "xks: no detached server\n");
    }
    unlink(pf.s);
    return 0;
}

/* ---- query, polygraph, doctor ---------------------------------------- */

static char *read_input(const char *file, size_t *n)
{
    if (file && strcmp(file, "-"))
        return read_file(file, n);
    sb_t b;
    sb_init(&b);
    char buf[65536];
    size_t k;
    while ((k = fread(buf, 1, sizeof buf, stdin)) > 0)
        sb_putn(&b, buf, k);
    *n = b.n;
    return sb_take(&b);
}

static void print_pretty(const char *body, size_t n)
{
    json_t *v = json_parse(body, n, NULL, 0);
    if (!v) {
        fwrite(body, 1, n, stdout);
        putchar('\n');
        return;
    }
    sb_t b;
    sb_init(&b);
    json_write_pretty(&b, v);
    puts(b.s);
    sb_free(&b);
    json_free(v);
}

static int query(const char *file, bool local, const char *bind)
{
    size_t n;
    char *body = read_input(file, &n);
    if (!body) {
        fprintf(stderr, "error: %s: unreadable\n", file);
        return 1;
    }
    char err[1024];
    json_t *req = json_parse(body, n, err, sizeof err);
    if (!req) {
        fprintf(stderr, "error: invalid request: %s\n", err);
        return 1;
    }
    if (check_request(req, err, sizeof err)) {
        fprintf(stderr, "error: %s\n", err);
        return 1;
    }
    if (!local && answers(bind)) {
        char *out;
        size_t on;
        int st = http_call(bind, "POST", "/v1/systemone", body, n, &out, &on, 3600000);
        if (st < 0) {
            fprintf(stderr, "error: %s did not answer\n", bind);
            return 1;
        }
        print_pretty(out, on);
        free(out);
        return st == 200 ? 0 : 1;
    }
    setup_t su;
    if (setup(&su, err, sizeof err)) {
        fprintf(stderr, "error: %s\n", err);
        return 1;
    }
    su.judge.debug = conf_long("XKS_DEBUG", 0) != 0;
    engine_t *eng = engine_open(&su.eng, err, sizeof err);
    if (!eng) {
        fprintf(stderr, "error: %s\n", err);
        return 1;
    }
    json_t *ev;
    int rc = judge_evaluate(eng, &su.judge, req, &ev, err, sizeof err);
    if (rc) {
        fprintf(stderr, "error: %s\n", err);
        engine_close(eng);
        return 1;
    }
    sb_t b;
    sb_init(&b);
    json_write_pretty(&b, ev);
    puts(b.s);
    sb_free(&b);
    json_free(ev);
    json_free(req);
    free(body);
    engine_close(eng);
    return 0;
}

/* The copy's exactness: every fingerprint of a request read forked
 * (score_many) and split (the same two decodes on sequence 0 without the
 * copy). With XKS_FORKS=1 both make the same decodes, so any difference is
 * the copy's. */
static int polygraph(const char *file)
{
    size_t n;
    char *body = read_input(file, &n);
    char err[1024];
    json_t *req = body ? json_parse(body, n, err, sizeof err) : NULL;
    if (!req || check_request(req, err, sizeof err)) {
        fprintf(stderr, "error: %s\n", body ? err : "unreadable");
        return 1;
    }
    setup_t su;
    if (setup(&su, err, sizeof err)) {
        fprintf(stderr, "error: %s\n", err);
        return 1;
    }
    engine_t *eng = engine_open(&su.eng, err, sizeof err);
    if (!eng) {
        fprintf(stderr, "error: %s\n", err);
        return 1;
    }
    question_t *qs;
    size_t nq;
    if (parse_questions(jobj_get(req, "questions"), &qs, &nq, err, sizeof err)) {
        fprintf(stderr, "error: %s\n", err);
        return 1;
    }
    char *state = render_state(jobj_get(req, "state"));
    segs_t prefix = prompt_prefix(su.judge.layout, su.judge.tpl, state);
    item_t *items = xcalloc(nq, sizeof *items);
    for (size_t i = 0; i < nq; i++) {
        rendered_t rd = prompt_render(su.judge.layout, su.judge.tpl, &qs[i], NULL);
        items[i].suffix = rd.suffix;
        items[i].labels = rd.labels;
        items[i].n = rd.n;
        for (size_t k = 0; k < rd.n; k++)
            free(rd.keys[k]);
        free(rd.keys);
    }
    size_t shared;
    if (engine_session_tokens(eng, &prefix, items, nq, &shared, err, sizeof err)) {
        fprintf(stderr, "error: %s\n", err);
        return 1;
    }
    scored_t *fk = xcalloc(nq, sizeof *fk);
    if (engine_score_many(eng, false, &prefix, items, nq, fk, err, sizeof err)) {
        fprintf(stderr, "error: %s\n", err);
        return 1;
    }
    double worst = 0.0;
    printf("{\"forks\":%u,\"session_tokens\":%zu,\"rows\":[", su.eng.forks, shared);
    for (size_t i = 0; i < nq; i++) {
        segs_t whole = segs_concat(&prefix, &items[i].suffix);
        scored_t sp = {0};
        if (engine_read_split(eng, &whole, items[i].labels, items[i].n, shared, &sp, err, sizeof err)) {
            fprintf(stderr, "error: %s\n", err);
            return 1;
        }
        double d = 0.0;
        for (size_t k = 0; k < items[i].n; k++)
            d = fmax(d, fabs(fk[i].logprobs[k] - sp.logprobs[k]));
        worst = fmax(worst, d);
        printf("%s{\"id\":\"%s\",\"max_abs_diff\":%.17g}", i ? "," : "", qs[i].id, d);
        scored_free(&sp);
        segs_free(&whole);
    }
    printf("],\"max_abs_diff\":%.17g}\n", worst);
    engine_close(eng);
    return 0;
}

static int doctor(void)
{
    char *root = conf_repo_root();
    printf("xks doctor: MechJev-v2 at %s\n", root ? root : "(not in a checkout)");
    free(root);
    bool ok = true;
    const char *s = conf_get("XKS_SUBJECT", NULL);
    struct stat st;
    if (s && !stat(s, &st))
        printf("  ok    subject %s (%.1f GB)\n", s, (double)st.st_size / 1e9);
    else {
        printf("  FAIL  subject %s missing (XKS_SUBJECT)\n", s ? s : "(unset)");
        ok = false;
    }
    printf("  ok    site %s (XKS_GPU_LAYERS=%s)\n", site_name(), conf_get("XKS_GPU_LAYERS", ""));
    printf("%s\n", ok ? "ready" : "not ready");
    return ok ? 0 : 1;
}

static void usage(void)
{
    fputs("xks (MechJev-v2): the decision engine, a local Jev (TypeSafe System One) in C\n"
          "  xks serve [--detach] [--bind HOST:PORT] [--kill-date SECONDS] [--debug]\n"
          "  xks stop\n"
          "  xks query [--file F] [--local]     one request (stdin without --file)\n"
          "  xks health\n"
          "  xks polygraph --file F             forked against split (XKS_FORKS=1: exactness)\n"
          "  xks doctor\n"
          "Defaults come from xks.conf, xks.local.conf, ~/.config/xks/xks.conf.\n",
          stderr);
}

int main(int argc, char **argv)
{
    conf_load();
    if (argc < 2) {
        usage();
        return 2;
    }
    const char *cmd = argv[1];
    const char *bind = conf_get("XKS_BIND", "127.0.0.1:8090");
    const char *file = NULL;
    bool det = false, local = false, debug = false;
    long kill = conf_long("XKS_KILL_DATE", 0);
    for (int i = 2; i < argc; i++) {
        if (!strcmp(argv[i], "--bind") && i + 1 < argc)
            bind = argv[++i];
        else if (!strcmp(argv[i], "--detach"))
            det = true;
        else if (!strcmp(argv[i], "--kill-date") && i + 1 < argc)
            kill = strtol(argv[++i], NULL, 10);
        else if (!strcmp(argv[i], "--file") && i + 1 < argc)
            file = argv[++i];
        else if (!strcmp(argv[i], "--local"))
            local = true;
        else if (!strcmp(argv[i], "--debug"))
            debug = true;
        else {
            fprintf(stderr, "error: unknown argument %s\n", argv[i]);
            usage();
            return 2;
        }
    }
    if (!strcmp(cmd, "serve"))
        return det ? detach(argc, argv, bind) : serve(bind, kill, debug);
    if (!strcmp(cmd, "stop"))
        return stop();
    if (!strcmp(cmd, "query"))
        return query(file, local, bind);
    if (!strcmp(cmd, "health")) {
        char *b = NULL;
        size_t n = 0;
        int st = http_call(bind, "GET", "/health", NULL, 0, &b, &n, 2000);
        if (st < 0) {
            fprintf(stderr, "xks: nothing answers on %s\n", bind);
            return 1;
        }
        print_pretty(b, n);
        free(b);
        return st == 200 ? 0 : 1;
    }
    if (!strcmp(cmd, "polygraph"))
        return polygraph(file);
    if (!strcmp(cmd, "doctor"))
        return doctor();
    usage();
    return 2;
}
