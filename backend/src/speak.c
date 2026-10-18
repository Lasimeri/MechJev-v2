/* speak.c: see speak.h and speak.md. */
#define _GNU_SOURCE
#include "speak.h"

#include <ctype.h>
#include <errno.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "activity.h"
#include "http.h"
#include "util.h"

#define STOP_KEY "(stop: the reply is complete)"
#define NOT_LISTED "(not listed)"
#define HISTORY_TURNS 4
#define REPLIES_SHOWN 2 /* of them, MechJev's own replies shown: the last two (conversation) */
#define WIDEN_ADD 10 /* words the engine adds from the lexicon past one Choice's worth */
#define WIDEN_PER_GROUP 3 /* of them, at most this many from each group of the lexicon */
#define PHRASE_NEXT 2 /* continuations a candidate word gets in the phrase lookahead */

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
    c->lookahead = 3;
    c->keep_drop = 0.05;
    c->truth = true;
    c->grammar = true;
    c->persona = true; /* "No." to "Have you ever been to Zorvania?" with it, "Yes." without (speak.md) */
    c->copy = true; /* told, judged by the teacher: 8 of 12 with it, 0 of 12 without (pointer.md) */
    c->gated = true; /* the user's choice, 2026-10-18: gates everywhere (cleared stages 41/51 right, 2 confidently wrong, against 25/25) */
    c->veto = true;
    c->abstain = true;
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
/* The local variable's statement, its name in a slot and in turn_vars. */
#define NOVELTY "The share of the person's words it had never heard."
#define PARENT_HEARD 2 /* the parent's words count this many hearings (learner_hear_as) */

/* Grammar, two plain statements about the caveman pattern (speak.md): the
 * old "The reply is grammatical English." passed "Know who that is." (0.611);
 * a Choice of three (complete / fragment / not English) overlapped on real
 * replies ("Yes." 0.74 to 0.83, "Know who?" 0.86). A sentence with no
 * subject, or one whose words do not fit together, fails the reply: 26 of 27
 * on cases framed as a live turn is ("Know who that is." no subject 0.794). */
#define NO_SUBJECT_MIN 0.6 /* "no subject" fails the reply from here: short questions sat at 0.5 live */
static const char *GRAMMAR[2] = {
    "A sentence of the newest reply has no subject: it starts with a verb and leaves out who does it, "
    "as Know who that is. or Am too. do. (Yes, no, greetings, thanks, short answers like Blue., and "
    "short questions like What spro? are "
    "not such sentences.)",
    "A sentence of the newest reply is broken: its words do not fit together as English, as My it is. or "
    "Yes am fine.",
};

/* The words a reply most often begins with: offered at the first word when
 * MechJev can say them, and judged by the lookahead as starts (speak.md) */
static const char *STARTERS[] = {"i", "no"}; /* "yes" and "it" were too: forcing "it" gave "It." first */

/* The truth, three plain statements combined (speak.md: one compound
 * statement judged the world's truth instead, "Paris." 0.963): a fact about the
 * world it was not told, or an experience it could not have had, fails the
 * reply. On 12 cases with known answers, 12 right. */
static const char *TRUTH[3] = {
    "The newest reply states a fact about the world outside the conversation, not something about the "
    "one replying itself.",
    "What the newest reply states was said by the person earlier in this conversation.",
    "Read as an answer to the person, the newest reply says or implies that the one replying did, saw, "
    "ate or went somewhere, other than talking with the person and learning words.",
};


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
    /* The novelty, a variable computed here (the user's ask, 2026-10-17:
     * "confusion when presented with unknown/contextualized information"):
     * the share of the person's words it had never heard before the turn. */
    var_t nv = {0};
    nv.local = true;
    nv.text = xstrdup(NOVELTY);
    sb_t name;
    sb_init(&name);
    sb_printf(&name, "turn %s", NOVELTY);
    nv.slot = learner_slot(l, name.s);
    sb_free(&name);
    if (nv.slot < 0) {
        free(nv.text);
        snprintf(err, errlen, "%s: all %d variable slots are taken (the novelty needs one)", path, VAR_SLOTS);
        free(t);
        return -1;
    }
    c->vars = xrealloc(c->vars, (c->nvars + 1) * sizeof *c->vars);
    c->vars[c->nvars++] = nv;
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
static void vars_questions(const speak_cfg_t *cfg, bool step, const char *reply_so_far, const char *known,
                           json_t *qs)
{
    for (size_t i = 0; i < cfg->nvars; i++) {
        const var_t *v = &cfg->vars[i];
        if (v->step != step || v->local)
            continue;
        sb_t ins;
        sb_init(&ins);
        if (step) {
            sb_puts(&ins, "The reply so far is ");
            json_quote(&ins, reply_so_far, strlen(reply_so_far));
            sb_puts(&ins, ". ");
        }
        /* MechJev's words, for a question about what it knows */
        const char *ph = known ? strstr(v->text, "{words}") : NULL;
        if (ph)
            sb_printf(&ins, "%.*s%s%s", (int)(ph - v->text), v->text, known, ph + strlen("{words}"));
        else
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
        if (v->step != step || v->local)
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

char *render_words(const learner_t *l, char **w, size_t n)
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
        /* as MechJev heard it written: "MechJev", not "mechjev" (the teacher's
         * check gave "Mechjev." 0.19 to 0.26 against "MechJev." 0.96) */
        int32_t wi = l ? lex_find(l, x) : -1;
        sb_puts(&b, wi >= 0 ? lex_shown(l, wi) : x);
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
/* persona: who MechJev is, for the engine choosing its words and judging
 * them (MJ2_PERSONA): with nothing said of it, the engine chose "Yes." for
 * MechJev to "Do you know the Kael?" (2026-10-17), as if it knew. */
#define PERSONA "The one replying is MechJev, a beginner learning English: it has only talked with the person "\
                "and learned words, and knows only what the person told it.\n"
static char *conversation(const history_t *h, const char *person, bool persona)
{
    sb_t b;
    sb_init(&b);
    sb_puts(&b, "A conversation in English. The person speaks; the reply is written one word at a time.\n");
    if (persona)
        sb_puts(&b, PERSONA);
    size_t from = h->n > HISTORY_TURNS ? h->n - HISTORY_TURNS : 0;
    /* the person's lines all, MechJev's own only the last REPLIES_SHOWN: its
     * earlier fragments steered the engine to more ("Know" first to "Do you
     * know the Plink?" with five "Know who that is." above it, "No" with a
     * clean history, 2026-10-17) */
    for (size_t i = from; i < h->n; i++) {
        sb_printf(&b, "Person: %s\n", h->person[i]);
        if (i + REPLIES_SHOWN >= h->n)
            sb_printf(&b, "Reply: %s\n", h->reply[i]);
    }
    sb_printf(&b, "Person: %s", person);
    return sb_take(&b);
}

/* ---- the engine ------------------------------------------------------ */

typedef struct {
    double ms;
    size_t calls;
    uint64_t tokens;
} cost_t;

/* POST {state, questions}; the answers object (owned by *resp). rotations
 * > 0 sets the engine's option rotations for this request's Choices (the
 * xks extension; 0: the engine's own XKS_PERMUTATIONS). */
static json_t *ask_x(const speak_cfg_t *cfg, const char *state, json_t *questions, size_t rotations,
                     json_t **resp, cost_t *cost, char *err, size_t errlen)
{
    json_t *req = jobj();
    jobj_set(req, "state", jstr(state));
    jobj_set(req, "questions", questions);
    /* the engine's extension: suffixes padded to equal length, one forward
     * pass a round instead of one a length (engine.c) */
    json_t *ext = jobj();
    jobj_set(ext, "pad_equal", jbool(true));
    if (rotations)
        jobj_set(ext, "permutations", ju64(rotations));
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

static json_t *ask(const speak_cfg_t *cfg, const char *state, json_t *questions, json_t **resp,
                   cost_t *cost, char *err, size_t errlen)
{
    return ask_x(cfg, state, questions, 0, resp, cost, err, errlen);
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

static double level_of(const json_t *answer, size_t levels);

static double coherence_of(const json_t *ans)
{
    return level_of(jobj_get(ans, "coherence"), sizeof COHERENCE / sizeof *COHERENCE);
}

/* The viability of an unfinished reply: how good a start it is. Only a
 * finished reply can reach the top of COHERENCE, so a one-word reply that
 * is already complete ("Yes.") beat every good opening ("I") there; these
 * levels let an unfinished start reach the top. */
static const char *VIABILITY[] = {"no sense, or no fitting reply can follow",
                                  "a poor start: a fitting reply is unlikely from here", "a fitting reply could follow",
                                  "a good start toward a fitting reply", "the best kind of start for a fitting reply"};

/* hint: the lesson's goal in a taught turn (the teacher's check), NULL: none */
static json_t *viability_q(const char *so_far, const char *hint)
{
    sb_t ins;
    sb_init(&ins);
    sb_puts(&ins, "The reply so far is ");
    json_quote(&ins, so_far, strlen(so_far));
    sb_puts(&ins, ". It is unfinished: more words will follow. How good a start is it for a clear, correct "
                  "English reply that answers what the person said last?");
    if (hint && *hint)
        sb_printf(&ins, " A good reply here: %s", hint);
    json_t *q = jobj();
    jobj_set(q, "type", jstr("score"));
    jobj_set(q, "instructions", jstr(ins.s));
    sb_free(&ins);
    json_t *lv = jarr();
    for (size_t i = 0; i < sizeof VIABILITY / sizeof *VIABILITY; i++)
        jarr_push(lv, jstr(VIABILITY[i]));
    jobj_set(q, "criteria", lv);
    return q;
}

/* A score answer as a fraction of its levels: 0 the lowest, 1 the highest. */
static double level_of(const json_t *answer, size_t levels)
{
    const json_t *s = jobj_get(answer, "score");
    return s && s->t == J_NUM && levels > 1 ? s->f / (double)(levels - 1) : 0;
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

/* The words the person said that may come next and are not shown yet,
 * added to cands (up to MAX_OPTS). Returns the new count. */
static size_t offer_heard(const learner_t *l, char **said, size_t t, char **heard, size_t nh, int32_t *cands,
                          size_t nc)
{
    for (size_t i = 0; i < nh && nc < MAX_OPTS; i++) {
        int32_t wi = lex_find(l, heard[i]);
        if (wi < 0 || is_punct(heard[i]) || !allowed(l, said, t, wi))
            continue;
        bool shown = false;
        for (size_t j = 0; j < nc && !shown; j++)
            shown = cands[j] == wi;
        if (!shown)
            cands[nc++] = wi;
    }
    return nc;
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
    char *r = render_words(l, w, n);
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
    char *so_far = render_words(l, said, t);
    sb_t ins;
    sb_init(&ins);
    if (t == 0) {
        sb_puts(&ins, "The reply to the person is empty so far. Each option is a first word for it. "
                      "Which option is the best way to begin a natural English reply to what the "
                      "person said last?");
    } else {
        sb_puts(&ins, "The reply so far is ");
        json_quote(&ins, so_far, strlen(so_far));
        sb_puts(&ins, ". Each option is the end of the reply with one more word or mark added. Which "
                      "option continues it best, as natural and correct English that answers what the "
                      "person said last? Choose " STOP_KEY " only if the reply is already complete.");
    }
    /* a taught turn: the lesson's goal, as the shown words are its answer's
     * words (the engine still chooses each word among MechJev's own) */
    if (cfg->extra_check && *cfg->extra_check)
        sb_printf(&ins, " A good reply here: %s", cfg->extra_check);
    json_t *qs = jobj();
    jobj_set(qs, "next", choice_q(ins.s, keys, no));
    sb_free(&ins);
    vars_questions(cfg, true, so_far, NULL, qs);
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
static int ask_reply_state(const learner_t *l, const speak_cfg_t *cfg, const char *state, char **said, size_t t, float *slots,
                           json_t *seen, double *coh, cost_t *cost, char *err, size_t errlen)
{
    json_t *qs = jobj();
    char *so_far = render_words(l, said, t);
    if (slots)
        vars_questions(cfg, true, so_far, NULL, qs);
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

/* Whether the reply as it is says all it should: the user's stop rule
 * (2026-10-17), asked of the engine as a Noul once a sentence has ended.
 * Before it, the reply stopped when no single next word made it read
 * better as a whole; one more word is always unfinished, so in block 6 the
 * reply stopped at 7 of the 11 points where it could, nearly always at the
 * first sentence. */
static json_t *complete_q(const char *so_far)
{
    sb_t ins;
    sb_init(&ins);
    sb_puts(&ins, "The reply so far is ");
    json_quote(&ins, so_far, strlen(so_far));
    sb_puts(&ins, ". It says all that a good reply to the person's last message should say.");
    json_t *q = jobj();
    jobj_set(q, "type", jstr("noul"));
    jobj_set(q, "instructions", jstr(ins.s));
    sb_free(&ins);
    return q;
}

/* The coherence lookahead (the user's choice, 2026-10-17): the top
 * `lookahead` words of the rerank, in the order the user's settings give
 * (the repeat penalty applied), each scored by the engine in one request.
 * The word said is the one that makes the best start (viability_q). Where a
 * sentence has ended, the same request asks whether the reply already says
 * all it should (complete_q), and the reply stops only if it does.
 * Choosing the word on completeness had favoured whatever finished a reply
 * at once: "I" never once won (0.31-0.39 against "Yes" 0.39-0.75) and the
 * replies collapsed into "Yes." (block 5); the viability wording put "I"
 * first in 4 of the 5 turns where the rerank wanted it and kept "Yes" on
 * every yes/no question (the A/B in speak.md). The rerank's own top and
 * every score go into the trace. */
/* The phrase lookahead (the user's choice, 2026-10-17, against "I know
 * plink it not?"): each candidate word that does not end a sentence is
 * continued first, one Choice each in one request, among the same words
 * MechJev can say here; its best PHRASE_NEXT continuations make two-word
 * phrases, and the viability question judges the phrases instead of the
 * single words, which told good starts from bad ones little (0.44 to 0.53
 * live; offline "I do" 0.683 against "Know who" 0.520). The word said is the
 * first of the best phrase. Writes ph[], the phrases' second words (-1:
 * the word alone), and returns how many phrases there are. */
static size_t phrases(learner_t *l, const speak_cfg_t *cfg, const char *state, char **said, size_t t,
                      const step_t *st, const int32_t *cand, size_t nc, int32_t *pc, int32_t *ph, cost_t *cost,
                      char *err, size_t errlen)
{
    char **s2 = xmalloc((t + 2) * sizeof *s2);
    for (size_t j = 0; j < t; j++)
        s2[j] = said[j];
    json_t *qs = jobj();
    int32_t (*opt)[MAX_OPTS] = xcalloc(nc, sizeof *opt);
    size_t *nopt = xcalloc(nc, sizeof *nopt);
    char ***keys = xcalloc(nc, sizeof *keys);
    for (size_t i = 0; i < nc; i++) {
        if (sentence_end(l->v[cand[i]].word))
            continue;
        s2[t] = l->v[cand[i]].word;
        keys[i] = xcalloc(MAX_OPTS, sizeof **keys);
        for (size_t k = 0; k < st->n && nopt[i] < MAX_OPTS; k++) {
            int32_t w = st->opt[k];
            if (!strcmp(l->v[w].word, END) || !allowed(l, s2, t + 1, w))
                continue;
            opt[i][nopt[i]] = w;
            keys[i][nopt[i]] = continuation(l, cfg, s2, t + 1, w);
            nopt[i]++;
        }
        if (nopt[i] < 2)
            continue;
        char *so_far = render_words(l, s2, t + 1);
        sb_t ins;
        sb_init(&ins);
        sb_puts(&ins, "The reply so far is ");
        json_quote(&ins, so_far, strlen(so_far));
        sb_puts(&ins, ". Each option is the end of the reply with one more word or mark added. Which option "
                      "continues it best, as natural and correct English that answers what the person said last?");
        free(so_far);
        char id[24];
        snprintf(id, sizeof id, "c%zu", i);
        jobj_set(qs, id, choice_q(ins.s, keys[i], nopt[i]));
        sb_free(&ins);
    }
    json_t *resp = NULL;
    json_t *ans = qs->n ? ask(cfg, state, qs, &resp, cost, err, errlen) : (json_free(qs), NULL);
    size_t np = 0;
    for (size_t i = 0; i < nc; i++) {
        char id[24];
        snprintf(id, sizeof id, "c%zu", i);
        const json_t *a = ans ? jobj_get(ans, id) : NULL;
        int32_t best[PHRASE_NEXT];
        double bp[PHRASE_NEXT];
        size_t nb = 0;
        for (size_t k = 0; a && k < nopt[i]; k++) {
            double p = prob_of(a, keys[i][k]);
            size_t at = nb;
            while (at > 0 && bp[at - 1] < p)
                at--;
            if (at >= PHRASE_NEXT)
                continue;
            size_t last = nb < PHRASE_NEXT ? nb : PHRASE_NEXT - 1;
            memmove(&best[at + 1], &best[at], (last - at) * sizeof *best);
            memmove(&bp[at + 1], &bp[at], (last - at) * sizeof *bp);
            best[at] = opt[i][k];
            bp[at] = p;
            if (nb < PHRASE_NEXT)
                nb++;
        }
        if (!nb) { /* a sentence end, or nothing to continue with: the word alone */
            pc[np] = cand[i];
            ph[np++] = -1;
        }
        for (size_t j = 0; j < nb && np < 2 * MAX_OPTS; j++) {
            pc[np] = cand[i];
            ph[np++] = best[j];
        }
        for (size_t k = 0; keys[i] && k < nopt[i]; k++)
            free(keys[i][k]);
        free(keys[i]);
    }
    json_free(resp);
    free(keys);
    free(opt);
    free(nopt);
    free(s2);
    return np;
}

static int lookahead(learner_t *l, const speak_cfg_t *cfg, const char *state, char **said, size_t t,
                     step_t *st, char **heard, size_t nh, json_t *seen, cost_t *cost, char *err, size_t errlen)
{
    if (!cfg->lookahead || !st->n)
        return 0;
    size_t idx[MAX_OPTS];
    double w[MAX_OPTS];
    for (size_t i = 0; i < st->n; i++) {
        bool rep = false;
        for (size_t j = 0; j < t; j++)
            rep |= !strcmp(said[j], l->v[st->opt[i]].word);
        w[i] = rep && cfg->sampling.repeat_penalty > 0 ? st->q[i] / cfg->sampling.repeat_penalty : st->q[i];
        idx[i] = i;
    }
    for (size_t i = 1; i < st->n; i++) /* by weight, high first, stable */
        for (size_t j = i; j > 0 && w[idx[j]] > w[idx[j - 1]]; j--) {
            size_t tmp = idx[j];
            idx[j] = idx[j - 1];
            idx[j - 1] = tmp;
        }
    int32_t cand[MAX_OPTS];
    size_t nc = 0;
    for (size_t i = 0; i < st->n && nc < cfg->lookahead; i++)
        if (strcmp(l->v[st->opt[idx[i]]].word, END))
            cand[nc++] = st->opt[idx[i]];
    /* The person's own words of this line, wherever the rerank put them: "What
     * blix" (0.05) never reached the three judged, and "What fren", a word
     * from an older line, was said (2026-10-18). Judged, not forced. */
    for (size_t i = 0; i < st->n && nc < MAX_OPTS; i++) {
        const char *w = l->v[st->opt[i]].word;
        bool theirs = false, have = false;
        for (size_t h = 0; h < nh && !theirs; h++)
            theirs = !is_punct(heard[h]) && !strcmp(heard[h], w);
        for (size_t s = 0; s < t && theirs; s++)
            theirs = strcmp(said[s], w) != 0; /* not one already said */
        for (size_t k = 0; k < nc && !have; k++)
            have = cand[k] == st->opt[i];
        if (theirs && !have && strcmp(w, END))
            cand[nc++] = st->opt[i];
    }
    /* At the first word the rerank judges single words blind ("I" 0.01 against
     * "Who" 0.37 to "Do you know the Plink?", 2026-10-17): the starters a
     * sentence most often begins with are judged as starts too, whatever the
     * rerank gave them. */
    for (size_t i = 0; t == 0 && i < st->n && nc < MAX_OPTS; i++) {
        const char *w = l->v[st->opt[i]].word;
        bool starter = false, have = false;
        for (size_t s = 0; s < sizeof STARTERS / sizeof *STARTERS && !starter; s++)
            starter = !strcmp(w, STARTERS[s]);
        for (size_t k = 0; k < nc && !have; k++)
            have = cand[k] == st->opt[i];
        if (starter && !have)
            cand[nc++] = st->opt[i];
    }
    if (!nc)
        return 0;
    int32_t end = lex_find(l, END);
    bool may_stop = end >= 0 && t > 0 && sentence_end(said[t - 1]);
    json_t *qs = jobj();
    /* what is judged as a start: two-word phrases (phrases), or the words */
    int32_t pc[2 * MAX_OPTS], ph[2 * MAX_OPTS];
    size_t np = cfg->phrases ? phrases(l, cfg, state, said, t, st, cand, nc, pc, ph, cost, err, errlen) : 0;
    if (!np) {
        for (size_t i = 0; i < nc; i++)
            pc[i] = cand[i], ph[i] = -1;
        np = nc;
    }
    char **w2 = xmalloc((t + 2) * sizeof *w2);
    for (size_t j = 0; j < t; j++)
        w2[j] = said[j];
    char **ptext = xcalloc(np, sizeof *ptext);
    for (size_t i = 0; i < np; i++) {
        w2[t] = l->v[pc[i]].word;
        if (ph[i] >= 0)
            w2[t + 1] = l->v[ph[i]].word;
        ptext[i] = render_words(l, w2, t + 1 + (ph[i] >= 0));
        char id[24];
        snprintf(id, sizeof id, "v%zu", i);
        jobj_set(qs, id, viability_q(ptext[i], cfg->extra_check));
    }
    free(w2);
    if (may_stop) {
        char *as_is = render_words(l, said, t);
        jobj_set(qs, "complete", complete_q(as_is));
        free(as_is);
    }
    json_t *resp = NULL;
    json_t *ans = ask(cfg, state, qs, &resp, cost, err, errlen);
    if (!ans) {
        for (size_t i = 0; i < np; i++)
            free(ptext[i]);
        free(ptext);
        return -1;
    }
    json_t *start = jobj();
    double best = -1;
    int32_t pick = -1;
    for (size_t i = 0; i < np; i++) {
        char id[24];
        snprintf(id, sizeof id, "v%zu", i);
        double v = level_of(jobj_get(ans, id), sizeof VIABILITY / sizeof *VIABILITY);
        jobj_set(start, ptext[i], jf64(round(v * 1e4) / 1e4)); /* the phrase as judged */
        if (v > best) {
            best = v;
            pick = pc[i];
        }
    }
    for (size_t i = 0; i < np; i++)
        free(ptext[i]);
    free(ptext);
    jobj_set(seen, "start", start);
    if (may_stop) {
        const json_t *p = jobj_get(jobj_get(ans, "complete"), "noul");
        double complete = p && p->t == J_NUM ? p->f : 0;
        jobj_set(seen, "complete", jf64(round(complete * 1e4) / 1e4));
        if (complete >= 0.5)
            pick = end;
    }
    json_free(resp);
    st->pick = pick;
    for (size_t i = 0; i < st->n; i++) /* p: the said word's share of the shown */
        if (st->opt[i] == pick)
            st->p_pick = st->q[i];
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
        if (!lex_speakable(l, wi) || !allowed(l, said, t, wi) ||
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
    char *so_far = render_words(l, said, t);
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
    /* Each group's best WIDEN_PER_GROUP words above "(not listed)", not only
     * its first: with one a group, "I" and "," never came after "No" to "Do
     * you know the Plink?" (2026-10-17), and 84 words in 4 groups gave 4 at
     * most, not WIDEN_ADD. The same answers, no more questions. */
    for (size_t g = 0; ans && g < ng; g++) {
        char id[24];
        snprintf(id, sizeof id, "g%zu", g);
        json_t *a = jobj_get(ans, id);
        double none = prob_of(a, NOT_LISTED);
        size_t at0 = g * cfg->group, m = np - at0 < cfg->group ? np - at0 : cfg->group;
        int32_t best[WIDEN_PER_GROUP];
        double bp[WIDEN_PER_GROUP];
        size_t nb = 0;
        for (size_t i = 0; i < m; i++) {
            int32_t wi = pool[at0 + i];
            double p = prob_of(a, l->v[wi].word) - none;
            if (p <= 0)
                continue;
            size_t at = nb;
            while (at > 0 && bp[at - 1] < p)
                at--;
            if (at >= WIDEN_PER_GROUP)
                continue;
            size_t last = nb < WIDEN_PER_GROUP ? nb : WIDEN_PER_GROUP - 1;
            memmove(&best[at + 1], &best[at], (last - at) * sizeof *best);
            memmove(&bp[at + 1], &bp[at], (last - at) * sizeof *bp);
            best[at] = wi;
            bp[at] = p;
            if (nb < WIDEN_PER_GROUP)
                nb++;
        }
        for (size_t j = 0; j < nb; j++) {
            size_t at = got;
            while (at > 0 && score[at - 1] < bp[j])
                at--;
            if (at >= k)
                continue;
            size_t last = got < k ? got : k - 1;
            memmove(&out[at + 1], &out[at], (last - at) * sizeof *out);
            memmove(&score[at + 1], &score[at], (last - at) * sizeof *score);
            out[at] = best[j];
            score[at] = bp[j];
            if (got < k)
                got++;
        }
    }
    free(score);
    json_free(resp);
    free(pool);
    return got;
}

/* ---- a turn ---------------------------------------------------------- */

static const char *LENGTHS[] = {"a word or two", "one short sentence", "two or three sentences",
                                "a paragraph"};
/* The words a level allows at most (the stop rule ends a reply sooner). "A
 * word or two" was 3, and capped "I do not know what that is" (7) to
 * caveman speech: "Know who that is." (2026-10-17, the user's ask "fix the
 * caveman-like speech"); 8 leaves room for a whole short sentence. */
static const size_t LENGTH_WORDS[] = {8, 12, 30, 60};

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

/* A passed reply into the n-gram memory, at once: each example's word with
 * the NG_ORD words said before it and the meaning it was chosen under. The
 * words are said[0..), the reply as it was kept (the keeper's cut). */
static size_t ngram_learn_turn(learner_t *l, char **said, const example_t *ex, const int32_t *labels, size_t nex)
{
    int32_t start = lex_find(l, START);
    size_t n = 0;
    for (size_t k = 0; k < nex; k++) {
        if (labels[k] < 0)
            continue;
        size_t pos = ex[k].pos;
        int32_t hist[NG_ORD];
        for (size_t j = 0; j < NG_ORD; j++)
            hist[j] = pos + j >= NG_ORD ? lex_find(l, said[pos + j - NG_ORD]) : start;
        ng_learn(&l->ng, hist, labels[k], ex[k].vars);
        n++;
    }
    return n;
}

/* The requirements on a finished reply to the conversation `state`: each
 * line of restrictions.txt a Noul, and the grammar and the truth composed
 * from plain statements (GRAMMAR, TRUTH). *failed_out when one does not hold
 * (or the reply is empty, or the engine did not answer); the probabilities
 * into checks, *truth_j and *grammar_j. One path for a turn and for judging a
 * past reply again (speak_rejudge). */
static void judge_reply(const speak_cfg_t *cfg, const char *state, const char *reply, cost_t *cost, bool *failed_out,
                        json_t *checks, json_t **truth_j, json_t **grammar_j, char *err, size_t errlen)
{
    bool failed = !*reply;
    if (*reply && (cfg->nchecks || cfg->truth || cfg->grammar)) {
        sb_t s2;
        sb_init(&s2);
        /* The reply judged by name: the conversation holds earlier "Reply:"
         * lines too, and a bare "the reply" let the engine judge an older one
         * (2026-10-17: "Thank you." scored 0.035 on "grammatical English"). */
        sb_printf(&s2, "%s\n\nThe newest reply, the one to judge: ", state);
        json_quote(&s2, reply, strlen(reply));
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
        /* The truth (the user's ask, 2026-10-17: "Opinions need to be handled
         * seperately from experiences and facts"): three plain questions,
         * combined here, since one compound statement judged world truth, not
         * what MechJev could know (speak.md, "Opinions, experiences, facts") */
        for (size_t i = 0; cfg->truth && i < 3; i++) {
            json_t *q = jobj();
            jobj_set(q, "type", jstr("noul"));
            sb_t ci;
            sb_init(&ci);
            sb_printf(&ci, "About the newest reply only: %s", TRUTH[i]);
            jobj_set(q, "instructions", jstr(ci.s));
            sb_free(&ci);
            char id[24];
            snprintf(id, sizeof id, "t%zu", i);
            jobj_set(cq, id, q);
        }
        for (size_t i = 0; cfg->grammar && i < 2; i++) { /* grammar, two statements (GRAMMAR) */
            json_t *q = jobj();
            jobj_set(q, "type", jstr("noul"));
            sb_t ci;
            sb_init(&ci);
            sb_printf(&ci, "About the newest reply only: %s", GRAMMAR[i]);
            jobj_set(q, "instructions", jstr(ci.s));
            sb_free(&ci);
            char id[24];
            snprintf(id, sizeof id, "g%zu", i);
            jobj_set(cq, id, q);
        }
        json_t *r2 = NULL;
        json_t *a2 = ask(cfg, s2.s, cq, &r2, cost, err, errlen);
        sb_free(&s2);
        for (size_t i = 0; a2 && i < cfg->nchecks; i++) {
            char id[24];
            snprintf(id, sizeof id, "c%zu", i);
            json_t *v = jobj_get(jobj_get(a2, id), "noul");
            double p = v && v->t == J_NUM ? v->f : 0;
            failed |= p < 0.5;
            jobj_set(checks, cfg->checks[i], jf64(p));
        }
        if (a2 && cfg->grammar) {
            double gp[2];
            for (size_t i = 0; i < 2; i++) {
                char id[24];
                snprintf(id, sizeof id, "g%zu", i);
                json_t *v = jobj_get(jobj_get(a2, id), "noul");
                gp[i] = v && v->t == J_NUM ? v->f : 1;
            }
            /* no sentence without its subject (from NO_SUBJECT_MIN: the judge
             * gave "What fren?" 0.515 live, fragments 0.63 to 0.79), none broken */
            bool ok = gp[0] < NO_SUBJECT_MIN && gp[1] < 0.5;
            failed |= !ok;
            jobj_set(checks, "The reply is a complete English reply.", jf64(ok ? 1 : 0));
            json_t *gj = jobj();
            jobj_set(gj, "no_subject", jf64(round(gp[0] * 1e4) / 1e4));
            jobj_set(gj, "broken", jf64(round(gp[1] * 1e4) / 1e4));
            jobj_set(gj, "whole", jbool(ok));
            json_free(*grammar_j);
            *grammar_j = gj;
        }
        if (a2 && cfg->truth) {
            double tp[3];
            for (size_t i = 0; i < 3; i++) {
                char id[24];
                snprintf(id, sizeof id, "t%zu", i);
                json_t *v = jobj_get(jobj_get(a2, id), "noul");
                tp[i] = v && v->t == J_NUM ? v->f : 0;
            }
            /* a fact about the world it was not told, or an experience it could not have had */
            bool untrue = (tp[0] >= 0.5 && tp[1] < 0.5) || tp[2] >= 0.5;
            failed |= untrue;
            json_t *tj = jobj();
            jobj_set(tj, "fact", jf64(tp[0]));
            jobj_set(tj, "told", jf64(tp[1]));
            jobj_set(tj, "experience", jf64(tp[2]));
            jobj_set(tj, "true", jbool(!untrue));
            jobj_set(checks, "The reply claims no fact it was not told and no experience it could not have had.", jf64(untrue ? 0 : 1));
            json_free(*truth_j);
            *truth_j = tj; /* the three probabilities: the record's truth */
        }
        json_free(r2);
        if (!a2)
            failed = true;
    }
    *failed_out = failed;
}

/* The teacher's stage check of a taught turn, judged as the teacher judges
 * it (teach.c teacher_check: the person's line and the reply, no history,
 * no persona), its own request: in the turn's framing "Hot!" to "The ice is
 * cold. Is the fire hot?" passed it while the teacher gave 0.18, and the
 * reply was learned as right (2026-10-18). -1 if the engine did not answer. */
static int judge_teacher_check(const speak_cfg_t *cfg, const char *person, const char *reply, cost_t *cost,
                               double *p, char *err, size_t errlen)
{
    sb_t st;
    sb_init(&st);
    sb_printf(&st, "A conversation in English.\nPerson: %s\n\nThe newest reply, the one to judge: ", person);
    json_quote(&st, reply, strlen(reply));
    sb_t ci;
    sb_init(&ci);
    sb_printf(&ci, "About the newest reply only: %s", cfg->extra_check);
    json_t *q = jobj(), *qs = jobj();
    jobj_set(q, "type", jstr("noul"));
    jobj_set(q, "instructions", jstr(ci.s));
    sb_free(&ci);
    jobj_set(qs, "check", q);
    json_t *resp = NULL;
    json_t *ans = ask(cfg, st.s, qs, &resp, cost, err, errlen);
    sb_free(&st);
    if (!ans) {
        json_free(resp);
        return -1;
    }
    const json_t *v = jobj_get(jobj_get(ans, "check"), "noul");
    *p = v && v->t == J_NUM ? v->f : 0;
    json_free(resp);
    return 0;
}

/* ---- gated (cfg.gated) ----------------------------------------------- */

/* The user's ask (2026-10-18): "Rather than next word probabilities,
 * utilize the gated networks of the analog LLM we've made", both the
 * engine's choices and the mixing inside MechJev. Every word is MechJev's
 * by gates; the engine perceives the reply as it grows and judges it whole
 * (speak.md, "Gated"). */
#define GATED_TRIES 3  /* replies tried in a taught turn, the first accepted kept */
#define NG_GATE_ON 0.5 /* the n-gram says the word only with its gate this open... */
#define NG_AMONG 3     /* ...and only when the word is among the circuit's own best matches */
#define VETO_AMONG 4   /* the circuit's best matches the coherence veto may go down to */
/* the engine's viability a word needs to be said (gated, cfg.veto): from 17
 * test lines' 73 candidates against the teacher's check of each alone
 * (~/.cache/mechjev-v2-veto-calib.tsv): at 0.5, 14 lines right, 3 wrong, none
 * abstained; at 0.6, 13 right, 1 wrong, 3 abstained (said it did not know) */
#define COHERENCE_FLOOR 0.6
/* the teacher's own words in a taught turn (show): the stage's words, little
 * risk of a habit word, and the first word of a sentence comes only to "a
 * fitting reply could follow" (0.5: "My" 0.53 for "My day was good.", where
 * the one-word stages' 0.6 left it saying "What day?"); only as the first word:
 * later it let "my" end "My day is going my." */
#define COHERENCE_FLOOR_SHOWN 0.5

typedef struct {
    int32_t first;   /* the first word, -1: the gates' own */
    bool short_form; /* after the first word only a sentence's end, then the end */
} plan_t;

typedef struct {
    char **said;
    size_t t, words, nex;
    bool ended;
    example_t *ex;
    hv_t *ctxs;
    int32_t *labels;
    double *coh;
    bool *have;
    json_t *trace;
    int32_t first_alt; /* the circuit's best other first word (the third try's last resort) */
    bool abstained;       /* a word every gate offered was vetoed: it said it does not know instead */
    int abstain;          /* saying so: 1 the word next, 2 the question mark next, 3 the end next */
    int32_t abstain_word; /* the line's word it asks about (what the pointer attends to) */
} gen_t;

static void gen_init(gen_t *g, size_t room)
{
    memset(g, 0, sizeof *g);
    g->said = xcalloc(room + 1, sizeof *g->said);
    g->ex = xcalloc(room + 1, sizeof *g->ex);
    g->ctxs = xcalloc(room + 1, sizeof *g->ctxs);
    g->labels = xcalloc(room + 1, sizeof *g->labels);
    g->coh = xcalloc(room + 2, sizeof *g->coh);
    g->have = xcalloc(room + 2, sizeof *g->have);
    g->trace = jarr();
    g->first_alt = -1;
    g->abstain_word = -1;
}

static void gen_free(gen_t *g)
{
    free(g->said);
    free(g->ex);
    free(g->ctxs);
    free(g->labels);
    free(g->coh);
    free(g->have);
    json_free(g->trace);
    memset(g, 0, sizeof *g);
}

/* The first word of pool (in its own order) the engine finds coherent
 * enough to say next (viability_q, at floor or above): its index, -1 if
 * none is, -2 if the engine did not answer. Each word and its coherence go
 * onto vj, labelled src. The engine only says yes or no to a word; the
 * order is the pool's. */
static int first_coherent(const learner_t *l, const speak_cfg_t *cfg, const char *state, char **said, size_t t,
                          const int32_t *pool, size_t np, const char *src, double floor, json_t *vj, cost_t *cost,
                          char *err,
                          size_t errlen)
{
    if (!np)
        return -1;
    char **w = xmalloc((t + 2) * sizeof *w);
    for (size_t j = 0; j < t; j++)
        w[j] = said[j];
    json_t *q = jobj();
    for (size_t i = 0; i < np; i++) {
        w[t] = l->v[pool[i]].word;
        char *text = render_words(l, w, t + 1), id[16];
        snprintf(id, sizeof id, "c%zu", i);
        jobj_set(q, id, viability_q(text, cfg->extra_check));
        free(text);
    }
    free(w);
    json_t *resp = NULL;
    json_t *ans = ask(cfg, state, q, &resp, cost, err, errlen);
    if (!ans)
        return -2;
    int chosen = -1;
    for (size_t i = 0; i < np; i++) {
        char id[16];
        snprintf(id, sizeof id, "c%zu", i);
        double v = level_of(jobj_get(ans, id), sizeof VIABILITY / sizeof *VIABILITY);
        json_t *o = jobj();
        jobj_set(o, "word", jstr(l->v[pool[i]].word));
        jobj_set(o, "from", jstr(src));
        jobj_set(o, "coherence", jf64(round(v * 1e4) / 1e4));
        jarr_push(vj, o);
        if (chosen < 0 && v >= floor)
            chosen = (int)i;
    }
    json_free(resp);
    return chosen;
}

/* One reply by MechJev's gates. At each word: the plan's, else the
 * pointer's when its gate opens (pt_choose), else the n-gram's when its
 * meaning gate is NG_GATE_ON open and the word is among the circuit's
 * NG_AMONG best code matches (the one Markov-like source, kept below the
 * circuit), else the circuit's best code match (score_bits; no
 * distribution is mixed). The engine perceives the reply so far (the step
 * variables) and chooses nothing. Each word becomes an example; whether it
 * is learned is the judgment's (gated_tries). */
static int gated_generate(learner_t *l, const speak_cfg_t *cfg, const char *state, const float *slots0, size_t room,
                          size_t cap, size_t li, char **heard, size_t nh, char **show_w, size_t ns_show,
                          const pt_line_t *pl, plan_t plan, gen_t *g,
                          cost_t *cost, char *err, size_t errlen)
{
    float slots[VAR_SLOTS];
    memcpy(slots, slots0, sizeof slots);
    int32_t start = lex_find(l, START), end = lex_find(l, END), prev = start;
    if (cfg->on_word)
        cfg->on_word(cfg->on_word_ud, "");
    while (g->t < room && !(li == 0 && g->t > 0 && sentence_end(g->said[g->t - 1])) &&
           (g->words < cap || (g->t > 0 && !sentence_end(g->said[g->t - 1]) && g->words < cap + 4))) {
        size_t t = g->t;
        double ts = now_ms();
        uint64_t seed = clock_seed();
        uint8_t bits[N_IN];
        learner_inputs(l, slots, prev, (unsigned)t, seed, ABL_NONE, bits);
        hv_t ctx;
        learner_context(l, slots, (unsigned)t, heard, nh, &ctx);
        bool allow_end = t > 0 && sentence_end(g->said[t - 1]);
        int32_t ranked[MAX_OPTS * 2];
        size_t got = learner_propose(l, bits, NULL, NULL, ranked, MAX_OPTS * 2, allow_end, NULL);
        size_t nr = 0; /* the circuit's code matches, best first, the allowed words only */
        for (size_t i = 0; i < got; i++)
            if (allowed(l, g->said, t, ranked[i]))
                ranked[nr++] = ranked[i];
        json_t *step = jobj();
        jobj_set(step, "t", ju64(t));
        jobj_set(step, "seed_inputs", ju64(seed));
        jobj_set(step, "circuit_top1", nr ? jstr(l->v[ranked[0]].word) : jnull());
        int32_t word = -1;
        const char *from = "circuit";
        if (g->abstain) { /* saying it does not know: "what", the word, "?", the end */
            if (g->abstain == 1 && g->abstain_word >= 0)
                word = g->abstain_word, g->abstain = 2;
            else if (g->abstain <= 2)
                word = lex_find(l, "?"), g->abstain = 3;
            else
                word = end;
            from = "abstained";
        } else if (t == 0 && plan.first >= 0)
            word = plan.first, from = "tried";
        else if (plan.short_form && t == 1) {
            for (size_t i = 0; i < nr && word < 0; i++)
                if (sentence_end(l->v[ranked[i]].word))
                    word = ranked[i];
            if (word < 0)
                word = lex_find(l, ".");
            from = "short";
        } else if (plan.short_form && t >= 2)
            word = end, from = "short";
        /* the gates' words, in their order: the pointer's, the n-gram's, the
         * circuit's best code matches */
        int32_t order[VETO_AMONG + 2];
        const char *why[VETO_AMONG + 2];
        size_t no = 0;
        float att[PT_MAXT]; /* the pointer's attention over the line (the veto's second words) */
        bool have_att = false;
        if (word < 0 && cfg->copy && pl->n) { /* the pointer */
            uint8_t oa[OUT_BITS];
            learner_out_bits(l, bits, oa);
            int32_t sid[PT_SAID];
            size_t nsid = 0;
            for (size_t q = 0; q < t && nsid < PT_SAID; q++)
                sid[nsid++] = lex_find(l, g->said[q]);
            have_att = true;
            float pg = pt_forward(&l->pt, oa, slots, pl, t, sid, nsid, att);
            int32_t w = pt_choose(pl, att, pg);
            jobj_set(step, "pointer_gate", jf64(round(pg * 1e4) / 1e4));
            if (w >= 0 && w != start && allowed(l, g->said, t, w))
                order[no] = w, why[no++] = "pointer";
        }
        if (word < 0 && nr) { /* the n-gram, only among the circuit's own best */
            int32_t hist[NG_ORD];
            for (size_t k = 0; k < NG_ORD; k++)
                hist[k] = t + k >= NG_ORD ? lex_find(l, g->said[t + k - NG_ORD]) : start;
            double *p = xcalloc(l->n ? l->n : 1, sizeof *p);
            double gate = ng_predict(&l->ng, hist, slots, l->n, p);
            int32_t nw = -1;
            for (size_t i = 0; i < l->n; i++)
                if (p[i] > 0 && (nw < 0 || p[i] > p[nw]))
                    nw = (int32_t)i;
            free(p);
            jobj_set(step, "ngram_top", nw >= 0 ? jstr(l->v[nw].word) : jnull());
            jobj_set(step, "ngram_gate", jf64(round(gate * 1e4) / 1e4));
            bool among = false;
            for (size_t i = 0; i < nr && i < NG_AMONG; i++)
                among |= ranked[i] == nw;
            if (gate >= NG_GATE_ON && among && !(no && order[0] == nw))
                order[no] = nw, why[no++] = "ngram";
        }
        for (size_t i = 0; word < 0 && i < nr && i < VETO_AMONG; i++) {
            bool dup = false;
            for (size_t j = 0; j < no; j++)
                dup |= order[j] == ranked[i];
            if (!dup)
                order[no] = ranked[i], why[no++] = "circuit";
        }
        if (word < 0 && no) {
            word = order[0], from = why[0];
            /* The coherence veto (the user's ask, 2026-10-18: "using the LLM
             * as a coherence reference"): the engine says how coherent the
             * reply would be with each of the gates' words (viability_q),
             * and the first in the gates' order at COHERENCE_FLOOR or above
             * is said; the engine ranks nothing. A mark or the end is the
             * gates' alone. Measured: wrong habit words 0.28 to 0.30 ("Wet"
             * to a name question), right ones 0.52 and above
             * (~/.cache/mechjev-v2-judge-word.txt). */
            /* A sentence's end or the end is judged too, in a taught turn with
             * the teacher's words: whether the reply as it is says all it
             * should (complete_q). Passed unasked, the circuit's one-word habit
             * ended "My" at once ("My.", the day stage). Not in a turn alone: there
             * it refused right ends and line words were tacked on ("I am good
             * today MechJev.", "I am feeling fine now are."); a comma passes. */
            bool ask_ends = show_w && ns_show, may_end = false;
            for (size_t i = 0; ask_ends && i < no; i++)
                may_end |= t > 0 && (order[i] == end || sentence_end(l->v[order[i]].word));
            bool ends_first = is_punct(l->v[word].word) || word == end;
            if (cfg->veto && (ask_ends ? !(ends_first && !sentence_end(l->v[word].word) && word != end) : !ends_first)) {
                char **w2 = xmalloc((t + 2) * sizeof *w2);
                for (size_t j = 0; j < t; j++)
                    w2[j] = g->said[j];
                json_t *vq = jobj();
                for (size_t i = 0; i < no; i++) {
                    if (is_punct(l->v[order[i]].word) || order[i] == end)
                        continue;
                    w2[t] = l->v[order[i]].word;
                    char *text = render_words(l, w2, t + 1), id[16];
                    snprintf(id, sizeof id, "v%zu", i);
                    jobj_set(vq, id, viability_q(text, cfg->extra_check));
                    free(text);
                }
                free(w2);
                if (may_end) {
                    char *as_is = render_words(l, g->said, t);
                    jobj_set(vq, "complete", complete_q(as_is));
                    free(as_is);
                }
                json_t *vresp = NULL;
                json_t *vans = ask(cfg, state, vq, &vresp, cost, err, errlen);
                if (!vans) {
                    json_free(step);
                    return -1;
                }
                const json_t *cpl = may_end ? jobj_get(jobj_get(vans, "complete"), "noul") : NULL;
                double complete = cpl && cpl->t == J_NUM ? cpl->f : 1;
                json_t *vj = jarr();
                bool chosen = false;
                for (size_t i = 0; i < no; i++) {
                    char id[16];
                    snprintf(id, sizeof id, "v%zu", i);
                    const json_t *a = jobj_get(vans, id);
                    bool ends = order[i] == end || sentence_end(l->v[order[i]].word);
                    bool mark = is_punct(l->v[order[i]].word) || order[i] == end;
                    double v = ends ? complete : mark ? 1 : level_of(a, sizeof VIABILITY / sizeof *VIABILITY);
                    json_t *o = jobj();
                    jobj_set(o, "word", jstr(l->v[order[i]].word));
                    jobj_set(o, "from", jstr(why[i]));
                    jobj_set(o, ends ? "complete" : "coherence", mark && !ends ? jnull() : jf64(round(v * 1e4) / 1e4));
                    jarr_push(vj, o);
                    if (!chosen && v >= (ends ? 0.5 : COHERENCE_FLOOR)) {
                        chosen = true;
                        word = order[i], from = why[i];
                        if (i)
                            jobj_set(step, "vetoed", ju64(i)); /* the gates' words before it, vetoed */
                    }
                }
                json_free(vresp);
                /* Every word of the gates' vetoed: the line's own words, in the
                 * order the pointer's attention ranks them (its gate shut), are
                 * the next proposals. The right word is often there and in no
                 * gate's first four: "round" to "The moon is round. Is the moon
                 * round?" while the circuit offered wet 0.13, hot 0.15, big
                 * 0.30, know 0.46. */
                if (!chosen && have_att) {
                    int32_t line[VETO_AMONG];
                    size_t nl = 0;
                    bool used_tok[PT_MAXT] = {false};
                    while (nl < VETO_AMONG) {
                        int best = -1;
                        for (int j = 0; j < pl->n; j++) {
                            int32_t w = pl->tok[j];
                            if (used_tok[j] || w < 0 || w == start || w == end || is_punct(l->v[w].word) ||
                                !allowed(l, g->said, t, w))
                                continue;
                            bool dup = false;
                            for (size_t i = 0; i < no; i++)
                                dup |= order[i] == w;
                            for (size_t i = 0; i < nl; i++)
                                dup |= line[i] == w;
                            if (dup) {
                                used_tok[j] = true;
                                continue;
                            }
                            if (best < 0 || att[j] > att[best])
                                best = j;
                        }
                        if (best < 0)
                            break;
                        used_tok[best] = true;
                        line[nl++] = pl->tok[best];
                    }
                    int c2 = first_coherent(l, cfg, state, g->said, t, line, nl, "line", COHERENCE_FLOOR, vj, cost,
                                            err, errlen);
                    if (c2 == -2) {
                        json_free(vj);
                        json_free(step);
                        return -1;
                    }
                    if (c2 >= 0) {
                        chosen = true;
                        word = line[c2], from = "line";
                        jobj_set(step, "vetoed", ju64(no + (size_t)c2));
                    }
                }
                /* Then, in a taught turn, the teacher's own words (`show`), in
                 * the order it gave them: a reply of words MechJev never said
                 * can come only so ("My day was good." for the day stage,
                 * where the gates and the line offered "What day?" and "My."
                 * and every try was rejected), as a child repeats what it was
                 * shown, word by word, while the parent nods. */
                if (!chosen && show_w && ns_show) {
                    int32_t shown[2 * VETO_AMONG];
                    size_t nsw = 0;
                    for (size_t i = 0; i < ns_show && nsw < 2 * VETO_AMONG; i++) {
                        int32_t w = lex_find(l, show_w[i]);
                        if (w < 0 || w == start || w == end || is_punct(show_w[i]) || !allowed(l, g->said, t, w))
                            continue;
                        bool dup = false;
                        for (size_t j = 0; j < no; j++)
                            dup |= order[j] == w;
                        for (size_t j = 0; j < nsw; j++)
                            dup |= shown[j] == w;
                        if (!dup)
                            shown[nsw++] = w;
                    }
                    double fl = t ? COHERENCE_FLOOR : COHERENCE_FLOOR_SHOWN;
                    int c3 = first_coherent(l, cfg, state, g->said, t, shown, nsw, "shown", fl, vj, cost, err, errlen);
                    if (c3 == -2) {
                        json_free(vj);
                        json_free(step);
                        return -1;
                    }
                    if (c3 >= 0) {
                        chosen = true;
                        word = shown[c3], from = "shown";
                        jobj_set(step, "vetoed", ju64(no + (size_t)c3));
                    }
                }
                /* Nothing any gate offered is coherent: it says it does not
                 * know, never the gates' first anyway (the user, 2026-10-18:
                 * "The model architecture is still badly hallucinating";
                 * "Wet." came so, after wet 0.13, hot 0.15, big 0.30, know
                 * 0.46). At the first word its own form for the unknown, as
                 * the unknown stage taught it: "What", the line's word the
                 * pointer attends to, "?"; later, the reply ends where it is. */
                if (!chosen && cfg->abstain) {
                    int32_t what = lex_find(l, "what");
                    if (t > 0) {
                        word = sentence_end(g->said[t - 1]) ? end : lex_find(l, ".");
                        g->abstain = 3;
                    } else if (what >= 0 && allowed(l, g->said, 0, what)) {
                        /* the word asked about: one it never heard before this
                         * turn when the line has one ("What blorple?", not
                         * "What at?" to "Did you see the blorple at
                         * Zentara?"), else the one the pointer attends to */
                        int best = -1;
                        for (int pass = 0; pass < 2 && best < 0; pass++)
                            for (int j = 0; (pass == 0 || have_att) && j < pl->n; j++) {
                                int32_t w = pl->tok[j];
                                if (w < 0 || w == start || w == end || w == what || is_punct(l->v[w].word))
                                    continue;
                                if (pass == 0 && pl->f[j][0] <= 0) /* first: the never heard */
                                    continue;
                                if (best < 0 || (have_att && att[j] > att[best]))
                                    best = j;
                            }
                        word = what;
                        g->abstain = 1;
                        g->abstain_word = best >= 0 ? pl->tok[best] : -1;
                    }
                    if (g->abstain) {
                        g->abstained = true;
                        from = "abstained";
                        jobj_set(step, "abstained", jbool(true));
                    }
                }
                jobj_set(step, "veto", vj);
            }
        }
        if (t == 0)
            for (size_t i = 0; i < nr && g->first_alt < 0; i++)
                if (ranked[i] != word)
                    g->first_alt = ranked[i];
        if (word < 0) {
            jobj_set(step, "pick", jnull());
            jobj_set(step, "note", jstr("no word it knows may come here"));
            jarr_push(g->trace, step);
            break;
        }
        float used[VAR_SLOTS];
        memcpy(used, slots, sizeof used); /* what the circuit saw, before this step's answers */
        json_t *step_vars = jobj();
        if (ask_reply_state(l, cfg, state, g->said, t, slots, step_vars, &g->coh[t], cost, err, errlen)) {
            json_free(step);
            json_free(step_vars);
            return -1;
        }
        g->have[t] = true;
        g->ctxs[g->nex] = ctx;
        g->labels[g->nex] = word;
        example_t *e = &g->ex[g->nex++];
        memcpy(e->vars, used, sizeof e->vars);
        e->prev = prev;
        e->pos = (uint16_t)t;
        e->n = 1;
        e->opt[0] = word;
        e->top = e->said = word;
        e->id = seed;
        jobj_set(step, "pick", jstr(l->v[word].word));
        jobj_set(step, "from", jstr(from));
        jobj_set(step, "reply_vars", step_vars);
        jobj_set(step, "ms", jf64(round(now_ms() - ts)));
        jarr_push(g->trace, step);
        if (word == end) {
            g->ended = true;
            break;
        }
        g->said[g->t++] = l->v[word].word;
        g->words += !is_punct(l->v[word].word);
        prev = word;
        if (cfg->on_word) {
            char *so_far = render_words(l, g->said, g->t);
            cfg->on_word(cfg->on_word_ud, so_far);
            free(so_far);
        }
    }
    if (g->t && !g->have[g->t]) {
        if (ask_reply_state(l, cfg, state, g->said, g->t, NULL, NULL, &g->coh[g->t], cost, err, errlen))
            return -1;
        g->have[g->t] = true;
    }
    return 0;
}

/* A taught turn by gates: up to GATED_TRIES replies, each judged whole as a
 * turn is (the requirements, and the stage check in the teacher's framing),
 * the first accepted kept, never the best scored. The tries: the gates'
 * reply; its first word ended at once (the misses ran on: "Round it
 * green." where "Round!" passes); another first word, the stage's shown
 * words first, then the person's, then the circuit's next match (a word it
 * never said can come only so: "Welcome", "Bye"). A check, a probe, or a
 * turn that learns nothing has one try. The kept try's words are the
 * examples, and a rejected try's word where it first parts from them goes
 * with them as the one word beaten (no other credit can be told from the
 * judge's noise). Out: the kept try in *out, its judgment, the tries. */
static int gated_tries(learner_t *l, const speak_cfg_t *cfg, const char *state, const char *person,
                       const float *slots0, size_t room, size_t cap, size_t li, char **heard, size_t nh,
                       char **show_w, size_t ns_show, const pt_line_t *pl, gen_t *out, json_t **checks,
                       json_t **truth_j, json_t **grammar_j, bool *failed, json_t **tries, cost_t *cost, char *err,
                       size_t errlen)
{
    gen_t g[GATED_TRIES];
    json_t *ck[GATED_TRIES], *tj[GATED_TRIES], *gj[GATED_TRIES];
    bool fl[GATED_TRIES];
    int ng = 0, accepted = -1;
    size_t ntries = cfg->alone || cfg->probe || !cfg->learn ? 1 : GATED_TRIES;
    *tries = jarr();
    for (size_t k = 0; k < ntries && accepted < 0; k++) {
        plan_t plan = {-1, false};
        if (k == 1) {
            if (!ng || g[0].t <= 2) /* the gates' reply was a word and a mark already */
                continue;
            plan.first = lex_find(l, g[0].said[0]);
            plan.short_form = true;
        } else if (k == 2) {
            for (size_t pass = 0; pass < 2 && plan.first < 0; pass++) {
                char **w = pass ? heard : show_w;
                size_t n = pass ? nh : ns_show;
                for (size_t i = 0; i < n && plan.first < 0; i++) {
                    int32_t wi = lex_find(l, w[i]);
                    bool tried = wi < 0 || is_punct(w[i]) || !allowed(l, NULL, 0, wi);
                    for (int r = 0; r < ng && !tried; r++)
                        tried = g[r].nex && g[r].ex[0].top == wi;
                    if (!tried)
                        plan.first = wi;
                }
            }
            if (plan.first < 0 && ng && g[0].first_alt >= 0)
                plan.first = g[0].first_alt;
            if (plan.first < 0)
                break;
        }
        gen_init(&g[ng], room);
        if (gated_generate(l, cfg, state, slots0, room, cap, li, heard, nh, cfg->alone ? NULL : show_w,
                           cfg->alone ? 0 : ns_show, pl, plan, &g[ng], cost, err, errlen)) {
            for (int r = 0; r <= ng; r++)
                gen_free(&g[r]);
            for (int r = 0; r < ng; r++)
                json_free(ck[r]), json_free(tj[r]), json_free(gj[r]);
            json_free(*tries);
            return -1;
        }
        char *reply = render_words(l, g[ng].said, g[ng].t);
        ck[ng] = jobj();
        tj[ng] = jnull();
        gj[ng] = jnull();
        fl[ng] = false;
        judge_reply(cfg, state, reply, cost, &fl[ng], ck[ng], &tj[ng], &gj[ng], err, errlen);
        if (*reply && cfg->extra_check && *cfg->extra_check) {
            double tp = 0;
            bool ok = !judge_teacher_check(cfg, person, reply, cost, &tp, err, errlen) && tp >= 0.5;
            fl[ng] |= !ok;
            sb_t kk;
            sb_init(&kk);
            sb_printf(&kk, "The teacher's check: %s", cfg->extra_check);
            jobj_set(ck[ng], kk.s, jf64(tp));
            sb_free(&kk);
        }
        json_t *tr = jobj();
        jobj_set(tr, "try", ju64((uint64_t)k + 1));
        jobj_set(tr, "reply", jstr(reply));
        jobj_set(tr, "accepted", jbool(!fl[ng]));
        jobj_set(tr, "abstained", jbool(g[ng].abstained));
        jobj_set(tr, "checks", jclone(ck[ng]));
        jarr_push(*tries, tr);
        /* the try, where the harness shows it (activity.h) */
        json_t *a = jobj();
        jobj_set(a, "kind", jstr("try"));
        jobj_set(a, "instance", cfg->instance ? jstr(cfg->instance) : jnull());
        jobj_set(a, "try", ju64((uint64_t)k + 1));
        jobj_set(a, "person", jstr(person));
        jobj_set(a, "reply", jstr(reply));
        jobj_set(a, "accepted", jbool(!fl[ng]));
        jobj_set(a, "abstained", jbool(g[ng].abstained));
        jobj_set(a, "checks", jclone(ck[ng]));
        activity_append(a);
        json_free(a);
        free(reply);
        if (!fl[ng])
            accepted = ng;
        ng++;
    }
    int c = accepted >= 0 ? accepted : 0;
    /* the word each rejected try said where it first parts from the kept one */
    for (int r = 0; accepted >= 0 && r < ng; r++) {
        if (r == c)
            continue;
        size_t p = 0;
        while (p < g[r].nex && p < g[c].nex && g[r].ex[p].top == g[c].ex[p].top)
            p++;
        if (p < g[r].nex && p < g[c].nex && g[c].ex[p].n < MAX_OPTS)
            g[c].ex[p].opt[g[c].ex[p].n++] = g[r].ex[p].top;
    }
    *out = g[c];
    *checks = ck[c];
    *truth_j = tj[c];
    *grammar_j = gj[c];
    *failed = accepted < 0;
    for (int r = 0; r < ng; r++)
        if (r != c) {
            gen_free(&g[r]);
            json_free(ck[r]);
            json_free(tj[r]);
            json_free(gj[r]);
        }
    return 0;
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
    char **show_w = NULL;
    size_t ns_show = cfg->show && *cfg->show ? split_words(cfg->show, &show_w) : 0;
    size_t lex_before = l->n;
    /* the novelty: the share of the person's words it had never heard */
    size_t content = 0, novel = 0;
    for (size_t i = 0; i < nh; i++) {
        if (is_punct(heard[i]))
            continue;
        content++;
        int32_t wi = lex_find(l, heard[i]);
        novel += wi < 0 || l->v[wi].heard == 0;
    }
    double novelty = content ? (double)novel / (double)content : 0;
    /* the pointer's view of the line: each token's hearings before this turn */
    unsigned pt_heard[PT_MAXT];
    bool pt_mark[PT_MAXT];
    int pt_n = nh < PT_MAXT ? (int)nh : PT_MAXT;
    for (int j = 0; j < pt_n; j++) {
        int32_t wi = lex_find(l, heard[j]);
        pt_heard[j] = wi < 0 ? 0 : l->v[wi].heard;
        pt_mark[j] = is_punct(heard[j]);
    }
    /* a probe measures, it changes nothing; the parent's words count more
     * (the user's ask: MechJev "needs to imprint itself on the user like a
     * child would as it learns language from a parent") */
    if (!cfg->probe) {
        learner_hear_as(l, heard, nh, cfg->parent ? PARENT_HEARD : 1);
        learner_hear_casing(l, person); /* and how it is written ("MechJev") */
    }
    if (!cfg->probe)
        l->turns++;
    char *state = conversation(h, person, cfg->persona);
    /* the line's tokens now that they are heard (a new word has its id) */
    pt_line_t pl;
    {
        int32_t ids[PT_MAXT];
        for (int j = 0; j < pt_n; j++)
            ids[j] = lex_find(l, heard[j]);
        pt_line(&pl, ids, pt_heard, pt_mark, pt_n);
    }
    /* the words it can say, for a question about what it knows ({words}) */
    sb_t known;
    sb_init(&known);
    for (size_t i = 0; i < l->n; i++)
        if (lex_speakable(l, (int32_t)i) && strcmp(l->v[i].word, END) && !is_punct(l->v[i].word))
            sb_printf(&known, "%s%s", known.n ? ", " : "", l->v[i].word);

    /* The turn's variables and how much to say, in one request. The cap is
     * the learner's stage: a few words more for every four it can say. */
    float slots[VAR_SLOTS];
    for (int i = 0; i < VAR_SLOTS; i++)
        slots[i] = 0.5f;
    json_t *qs = jobj();
    vars_questions(cfg, false, NULL, known.s ? known.s : "", qs);
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
    sb_free(&known); /* asked */
    if (!ans) {
        free(state);
        free_words(heard, nh);
        free_words(show_w, ns_show);
        return -1;
    }
    json_t *turn_vars = jobj();
    vars_read(cfg, false, ans, slots, turn_vars);
    for (size_t i = 0; i < cfg->nvars; i++) /* the local variables: the novelty */
        if (cfg->vars[i].local && !cfg->vars[i].step) {
            slots[cfg->vars[i].slot] = (float)novelty;
            jobj_set(turn_vars, cfg->vars[i].text, jf64(round(novelty * 1e4) / 1e4));
        }
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
    size_t room = 2 * (cap + 4) + 2, words = 0;
    char **said = xcalloc(room + 1, sizeof *said);
    example_t *ex = xcalloc(room + 1, sizeof *ex);
    hv_t *ctxs = xcalloc(room + 1, sizeof *ctxs); /* each example's context, for the memory */
    int32_t *labels = xcalloc(room + 1, sizeof *labels);
    double *coh = xcalloc(room + 2, sizeof *coh); /* coh[k]: the coherence of the first k words */
    bool *have = xcalloc(room + 2, sizeof *have);
    size_t t = 0, nex = 0, widened = 0;
    /* the coherence keeper: the most coherent sentence end so far */
    size_t best_k = 0;
    double best_c = -1;
    bool cut = false;
    json_t *trace = jarr();
    bool ended = false;
    int32_t prev = lex_find(l, START);
    const int32_t prev_start = prev; /* START, where the reply has no word yet */
    /* Past the cap a reply only finishes its sentence, 4 words at most (a
     * cap of 3 cut "Hello! I am" off mid-sentence, 2026-10-17). */
    /* Level 0 ("a word or two") is one sentence: its first sentence end ends
     * it (the cap of 8 let "It. Plink? Yes! Feel that? ..." run on). */
    /* gated (cfg.gated): MechJev's gates make every word, the replies judged
     * whole; the kept try takes the place of the step loop below */
    json_t *tries_j = NULL, *g_checks = NULL, *g_truth = NULL, *g_grammar = NULL;
    bool g_failed = false, g_abstained = false;
    if (cfg->gated) {
        gen_t kept_try;
        if (gated_tries(l, cfg, state, person, slots, room, cap, li, heard, nh, show_w, ns_show, &pl, &kept_try,
                        &g_checks, &g_truth, &g_grammar, &g_failed, &tries_j, &cost, err, errlen))
            goto fail;
        free(said), free(ex), free(ctxs), free(labels), free(coh), free(have), json_free(trace);
        said = kept_try.said, ex = kept_try.ex, ctxs = kept_try.ctxs, labels = kept_try.labels;
        coh = kept_try.coh, have = kept_try.have, trace = kept_try.trace;
        t = kept_try.t, words = kept_try.words, nex = kept_try.nex, ended = kept_try.ended;
        g_abstained = kept_try.abstained;
        /* an accepted reply's words were MechJev's own and right: picked */
        for (size_t k = 0; !g_failed && cfg->learn && !cfg->alone && k < nex; k++) {
            l->v[ex[k].top].picked++;
            l->steps++;
        }
    }
    while (!cfg->gated && t < room && !(li == 0 && t > 0 && sentence_end(said[t - 1])) &&
           (words < cap || (t > 0 && !sentence_end(said[t - 1]) && words < cap + 4))) {
        double ts = now_ms();
        uint64_t seed = clock_seed();
        uint8_t bits[N_IN];
        learner_inputs(l, slots, prev, (unsigned)t, seed, ABL_NONE, bits);
        hv_t ctx;
        learner_context(l, slots, (unsigned)t, heard, nh, &ctx);
        int32_t ranked[MAX_OPTS * 2], cands[MAX_OPTS];
        double p_first = 0;
        bool allow_end = t > 0 && sentence_end(said[t - 1]);
        /* the n-gram memory's question: the words said before, the meaning */
        int32_t hist[NG_ORD];
        for (size_t k = 0; k < NG_ORD; k++)
            hist[k] = t + k >= NG_ORD ? lex_find(l, said[t + k - NG_ORD]) : prev_start;
        ng_query_t ngq = {hist, slots, -1, 0};
        size_t got = learner_propose(l, bits, &ctx, &ngq, ranked, MAX_OPTS * 2, allow_end, &p_first);
        size_t nr = 0; /* the circuit's ranking, allowed words only */
        for (size_t i = 0; i < got; i++)
            if (allowed(l, said, t, ranked[i]))
                ranked[nr++] = ranked[i];
        int32_t top1 = nr ? ranked[0] : -1;
        /* the pointer (cfg.copy, MJ2_COPY): adaptive attention over the
         * person's line; when its gate opens, the word it names is MechJev's
         * own word alone (a taught turn offers the person's words anyway) */
        float pt_gate = 1;
        int32_t pt_word = -1;
        if (cfg->copy && pl.n) {
            uint8_t oa[OUT_BITS];
            learner_out_bits(l, bits, oa);
            int32_t sid[PT_SAID];
            size_t nsid = 0;
            for (size_t q = 0; q < t && nsid < sizeof sid / sizeof *sid; q++)
                sid[nsid++] = lex_find(l, said[q]);
            float att[PT_MAXT];
            pt_gate = pt_forward(&l->pt, oa, slots, &pl, t, sid, nsid, att);
            int32_t w = pt_choose(&pl, att, pt_gate);
            if (w >= 0 && strcmp(l->v[w].word, START) && allowed(l, said, t, w))
                pt_word = w;
        }
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
        /* The person's own words, offered whatever the circuit ranks: in 20
         * turns (2026-10-17) 24 of the 34 content words the person used were
         * never shown, so the engine never had the choice of them. A word
         * heard only now is offered too: the person has just used it. The
         * engine still decides. */
        size_t heard_from = nc;
        if (!cfg->alone && ns_show) /* the teacher's answer words, in a demonstration */
            nc = offer_heard(l, said, t, show_w, ns_show, cands, nc);
        if (!cfg->alone)
            nc = offer_heard(l, said, t, heard, nh, cands, nc);
        /* the starters at the first word, when it can say them ("No" was not
         * shown to "Do you know the Plink?") */
        for (size_t s = 0; !cfg->alone && t == 0 && s < sizeof STARTERS / sizeof *STARTERS && nc < MAX_OPTS; s++) {
            int32_t wi = lex_find(l, STARTERS[s]);
            bool have = false;
            for (size_t k = 0; k < nc && !have; k++)
                have = cands[k] == wi;
            if (wi >= 0 && !have && lex_speakable(l, wi) && allowed(l, said, t, wi))
                cands[nc++] = wi;
        }
        json_t *step = jobj();
        jobj_set(step, "t", ju64(t));
        jobj_set(step, "seed_inputs", ju64(seed));
        jobj_set(step, "circuit_top1", top1 >= 0 ? jstr(l->v[top1].word) : jnull());
        jobj_set(step, "circuit_p_first", jf64(round(p_first * 1e4) / 1e4));
        jobj_set(step, "shown_all", jbool(everything));
        int32_t mem_top = learner_recall(l, &ctx, allow_end);
        jobj_set(step, "memory_top", mem_top >= 0 ? jstr(l->v[mem_top].word) : jnull());
        jobj_set(step, "ngram_top", ngq.top >= 0 ? jstr(l->v[ngq.top].word) : jnull());
        jobj_set(step, "ngram_gate", jf64(round(ngq.gate * 1e4) / 1e4));
        if (cfg->copy) {
            jobj_set(step, "pointer_gate", jf64(round(pt_gate * 1e4) / 1e4));
            jobj_set(step, "pointer_word", pt_word >= 0 ? jstr(l->v[pt_word].word) : jnull());
        }
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
            if (ask_reply_state(l, cfg, state, said, t, slots, step_vars, &coh[t], &cost, err, errlen)) {
                json_free(step);
                json_free(step_vars);
                goto fail;
            }
            have[t] = true;
            said_word = pt_word >= 0 ? pt_word : top1; /* the pointer's word when its gate opened */
            jobj_set(step, "pick", jstr(l->v[said_word].word));
            jobj_set(step, "from", jstr(pt_word >= 0 ? "pointer" : "circuit alone"));
        } else {
            step_t st = {0};
            float used[VAR_SLOTS];
            memcpy(used, slots, sizeof used); /* what the circuit saw, before this step's answers */
            if (rerank(l, cfg, state, said, t, cands, nc, slots, step_vars, &st, &cost, err, errlen)) {
                json_free(step);
                json_free(step_vars);
                goto fail;
            }
            int32_t rerank_top = st.top;
            json_t *look = jobj();
            if (lookahead(l, cfg, state, said, t, &st, heard, nh, look, &cost, err, errlen)) {
                json_free(look);
                json_free(step);
                json_free(step_vars);
                goto fail;
            }
            if (cfg->lookahead)
                st.top = st.pick; /* the decision the circuit learns: the coherent one */
            jobj_set(step, "rerank_top", jstr(l->v[rerank_top].word));
            jobj_set(step, "lookahead", look);
            size_t at_cand = nc; /* where the decided word was among the shown */
            for (size_t i = nc; i-- > 0;)
                if (cands[i] == st.top)
                    at_cand = i;
            ctxs[nex] = ctx;
            labels[nex] = st.top;
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
            jobj_set(step, "from", jstr(at_cand < own ? "circuit"
                                        : at_cand >= heard_from && at_cand < nc ? "heard"
                                        : everything ? "vocabulary"
                                                     : "widened"));
            jobj_set(step, "p", jf64(st.p_pick));
            jobj_set(step, "p_top", jf64(st.p_top));
            jobj_set(step, "options", st.shown);
        }
        jobj_set(step, "reply_vars", step_vars);
        jobj_set(step, "ms", jf64(round(now_ms() - ts)));
        jarr_push(trace, step);
        /* The coherence keeper (the user's ask, 2026-10-17: "a way of
         * managing coherence, and keeping it"). At every sentence end the
         * reply's coherence as a whole is held against the best sentence end
         * so far; falling more than keep_drop below it ends the reply, which
         * is then cut back to that best end (after the loop). Without it a
         * reply that never "said all" ran on and decayed: "Yes!" 0.772, then
         * "Yes! You too? Like to talk?" 0.558 (block 7). */
        if (cfg->keep_drop >= 0 && t > 0 && sentence_end(said[t - 1]) && have[t]) {
            if (coh[t] > best_c) {
                best_c = coh[t];
                best_k = t;
            } else if (best_c - coh[t] > cfg->keep_drop) {
                jobj_set(step, "keeper", jstr("fell from the best sentence end: cut back to it"));
                cut = true;
                ended = true;
                break;
            }
        }
        if (!strcmp(l->v[said_word].word, END)) {
            ended = true;
            break;
        }
        said[t++] = l->v[said_word].word;
        words += !is_punct(l->v[said_word].word);
        prev = said_word;
        if (cfg->on_word) {
            char *so_far = render_words(l, said, t);
            cfg->on_word(cfg->on_word_ud, so_far);
            free(so_far);
        }
    }
    /* The reply's final coherence, unless the step that ended it asked
     * already, then each word's reward: the change it made (RED). */
    if (t && !have[t]) {
        if (ask_reply_state(l, cfg, state, said, t, NULL, NULL, &coh[t], &cost, err, errlen))
            goto fail;
        have[t] = true;
    }
    /* The keeper's last look: a reply that ended (said all, or at its length)
     * more than keep_drop below its best sentence end is cut back too. Then
     * the cut: the reply ends at the best sentence end, and what the circuit
     * learns is the words up to it and the end there, nothing of what was cut
     * (the words after it were not said). */
    if (cfg->keep_drop >= 0 && !cut && best_k > 0 && t > best_k && have[t] && best_c - coh[t] > cfg->keep_drop)
        cut = true;
    json_t *kept = jnull();
    if (cut && best_k > 0 && best_k < t) {
        json_free(kept);
        kept = jobj();
        jobj_set(kept, "cut_at", ju64(best_k));
        jobj_set(kept, "tokens_before", ju64(t));
        jobj_set(kept, "coherence_best", jf64(round(best_c * 1e4) / 1e4));
        jobj_set(kept, "coherence_cut", jf64(round(coh[t] * 1e4) / 1e4));
        int32_t endw = lex_find(l, END);
        size_t keep = 0;
        for (size_t k = 0; k < nex; k++) {
            if (ex[k].pos > best_k || (ex[k].pos == best_k && endw < 0))
                continue;
            if (ex[k].pos == best_k) {
                ex[k].top = ex[k].said = endw;
                labels[k] = endw;
            }
            ex[keep] = ex[k];
            ctxs[keep] = ctxs[k];
            labels[keep] = labels[k];
            keep++;
        }
        nex = keep;
        t = best_k;
        words = 0;
        for (size_t k = 0; k < t; k++)
            words += !is_punct(said[k]);
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
    *reply = render_words(l, said, t);

    /* The requirements: a Noul each about the finished reply. One below
     * 0.5 and the reply failed: its examples are kept, never learned from. */
    json_t *checks = cfg->gated ? g_checks : jobj(), *truth_j = cfg->gated ? g_truth : jnull();
    json_t *grammar_j = cfg->gated ? g_grammar : jnull();
    bool failed = cfg->gated && g_failed; /* gated: the kept try is judged already */
    if (!cfg->gated)
        judge_reply(cfg, state, *reply, &cost, &failed, checks, &truth_j, &grammar_j, err, errlen);
    if (!cfg->gated && *reply && cfg->extra_check && *cfg->extra_check) {
        double tp = 0;
        bool ok = !judge_teacher_check(cfg, person, *reply, &cost, &tp, err, errlen) && tp >= 0.5;
        failed |= !ok;
        sb_t k;
        sb_init(&k);
        sb_printf(&k, "The teacher's check: %s", cfg->extra_check);
        jobj_set(checks, k.s, jf64(tp));
        sb_free(&k);
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
    /* The fast memory: what it would have said, measured before it learns;
     * then a reply that passed every requirement is learned at once (a
     * failed one is not: the memory keeps what it is given). */
    double mem_agree = learner_recall_agree(l, ctxs, labels, nex);
    memfit_t mfit = {0};
    double tm = now_ms();
    if (cfg->learn && !cfg->alone && !failed)
        mfit = learner_memorize(l, ctxs, labels, nex);
    size_t ng_learned = cfg->learn && !cfg->alone && !failed ? ngram_learn_turn(l, said, ex, labels, nex) : 0;
    double mem_ms = now_ms() - tm;
    double train_ms = now_ms() - tl;

    json_t *rec = jobj();
    jobj_set(rec, "turn", ju64(l->turns));
    jobj_set(rec, "person", jstr(person));
    jobj_set(rec, "reply", jstr(*reply));
    jobj_set(rec, "mode", jstr(cfg->probe ? "probe" : cfg->alone ? "alone" : "taught"));
    jobj_set(rec, "novelty", jf64(round(novelty * 1e4) / 1e4));
    jobj_set(rec, "parent", jbool(cfg->parent));
    jobj_set(rec, "truth", truth_j);
    jobj_set(rec, "grammar", grammar_j);
    jobj_set(rec, "failed_requirement", jbool(failed));
    jobj_set(rec, "lexicon", ju64(l->n));
    jobj_set(rec, "new_words", ju64(l->n - lex_before));
    jobj_set(rec, "speakable", ju64(lex_speakable_count(l)));
    jobj_set(rec, "length_planned", jstr(LENGTHS[li]));
    jobj_set(rec, "stage_cap", ju64(stage));
    jobj_set(rec, "words", ju64(words));
    jobj_set(rec, "tokens", ju64(t)); /* words and marks: what ms_per_word divides by */
    jobj_set(rec, "ended", jbool(ended));
    jobj_set(rec, "kept", kept); /* the coherence keeper's cut, or null */
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
    json_t *mj = jobj();
    size_t held = 0;
    for (size_t i = 0; i < l->mem.n; i++)
        held += l->mem.count[i] > 0;
    jobj_set(mj, "weight", jf64(l->mem_weight));
    jobj_set(mj, "agree_before", isnan(mem_agree) ? jnull() : jf64(round(mem_agree * 1e4) / 1e4));
    jobj_set(mj, "learned", ju64(mfit.learned));
    jobj_set(mj, "epochs", ju64(mfit.epochs));
    jobj_set(mj, "fit", mfit.epochs ? jf64(round(mfit.fit * 1e4) / 1e4) : jnull());
    jobj_set(mj, "words", ju64(held));
    jobj_set(mj, "contexts", ju64(l->mem.nreplay));
    jobj_set(mj, "ms", jf64(round(mem_ms)));
    jobj_set(rec, "memory", mj);
    json_t *nj = jobj(); /* the n-gram memory: its weight, what it learned now, its size */
    jobj_set(nj, "weight", jf64(l->ng_weight));
    jobj_set(nj, "learned", ju64(ng_learned));
    jobj_set(nj, "entries", ju64(l->ng.n));
    jobj_set(rec, "ngram", nj);
    jobj_set(rec, "engine_calls", ju64(cost.calls));
    jobj_set(rec, "engine_tokens", ju64(cost.tokens));
    jobj_set(rec, "engine_ms", jf64(round(cost.ms)));
    jobj_set(rec, "ms", jf64(round(now_ms() - t0)));
    jobj_set(rec, "ms_per_word", jf64(t ? round((now_ms() - t0) / (double)t) : 0));
    jobj_set(rec, "turn_vars", turn_vars);
    jobj_set(rec, "checks", checks);
    jobj_set(rec, "trace", trace);
    jobj_set(rec, "gated", jbool(cfg->gated));
    if (cfg->gated)
        jobj_set(rec, "abstained", jbool(g_abstained)); /* it said it does not know (gated, cfg.abstain) */
    if (tries_j)
        jobj_set(rec, "tries", tries_j); /* each try's reply and judgment, the kept one last accepted */
    *record = rec;
    if (!cfg->probe)
        history_add(h, person, *reply);
    free(said);
    free(ex);
    free(ctxs);
    free(labels);
    free_words(heard, nh);
    free_words(show_w, ns_show);
    free(coh);
    free(have);
    free(state);
    return 0;
fail:
    json_free(trace);
    json_free(turn_vars);
    json_free(tries_j);
    free(said);
    free(ex);
    free(ctxs);
    free(labels);
    free_words(heard, nh);
    free_words(show_w, ns_show);
    free(coh);
    free(have);
    free(state);
    return -1;
}

/* ---- the memory rebuilt from the record -------------------------------- */

/* vars_read the other way: a record's variables, by their text, into the
 * slots. */
static void vars_apply(const speak_cfg_t *cfg, bool step, const json_t *seen, float *slots)
{
    for (size_t i = 0; i < cfg->nvars; i++) {
        const var_t *v = &cfg->vars[i];
        const json_t *a = v->step == step ? jobj_get(seen, v->text) : NULL;
        if (!a)
            continue;
        if (!v->choice && a->t == J_NUM)
            slots[v->slot] = (float)a->f;
        for (size_t k = 0; v->choice && k < v->nopts; k++) {
            const json_t *p = jobj_get(a, v->opts[k]);
            if (p && p->t == J_NUM)
                slots[v->slot + (int)k] = (float)p->f;
        }
    }
}

static int cmp_double(const void *a, const void *b)
{
    double x = *(const double *)a, y = *(const double *)b;
    return x < y ? -1 : x > y;
}

int speak_memory_rebuild(learner_t *l, const speak_cfg_t *cfg, const char *turns_path, json_t **report,
                         char *err, size_t errlen)
{
    FILE *f = fopen(turns_path, "r");
    if (!f) {
        snprintf(err, errlen, "%s: %s", turns_path, strerror(errno));
        return -1;
    }
    double t0 = now_ms();
    learner_memory_reset(l);
    char *line = NULL;
    size_t cap = 0, turns = 0, steps = 0, tens_right = 0, tens_asked = 0, right_all = 0, asked_all = 0;
    size_t right_word = 0, asked_word = 0; /* labels that are words, not marks or the end */
    size_t nmargin = 0, capmargin = 0;
    double *margin = NULL;
    json_t *tens = jarr();
    ssize_t len;
    while ((len = getline(&line, &cap, f)) > 0) {
        char perr[128];
        json_t *r = json_parse(line, (size_t)len, perr, sizeof perr);
        const json_t *mode = r ? jobj_get(r, "mode") : NULL, *failed = r ? jobj_get(r, "failed_requirement") : NULL;
        const json_t *trace = r ? jobj_get(r, "trace") : NULL, *person = r ? jobj_get(r, "person") : NULL;
        if (!r || !mode || mode->t != J_STR || strcmp(mode->s, "taught") || !failed || failed->t != J_BOOL ||
            failed->b || !trace || trace->t != J_ARR || !person || person->t != J_STR) {
            json_free(r);
            continue;
        }
        float slots[VAR_SLOTS];
        for (int i = 0; i < VAR_SLOTS; i++)
            slots[i] = 0.5f;
        vars_apply(cfg, false, jobj_get(r, "turn_vars"), slots);
        char **heard;
        size_t nh = split_words(person->s, &heard);
        hv_t *ctxs = xcalloc(trace->n + 1, sizeof *ctxs);
        int32_t *labels = xcalloc(trace->n + 1, sizeof *labels);
        size_t n = 0;
        for (size_t k = 0; k < trace->n; k++) {
            const json_t *st = trace->items[k], *tt = jobj_get(st, "t");
            const json_t *w = jobj_get(st, "engine_top");
            if (!w || w->t != J_STR)
                w = jobj_get(st, "pick");
            int32_t wi = w && w->t == J_STR ? lex_find(l, w->s) : -1;
            if (wi >= 0 && tt && tt->t == J_NUM) {
                learner_context(l, slots, (unsigned)tt->u, heard, nh, &ctxs[n]);
                labels[n++] = wi;
            }
            /* the step's answers about the reply so far feed the next word
             * (as live: a step sees the answers the step before was given) */
            vars_apply(cfg, true, jobj_get(st, "reply_vars"), slots);
        }
        for (size_t k = 0; k < n; k++) {
            int32_t p = learner_recall(l, &ctxs[k], true);
            if (p < 0)
                continue;
            size_t ok = p == labels[k];
            tens_right += ok, right_all += ok;
            tens_asked++, asked_all++;
            if (!is_punct(l->v[labels[k]].word) && strcmp(l->v[labels[k]].word, END))
                right_word += ok, asked_word++;
            double m = learner_recall_margin(l, &ctxs[k]);
            if (!isnan(m)) {
                if (nmargin == capmargin) {
                    capmargin = capmargin ? capmargin * 2 : 256;
                    margin = xrealloc(margin, capmargin * sizeof *margin);
                }
                margin[nmargin++] = m;
            }
        }
        learner_memorize(l, ctxs, labels, n);
        steps += n;
        turns++;
        if (turns % 10 == 0) {
            jarr_push(tens, tens_asked ? jf64(round(1e4 * (double)tens_right / (double)tens_asked) / 1e4) : jnull());
            tens_right = tens_asked = 0;
        }
        free(ctxs);
        free(labels);
        free_words(heard, nh);
        json_free(r);
    }
    free(line);
    fclose(f);
    if (tens_asked)
        jarr_push(tens, jf64(round(1e4 * (double)tens_right / (double)tens_asked) / 1e4));
    qsort(margin, nmargin, sizeof *margin, cmp_double);
    json_t *o = jobj(), *mg = jobj();
    jobj_set(o, "turns", ju64(turns));
    jobj_set(o, "steps", ju64(steps));
    jobj_set(o, "agree_before_learning", asked_all ? jf64(round(1e4 * (double)right_all / (double)asked_all) / 1e4) : jnull());
    jobj_set(o, "agree_by_ten_turns", tens);
    jobj_set(o, "agree_on_words", asked_word ? jf64(round(1e4 * (double)right_word / (double)asked_word) / 1e4) : jnull());
    jobj_set(o, "agree_on_marks_and_end",
             asked_all > asked_word ? jf64(round(1e4 * (double)(right_all - right_word) / (double)(asked_all - asked_word)) / 1e4) : jnull());
    jobj_set(o, "asked_words", ju64(asked_word));
    for (size_t q = 0; q < 3 && nmargin; q++) {
        static const char *names[] = {"p10", "p50", "p90"};
        static const double at[] = {0.1, 0.5, 0.9};
        jobj_set(mg, names[q], jf64(round(1e4 * margin[(size_t)(at[q] * (double)(nmargin - 1))]) / 1e4));
    }
    jobj_set(o, "margin_first_second", mg);
    size_t held = 0;
    for (size_t i = 0; i < l->mem.n; i++)
        held += l->mem.count[i] > 0;
    jobj_set(o, "words", ju64(held));
    jobj_set(o, "contexts", ju64(l->mem.nreplay));
    jobj_set(o, "ms", jf64(round(now_ms() - t0)));
    free(margin);
    *report = o;
    return 0;
}

/* ---- shared with the teacher (teach.c) ------------------------------ */

json_t *speak_ask(const speak_cfg_t *cfg, const char *state, json_t *questions, json_t **resp, char *err,
                  size_t errlen)
{
    cost_t cost = {0};
    return ask(cfg, state, questions, resp, &cost, err, errlen);
}

json_t *speak_choice_q(const char *instructions, char **keys, size_t n) { return choice_q(instructions, keys, n); }

double speak_prob(const json_t *answer, const char *key) { return prob_of(answer, key); }

bool speak_sentence_end(const char *w) { return sentence_end(w); }

uint64_t speak_clock_seed(void) { return clock_seed(); }

char *speak_conversation(const history_t *h, const char *person) { return conversation(h, person, false); }

json_t *speak_ask_rotations(const speak_cfg_t *cfg, const char *state, json_t *questions, size_t rotations,
                            json_t **resp, char *err, size_t errlen)
{
    cost_t cost = {0};
    return ask_x(cfg, state, questions, rotations, resp, &cost, err, errlen);
}

int speak_ngram_rebuild(learner_t *l, const speak_cfg_t *cfg, const char *turns_path, size_t *turns_out,
                        size_t *steps_out, char *err, size_t errlen)
{
    ng_reset(&l->ng);
    *turns_out = *steps_out = 0;
    FILE *f = fopen(turns_path, "r");
    if (!f) {
        if (errno == ENOENT) /* a new learner: nothing said yet */
            return 0;
        snprintf(err, errlen, "%s: %s", turns_path, strerror(errno));
        return -1;
    }
    int32_t start = lex_find(l, START), end = lex_find(l, END);
    char *line = NULL;
    size_t cap = 0;
    ssize_t len;
    while ((len = getline(&line, &cap, f)) > 0) {
        char perr[128];
        json_t *r = json_parse(line, (size_t)len, perr, sizeof perr);
        const json_t *mode = r ? jobj_get(r, "mode") : NULL, *failed = r ? jobj_get(r, "failed_requirement") : NULL;
        const json_t *trace = r ? jobj_get(r, "trace") : NULL;
        if (!r || !mode || mode->t != J_STR || strcmp(mode->s, "taught") || !failed || failed->t != J_BOOL ||
            failed->b || !trace || trace->t != J_ARR) {
            json_free(r);
            continue;
        }
        float slots[VAR_SLOTS];
        for (int i = 0; i < VAR_SLOTS; i++)
            slots[i] = 0.5f;
        vars_apply(cfg, false, jobj_get(r, "turn_vars"), slots);
        /* the keeper's cut: the words after it were not said, and the end
         * is learned there */
        const json_t *kept = jobj_get(r, "kept"), *ca = kept ? jobj_get(kept, "cut_at") : NULL;
        size_t cut = ca && ca->t == J_NUM ? (size_t)ca->u : (size_t)-1;
        int32_t hist[NG_ORD];
        for (size_t j = 0; j < NG_ORD; j++)
            hist[j] = start;
        for (size_t k = 0; k < trace->n; k++) {
            const json_t *st = trace->items[k], *tt = jobj_get(st, "t");
            if (!tt || tt->t != J_NUM)
                continue;
            size_t t = (size_t)tt->u;
            if (t > cut)
                break;
            const json_t *w = jobj_get(st, "engine_top"), *pk = jobj_get(st, "pick");
            if (!w || w->t != J_STR)
                w = pk;
            int32_t label = t == cut ? end : w && w->t == J_STR ? lex_find(l, w->s) : -1;
            if (label >= 0) {
                ng_learn(&l->ng, hist, label, slots);
                (*steps_out)++;
            }
            if (t == cut)
                break;
            /* the step's answers about the reply so far feed the next word */
            vars_apply(cfg, true, jobj_get(st, "reply_vars"), slots);
            memmove(hist, hist + 1, (NG_ORD - 1) * sizeof *hist);
            hist[NG_ORD - 1] = pk && pk->t == J_STR ? lex_find(l, pk->s) : -1;
        }
        (*turns_out)++;
        json_free(r);
    }
    free(line);
    fclose(f);
    return 0;
}

int speak_casing_rebuild(learner_t *l, const char *turns_path, size_t *lines, char *err, size_t errlen)
{
    for (size_t i = 0; i < l->n; i++) {
        l->v[i].cap_mid = l->v[i].low_mid = l->v[i].cased_votes = 0;
        free(l->v[i].cased);
        l->v[i].cased = NULL;
    }
    *lines = 0;
    FILE *f = fopen(turns_path, "r");
    if (!f) {
        if (errno == ENOENT)
            return 0;
        snprintf(err, errlen, "%s: %s", turns_path, strerror(errno));
        return -1;
    }
    char *line = NULL;
    size_t cap = 0;
    ssize_t len;
    while ((len = getline(&line, &cap, f)) > 0) {
        char perr[128];
        json_t *r = json_parse(line, (size_t)len, perr, sizeof perr);
        const json_t *person = r ? jobj_get(r, "person") : NULL, *mode = r ? jobj_get(r, "mode") : NULL;
        if (person && person->t == J_STR && !(mode && mode->t == J_STR && !strcmp(mode->s, "probe"))) {
            learner_hear_casing(l, person->s);
            (*lines)++;
        }
        json_free(r);
    }
    free(line);
    fclose(f);
    return 0;
}

int speak_pointer_data(learner_t *l, const speak_cfg_t *cfg, const char *turns_path, pt_data_t *d, char *err,
                       size_t errlen)
{
    memset(d, 0, sizeof *d);
    FILE *f = fopen(turns_path, "r");
    if (!f) {
        if (errno == ENOENT)
            return 0;
        snprintf(err, errlen, "%s: %s", turns_path, strerror(errno));
        return -1;
    }
    int32_t start = lex_find(l, START), end = lex_find(l, END);
    /* the hearings replayed as a turn makes them (speak_turn): every turn
     * but a probe, the parent's twice; a word heard through /v1/hear is not
     * in turns.jsonl and is missed */
    unsigned *heard = xcalloc(l->n + 1, sizeof *heard);
    size_t capl = 0, caps = 0;
    char *line = NULL;
    size_t cap = 0;
    ssize_t len;
    while ((len = getline(&line, &cap, f)) > 0) {
        char perr[128];
        json_t *r = json_parse(line, (size_t)len, perr, sizeof perr);
        const json_t *person = r ? jobj_get(r, "person") : NULL, *mode = r ? jobj_get(r, "mode") : NULL;
        if (!r || !person || person->t != J_STR || !mode || mode->t != J_STR || !strcmp(mode->s, "probe")) {
            json_free(r);
            continue;
        }
        char **words = NULL;
        size_t nw = split_words(person->s, &words);
        const json_t *failed = jobj_get(r, "failed_requirement"), *trace = jobj_get(r, "trace");
        if (!strcmp(mode->s, "taught") && failed && failed->t == J_BOOL && !failed->b && trace && trace->t == J_ARR &&
            nw) {
            int n = nw < PT_MAXT ? (int)nw : PT_MAXT;
            int32_t ids[PT_MAXT];
            unsigned hn[PT_MAXT];
            bool mark[PT_MAXT];
            for (int j = 0; j < n; j++) {
                ids[j] = lex_find(l, words[j]);
                hn[j] = ids[j] < 0 ? 0 : heard[ids[j]];
                mark[j] = is_punct(words[j]);
            }
            if (d->nlines == capl)
                d->lines = xrealloc(d->lines, (capl = capl ? capl * 2 : 64) * sizeof *d->lines);
            size_t li = d->nlines++;
            pt_line(&d->lines[li], ids, hn, mark, n);
            float slots[VAR_SLOTS];
            for (int i = 0; i < VAR_SLOTS; i++)
                slots[i] = 0.5f;
            vars_apply(cfg, false, jobj_get(r, "turn_vars"), slots);
            const json_t *kept = jobj_get(r, "kept"), *ca = kept ? jobj_get(kept, "cut_at") : NULL;
            size_t cut = ca && ca->t == J_NUM ? (size_t)ca->u : (size_t)-1;
            int32_t said[PT_SAID], prev = start;
            uint32_t ns = 0;
            for (size_t k = 0; k < trace->n; k++) {
                const json_t *st = trace->items[k], *tt = jobj_get(st, "t"), *sd = jobj_get(st, "seed_inputs");
                if (!tt || tt->t != J_NUM)
                    continue;
                size_t t = (size_t)tt->u;
                if (t > cut)
                    break;
                const json_t *w = jobj_get(st, "engine_top"), *pk = jobj_get(st, "pick");
                if (!w || w->t != J_STR)
                    w = pk;
                int32_t label = t == cut ? end : w && w->t == J_STR ? lex_find(l, w->s) : -1;
                if (label >= 0 && sd && sd->t == J_NUM) {
                    if (d->nsteps == caps)
                        d->steps = xrealloc(d->steps, (caps = caps ? caps * 2 : 256) * sizeof *d->steps);
                    pt_step_t *s = &d->steps[d->nsteps++];
                    uint8_t bits[N_IN];
                    learner_inputs(l, slots, prev, (unsigned)t, sd->u, ABL_NONE, bits);
                    learner_out_bits(l, bits, s->a);
                    memcpy(s->slots, slots, sizeof s->slots);
                    s->line = li;
                    s->t = (uint32_t)t;
                    s->ns = ns;
                    memcpy(s->said, said, ns * sizeof *said);
                    s->label = label;
                    /* The circuit as it was before it learned this turn (the
                     * trace): today's circuit has learned these very turns, and
                     * a fit to its probabilities taught the gate it never needs
                     * the line (held out over 116 turns the pointer added one
                     * word to the circuit's 76 of 149; live it said "Wet.").
                     * Its first word then and that word's probability: the
                     * label's if it was the label, else none. */
                    const json_t *ct = jobj_get(st, "circuit_top1"), *cp = jobj_get(st, "circuit_p_first");
                    s->ctop_then = ct && ct->t == J_STR ? lex_find(l, ct->s) : -1;
                    s->pc = s->ctop_then == label ? (cp && cp->t == J_NUM ? (float)cp->f : 0.5f) : 0.0f;
                }
                if (t == cut)
                    break;
                vars_apply(cfg, true, jobj_get(st, "reply_vars"), slots);
                int32_t pw = pk && pk->t == J_STR ? lex_find(l, pk->s) : -1;
                if (ns < PT_SAID)
                    said[ns++] = pw;
                prev = pw >= 0 ? pw : prev;
            }
        }
        const json_t *par = jobj_get(r, "parent");
        unsigned times = par && par->t == J_BOOL && par->b ? PARENT_HEARD : 1;
        for (size_t j = 0; j < nw; j++) {
            int32_t i = lex_find(l, words[j]);
            if (i >= 0)
                heard[i] += times;
        }
        free_words(words, nw);
        json_free(r);
    }
    free(line);
    free(heard);
    fclose(f);
    return 0;
}

int speak_pointer_rebuild(learner_t *l, const speak_cfg_t *cfg, const char *turns_path, size_t *turns,
                          size_t *steps, char *err, size_t errlen)
{
    pt_data_t d;
    if (speak_pointer_data(l, cfg, turns_path, &d, err, errlen))
        return -1;
    pt_fit(&l->pt, &d, (size_t)-1);
    *turns = d.nlines;
    *steps = d.nsteps;
    pt_data_free(&d);
    return 0;
}

/* The labels a taught turn's record teaches, in order: each step's word
 * learned, up to the keeper's cut, the end there. n of them into *out. */
static size_t turn_labels(const learner_t *l, const json_t *r, int32_t **out)
{
    const json_t *trace = jobj_get(r, "trace");
    const json_t *kept = jobj_get(r, "kept"), *ca = kept ? jobj_get(kept, "cut_at") : NULL;
    size_t cut = ca && ca->t == J_NUM ? (size_t)ca->u : (size_t)-1, n = 0;
    *out = xcalloc(trace && trace->t == J_ARR ? trace->n + 1 : 1, sizeof **out);
    for (size_t k = 0; trace && trace->t == J_ARR && k < trace->n; k++) {
        const json_t *st = trace->items[k], *tt = jobj_get(st, "t");
        if (!tt || tt->t != J_NUM)
            continue;
        size_t t = (size_t)tt->u;
        if (t > cut)
            break;
        const json_t *w = jobj_get(st, "engine_top");
        if (!w || w->t != J_STR)
            w = jobj_get(st, "pick");
        int32_t label = t == cut ? lex_find(l, END) : w && w->t == J_STR ? lex_find(l, w->s) : -1;
        if (label >= 0)
            (*out)[n++] = label;
        if (t == cut)
            break;
    }
    return n;
}

int speak_rejudge(learner_t *l, const speak_cfg_t *cfg, const char *turns_path, size_t *judged, size_t *now_failed,
                  size_t *examples, char *err, size_t errlen)
{
    *judged = *now_failed = *examples = 0;
    FILE *f = fopen(turns_path, "r");
    if (!f) {
        snprintf(err, errlen, "%s: %s", turns_path, strerror(errno));
        return -1;
    }
    sb_t out;
    sb_init(&out);
    char *line = NULL;
    size_t cap = 0;
    ssize_t len;
    int rc = 0;
    while ((len = getline(&line, &cap, f)) > 0) {
        char perr[128];
        json_t *r = json_parse(line, (size_t)len, perr, sizeof perr);
        const json_t *mode = r ? jobj_get(r, "mode") : NULL, *fr = r ? jobj_get(r, "failed_requirement") : NULL;
        const json_t *person = r ? jobj_get(r, "person") : NULL, *reply = r ? jobj_get(r, "reply") : NULL;
        if (!r || !mode || mode->t != J_STR || strcmp(mode->s, "taught") || !fr || fr->t != J_BOOL || fr->b ||
            !person || person->t != J_STR || !reply || reply->t != J_STR) {
            sb_puts(&out, line); /* as it was */
            json_free(r);
            continue;
        }
        /* judged as a turn's reply is, on the person's line (the history of
         * that time is not kept) */
        sb_t st;
        sb_init(&st);
        sb_printf(&st, "A conversation in English. The person speaks; the reply is written one word at a time.\n"
                       "Person: %s",
                  person->s);
        cost_t cost = {0};
        bool failed = false;
        json_t *checks = jobj(), *tj = jnull(), *gj = jnull();
        /* only the new checks: the old requirements were judged with the
         * history of their time, which is not kept */
        speak_cfg_t only_new = *cfg;
        only_new.nchecks = 0;
        judge_reply(&only_new, st.s, reply->s, &cost, &failed, checks, &tj, &gj, err, errlen);
        sb_free(&st);
        (*judged)++;
        fprintf(stderr, "%-44.44s -> %-36.36s %s\n", person->s, reply->s, failed ? "FAILS NOW" : "passes");
        if (failed) {
            (*now_failed)++;
            /* the replay: every run of examples teaching exactly this reply */
            int32_t *lab;
            size_t nl = turn_labels(l, r, &lab);
            for (size_t i = 0; nl && i + nl <= l->nreplay; i++) {
                bool same = true;
                for (size_t j = 0; j < nl && same; j++)
                    same = l->replay[i + j].pos == j && l->replay[i + j].top == lab[j];
                if (same && (i + nl == l->nreplay || l->replay[i + nl].pos == 0)) {
                    for (size_t j = 0; j < nl; j++)
                        *examples += !l->replay[i + j].failed, l->replay[i + j].failed = true;
                }
            }
            free(lab);
            json_t *rj = jobj();
            char when[32];
            time_t now = time(NULL);
            strftime(when, sizeof when, "%Y-%m-%dT%H:%M:%S", localtime(&now));
            jobj_set(rj, "time", jstr(when));
            jobj_set(rj, "checks", checks);
            jobj_set(rj, "truth", tj);
            jobj_set(rj, "grammar", gj);
            jobj_set(r, "failed_requirement", jbool(true));
            jobj_set(r, "rejudged", rj);
            char *s = json_dump(r);
            sb_printf(&out, "%s\n", s);
            free(s);
        } else {
            json_free(checks);
            json_free(tj);
            json_free(gj);
            sb_puts(&out, line);
        }
        json_free(r);
    }
    free(line);
    fclose(f);
    /* turns.jsonl replaced whole, never half written */
    sb_t tmp;
    sb_init(&tmp);
    sb_printf(&tmp, "%s.tmp", turns_path);
    FILE *g = fopen(tmp.s, "w");
    if (!g || fwrite(out.s ? out.s : "", 1, out.n, g) != out.n || fclose(g) || rename(tmp.s, turns_path)) {
        snprintf(err, errlen, "%s: %s", turns_path, strerror(errno));
        rc = -1;
    }
    sb_free(&tmp);
    sb_free(&out);
    return rc;
}
