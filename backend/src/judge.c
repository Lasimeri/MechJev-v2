/* judge.c: see judge.h and judge.md. */
#define _GNU_SOURCE
#include "judge.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void judge_cfg_default(judge_cfg_t *c)
{
    memset(c, 0, sizeof *c);
    c->tpl = T_CHATML;
    c->layout = L_LETTERS;
    c->cal.dflt = 1.0;
    c->permutations = 1;
}

/* ---- score.rs -------------------------------------------------------- */

void softmax(const double *lp, size_t n, double t, double *out)
{
    double m = -INFINITY;
    for (size_t i = 0; i < n; i++)
        m = fmax(m, lp[i]);
    if (!isfinite(m)) {
        for (size_t i = 0; i < n; i++)
            out[i] = 1.0 / (double)(n ? n : 1);
        return;
    }
    double z = 0.0;
    for (size_t i = 0; i < n; i++) {
        out[i] = exp((lp[i] - m) / t);
        z += out[i];
    }
    for (size_t i = 0; i < n; i++)
        out[i] /= z;
}

static void normalized(const double *p, size_t n, double *out)
{
    double total = 0.0;
    for (size_t i = 0; i < n; i++)
        total += p[i];
    for (size_t i = 0; i < n; i++)
        out[i] = total <= 0.0 ? 1.0 / (double)n : p[i] / total;
}

size_t argmax(const double *p, size_t n)
{
    size_t best = 0;
    for (size_t i = 0; i < n; i++)
        if (p[i] > p[best])
            best = i;
    return best;
}

/* system-one-adapter's choice_confidence: (p_max - 1/n) / (1 - 1/n). */
double choice_confidence(const double *probs, size_t n)
{
    if (n < 2)
        return 1.0;
    double *p = xmalloc(n * sizeof *p);
    normalized(probs, n, p);
    double u = 1.0 / (double)n, pmax = 0.0;
    for (size_t i = 0; i < n; i++)
        pmax = fmax(pmax, p[i]);
    free(p);
    double c = (pmax - u) / (1.0 - u);
    return c < 0 ? 0 : c > 1 ? 1 : c;
}

/* system-one-adapter's score_confidence: one minus the mean distance from
 * the modal level over a uniform distribution's mean absolute deviation. */
double score_confidence(const double *probs, size_t n)
{
    if (n < 2)
        return 1.0;
    double *p = xmalloc(n * sizeof *p);
    normalized(probs, n, p);
    double mode = (double)argmax(p, n), spread = 0.0;
    for (size_t i = 0; i < n; i++)
        spread += p[i] * fabs((double)i - mode);
    free(p);
    double nn = (double)n, center = (nn - 1.0) / 2.0, mad = 0.0;
    for (size_t i = 0; i < n; i++)
        mad += fabs((double)i - center);
    mad /= nn;
    double c = 1.0 - spread / mad;
    return c > 0 ? c : 0.0;
}

double round4(double x) { return round(x * 1e4) / 1e4; }

/* ---- conditioning ---------------------------------------------------- */

static void bucket_name(char *b, size_t bl, const char *kind, size_t n)
{
    const char *r = n <= 2 ? "2" : n <= 5 ? "3-5" : n <= 10 ? "6-10" : "11+";
    snprintf(b, bl, "%s:%s", kind, r);
}

double calibration_temperature(const calibration_t *c, const char *kind, size_t n)
{
    char b[32];
    bucket_name(b, sizeof b, kind, n);
    double t = c->dflt;
    for (size_t i = 0; i < c->n; i++)
        if (!strcmp(c->bucket[i], b))
            t = c->temp[i];
    return (t > 0.0 && isfinite(t)) ? t : 1.0;
}

int calibration_load(const char *path, calibration_t *c, char *err, size_t errlen)
{
    size_t len;
    char *text = read_file(path, &len);
    if (!text) {
        snprintf(err, errlen, "conditioning %s: unreadable", path);
        return -1;
    }
    char perr[256];
    json_t *v = json_parse(text, len, perr, sizeof perr);
    free(text);
    if (!v || v->t != J_OBJ) {
        snprintf(err, errlen, "conditioning %s: %s", path, v ? "not an object" : perr);
        json_free(v);
        return -1;
    }
    c->dflt = 1.0;
    json_t *d = jobj_get(v, "default");
    if (d && d->t == J_NUM)
        c->dflt = d->f;
    json_t *bb = jobj_get(v, "by_bucket");
    if (bb && bb->t == J_OBJ) {
        c->n = bb->n;
        c->bucket = xcalloc(bb->n, sizeof *c->bucket);
        c->temp = xcalloc(bb->n, sizeof *c->temp);
        for (size_t i = 0; i < bb->n; i++) {
            c->bucket[i] = xstrndup(bb->keys[i], bb->klens[i]);
            c->temp[i] = bb->items[i]->t == J_NUM ? bb->items[i]->f : 1.0;
        }
    }
    json_free(v);
    return 0;
}

/* ---- the judge ------------------------------------------------------- */

void raws_free(raw_t *r, size_t n)
{
    for (size_t i = 0; i < n; i++) {
        free(r[i].id);
        for (size_t k = 0; k < r[i].n; k++)
            free(r[i].keys[k]);
        free(r[i].keys);
        free(r[i].logprobs);
    }
    free(r);
}

static const char *kind_name(qkind_t k)
{
    return k == Q_NOUL ? "noul" : k == Q_CHOICE ? "choice" : "score";
}

int judge_raw(engine_t *e, const judge_cfg_t *cfg, const json_t *req, raw_t **out, size_t *nout,
              char *err, size_t errlen)
{
    *out = NULL;
    *nout = 0;
    question_t *qs;
    size_t nq;
    if (parse_questions(jobj_get(req, "questions"), &qs, &nq, err, errlen))
        return E_REJECTED;
    char *state = render_state(jobj_get(req, "state"));
    segs_t prefix = prompt_prefix(cfg->layout, cfg->tpl, state);
    free(state);

    /* Every fingerprint (each question, each rotation) rendered first, so
     * the engine sees the whole session at once and can fork it. */
    size_t cap = 0, ni = 0;
    item_t *items = NULL;
    size_t *perms = xcalloc(nq, sizeof *perms);
    size_t **orders = xcalloc(nq, sizeof *orders); /* perms[q] * n */
    raw_t *raws = xcalloc(nq, sizeof *raws);
    int rc = E_OK;
    for (size_t qi = 0; qi < nq && !rc; qi++) {
        const question_t *q = &qs[qi];
        size_t n = q->n;
        if (n > MAX_OPTIONS) {
            snprintf(err, errlen, "question `%s`: %zu options; a Choice takes at most %d", q->id, n,
                     MAX_OPTIONS);
            rc = E_REJECTED;
            break;
        }
        size_t p = 1;
        if (q->kind == Q_CHOICE) {
            p = cfg->permutations < 1 ? 1 : cfg->permutations;
            if (p > n)
                p = n;
        }
        perms[qi] = p;
        orders[qi] = xmalloc(p * n * sizeof **orders);
        raws[qi].id = xstrdup(q->id);
        raws[qi].kind = q->kind;
        raws[qi].n = n;
        for (size_t r = 0; r < p; r++) {
            size_t *order = &orders[qi][r * n];
            for (size_t i = 0; i < n; i++)
                order[i] = (i + r) % n;
            rendered_t rd = prompt_render(cfg->layout, cfg->tpl, q, order);
            if (r == 0) { /* rotation 0 is the identity: keys in request order */
                raws[qi].keys = rd.keys;
                rd.keys = NULL;
            } else {
                for (size_t k = 0; k < rd.n; k++)
                    free(rd.keys[k]);
                free(rd.keys);
            }
            if (ni == cap) {
                cap = cap ? cap * 2 : 16;
                items = xrealloc(items, cap * sizeof *items);
            }
            items[ni].suffix = rd.suffix;
            items[ni].labels = rd.labels;
            items[ni].n = rd.n;
            ni++;
        }
    }
    scored_t *sc = NULL;
    if (!rc) {
        sc = xcalloc(ni ? ni : 1, sizeof *sc);
        /* the xks extension {"xks": {"pad_equal": true}}: equal-length suffixes */
        const json_t *xx = jobj_get(jobj_get(req, "xks"), "pad_equal");
        rc = engine_score_many(e, xx && xx->t == J_BOOL && xx->b, &prefix, items, ni, sc, err, errlen);
    }
    if (!rc) {
        size_t at = 0;
        for (size_t qi = 0; qi < nq; qi++) {
            size_t n = raws[qi].n, p = perms[qi];
            double *mean = xcalloc(n, sizeof *mean);
            double *pr = xmalloc(n * sizeof *pr);
            double mass = 0.0, lat = 0.0;
            uint64_t ev = 0, ca = 0;
            for (size_t r = 0; r < p; r++) {
                scored_t *s = &sc[at++];
                double m = 0.0;
                for (size_t i = 0; i < n; i++)
                    m += exp(s->logprobs[i]);
                mass += m / (double)p;
                softmax(s->logprobs, n, 1.0, pr);
                const size_t *order = &orders[qi][r * n];
                for (size_t pos = 0; pos < n; pos++)
                    mean[order[pos]] += pr[pos] / (double)p;
                ev += s->prompt_evaluated;
                ca += s->prompt_cached;
                lat += s->latency_ms;
            }
            for (size_t i = 0; i < n; i++)
                mean[i] = log(fmax(mean[i], 1e-300));
            raws[qi].logprobs = mean;
            raws[qi].prompt_evaluated = ev;
            raws[qi].prompt_cached = ca;
            raws[qi].label_mass = mass;
            raws[qi].latency_ms = lat;
            free(pr);
        }
    }
    for (size_t i = 0; i < ni; i++) {
        segs_free(&items[i].suffix);
        for (size_t k = 0; k < items[i].n; k++)
            free(items[i].labels[k]);
        free(items[i].labels);
        if (sc)
            scored_free(&sc[i]);
    }
    free(items);
    free(sc);
    for (size_t qi = 0; qi < nq; qi++)
        free(orders[qi]);
    free(orders);
    free(perms);
    segs_free(&prefix);
    questions_free(qs, nq);
    if (rc) {
        raws_free(raws, nq);
        return rc;
    }
    *out = raws;
    *nout = nq;
    return E_OK;
}

static json_t *prob_map(char **keys, const double *p, size_t n)
{
    json_t *o = jobj();
    for (size_t i = 0; i < n; i++)
        jobj_set(o, keys[i], jf64(round4(p[i])));
    return o;
}

/* Every answer with its whole distribution: the sea of glass (Revelation
 * 4:6), clear to the bottom, never a verdict alone. */
int judge_evaluate(engine_t *e, const judge_cfg_t *cfg, const json_t *req, json_t **out, char *err,
                   size_t errlen)
{
    raw_t *raws;
    size_t nr;
    int rc = judge_raw(e, cfg, req, &raws, &nr, err, errlen);
    if (rc)
        return rc;
    json_t *answers = jobj();
    json_t *debug = jarr();
    uint64_t input = 0;
    const json_t *questions = jobj_get(req, "questions");
    for (size_t i = 0; i < nr; i++) {
        raw_t *r = &raws[i];
        const char *kind = kind_name(r->kind);
        double t = calibration_temperature(&cfg->cal, kind, r->n);
        double *p = xmalloc(r->n * sizeof *p);
        softmax(r->logprobs, r->n, t, p);
        json_t *a = jobj();
        jobj_set(a, "type", jstr(kind));
        if (r->kind == Q_NOUL) {
            jobj_set(a, "noul", jf64(round4(p[0])));
        } else if (r->kind == Q_CHOICE) {
            jobj_set(a, "choice", jstr(r->keys[argmax(p, r->n)]));
            jobj_set(a, "probabilities", prob_map(r->keys, p, r->n));
            jobj_set(a, "confidence", jf64(round4(choice_confidence(p, r->n))));
        } else {
            double lvl = 0.0;
            for (size_t k = 0; k < r->n; k++)
                lvl += (double)k * p[k];
            json_t *legend = jobj();
            const json_t *qv = jobj_get(questions, r->id);
            const json_t *crit = qv ? jobj_get(qv, "criteria") : NULL;
            if (crit && crit->t == J_ARR)
                for (size_t k = 0; k < crit->n; k++) {
                    char key[24];
                    snprintf(key, sizeof key, "%zu", k);
                    jobj_set(legend, key, jclone(crit->items[k]));
                }
            jobj_set(a, "score", jf64(round4(lvl)));
            jobj_set(a, "legend", legend);
            jobj_set(a, "probabilities", prob_map(r->keys, p, r->n));
            jobj_set(a, "confidence", jf64(round4(score_confidence(p, r->n))));
        }
        jobj_set(answers, r->id, a);
        input += r->prompt_evaluated;
        if (cfg->debug) {
            json_t *d = jobj();
            jobj_set(d, "id", jstr(r->id));
            jobj_set(d, "kind", jstr(kind));
            jobj_set(d, "temperature", jf64(t));
            json_t *lp = jarr();
            for (size_t k = 0; k < r->n; k++)
                jarr_push(lp, jf64(r->logprobs[k]));
            jobj_set(d, "raw_logprobs", lp);
            jobj_set(d, "prompt_evaluated", ju64(r->prompt_evaluated));
            jobj_set(d, "prompt_cached", ju64(r->prompt_cached));
            jobj_set(d, "label_mass", jf64(round4(r->label_mass)));
            jobj_set(d, "latency_ms", jf64(round4(r->latency_ms)));
            jarr_push(debug, d);
        }
        free(p);
    }
    raws_free(raws, nr);
    json_t *ev = jobj();
    jobj_set(ev, "model", jstr(engine_name(e)));
    jobj_set(ev, "answers", answers);
    json_t *usage = jobj();
    jobj_set(usage, "input_tokens", ju64(input));
    jobj_set(usage, "output_tokens", ju64(0));
    jobj_set(ev, "usage", usage);
    if (cfg->debug)
        jobj_set(ev, "debug", debug);
    else
        json_free(debug);
    *out = ev;
    return E_OK;
}
