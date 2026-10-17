/* learner_test: the learner store's gate (docs/plan.md, gate 3), no model:
 * 1. encoding is bit-exact: two learners with one seed give the same
 *    vectors and contexts; a saved and reloaded store predicts the same;
 * 2. a store of another version or dimension is refused;
 * 3. few shots of a new phrase are learned and the old phrases kept
 *    (booleanclass's no-forgetting claim, measured here on sequences).
 * The text is artificial on purpose: numbered tokens, not language. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../src/learner.h"

static int fails;
#define CHECK(c, ...)                                                                              \
    do {                                                                                           \
        if (!(c)) {                                                                                \
            fails++;                                                                               \
            printf("FAIL %s:%d: ", __FILE__, __LINE__);                                            \
            printf(__VA_ARGS__);                                                                   \
            putchar('\n');                                                                         \
        }                                                                                          \
    } while (0)

/* Next-word accuracy over every position past the first of the given lines
 * (the first word follows the start alone, the same context for every line,
 * so only one line can be right there: a ceiling, not a measure). */
static double accuracy(learner_t *l, const char **lines, size_t nl)
{
    size_t ok = 0, all = 0;
    for (size_t i = 0; i < nl; i++) {
        char **w;
        size_t n = split_words(lines[i], &w);
        for (size_t t = 1; t < n; t++) {
            hv_t ctx;
            learner_context(l, w, t, NULL, 0, &ctx);
            cand_t c[1];
            if (learner_predict(l, &ctx, c, 1, false) && !strcmp(l->v[c[0].word].word, w[t]))
                ok++;
            all++;
        }
        free_words(w, n);
    }
    return all ? (double)ok / (double)all : 0;
}

int main(void)
{
    /* 1. bit-exact */
    learner_t *a = learner_new(42), *b = learner_new(42);
    hv_t x, y;
    hv_word(&x, 42, "tok7");
    hv_word(&y, 42, "tok7");
    CHECK(!memcmp(&x, &y, sizeof x), "word vector differs for one seed");
    hv_word(&y, 43, "tok7");
    CHECK(hv_hamming(&x, &y) > 3800 && hv_hamming(&x, &y) < 4400,
          "other seed should be near-orthogonal: %u", hv_hamming(&x, &y));
    hv_t r;
    hv_rot(&r, &x, 8192);
    CHECK(!memcmp(&r, &x, sizeof x), "a full rotation is the identity");
    hv_rot(&r, &x, 1);
    hv_t back;
    hv_rot(&back, &r, 8191);
    CHECK(!memcmp(&back, &x, sizeof x), "rot 1 then 8191 is the identity");

    const char *old[] = {
        "tok1 tok2 tok3 tok4 .",
        "tok5 tok6 tok7 tok8 .",
        "tok9 tok10 tok11 tok12 .",
        "tok13 tok14 tok15 tok16 .",
    };
    for (int rep = 0; rep < 3; rep++)
        for (size_t i = 0; i < 4; i++) {
            char **w;
            size_t n = split_words(old[i], &w);
            learner_hear(a, w, n, NULL, 0);
            learner_hear(b, w, n, NULL, 0);
            free_words(w, n);
        }
    char *said[] = {"tok1", "tok2"};
    hv_t ca, cb;
    learner_context(a, said, 2, NULL, 0, &ca);
    learner_context(b, said, 2, NULL, 0, &cb);
    CHECK(!memcmp(&ca, &cb, sizeof ca), "context differs between identical learners");
    double acc_old = accuracy(a, old, 4);
    printf("old phrases after 3 hearings: next-word accuracy %.3f\n", acc_old);
    CHECK(acc_old >= 0.95, "old phrases not learned: %.3f", acc_old);

    char err[256];
    const char *path = "backend/build/learner_test.store";
    CHECK(!learner_save(a, path, err, sizeof err), "save: %s", err);
    learner_t *c = NULL;
    CHECK(!learner_load(path, &c, err, sizeof err), "load: %s", err);
    if (c) {
        CHECK(c->n == a->n, "lexicon size %zu vs %zu", c->n, a->n);
        double acc_c = accuracy(c, old, 4);
        CHECK(acc_c == acc_old, "reloaded store predicts differently: %.3f vs %.3f", acc_c, acc_old);
        for (size_t i = 0; i < a->n; i++)
            if (a->v[i].proto)
                CHECK(c->v[i].proto && !memcmp(a->v[i].proto->acc, c->v[i].proto->acc,
                                                sizeof a->v[i].proto->acc),
                      "prototype %zu differs after reload", i);
        learner_free(c);
    }

    /* 2. a mismatched header is refused */
    FILE *f = fopen(path, "r+b");
    if (f) {
        fseek(f, 8, SEEK_SET);
        unsigned v = 99;
        fwrite(&v, 4, 1, f);
        fclose(f);
    }
    learner_t *d = NULL;
    CHECK(learner_load(path, &d, err, sizeof err) != 0, "a version-99 store was accepted");
    printf("version 99 refused: %s\n", err);

    /* 3. few shots of a new phrase, the old ones kept */
    const char *novel[] = {"tok20 tok21 tok22 tok23 ."};
    double before_new = accuracy(a, novel, 1);
    for (int shot = 0; shot < 5; shot++) {
        char **w;
        size_t n = split_words(novel[0], &w);
        learner_hear(a, w, n, NULL, 0);
        free_words(w, n);
    }
    double after_new = accuracy(a, novel, 1), after_old = accuracy(a, old, 4);
    printf("new phrase: %.3f before, %.3f after 5 shots; old phrases %.3f -> %.3f\n", before_new,
           after_new, acc_old, after_old);
    CHECK(after_new >= 0.8, "new phrase not learned in 5 shots: %.3f", after_new);
    CHECK(after_old >= acc_old - 0.05, "old phrases forgotten: %.3f -> %.3f", acc_old, after_old);

    learner_free(a);
    learner_free(b);
    printf("%s\n", fails ? "learner_test: FAILED" : "learner_test: all checks passed");
    return fails ? 1 : 0;
}
