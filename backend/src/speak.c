/* speak.c: see speak.h and speak.md. */
#define _GNU_SOURCE
#include "speak.h"

#include <ctype.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "http.h"
#include "util.h"

#define STOP_KEY "(stop: the reply is complete)"
#define NOT_LISTED "(not listed)"
#define HISTORY_TURNS 4
#define WIDEN_ADD 10 /* words the engine adds from the lexicon past one Choice's worth */

void speak_cfg_default(speak_cfg_t *c)
{
    memset(c, 0, sizeof *c);
    c->engine = "127.0.0.1:8090";
    c->propose = 8;
    c->window = 8;
    c->group = 25;
    c->max_words = 60;
    c->train_ms = 3000;
    c->learn = true;
    c->sampling = (sampling_t){
        .temp = 0.5, .top_p = 0.95, .min_p = 0.05, .repeat_penalty = 1.2, .top_k = 75, .ties_only = true};
}

/* ---- variables ------------------------------------------------------- */

static char *trimmed(const char *s, size_t n)
{
    while (n && isspace((unsigned char)*s))
        s++, n--;
    while (n && isspace((unsigned char)s[n - 1]))
        n--;
    return xstrndup(s, n);
}

int vars_load(speak_cfg_t *c, learner_t *l, const char *path, char *err, size_t errlen)
{
    char *t = read_file(path, NULL);
    if (!t) {
        snprintf(err, errlen, "%s: unreadable (the variables the engine decides)", path);
        return -1;
    }
    char *sv = NULL;
    int line_no = 0;
    for (char *line = strtok_r(t, "\n", &sv); line; line = strtok_r(NULL, "\n", &sv)) {
        line_no++;
        while (*line == ' ' || *line == '\t')
            line++;
        if (!*line || *line == '#')
            continue;
        var_t v = {0};
        if (!strncmp(line, "turn ", 5))
            line += 5;
        else if (!strncmp(line, "step ", 5))
            v.step = true, line += 5;
        else {
            snprintf(err, errlen, "%s: line %d: begins with turn or step", path, line_no);
            free(t);
            return -1;
        }
        if (!strncmp(line, "noul ", 5))
            line += 5;
        else if (!strncmp(line, "choice ", 7))
            v.choice = true, line += 7;
        else {
            snprintf(err, errlen, "%s: line %d: noul or choice after %s", path, line_no,
                     v.step ? "step" : "turn");
            free(t);
            return -1;
        }
        char *bar = v.choice ? strchr(line, '|') : NULL;
        v.text = trimmed(line, bar ? (size_t)(bar - line) : strlen(line));
        while (bar) {
            char *next = strchr(bar + 1, '|');
            v.opts = xrealloc(v.opts, (v.nopts + 1) * sizeof *v.opts);
            v.opts[v.nopts++] = trimmed(bar + 1, next ? (size_t)(next - bar - 1) : strlen(bar + 1));
            bar = next;
        }
        if (v.choice && v.nopts < 2) {
            snprintf(err, errlen, "%s: line %d: a choice needs two options or more, `| a | b`", path, line_no);
            free(t);
            return -1;
        }
        /* slots: a Noul one (its statement), a Choice one per option */
        size_t ns = v.choice ? v.nopts : 1;
        for (size_t k = 0; k < ns; k++) {
            sb_t name;
            sb_init(&name);
            sb_printf(&name, "%s %s%s%s", v.step ? "step" : "turn", v.text, v.choice ? " = " : "",
                      v.choice ? v.opts[k] : "");
            int s = learner_slot(l, name.s);
            sb_free(&name);
            if (s < 0) {
                snprintf(err, errlen, "%s: line %d: all %d variable slots are taken", path, line_no, VAR_SLOTS);
                free(t);
                return -1;
            }
            if (k == 0)
                v.slot = s;
            else if (s != v.slot + (int)k) {
                snprintf(err, errlen, "%s: line %d: a choice's options must take adjacent slots; "
                                      "append new variables at the end",
                         path, line_no);
                free(t);
                return -1;
            }
        }
        c->vars = xrealloc(c->vars, (c->nvars + 1) * sizeof *c->vars);
        c->vars[c->nvars++] = v;
    }
    free(t);
    return 0;
}

void vars_free(speak_cfg_t *c)
{
    for (size_t i = 0; i < c->nvars; i++) {
        free(c->vars[i].text);
        for (size_t k = 0; k < c->vars[i].nopts; k++)
            free(c->vars[i].opts[k]);
        free(c->vars[i].opts);
    }
    free(c->vars);
    c->vars = NULL;
    c->nvars = 0;
}

/* The questions for a scope's variables, ids v<index>; the reply so far is
 * put before each step variable's statement. */
static void vars_questions(const speak_cfg_t *cfg, bool step, const char *reply_so_far, json_t *qs)
{
    for (size_t i = 0; i < cfg->nvars; i++) {
        const var_t *v = &cfg->vars[i];
        if (v->step != step)
            continue;
        sb_t ins;
        sb_init(&ins);
        if (step) {
            sb_puts(&ins, "The reply so far is ");
            json_quote(&ins, reply_so_far, strlen(reply_so_far));
            sb_puts(&ins, ". ");
        }
        sb_puts(&ins, v->text);
        json_t *q = jobj();
        jobj_set(q, "type", jstr(v->choice ? "choice" : "noul"));
        jobj_set(q, "instructions", jstr(ins.s));
        sb_free(&ins);
        if (v->choice) {
            json_t *cr = jobj();
            for (size_t k = 0; k < v->nopts; k++)
                jobj_set(cr, v->opts[k], jnull());
            jobj_set(q, "criteria", cr);
        }
        char id[24];
        snprintf(id, sizeof id, "v%zu", i);
        jobj_set(qs, id, q);
    }
}

/* The answers into the slots, and a readable copy for the trace. */
static void vars_read(const speak_cfg_t *cfg, bool step, const json_t *ans, float *slots, json_t *seen)
{
    for (size_t i = 0; i < cfg->nvars; i++) {
        const var_t *v = &cfg->vars[i];
        if (v->step != step)
            continue;
        char id[24];
        snprintf(id, sizeof id, "v%zu", i);
        const json_t *a = jobj_get(ans, id);
        if (!v->choice) {
            const json_t *p = jobj_get(a, "noul");
            float x = p && p->t == J_NUM ? (float)p->f : 0.5f;
            slots[v->slot] = x;
            if (seen)
                jobj_set(seen, v->text, jf64(x));
        } else {
            const json_t *pr = jobj_get(a, "probabilities");
            json_t *o = seen ? jobj() : NULL;
            for (size_t k = 0; k < v->nopts; k++) {
                const json_t *p = jobj_get(pr, v->opts[k]);
                float x = p && p->t == J_NUM ? (float)p->f : 0.5f;
                slots[v->slot + (int)k] = x;
                if (o)
                    jobj_set(o, v->opts[k], jf64(x));
            }
            if (seen)
                jobj_set(seen, v->text, o);
        }
    }
}

/* ---- history and English --------------------------------------------- */

void history_add(history_t *h, const char *person, const char *reply)
{
    h->person = xrealloc(h->person, (h->n + 1) * sizeof *h->person);
    h->reply = xrealloc(h->reply, (h->n + 1) * sizeof *h->reply);
    h->person[h->n] = xstrdup(person);
    h->reply[h->n] = xstrdup(reply);
    h->n++;
}

void history_free(history_t *h)
{
    for (size_t i = 0; i < h->n; i++) {
        free(h->person[i]);
        free(h->reply[i]);
    }
    free(h->person);
    free(h->reply);
    memset(h, 0, sizeof *h);
}

static bool sentence_end(const char *w) { return !strcmp(w, ".") || !strcmp(w, "?") || !strcmp(w, "!"); }

char *render_words(char **w, size_t n)
{
    sb_t b;
    sb_init(&b);
    bool cap = true;
    for (size_t i = 0; i < n; i++) {
        const char *x = w[i];
        if (!strcmp(x, END))
            continue;
        if (is_punct(x)) {
            sb_puts(&b, x);
            if (sentence_end(x))
                cap = true;
            continue;
        }
        if (b.n)
            sb_putc(&b, ' ');
        size_t at = b.n;
        sb_puts(&b, x);
        if (x[0] == 'i' && (x[1] == 0 || x[1] == '\'')) /* I, I'm, I've, I'll, I'd */
            b.s[at] = 'I';
        if (cap && (unsigned char)b.s[at] < 0x80)
            b.s[at] = (char)toupper((unsigned char)b.s[at]);
        cap = false;
    }
    return sb_take(&b);
}

/* The conversation as the engine reads it: fixed for the whole turn, so
 * the engine keeps it prefilled and each word costs only its questions. */
static char *conversation(const history_t *h, const char *person)
{
    sb_t b;
    sb_init(&b);
    sb_puts(&b, "A conversation in English. The person speaks; the reply is written one word at a time.\n");
    size_t from = h->n > HISTORY_TURNS ? h->n - HISTORY_TURNS : 0;
    for (size_t i = from; i < h->n; i++)
        sb_printf(&b, "Person: %s\nReply: %s\n", h->person[i], h->reply[i]);
    sb_printf(&b, "Person: %s", person);
    return sb_take(&b);
}

/* ---- the engine ------------------------------------------------------ */

typedef struct {
    double ms;
    size_t calls;
    uint64_t tokens;
} cost_t;

/* POST {state, questions}; the answers object (owned by *resp). */
static json_t *ask(const speak_cfg_t *cfg, const char *state, json_t *questions, json_t **resp,
                   cost_t *cost, char *err, size_t errlen)
{
    json_t *req = jobj();
    jobj_set(req, "state", jstr(state));
    jobj_set(req, "questions", questions);
    /* the engine's extension: suffixes padded to equal length, one forward
     * pass a round instead of one a length (engine.c) */
    json_t *ext = jobj();
    jobj_set(ext, "pad_equal", jbool(true));
    jobj_set(req, "xks", ext);
    char *body = json_dump(req);
    json_free(req);
    char *out = NULL;
    size_t on = 0;
    double t0 = now_ms();
    int st = http_call(cfg->engine, "POST", "/v1/systemone", body, strlen(body), &out, &on, 900000);
    free(body);
    cost->ms += now_ms() - t0;
    cost->calls++;
    if (st < 0) {
        snprintf(err, errlen, "the decision engine at %s does not answer (make serve)", cfg->engine);
        free(out);
        return NULL;
    }
    char perr[256];
    json_t *v = json_parse(out, on, perr, sizeof perr);
    free(out);
    if (st != 200 || !v) {
        json_t *m = v ? jobj_get(v, "message") : NULL;
        snprintf(err, errlen, "the decision engine said %d: %s", st,
                 m && m->t == J_STR ? m->s : (v ? "?" : perr));
        json_free(v);
        return NULL;
    }
    json_t *u = jobj_get(jobj_get(v, "usage"), "input_tokens");
    if (u && u->t == J_NUM)
        cost->tokens += u->u;
    *resp = v;
    return jobj_get(v, "answers");
}

static json_t *choice_q(const char *instructions, char **keys, size_t n)
{
    json_t *q = jobj();
    jobj_set(q, "type", jstr("choice"));
    jobj_set(q, "instructions", jstr(instructions));
    json_t *c = jobj();
    for (size_t i = 0; i < n; i++)
        jobj_set(c, keys[i], jnull());
    jobj_set(q, "criteria", c);
    return q;
}

static double prob_of(const json_t *answer, const char *key)
{
    const json_t *p = jobj_get(jobj_get(answer, "probabilities"), key);
    return p && p->t == J_NUM ? p->f : 0.0;
}

/* ---- sampling -------------------------------------------------------- */

/* A seed from the current time: the realtime clock in nanoseconds, one per
 * word (the user, 2026-10-17: "the current time needs to be utilized to
 * generate the model's seed per token"). Recorded in the trace, so a reply
 * can be replayed. */
static uint64_t clock_seed(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

/* One draw from the engine's distribution over the shown options, in
 * llama.cpp's default order (common/common.h: penalties, top-k, top-p,
 * min-p, temperature). The repeat penalty divides an already said word's
 * probability by `repeat_penalty`, the probability-space form of
 * llama.cpp's logit penalty (an approximation, documented as one). */
static size_t sample_index(const double *p, const bool *repeated, size_t n, const sampling_t *s,
                           uint64_t seed)
{
    double *q = xmalloc(n * sizeof *q);
    size_t *ix = xmalloc(n * sizeof *ix);
    for (size_t i = 0; i < n; i++) {
        q[i] = repeated[i] && s->repeat_penalty > 0 ? p[i] / s->repeat_penalty : p[i];
        ix[i] = i;
    }
    for (size_t i = 1; i < n; i++) /* by q, high first (n is small) */
        for (size_t j = i; j > 0 && q[ix[j]] > q[ix[j - 1]]; j--) {
            size_t tmp = ix[j];
            ix[j] = ix[j - 1];
            ix[j - 1] = tmp;
        }
    size_t keep = n;
    if (s->top_k > 0 && (size_t)s->top_k < keep)
        keep = (size_t)s->top_k;
    double total = 0;
    for (size_t i = 0; i < keep; i++)
        total += q[ix[i]];
    if (s->top_p < 1.0 && total > 0) {
        double cum = 0;
        for (size_t i = 0; i < keep; i++) {
            cum += q[ix[i]] / total;
            if (cum >= s->top_p) {
                keep = i + 1;
                break;
            }
        }
    }
    if (s->min_p > 0)
        while (keep > 1 && q[ix[keep - 1]] < s->min_p * q[ix[0]])
            keep--;
    size_t pick = ix[0];
    if (s->ties_only) {
        /* The user's choice (2026-10-17): their settings, the clock seed for
         * exact ties only. After the penalty the filters and the temperature
         * keep the order, so the word is the most likely one left; among
         * options tied exactly at the top, the seed picks. */
        size_t tied = 1;
        while (tied < keep && q[ix[tied]] == q[ix[0]])
            tied++;
        uint64_t st = seed;
        pick = ix[tied > 1 ? splitmix64(&st) % tied : 0];
        free(q);
        free(ix);
        return pick;
    }
    if (s->temp > 0 && keep > 1) {
        double *w = xmalloc(keep * sizeof *w), z = 0;
        double m = log(fmax(q[ix[0]], 1e-300));
        for (size_t i = 0; i < keep; i++) {
            w[i] = exp((log(fmax(q[ix[i]], 1e-300)) - m) / s->temp);
            z += w[i];
        }
        uint64_t st = seed;
        double u = (double)(splitmix64(&st) >> 11) * 0x1.0p-53 * z;
        pick = ix[keep - 1];
        for (size_t i = 0; i < keep; i++) {
            if (u < w[i]) {
                pick = ix[i];
                break;
            }
            u -= w[i];
        }
        free(w);
    }
    free(q);
    free(ix);
    return pick;
}

/* ---- coherence, the reward model of the per-token scoring ------------ */

/* The engine's coherence of the reply so far: a Score over five levels,
 * normalized to [0, 1]. It is R(x, y<=t) of RED (Li et al., "Unleashing
 * Token-Level Rewards from Holistic Feedback via Reward Redistribution",
 * arXiv:2411.08302): each word's reward is the change it makes. */
static const char *COHERENCE[] = {"no reply yet, or no sense at all", "words without a sense",
                                  "partly makes sense", "makes sense so far",
                                  "clear, correct and on topic"};

static json_t *coherence_q(const char *so_far)
{
    sb_t ins;
    sb_init(&ins);
    sb_puts(&ins, "The reply so far is ");
    json_quote(&ins, so_far, strlen(so_far));
    sb_puts(&ins, ". How coherent is it, as the beginning of an English reply to what the person said "
                  "last?");
    json_t *q = jobj();
    jobj_set(q, "type", jstr("score"));
    jobj_set(q, "instructions", jstr(ins.s));
    sb_free(&ins);
    json_t *lv = jarr();
    for (size_t i = 0; i < sizeof COHERENCE / sizeof *COHERENCE; i++)
        jarr_push(lv, jstr(COHERENCE[i]));
    jobj_set(q, "criteria", lv);
    return q;
}

static double coherence_of(const json_t *ans)
{
    const json_t *s = jobj_get(jobj_get(ans, "coherence"), "score");
    double levels = (double)(sizeof COHERENCE / sizeof *COHERENCE) - 1;
    return s && s->t == J_NUM ? s->f / levels : 0;
}

/* ---- one word -------------------------------------------------------- */

static bool repeats(char **said, size_t t, const char *w)
{
    if (t && !strcmp(said[t - 1], w))
        return true;
    if (t >= 2)
        for (size_t i = 0; i + 2 < t; i++)
            if (!strcmp(said[i], said[t - 2]) && !strcmp(said[i + 1], said[t - 1]) &&
                !strcmp(said[i + 2], w))
                return true;
    return false;
}

static bool allowed(const learner_t *l, char **said, size_t t, int32_t wi)
{
    const char *w = l->v[wi].word;
    if (!strcmp(w, START))
        return false;
    if (!strcmp(w, END))
        return t > 0 && sentence_end(said[t - 1]);
    if (is_punct(w) && (t == 0 || is_punct(said[t - 1])))
        return false;
    return !repeats(said, t, w);
}

/* The option shown for word wi after said[0..t): the last `window` words
 * of the reply with it added, as English. */
static char *continuation(const learner_t *l, const speak_cfg_t *cfg, char **said, size_t t, int32_t wi)
{
    size_t from = t > cfg->window ? t - cfg->window : 0;
    char **w = xmalloc((t - from + 1) * sizeof *w);
    size_t n = 0;
    for (size_t i = from; i < t; i++)
        w[n++] = said[i];
    w[n++] = l->v[wi].word;
    char *r = render_words(w, n);
    free(w);
    if (from == 0)
        return r;
    sb_t b;
    sb_init(&b);
    sb_puts(&b, "...");
    if (r[0] && (unsigned char)r[0] < 0x80 && !(r[0] == 'I' && (r[1] == ' ' || r[1] == '\'' || !r[1])))
        r[0] = (char)tolower((unsigned char)r[0]);
    sb_puts(&b, r);
    free(r);
    return sb_take(&b);
}

typedef struct {
    int32_t top, pick; /* the engine's choice; the word drawn and said */
    uint64_t seed;
    double p_pick, p_top;
    size_t n;
    int32_t opt[MAX_OPTS];
    float q[MAX_OPTS];
    json_t *shown; /* option text -> probability */
    double coh;    /* the engine's coherence of the reply so far, before this word */
} step_t;

/* The rerank: one Choice over the candidates as continuations of the
 * reply, the step variables asked in the same request. */
static int rerank(learner_t *l, const speak_cfg_t *cfg, const char *state, char **said, size_t t,
                  const int32_t *cands, size_t nc, float *slots, json_t *seen, step_t *st,
                  cost_t *cost, char *err, size_t errlen)
{
    char **keys = xcalloc(nc ? nc : 1, sizeof *keys);
    int32_t *word = xcalloc(nc ? nc : 1, sizeof *word);
    size_t no = 0;
    for (size_t i = 0; i < nc && no < MAX_OPTS; i++) {
        char *k = !strcmp(l->v[cands[i]].word, END) ? xstrdup(STOP_KEY)
                                                     : continuation(l, cfg, said, t, cands[i]);
        bool dup = false;
        for (size_t j = 0; j < no; j++)
            dup |= !strcmp(keys[j], k);
        if (dup) {
            free(k);
            continue;
        }
        keys[no] = k;
        word[no++] = cands[i];
    }
    /* Shown in an order the circuit did not choose: the candidates arrive
     * ranked by the circuit, and cyclic rotations keep its first word at the
     * front, so any pull of the engine toward the first option would favour
     * the circuit's own pick and inflate its agreement. A Fisher-Yates shuffle
     * from a clock seed. */
    uint64_t sh = clock_seed() ^ 0x5348554646ull;
    for (size_t i = no; i > 1; i--) {
        size_t j = (size_t)(splitmix64(&sh) % i);
        char *tk = keys[i - 1];
        keys[i - 1] = keys[j];
        keys[j] = tk;
        int32_t tw = word[i - 1];
        word[i - 1] = word[j];
        word[j] = tw;
    }
    char *so_far = render_words(said, t);
    sb_t ins;
    sb_init(&ins);
    if (t == 0) {
        sb_puts(&ins, "The reply to the person is empty so far. Each option is a first word for it. "
                      "Which option is the best way to begin a short, natural English reply to what the "
                      "person said last?");
    } else {
        sb_puts(&ins, "The reply so far is ");
        json_quote(&ins, so_far, strlen(so_far));
        sb_puts(&ins, ". Each option is the end of the reply with one more word or mark added. Which "
                      "option continues it best, as natural and correct English that answers what the "
                      "person said last? Choose " STOP_KEY " only if the reply is already complete.");
    }
    json_t *qs = jobj();
    jobj_set(qs, "next", choice_q(ins.s, keys, no));
    sb_free(&ins);
    vars_questions(cfg, true, so_far, qs);
    jobj_set(qs, "coherence", coherence_q(so_far));
    free(so_far);
    json_t *resp = NULL;
    json_t *ans = ask(cfg, state, qs, &resp, cost, err, errlen);
    int rc = -1;
    if (ans) {
        json_t *a = jobj_get(ans, "next"), *c = jobj_get(a, "choice");
        double *p = xmalloc((no ? no : 1) * sizeof *p), z = 0;
        bool *rep = xcalloc(no ? no : 1, sizeof *rep);
        size_t best = no;
        for (size_t i = 0; i < no; i++) {
            p[i] = prob_of(a, keys[i]);
            z += p[i];
            for (size_t j = 0; j < t; j++)
                rep[i] |= !strcmp(said[j], l->v[word[i]].word);
            if (c && c->t == J_STR && !strcmp(keys[i], c->s))
                best = i;
        }
        if (best == no) {
            snprintf(err, errlen, "the engine chose an option that was not shown");
        } else {
            st->top = word[best];
            st->seed = clock_seed();
            size_t k = sample_index(p, rep, no, &cfg->sampling, st->seed);
            st->pick = word[k];
            st->p_pick = p[k];
            st->p_top = p[best];
            st->n = no;
            st->shown = jobj();
            for (size_t i = 0; i < no; i++) {
                st->opt[i] = word[i];
                st->q[i] = (float)(z > 0 ? p[i] / z : 1.0 / (double)no);
                jobj_set(st->shown, keys[i], jf64(p[i]));
            }
            vars_read(cfg, true, ans, slots, seen);
            st->coh = coherence_of(ans);
            rc = 0;
        }
        free(p);
        free(rep);
    }
    json_free(resp);
    for (size_t i = 0; i < no; i++)
        free(keys[i]);
    free(keys);
    free(word);
    return rc;
}

/* The engine's perception of the reply so far without a word choice: the
 * step variables (into slots, when given) and its coherence. Used when the
 * circuit speaks alone, and once after the last word for the reply's final
 * coherence (the last term of the per-token rewards). */
static int ask_reply_state(const speak_cfg_t *cfg, const char *state, char **said, size_t t, float *slots,
                           json_t *seen, double *coh, cost_t *cost, char *err, size_t errlen)
{
    json_t *qs = jobj();
    char *so_far = render_words(said, t);
    if (slots)
        vars_questions(cfg, true, so_far, qs);
    jobj_set(qs, "coherence", coherence_q(so_far));
    free(so_far);
    json_t *resp = NULL;
    json_t *ans = ask(cfg, state, qs, &resp, cost, err, errlen);
    if (!ans)
        return -1;
    if (slots)
        vars_read(cfg, true, ans, slots, seen);
    *coh = coherence_of(ans);
    json_free(resp);
    return 0;
}

/* The circuit unsure: the lexicon put to the engine in groups, each
 * group's word (when it beats "not listed") a candidate. */
static size_t widen(learner_t *l, const speak_cfg_t *cfg, const char *state, char **said, size_t t,
                    const int32_t *shown, size_t nshown, int32_t *out, size_t k, cost_t *cost,
                    char *err, size_t errlen)
{
    int32_t *pool = xmalloc((l->n ? l->n : 1) * sizeof *pool);
    size_t np = 0;
    for (size_t i = 0; i < l->n; i++) {
        int32_t wi = (int32_t)i;
        if (!lex_speakable(l, wi) || !allowed(l, said, t, wi) || is_punct(l->v[i].word) ||
            !strcmp(l->v[i].word, END))
            continue;
        bool was = false;
        for (size_t j = 0; j < nshown; j++)
            was |= shown[j] == wi;
        if (!was)
            pool[np++] = wi;
    }
    if (!np) {
        free(pool);
        return 0;
    }
    char *so_far = render_words(said, t);
    sb_t ins;
    sb_init(&ins);
    if (t == 0)
        sb_puts(&ins, "The reply to the person is empty so far. Which of these words should the reply "
                      "begin with?");
    else {
        sb_puts(&ins, "The reply so far is ");
        json_quote(&ins, so_far, strlen(so_far));
        sb_puts(&ins, ". Which of these words should come next in it?");
    }
    sb_puts(&ins, " Choose " NOT_LISTED " if the next word should be none of these.");
    free(so_far);
    json_t *qs = jobj();
    size_t ng = 0;
    for (size_t at = 0; at < np; at += cfg->group, ng++) {
        size_t m = np - at < cfg->group ? np - at : cfg->group;
        char **keys = xmalloc((m + 1) * sizeof *keys);
        for (size_t i = 0; i < m; i++)
            keys[i] = l->v[pool[at + i]].word;
        keys[m] = NOT_LISTED;
        char id[24];
        snprintf(id, sizeof id, "g%zu", ng);
        jobj_set(qs, id, choice_q(ins.s, keys, m + 1));
        free(keys);
    }
    sb_free(&ins);
    json_t *resp = NULL;
    json_t *ans = ask(cfg, state, qs, &resp, cost, err, errlen);
    size_t got = 0;
    double *score = xmalloc((k ? k : 1) * sizeof *score);
    for (size_t g = 0; ans && g < ng; g++) {
        char id[24];
        snprintf(id, sizeof id, "g%zu", g);
        json_t *a = jobj_get(ans, id), *c = jobj_get(a, "choice");
        if (!c || c->t != J_STR || !strcmp(c->s, NOT_LISTED))
            continue;
        double p = prob_of(a, c->s) - prob_of(a, NOT_LISTED);
        int32_t wi = lex_find(l, c->s);
        if (wi < 0)
            continue;
        size_t at = got;
        while (at > 0 && score[at - 1] < p)
            at--;
        if (at >= k)
            continue;
        size_t last = got < k ? got : k - 1;
        memmove(&out[at + 1], &out[at], (last - at) * sizeof *out);
        memmove(&score[at + 1], &score[at], (last - at) * sizeof *score);
        out[at] = wi;
        score[at] = p;
        if (got < k)
            got++;
    }
    free(score);
    json_free(resp);
    free(pool);
    return got;
}

/* ---- a turn ---------------------------------------------------------- */

static const char *LENGTHS[] = {"a word or two", "one short sentence", "two or three sentences",
                                "a paragraph"};
static const size_t LENGTH_WORDS[] = {3, 12, 30, 60};

static json_t *eval_json(evalres_t r)
{
    json_t *o = jobj();
    jobj_set(o, "n", ju64(r.n));
    /* nothing measured (a turn of the circuit alone has no examples): null,
     * never a zero that reads as one */
    jobj_set(o, "agree", r.n ? jf64(round(r.agree * 1e4) / 1e4) : jnull());
    jobj_set(o, "nll", r.n ? jf64(round(r.nll * 1e4) / 1e4) : jnull());
    jobj_set(o, "relaxed_same", r.n ? jf64(round(r.gap * 1e4) / 1e4) : jnull());
    return o;
}

int speak_turn(learner_t *l, const speak_cfg_t *cfg, history_t *h, const char *person, char **reply,
               json_t **record, char *err, size_t errlen)
{
    double t0 = now_ms();
    cost_t cost = {0};
    *reply = NULL;
    *record = NULL;
    char **heard;
    size_t nh = split_words(person, &heard);
    size_t lex_before = l->n;
    learner_hear(l, heard, nh);
    free_words(heard, nh);
    l->turns++;
    char *state = conversation(h, person);

    /* The turn's variables and how much to say, in one request. The cap is
     * the learner's stage: a few words more for every four it can say. */
    float slots[VAR_SLOTS];
    for (int i = 0; i < VAR_SLOTS; i++)
        slots[i] = 0.5f;
    json_t *qs = jobj();
    vars_questions(cfg, false, NULL, qs);
    json_t *sq = jobj();
    jobj_set(sq, "type", jstr("score"));
    jobj_set(sq, "instructions", jstr("How long should the reply to what the person said last be?"));
    json_t *lv = jarr();
    for (size_t i = 0; i < 4; i++)
        jarr_push(lv, jstr(LENGTHS[i]));
    jobj_set(sq, "criteria", lv);
    jobj_set(qs, "length", sq);
    json_t *resp = NULL;
    json_t *ans = ask(cfg, state, qs, &resp, &cost, err, errlen);
    if (!ans) {
        free(state);
        return -1;
    }
    json_t *turn_vars = jobj();
    vars_read(cfg, false, ans, slots, turn_vars);
    json_t *sc = jobj_get(jobj_get(ans, "length"), "score");
    size_t li = (size_t)fmin(3.0, fmax(0.0, round(sc && sc->t == J_NUM ? sc->f : 1.0)));
    json_free(resp);
    size_t speakable = lex_speakable_count(l);
    size_t stage = 3 + speakable / 4;
    size_t cap = LENGTH_WORDS[li] < stage ? LENGTH_WORDS[li] : stage;
    if (cap > cfg->max_words)
        cap = cfg->max_words;

    /* The cap counts words; punctuation and the stop come free (a word or
     * two is "Yes, I am." not "Yes, I": measured 2026-10-17, the cap cut the
     * circuit alone off mid-reply). Room for a mark after every word. */
    size_t room = 2 * cap + 2, words = 0;
    char **said = xcalloc(room + 1, sizeof *said);
    example_t *ex = xcalloc(room + 1, sizeof *ex);
    double *coh = xcalloc(room + 2, sizeof *coh); /* coh[k]: the coherence of the first k words */
    bool *have = xcalloc(room + 2, sizeof *have);
    size_t t = 0, nex = 0, widened = 0;
    json_t *trace = jarr();
    bool ended = false;
    int32_t prev = lex_find(l, START);
    while (words < cap && t < room) {
        double ts = now_ms();
        uint64_t seed = clock_seed();
        uint8_t bits[N_IN];
        learner_inputs(l, slots, prev, (unsigned)t, seed, ABL_NONE, bits);
        int32_t ranked[MAX_OPTS * 2], cands[MAX_OPTS];
        double p_first = 0;
        bool allow_end = t > 0 && sentence_end(said[t - 1]);
        size_t got = learner_propose(l, bits, ranked, MAX_OPTS * 2, allow_end, &p_first);
        size_t nr = 0; /* the circuit's ranking, allowed words only */
        for (size_t i = 0; i < got; i++)
            if (allowed(l, said, t, ranked[i]))
                ranked[nr++] = ranked[i];
        int32_t top1 = nr ? ranked[0] : -1;
        /* What the engine is shown: every word the learner may say while they
         * fit in one Choice (a teacher sees a beginner's whole vocabulary),
         * else the circuit's best and, when it is unsure, words the engine
         * adds from the lexicon. */
        size_t nc = 0, own;
        bool everything = got < MAX_OPTS * 2 && nr <= MAX_OPTS;
        if (everything) {
            for (size_t i = 0; i < nr; i++)
                cands[nc++] = ranked[i];
            own = nc < cfg->propose ? nc : cfg->propose;
        } else {
            for (size_t i = 0; i < nr && nc < cfg->propose; i++)
                cands[nc++] = ranked[i];
            own = nc;
            /* Past one Choice's worth of words the engine always looks over the
             * whole vocabulary too: with only the circuit's 8, the word a
             * reply needed was missing ("am" after "Hello! I", measured
             * 2026-10-17) and the engine chose among the wrong ones. */
            if (!cfg->alone) {
                int32_t wide[WIDEN_ADD];
                size_t nw = widen(l, cfg, state, said, t, cands, nc, wide, WIDEN_ADD, &cost, err, errlen);
                for (size_t i = 0; i < nw && nc < MAX_OPTS; i++)
                    cands[nc++] = wide[i];
                widened += nw > 0;
            }
        }
        json_t *step = jobj();
        jobj_set(step, "t", ju64(t));
        jobj_set(step, "seed_inputs", ju64(seed));
        jobj_set(step, "circuit_top1", top1 >= 0 ? jstr(l->v[top1].word) : jnull());
        jobj_set(step, "circuit_p_first", jf64(round(p_first * 1e4) / 1e4));
        jobj_set(step, "shown_all", jbool(everything));
        if (!nc) {
            jobj_set(step, "pick", jnull());
            jobj_set(step, "note", jstr("no word it knows may come here"));
            jarr_push(trace, step);
            break;
        }
        json_t *step_vars = jobj();
        int32_t said_word;
        if (cfg->alone) {
            /* The circuit alone: its own first word, no engine choice. The
             * engine still perceives the reply so far (the step variables). */
            if (ask_reply_state(cfg, state, said, t, slots, step_vars, &coh[t], &cost, err, errlen)) {
                json_free(step);
                json_free(step_vars);
                goto fail;
            }
            have[t] = true;
            said_word = top1;
            jobj_set(step, "pick", jstr(l->v[said_word].word));
            jobj_set(step, "from", jstr("circuit alone"));
        } else {
            step_t st = {0};
            float used[VAR_SLOTS];
            memcpy(used, slots, sizeof used); /* what the circuit saw, before this step's answers */
            if (rerank(l, cfg, state, said, t, cands, nc, slots, step_vars, &st, &cost, err, errlen)) {
                json_free(step);
                json_free(step_vars);
                goto fail;
            }
            bool from_circuit = false;
            for (size_t i = 0; i < own; i++)
                from_circuit |= cands[i] == st.top;
            example_t *e = &ex[nex++];
            memcpy(e->vars, used, sizeof e->vars);
            e->prev = prev;
            e->pos = (uint16_t)t;
            e->n = (uint16_t)st.n;
            memcpy(e->opt, st.opt, st.n * sizeof *st.opt);
            memcpy(e->q, st.q, st.n * sizeof *st.q);
            e->top = st.top;
            e->id = seed;
            l->v[st.top].picked++;
            l->steps++;
            said_word = st.pick;
            e->said = st.pick;
            coh[t] = st.coh;
            have[t] = true;
            jobj_set(step, "seed_draw", ju64(st.seed));
            jobj_set(step, "pick", jstr(l->v[st.pick].word));
            jobj_set(step, "engine_top", jstr(l->v[st.top].word));
            jobj_set(step, "from", jstr(from_circuit ? "circuit" : everything ? "vocabulary" : "widened"));
            jobj_set(step, "p", jf64(st.p_pick));
            jobj_set(step, "p_top", jf64(st.p_top));
            jobj_set(step, "options", st.shown);
        }
        jobj_set(step, "reply_vars", step_vars);
        jobj_set(step, "ms", jf64(round(now_ms() - ts)));
        jarr_push(trace, step);
        if (!strcmp(l->v[said_word].word, END)) {
            ended = true;
            break;
        }
        said[t++] = l->v[said_word].word;
        words += !is_punct(l->v[said_word].word);
        prev = said_word;
        if (cfg->on_word) {
            char *so_far = render_words(said, t);
            cfg->on_word(cfg->on_word_ud, so_far);
            free(so_far);
        }
    }
    /* The reply's final coherence, unless the step that ended it asked
     * already, then each word's reward: the change it made (RED). */
    if (t && !have[t]) {
        if (ask_reply_state(cfg, state, said, t, NULL, NULL, &coh[t], &cost, err, errlen))
            goto fail;
        have[t] = true;
    }
    double reward_sum = 0;
    for (size_t k = 0; k < nex; k++) {
        size_t at = ex[k].pos;
        bool word = ex[k].said >= 0 && strcmp(l->v[ex[k].said].word, END) && at + 1 <= t && have[at] && have[at + 1];
        ex[k].reward = word ? (float)(coh[at + 1] - coh[at]) : 0.0f;
        reward_sum += ex[k].reward;
    }
    for (size_t k = 0; k < trace->n; k++) {
        json_t *st = trace->items[k];
        const json_t *tt = jobj_get(st, "t");
        size_t at = tt && tt->t == J_NUM ? (size_t)tt->u : 0;
        if (have[at])
            jobj_set(st, "coherence_before", jf64(round(coh[at] * 1e4) / 1e4));
        for (size_t j = 0; j < nex; j++)
            if (ex[j].pos == at)
                jobj_set(st, "reward", jf64(round(ex[j].reward * 1e4) / 1e4));
    }
    *reply = render_words(said, t);

    /* The requirements: a Noul each about the finished reply. One below
     * 0.5 and the reply failed: its examples are kept, never learned from. */
    json_t *checks = jobj();
    bool failed = t == 0;
    if (cfg->nchecks && t) {
        sb_t s2;
        sb_init(&s2);
        /* The reply judged by name: the conversation holds earlier "Reply:"
         * lines too, and a bare "the reply" let the engine judge an older one
         * (2026-10-17: "Thank you." scored 0.035 on "grammatical English"). */
        sb_printf(&s2, "%s\n\nThe newest reply, the one to judge: ", state);
        json_quote(&s2, *reply, strlen(*reply));
        json_t *cq = jobj();
        for (size_t i = 0; i < cfg->nchecks; i++) {
            json_t *q = jobj();
            jobj_set(q, "type", jstr("noul"));
            sb_t ci;
            sb_init(&ci);
            sb_printf(&ci, "About the newest reply only: %s", cfg->checks[i]);
            jobj_set(q, "instructions", jstr(ci.s));
            sb_free(&ci);
            char id[24];
            snprintf(id, sizeof id, "c%zu", i);
            jobj_set(cq, id, q);
        }
        json_t *r2 = NULL;
        json_t *a2 = ask(cfg, s2.s, cq, &r2, &cost, err, errlen);
        sb_free(&s2);
        for (size_t i = 0; a2 && i < cfg->nchecks; i++) {
            char id[24];
            snprintf(id, sizeof id, "c%zu", i);
            json_t *v = jobj_get(jobj_get(a2, id), "noul");
            double p = v && v->t == J_NUM ? v->f : 0;
            failed |= p < 0.5;
            jobj_set(checks, cfg->checks[i], jf64(p));
        }
        json_free(r2);
        if (!a2)
            failed = true;
    }
    for (size_t i = 0; i < nex; i++)
        ex[i].failed = failed;

    /* Measured before learning (prequential): with every input, with the
     * engine's variables blanked (0.5), with the previous word blanked. If
     * blanking the variables costs nothing, it is a chain again. */
    evalres_t full = learner_eval(l, ex, nex, ABL_NONE);
    evalres_t no_vars = learner_eval(l, ex, nex, ABL_CONCEPTS);
    evalres_t no_prev = learner_eval(l, ex, nex, ABL_PREV);
    double tl = now_ms();
    size_t passes = cfg->learn && !cfg->alone ? learner_learn(l, ex, nex, cfg->train_ms, 0) : 0;
    double train_ms = now_ms() - tl;

    json_t *rec = jobj();
    jobj_set(rec, "turn", ju64(l->turns));
    jobj_set(rec, "person", jstr(person));
    jobj_set(rec, "reply", jstr(*reply));
    jobj_set(rec, "mode", jstr(cfg->alone ? "alone" : "taught"));
    jobj_set(rec, "failed_requirement", jbool(failed));
    jobj_set(rec, "lexicon", ju64(l->n));
    jobj_set(rec, "new_words", ju64(l->n - lex_before));
    jobj_set(rec, "speakable", ju64(lex_speakable_count(l)));
    jobj_set(rec, "length_planned", jstr(LENGTHS[li]));
    jobj_set(rec, "stage_cap", ju64(stage));
    jobj_set(rec, "words", ju64(words));
    jobj_set(rec, "tokens", ju64(t)); /* words and marks: what ms_per_word divides by */
    jobj_set(rec, "ended", jbool(ended));
    jobj_set(rec, "steps", ju64(nex));
    jobj_set(rec, "widened", ju64(widened));
    jobj_set(rec, "coherence_final", t && have[t] ? jf64(round(coh[t] * 1e4) / 1e4) : jnull());
    jobj_set(rec, "reward_sum", jf64(round(reward_sum * 1e4) / 1e4));
    jobj_set(rec, "eval", eval_json(full));
    jobj_set(rec, "eval_no_variables", eval_json(no_vars));
    jobj_set(rec, "eval_no_prev_word", eval_json(no_prev));
    jobj_set(rec, "train_passes", ju64(passes));
    jobj_set(rec, "train_ms", jf64(round(train_ms)));
    jobj_set(rec, "replay", ju64(l->nreplay));
    jobj_set(rec, "engine_calls", ju64(cost.calls));
    jobj_set(rec, "engine_tokens", ju64(cost.tokens));
    jobj_set(rec, "engine_ms", jf64(round(cost.ms)));
    jobj_set(rec, "ms", jf64(round(now_ms() - t0)));
    jobj_set(rec, "ms_per_word", jf64(t ? round((now_ms() - t0) / (double)t) : 0));
    jobj_set(rec, "turn_vars", turn_vars);
    jobj_set(rec, "checks", checks);
    jobj_set(rec, "trace", trace);
    *record = rec;
    history_add(h, person, *reply);
    free(said);
    free(ex);
    free(coh);
    free(have);
    free(state);
    return 0;
fail:
    json_free(trace);
    json_free(turn_vars);
    free(said);
    free(ex);
    free(coh);
    free(have);
    free(state);
    return -1;
}
