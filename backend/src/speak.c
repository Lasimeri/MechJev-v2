/* speak.c: see speak.h and speak.md. */
#define _GNU_SOURCE
#include "speak.h"

#include <ctype.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "http.h"
#include "util.h"

#define NONE_KEY "(none of these)"
#define STOP_KEY "(stop: the reply is complete)"
#define NOT_LISTED "(not listed)"
#define HISTORY_TURNS 4

void speak_cfg_default(speak_cfg_t *c)
{
    memset(c, 0, sizeof *c);
    c->engine = "127.0.0.1:8090";
    c->propose = 8;
    c->window = 8;
    c->group = 25;
    c->max_words = 60;
    c->learn = true;
}

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
        /* I, I'm, I've, I'll, I'd */
        if (x[0] == 'i' && (x[1] == 0 || x[1] == '\''))
            b.s[at] = 'I';
        if (cap && (unsigned char)b.s[at] < 0x80)
            b.s[at] = (char)toupper((unsigned char)b.s[at]);
        cap = false;
    }
    return sb_take(&b);
}

/* ---- the engine ------------------------------------------------------ */

typedef struct {
    double ms;
    size_t calls;
    uint64_t tokens;
} cost_t;

/* POST {state, questions} and return the answers object (owned by *resp). */
static json_t *ask(const speak_cfg_t *cfg, const char *state, json_t *questions, json_t **resp,
                   cost_t *cost, char *err, size_t errlen)
{
    json_t *req = jobj();
    jobj_set(req, "state", jstr(state));
    jobj_set(req, "questions", questions);
    char *body = json_dump(req);
    json_free(req);
    char *out = NULL;
    size_t on = 0;
    double t0 = now_ms();
    int st = http_call(cfg->engine, "POST", "/v1/systemone", body, strlen(body), &out, &on, 900000);
    free(body);
    if (cost) {
        cost->ms += now_ms() - t0;
        cost->calls++;
    }
    if (st < 0) {
        snprintf(err, errlen, "the decision engine at %s does not answer (make serve)", cfg->engine);
        free(out);
        return NULL;
    }
    char perr[256];
    json_t *v = json_parse(out, on, perr, sizeof perr);
    if (st != 200 || !v) {
        json_t *m = v ? jobj_get(v, "message") : NULL;
        snprintf(err, errlen, "the decision engine said %d: %s", st,
                 m && m->t == J_STR ? m->s : (v ? "?" : perr));
        free(out);
        json_free(v);
        return NULL;
    }
    free(out);
    json_t *u = jobj_get(jobj_get(v, "usage"), "input_tokens");
    if (cost && u && u->t == J_NUM)
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

/* The conversation as the engine reads it: fixed for the whole turn, so
 * the engine keeps it prefilled and each word costs only its question. */
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

/* The option shown for word wi after said[0..t): the last `window` words of
 * the reply with it added, as English. */
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
    /* a window that starts mid-sentence starts lower case */
    if (r[0] && (unsigned char)r[0] < 0x80 && !(r[0] == 'I' && (r[1] == ' ' || r[1] == '\'' || !r[1])))
        r[0] = (char)tolower((unsigned char)r[0]);
    sb_puts(&b, r);
    free(r);
    return sb_take(&b);
}

typedef struct {
    int32_t word;
    char *key;
} opt_t;

/* The rerank: the reply so far and one Choice over continuations. */
static int rerank(learner_t *l, const speak_cfg_t *cfg, const char *state, char **said, size_t t,
                  const int32_t *cands, size_t nc, bool with_none, int32_t *pick, double *p_pick,
                  json_t **shown, cost_t *cost, char *err, size_t errlen)
{
    opt_t *opt = xcalloc(nc + 2, sizeof *opt);
    size_t no = 0;
    for (size_t i = 0; i < nc; i++) {
        char *k = !strcmp(l->v[cands[i]].word, END) ? xstrdup(STOP_KEY)
                                                     : continuation(l, cfg, said, t, cands[i]);
        bool dup = false;
        for (size_t j = 0; j < no; j++)
            dup |= !strcmp(opt[j].key, k);
        if (dup) {
            free(k);
            continue;
        }
        opt[no++] = (opt_t){cands[i], k};
    }
    if (with_none)
        opt[no++] = (opt_t){-1, xstrdup(NONE_KEY)};
    char **keys = xmalloc(no * sizeof *keys);
    for (size_t i = 0; i < no; i++)
        keys[i] = opt[i].key;
    sb_t ins;
    sb_init(&ins);
    if (t == 0) {
        sb_puts(&ins, "The reply to the person is empty so far. Each option is a first word for it. "
                      "Which option is the best way to begin a short, natural English reply to what the "
                      "person said last?");
    } else {
        char *so_far = render_words(said, t);
        sb_t q;
        sb_init(&q);
        json_quote(&q, so_far, strlen(so_far));
        sb_printf(&ins, "The reply so far is %s. Each option is the end of the reply with one more word "
                        "or mark added. Which option continues it best, as natural and correct English "
                        "that answers what the person said last? Choose " STOP_KEY " only if the reply is "
                        "already complete.",
                  q.s);
        sb_free(&q);
        free(so_far);
    }
    if (with_none)
        sb_puts(&ins, " Choose " NONE_KEY " if no option continues the reply well.");
    json_t *qs = jobj();
    jobj_set(qs, "next", choice_q(ins.s, keys, no));
    sb_free(&ins);
    json_t *resp = NULL;
    json_t *ans = ask(cfg, state, qs, &resp, cost, err, errlen);
    int rc = -1;
    if (ans) {
        json_t *a = jobj_get(ans, "next");
        json_t *c = jobj_get(a, "choice");
        *pick = -2;
        for (size_t i = 0; i < no && c && c->t == J_STR; i++)
            if (!strcmp(opt[i].key, c->s)) {
                *pick = opt[i].word;
                *p_pick = prob_of(a, opt[i].key);
            }
        json_t *sh = jobj();
        for (size_t i = 0; i < no; i++)
            jobj_set(sh, opt[i].key, jf64(prob_of(a, opt[i].key)));
        *shown = sh;
        rc = *pick == -2 ? -1 : 0;
        if (rc)
            snprintf(err, errlen, "the engine chose an option that was not shown");
    }
    json_free(resp);
    for (size_t i = 0; i < no; i++)
        free(opt[i].key);
    free(opt);
    free(keys);
    return rc;
}

/* None of the learner's words fit: the lexicon put to the engine in
 * groups, each group's word (when it beats "not listed") a candidate. */
static size_t widen(learner_t *l, const speak_cfg_t *cfg, const char *state, char **said, size_t t,
                    const int32_t *shown, size_t nshown, int32_t *out, size_t k, cost_t *cost,
                    char *err, size_t errlen)
{
    int32_t *pool = xmalloc(l->n * sizeof *pool);
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
        sb_t q;
        sb_init(&q);
        json_quote(&q, so_far, strlen(so_far));
        sb_printf(&ins, "The reply so far is %s. Which of these words should come next in it?", q.s);
        sb_free(&q);
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
        char id[32];
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
        char id[32];
        snprintf(id, sizeof id, "g%zu", g);
        json_t *a = jobj_get(ans, id);
        json_t *c = jobj_get(a, "choice");
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
    learner_hear(l, heard, nh, heard, nh);
    l->turns++;
    char *state = conversation(h, person);

    /* How much to say: the engine's choice, capped by what the learner can
     * say (its stage: a few words more for every four it can speak). */
    size_t speakable = lex_speakable_count(l);
    size_t stage = 3 + speakable / 4;
    json_t *qs = jobj();
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
        free_words(heard, nh);
        return -1;
    }
    json_t *sc = jobj_get(jobj_get(ans, "length"), "score");
    double level = sc && sc->t == J_NUM ? sc->f : 1.0;
    size_t li = (size_t)fmin(3.0, fmax(0.0, round(level)));
    size_t plan = LENGTH_WORDS[li];
    json_free(resp);
    size_t cap = plan < stage ? plan : stage;
    if (cap > cfg->max_words)
        cap = cfg->max_words;

    char **said = xcalloc(cap + 1, sizeof *said);
    size_t t = 0, agree = 0, in_prop = 0, widened = 0, steps = 0;
    double nll_sum = 0;
    json_t *trace = jarr();
    bool ended = false;
    while (t < cap) {
        double ts = now_ms();
        hv_t ctx;
        learner_context(l, said, t, heard, nh, &ctx);
        cand_t *c = xmalloc((cfg->propose * 4 + 4) * sizeof *c);
        size_t got = learner_predict(l, &ctx, c, cfg->propose * 4 + 4, t > 0 && sentence_end(said[t - 1]));
        int32_t *cands = xmalloc((cfg->propose + 1) * sizeof *cands);
        size_t nc = 0;
        for (size_t i = 0; i < got && nc < cfg->propose; i++)
            if (allowed(l, said, t, c[i].word))
                cands[nc++] = c[i].word;
        int32_t top1 = nc ? cands[0] : -1;
        free(c);
        int32_t pick = -1;
        double p_pick = 0;
        json_t *shown = NULL;
        const char *from = "learner";
        if (nc && rerank(l, cfg, state, said, t, cands, nc, true, &pick, &p_pick, &shown, &cost, err,
                         errlen)) {
            free(cands);
            json_free(trace);
            goto fail;
        }
        if (pick < 0) {
            /* none of these, or nothing to propose */
            json_free(shown);
            shown = NULL;
            int32_t wide[16];
            size_t nw = widen(l, cfg, state, said, t, cands, nc, wide, cfg->propose < 16 ? cfg->propose : 16,
                              &cost, err, errlen);
            if (nw) {
                widened++;
                from = "widened";
                if (rerank(l, cfg, state, said, t, wide, nw, false, &pick, &p_pick, &shown, &cost, err,
                           errlen)) {
                    free(cands);
                    json_free(trace);
                    goto fail;
                }
            }
        }
        free(cands);
        steps++;
        json_t *st = jobj();
        jobj_set(st, "t", ju64(t));
        if (pick < 0) {
            /* nothing fits: the reply ends here */
            jobj_set(st, "pick", jnull());
            jobj_set(st, "from", jstr("nothing fits"));
            jobj_set(st, "ms", jf64(round(now_ms() - ts)));
            jarr_push(trace, st);
            json_free(shown);
            break;
        }
        double nll = learner_nll(l, &ctx, pick, 128.0); /* before learning it: prequential */
        nll_sum += nll;
        agree += pick == top1;
        in_prop += strcmp(from, "learner") == 0;
        if (cfg->learn)
            learner_update(l, &ctx, pick, top1);
        l->v[pick].picked++;
        l->steps++;
        jobj_set(st, "pick", jstr(l->v[pick].word));
        jobj_set(st, "from", jstr(from));
        jobj_set(st, "learner_top1", top1 >= 0 ? jstr(l->v[top1].word) : jnull());
        jobj_set(st, "p", jf64(p_pick));
        jobj_set(st, "nll", jf64(round(nll * 1e4) / 1e4));
        jobj_set(st, "options", shown ? shown : jobj());
        jobj_set(st, "ms", jf64(round(now_ms() - ts)));
        jarr_push(trace, st);
        if (!strcmp(l->v[pick].word, END)) {
            ended = true;
            break;
        }
        said[t++] = l->v[pick].word;
    }
    *reply = render_words(said, t);

    /* The restrictions: each a Noul about the finished reply. */
    json_t *checks = jobj();
    if (cfg->nchecks && t) {
        sb_t s2;
        sb_init(&s2);
        sb_printf(&s2, "%s\nReply: %s", state, *reply);
        json_t *cq = jobj();
        for (size_t i = 0; i < cfg->nchecks; i++) {
            json_t *q = jobj();
            jobj_set(q, "type", jstr("noul"));
            jobj_set(q, "instructions", jstr(cfg->checks[i]));
            char id[32];
            snprintf(id, sizeof id, "c%zu", i);
            jobj_set(cq, id, q);
        }
        json_t *r2 = NULL;
        json_t *a2 = ask(cfg, s2.s, cq, &r2, &cost, err, errlen);
        sb_free(&s2);
        for (size_t i = 0; a2 && i < cfg->nchecks; i++) {
            char id[32];
            snprintf(id, sizeof id, "c%zu", i);
            json_t *v = jobj_get(jobj_get(a2, id), "noul");
            jobj_set(checks, cfg->checks[i], jf64(v && v->t == J_NUM ? v->f : -1));
        }
        json_free(r2);
    }

    json_t *rec = jobj();
    jobj_set(rec, "turn", ju64(l->turns));
    jobj_set(rec, "person", jstr(person));
    jobj_set(rec, "reply", jstr(*reply));
    jobj_set(rec, "lexicon", ju64(l->n));
    jobj_set(rec, "new_words", ju64(l->n - lex_before));
    jobj_set(rec, "speakable", ju64(lex_speakable_count(l)));
    jobj_set(rec, "length_planned", jstr(LENGTHS[li]));
    jobj_set(rec, "stage_cap", ju64(stage));
    jobj_set(rec, "words", ju64(t));
    jobj_set(rec, "ended", jbool(ended));
    jobj_set(rec, "steps", ju64(steps));
    jobj_set(rec, "agree_top1", jf64(steps ? (double)agree / (double)steps : 0));
    jobj_set(rec, "from_learner", jf64(steps ? (double)in_prop / (double)steps : 0));
    jobj_set(rec, "widened", ju64(widened));
    jobj_set(rec, "nll_mean", jf64(steps ? nll_sum / (double)steps : 0));
    jobj_set(rec, "engine_calls", ju64(cost.calls));
    jobj_set(rec, "engine_tokens", ju64(cost.tokens));
    jobj_set(rec, "engine_ms", jf64(round(cost.ms)));
    jobj_set(rec, "ms", jf64(round(now_ms() - t0)));
    jobj_set(rec, "ms_per_word", jf64(t ? round((now_ms() - t0) / (double)t) : 0));
    jobj_set(rec, "checks", checks);
    jobj_set(rec, "trace", trace);
    *record = rec;
    history_add(h, person, *reply);
    free(said);
    free(state);
    free_words(heard, nh);
    return 0;
fail:
    free(said);
    free(state);
    free_words(heard, nh);
    return -1;
}
