/* judge.h: render each question against the shared state prefix, read
 * the option labels with the engine, condition (calibrate) them and build
 * the typed answers: Intel Phi Jev's src/judge.rs and src/score.rs in C.
 * The pair of balances (Revelation 6:5): a distribution weighed. */
#ifndef MJ_JUDGE_H
#define MJ_JUDGE_H

#include "engine.h"
#include "json.h"
#include "prompt.h"

typedef struct {
    double dflt;
    char **bucket; /* "choice:3-5" ... */
    double *temp;
    size_t n;
} calibration_t;

typedef struct {
    template_t tpl;
    layout_t layout;
    calibration_t cal;
    size_t permutations; /* Hemi-Sync: option rotations averaged */
    bool debug;
} judge_cfg_t;

/* A raw reading of one question, before conditioning. */
typedef struct {
    char *id;
    qkind_t kind;
    char **keys;
    size_t n;
    double *logprobs; /* log of the mean over rotations */
    uint64_t prompt_evaluated, prompt_cached;
    double label_mass, latency_ms;
} raw_t;

void judge_cfg_default(judge_cfg_t *c);
int calibration_load(const char *path, calibration_t *c, char *err, size_t errlen);
double calibration_temperature(const calibration_t *c, const char *kind, size_t n);

/* Raw readings of every question of a request (state + questions). */
int judge_raw(engine_t *e, const judge_cfg_t *cfg, const json_t *req, raw_t **out, size_t *nout,
              char *err, size_t errlen);
void raws_free(raw_t *r, size_t n);

/* The whole /v1/systemone evaluation; a new JSON object on success. */
int judge_evaluate(engine_t *e, const judge_cfg_t *cfg, const json_t *req, json_t **out, char *err,
                   size_t errlen);

/* score.rs */
void softmax(const double *lp, size_t n, double t, double *out);
double choice_confidence(const double *p, size_t n);
double score_confidence(const double *p, size_t n);
size_t argmax(const double *p, size_t n);
double round4(double x);

#endif
