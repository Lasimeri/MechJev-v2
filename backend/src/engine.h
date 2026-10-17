/* engine.h: ARTICHOKE in C, the interrogation engine and the only part of
 * MechJev-v2 that links llama.cpp. It never lets the subject write: it
 * reads the next-token distribution over option labels for every
 * fingerprint of a session at once. A port of Intel Phi Jev's
 * src/artichoke/mod.rs (26866cb), same plans, same batches, same
 * arithmetic, so the same floats. See engine.md. */
#ifndef MJ_ENGINE_H
#define MJ_ENGINE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "prompt.h"

typedef struct {
    const char *gguf;
    uint32_t n_ctx;   /* 16384 */
    uint32_t forks;   /* 15 */
    uint32_t n_batch; /* 2048 */
    uint32_t n_ubatch;/* 512 */
    int32_t threads;  /* 16 */
    bool verbose;
    bool repack;      /* false */
    bool flash_attn;  /* true: llama.cpp decides (auto) */
} engine_opts_t;

void engine_opts_default(engine_opts_t *o, const char *gguf);

typedef struct engine engine_t;

/* One fingerprint to read: the suffix after the shared prefix, and its
 * candidate labels in rendered order. */
typedef struct {
    segs_t suffix;
    char **labels;
    size_t n;
} item_t;

typedef struct {
    double *logprobs; /* n */
    uint64_t prompt_evaluated, prompt_cached;
    double latency_ms;
} scored_t;

/* Error kinds, as the server maps them: a refusal is the caller's (422), a
 * failure the engine's (502). */
enum { E_OK = 0, E_REJECTED = 1, E_FAILED = 2 };

engine_t *engine_open(const engine_opts_t *o, char *err, size_t errlen);
void engine_close(engine_t *e);
const char *engine_name(const engine_t *e);
bool engine_recurrent(const engine_t *e);

/* Every fingerprint of one session; out[i].logprobs is malloc'd. */
/* pad_equal: pad every suffix of a round to its longest (one forward pass
 * a round on a hybrid model; the prompts change by leading newlines). */
int engine_score_many(engine_t *e, bool pad_equal, const segs_t *prefix, const item_t *items, size_t n,
                      scored_t *out, char *err, size_t errlen);
/* The polygraph's split reading: the whole prompt on sequence 0 from an
 * empty cache, cut at `at` into two decodes; one-token labels only. */
int engine_read_split(engine_t *e, const segs_t *prompt, char **labels, size_t n, size_t at,
                      scored_t *out, char *err, size_t errlen);
/* Where score_many would fork these fingerprints from their session. */
int engine_session_tokens(engine_t *e, const segs_t *prefix, const item_t *items, size_t n,
                          size_t *shared, char *err, size_t errlen);
/* Tokens of text as user text (no special tokens, no BOS). */
int engine_tokenize_plain(engine_t *e, const char *text, ivec_t *out);

void scored_free(scored_t *s);

/* Generation, for the teacher's lines only (POST /v1/generate; teach.md):
 * MechJev's replies are never generated. The sampling settings are the
 * user's (temperature 0.5, top-k 75, top-p 0.95, min-p 0.05, repeat
 * penalty 1.2), the seed the caller's (a clock seed). */
typedef struct {
    float temp, top_p, min_p, repeat;
    int32_t top_k, max_tokens;
    uint32_t seed;
} gen_opts_t;
void gen_opts_default(gen_opts_t *o);
/* `prompt` is a whole rendered chat (special tokens parsed); *text the
 * reply up to the end of generation or max_tokens, *ntok its tokens. The
 * decision path's cached prefix is forgotten (the next decision prefills
 * again): the hybrid model's recurrent state cannot share it. */
int engine_generate(engine_t *e, const char *prompt, const gen_opts_t *o, char **text, size_t *ntok, char *err,
                    size_t errlen);

#define BRANCH_LIMIT 32000
#define REQUEST_LIMIT 64000

#endif
