/* learner_test: the circuit learner's gate (docs/plan.md, gate 3), no
 * model and no engine:
 * 1. the circuit: the 16 relaxed gates on bits equal their truth tables;
 *    a store saves and reloads to the same circuit and the same proposals;
 *    a version 1 store is refused;
 * 2. grounding: a toy where the right word depends only on one variable
 *    (P set by hand, as the engine would) is learned, and blanking that
 *    variable undoes it (the concept path carries it, not the word before);
 * 3. the relaxed and discrete circuits agree on held-out inputs (the rate);
 * 4. the exported C circuit, compiled, is bit-identical to the interpreter.
 * 5. the fast memory (memory_tests).
 * The toy's words are English but the examples are artificial on purpose. */
#define _GNU_SOURCE
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../src/learner.h"
#include "../src/util.h"

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

/* An example as the speaker would make it: slot 0 the variable "the person
 * greeted" at probability p, the engine's choice among hello/fine/music. */
static example_t toy(learner_t *l, float p, uint64_t id)
{
    example_t e;
    memset(&e, 0, sizeof e);
    for (int i = 0; i < VAR_SLOTS; i++)
        e.vars[i] = 0.5f;
    e.vars[0] = p;
    e.prev = lex_find(l, START);
    e.pos = 0;
    const char *w[3] = {"hello", "fine", "music"};
    e.n = 3;
    int32_t right = lex_find(l, p > 0.5f ? "hello" : "fine");
    for (int k = 0; k < 3; k++) {
        e.opt[k] = lex_find(l, w[k]);
        e.q[k] = e.opt[k] == right ? 0.9f : 0.05f;
    }
    e.top = right;
    e.id = id;
    return e;
}


static learner_t *toy_learner(uint64_t seed)
{
    learner_t *l = learner_new(seed);
    const char *words[] = {"hello", "fine", "music", "hello", "fine", "music"};
    learner_hear(l, (char **)words, 6); /* each heard twice: speakable */
    learner_slot(l, "turn The person greeted.");
    return l;
}

static void toy_sets(learner_t *l, example_t *train, example_t *held)
{
    for (int i = 0; i < 60; i++)
        train[i] = toy(l, (i % 2) ? 0.95f : 0.05f, 1000 + (uint64_t)i);
    for (int i = 0; i < 40; i++)
        held[i] = toy(l, (i % 2) ? 0.9f : 0.1f, 5000 + (uint64_t)i);
}

static double mean(const double *x, int n)
{
    double s = 0;
    for (int i = 0; i < n; i++)
        s += x[i];
    return s / n;
}


/* The share of examples whose right word the circuit ranks first among
 * the words shown (the decision the per-token reward is about). */
static double shown_agree(learner_t *l, const example_t *ex, int n)
{
    int ok = 0;
    int32_t *rank = xmalloc((l->n ? l->n : 1) * sizeof *rank);
    for (int i = 0; i < n; i++) {
        uint8_t bits[N_IN];
        learner_inputs(l, ex[i].vars, ex[i].prev, ex[i].pos, ex[i].id * 0x9E3779B97F4A7C15ull, ABL_NONE, bits);
        size_t m = learner_propose(l, bits, NULL, rank, l->n, true, NULL);
        for (size_t r = 0; r < m; r++) {
            bool shown = false;
            for (int k = 0; k < ex[i].n; k++)
                shown |= ex[i].opt[k] == rank[r];
            if (shown) {
                ok += rank[r] == ex[i].top;
                break;
            }
        }
    }
    free(rank);
    return (double)ok / n;
}
static void spread(const char *what, const double *x, int n)
{
    double lo = x[0], hi = x[0];
    for (int i = 1; i < n; i++) {
        lo = x[i] < lo ? x[i] : lo;
        hi = x[i] > hi ? x[i] : hi;
    }
    printf("%s %.3f  %.3f  %.3f\n", what, mean(x, n), lo, hi);
}
/* 5. The fast memory (hdmem.h; Khasia, SSRN 6459123): Lemma 1 holds
 *    exactly; identifiers are near-orthogonal; a word learned from ONE
 *    context is recalled for it, and through 10% noise; learning a new word
 *    keeps every old one (no catastrophic forgetting); the person's words
 *    move a context, partly; a store keeps the memory. */
static void random_context(learner_t *l, uint64_t *s, hv_t *out)
{
    float vars[VAR_SLOTS];
    for (int i = 0; i < VAR_SLOTS; i++)
        vars[i] = (float)((splitmix64(s) >> 11) * 0x1.0p-53);
    learner_context(l, vars, 0, NULL, 0, out);
}

static void memory_tests(void)
{
    learner_t *l = learner_new(7);
    hdmem_t *h = &l->mem;
    bool iso = true;
    for (int i = 0; i < HD_LEVELS; i++)
        for (int j = 0; j < HD_LEVELS; j++) {
            size_t fi = (size_t)i * (HD_D / 2) / (HD_LEVELS - 1), fj = (size_t)j * (HD_D / 2) / (HD_LEVELS - 1);
            iso &= hd_distance(&h->level[i], &h->level[j]) == (fi > fj ? fi - fj : fj - fi);
        }
    CHECK(iso, "level hypervectors are not isometric (Lemma 1)");
    CHECK(hd_distance(&h->level[0], &h->level[HD_LEVELS - 1]) == HD_D / 2, "L_0 to L_last is not D/2");
    hv_t a, b;
    hd_id(h, 1, 0, &a);
    hd_id(h, 1, 1, &b);
    unsigned d = hd_distance(&a, &b);
    /* D/2 within 6.6 sigma (sigma = sqrt(D)/2 = 45.3) */
    CHECK(d > 3800 && d < 4392, "neighbouring identifiers are not near-orthogonal: %u of %d", d, HD_D);

    const char *words[] = {"rain", "music", "walk", "book", "cold", "tea", "blue", "night", "home", "song", "cat", "sun"};
    enum { NW = 12 };
    for (int k = 0; k < 2; k++)
        learner_hear(l, (char **)words, NW);
    char name[32];
    for (int i = 0; i < 24; i++) {
        snprintf(name, sizeof name, "turn variable %d", i);
        learner_slot(l, name);
    }
    hv_t ctx[NW + 1];
    int32_t lab[NW + 1];
    uint64_t s = 99;
    for (int i = 0; i <= NW; i++) {
        random_context(l, &s, &ctx[i]);
        lab[i] = lex_find(l, words[i % NW]);
    }
    memfit_t fit = learner_memorize(l, ctx, lab, NW);
    int right = 0, noisy = 0;
    for (int i = 0; i < NW; i++) {
        right += learner_recall(l, &ctx[i], false) == lab[i];
        hv_t x = ctx[i];
        for (int k = 0; k < HD_D / 10; k++) { /* 819 distinct-ish bit flips */
            uint64_t bit = splitmix64(&s) % HD_D;
            x.w[bit / 64] ^= 1ull << (bit % 64);
        }
        noisy += learner_recall(l, &x, false) == lab[i];
    }
    CHECK(right == NW, "one-shot recall: %d of %d", right, NW);
    CHECK(noisy == NW, "one-shot recall through 10%% noise: %d of %d", noisy, NW);
    printf("memory: %d words from one context each: recalled %d/%d, through 10%% noise %d/%d (fit %.2f, %zu "
           "epochs)\n",
           NW, right, NW, noisy, NW, fit.fit, fit.epochs);
    /* a new context for an old word ("rain" again) and the old ones */
    learner_memorize(l, &ctx[NW], &lab[NW], 1);
    int kept = 0;
    for (int i = 0; i <= NW; i++)
        kept += learner_recall(l, &ctx[i], false) == lab[i];
    CHECK(kept == NW + 1, "after one more context: %d of %d recalled", kept, NW + 1);

    float vars[VAR_SLOTS];
    for (int i = 0; i < VAR_SLOTS; i++)
        vars[i] = 0.5f;
    char *m1[] = {"do", "you", "like", "music"}, *m2[] = {"do", "you", "like", "rain"};
    hv_t c0, c1, c2;
    learner_context(l, vars, 0, NULL, 0, &c0);
    learner_context(l, vars, 0, m1, 4, &c1);
    learner_context(l, vars, 0, m2, 4, &c2);
    unsigned d12 = hd_distance(&c1, &c2), d01 = hd_distance(&c0, &c1);
    CHECK(d12 > 0 && d12 < HD_D / 2 && d01 > 0 && d01 < HD_D / 2,
          "the person's words should move the context, partly: %u and %u of %d", d12, d01, HD_D);

    char err[256];
    const char *path = "backend/build/learner_test_memory.store";
    CHECK(!learner_save(l, path, err, sizeof err), "save: %s", err);
    learner_t *r = NULL;
    CHECK(!learner_load(path, &r, err, sizeof err), "load: %s", err);
    if (r) {
        bool same = r->mem.n == l->mem.n && r->mem.nreplay == l->mem.nreplay;
        for (int i = 0; same && i <= NW; i++)
            same = hd_sim(&r->mem, (size_t)lab[i], &ctx[i]) == hd_sim(&l->mem, (size_t)lab[i], &ctx[i]) &&
                   learner_recall(r, &ctx[i], false) == lab[i];
        CHECK(same, "the reloaded memory differs");
        learner_free(r);
    }
    learner_free(l);
}

int main(void)
{
    /* 0. hearing across the lexicon's growth: 300 new words, each heard
     * once, crossing every reallocation (64, 128, 256). The live learner
     * corrupted its heap at 64 (an unsequenced l->v[lex_add(...)]); under
     * AddressSanitizer (make check runs this test so) a stale write fails. */
    {
        learner_t *g = learner_new(7);
        size_t base = g->n;
        for (int i = 0; i < 300; i++) {
            char w[16];
            snprintf(w, sizeof w, "word%d", i);
            char *one[1] = {w};
            learner_hear(g, one, 1);
        }
        bool ok = g->n == base + 300;
        for (int i = 0; i < 300 && ok; i++) {
            char w[16];
            snprintf(w, sizeof w, "word%d", i);
            int32_t k = lex_find(g, w);
            ok = k >= 0 && g->v[k].heard == 1;
        }
        CHECK(ok, "hearing across the lexicon's growth lost or misplaced counts");
        learner_free(g);
    }
    /* 1. gates on bits against the truth tables: Table 1, ID = bits 00 01 10 11 */
    circuit_t c;
    int sizes[1] = {16};
    circuit_init(&c, 2, 1, sizes, 1);
    for (int g = 0; g < 16; g++) /* force neuron g to gate g on inputs 0, 1 */
        for (int i = 0; i < 16; i++)
            c.w[0][g * 16 + i] = i == g ? 10.0f : 0.0f;
    for (int g = 0; g < 16; g++)
        c.wa[0][g] = 0, c.wb[0][g] = 1;
    for (int a = 0; a < 2; a++)
        for (int b = 0; b < 2; b++) {
            uint8_t in[2] = {(uint8_t)a, (uint8_t)b}, out[16];
            circuit_run(&c, in, out);
            for (int g = 0; g < 16; g++)
                CHECK(out[g] == ((g >> (3 - (a * 2 + b))) & 1), "gate %d on %d%d gave %d", g, a, b, out[g]);
        }
    circuit_free(&c);

    /* 2. grounding: the same toy learned from 8 seeds, a fixed number of
     * passes each (time-bounded training made the result vary between runs:
     * 0.800 to 0.925 at about 10,000 passes, measured 2026-10-17), the
     * spread reported; then one seed twice, which must be identical. */
    enum { SEEDS = 8, PASSES = 6000 };
    double ag[SEEDS], bl[SEEDS], np[SEEDS], gp[SEEDS];
    learner_t *l = NULL;
    for (int s = 0; s < SEEDS; s++) {
        learner_free(l);
        l = toy_learner(100 + (uint64_t)s);
        example_t train[60], held[40];
        toy_sets(l, train, held);
        learner_learn(l, train, 60, 1e9, PASSES);
        evalres_t a = learner_eval(l, held, 40, ABL_NONE);
        ag[s] = a.agree;
        gp[s] = a.gap;
        bl[s] = learner_eval(l, held, 40, ABL_CONCEPTS).agree;
        np[s] = learner_eval(l, held, 40, ABL_PREV).agree;
    }
    printf("grounding over %d seeds, %d passes each (mean, min, max):\n", SEEDS, PASSES);
    spread("  held-out agreement        ", ag, SEEDS);
    spread("  with the variable blanked ", bl, SEEDS);
    spread("  with the word before blanked", np, SEEDS);
    spread("  relaxed = discrete        ", gp, SEEDS);
    CHECK(mean(ag, SEEDS) >= 0.85, "the toy was not learned: mean %.3f", mean(ag, SEEDS));
    CHECK(mean(bl, SEEDS) <= 0.65, "blanking the variable should undo it: mean %.3f", mean(bl, SEEDS));
    CHECK(mean(gp, SEEDS) >= 0.85, "relaxed and discrete circuits disagree: mean %.3f", mean(gp, SEEDS));
    /* 2b. what the per-token reward (RED) adds: it can overrule the engine's
     * local preference when the words it led to read better. One context
     * (the person greeted); the engine weakly prefers the wrong word at the
     * step (0.4 against 0.3 and 0.3), every reply failed (no cross-entropy);
     * the word said alternates; A: no rewards, the KL alone; B: the word said
     * credited +0.5 when right, -0.5 when wrong. Measured as the right word
     * ranked first among the words shown: A should not, B should. */
    {
        double ka[SEEDS], kb[SEEDS];
        for (int arm = 0; arm < 2; arm++)
            for (int s = 0; s < SEEDS; s++) {
                learner_t *x = toy_learner(200 + (uint64_t)s);
                example_t tr[80], hd[40];
                int32_t hello = lex_find(x, "hello"), fine = lex_find(x, "fine");
                for (int i = 0; i < 80; i++) {
                    tr[i] = toy(x, 0.95f, 2000 + (uint64_t)i); /* right: hello */
                    for (int k = 0; k < tr[i].n; k++)
                        tr[i].q[k] = tr[i].opt[k] == fine ? 0.4f : 0.3f;
                    tr[i].failed = 1;
                    tr[i].said = i % 2 ? hello : fine;
                    tr[i].reward = arm == 0 ? 0.0f : tr[i].said == hello ? 0.5f : -0.5f;
                }
                for (int i = 0; i < 40; i++)
                    hd[i] = toy(x, 0.95f, 6000 + (uint64_t)i);
                learner_learn(x, tr, 80, 1e9, PASSES);
                (arm ? kb : ka)[s] = shown_agree(x, hd, 40);
                learner_free(x);
            }
        printf("per-token reward against a weak contrary engine preference (hello first among the shown):\n");
        spread("  A: KL alone              ", ka, SEEDS);
        spread("  B: KL + per-token reward ", kb, SEEDS);
        CHECK(mean(ka, SEEDS) <= 0.25, "A should follow the engine's preference for fine: %.3f", mean(ka, SEEDS));
        CHECK(mean(kb, SEEDS) >= 0.75, "B: the per-token reward did not overrule it: %.3f", mean(kb, SEEDS));
    }
    {
        learner_t *x = toy_learner(100), *y = toy_learner(100);
        example_t tx[60], hx[40], ty[60], hy[40];
        toy_sets(x, tx, hx);
        toy_sets(y, ty, hy);
        learner_learn(x, tx, 60, 1e9, PASSES);
        learner_learn(y, ty, 60, 1e9, PASSES);
        bool same = true;
        for (int k = 0; k < x->circ.n_layers; k++)
            same &= !memcmp(x->circ.w[k], y->circ.w[k], (size_t)x->circ.size[k] * 16 * sizeof(float));
        CHECK(same, "one seed, two runs, different circuits");
        if (same)
            printf("one seed trained twice: identical circuits\n");
        learner_free(x);
        learner_free(y);
    }
    example_t train[60], held[40];
    toy_sets(l, train, held);
    evalres_t after = learner_eval(l, held, 40, ABL_NONE);

    /* store */
    char err[256];
    const char *path = "backend/build/learner_test.store";
    CHECK(!learner_save(l, path, err, sizeof err), "save: %s", err);
    learner_t *r = NULL;
    CHECK(!learner_load(path, &r, err, sizeof err), "load: %s", err);
    if (r) {
        evalres_t again = learner_eval(r, held, 40, ABL_NONE);
        CHECK(again.agree == after.agree && again.nll == after.nll, "reloaded store differs: %.3f %.4f vs %.3f %.4f",
              again.agree, again.nll, after.agree, after.nll);
        CHECK(r->nreplay == l->nreplay, "replay %zu vs %zu", r->nreplay, l->nreplay);
        learner_free(r);
    }
    FILE *f = fopen(path, "r+b");
    if (f) {
        fwrite("MJ2LRN\0\0", 1, 8, f); /* the retired version 1 magic */
        fclose(f);
    }
    r = NULL;
    CHECK(learner_load(path, &r, err, sizeof err) != 0, "a version 1 store was accepted");
    printf("version 1 refused: %s\n", err);

    /* 4. the exported circuit, compiled, against the interpreter */
    const char *src = "backend/build/learner_test_circuit.c";
    f = fopen(src, "w");
    CHECK(f && !circuit_export(&l->circ, f, "mechjev_circuit"), "export failed");
    if (f) {
        fprintf(f, "#include <stdio.h>\nint main(void){unsigned char in[%d],out[%d];int c;"
                   "while(fread(in,1,%d,stdin)==%d){mechjev_circuit(in,out);fwrite(out,1,%d,stdout);}"
                   "(void)c;return 0;}\n",
                N_IN, OUT_BITS, N_IN, N_IN, OUT_BITS);
        fclose(f);
    }
    int rc = system("tcc -o backend/build/learner_test_circuit backend/build/learner_test_circuit.c 2>&1");
    CHECK(rc == 0, "the exported circuit did not compile (tcc)");
    FILE *in = fopen("backend/build/learner_test_circuit.in", "wb");
    uint8_t *want = xmalloc(200 * OUT_BITS);
    for (int k = 0; k < 200; k++) {
        uint8_t bits[N_IN];
        example_t e = toy(l, (k % 3) ? 0.8f : 0.2f, 9000 + (uint64_t)k);
        learner_inputs(l, e.vars, e.prev, (unsigned)(k % 12), e.id, ABL_NONE, bits);
        fwrite(bits, 1, N_IN, in);
        circuit_run(&l->circ, bits, want + k * OUT_BITS);
    }
    fclose(in);
    rc = system("backend/build/learner_test_circuit < backend/build/learner_test_circuit.in > "
                "backend/build/learner_test_circuit.out");
    size_t n = 0;
    char *got = read_file("backend/build/learner_test_circuit.out", &n);
    CHECK(rc == 0 && got && n == 200 * OUT_BITS && !memcmp(got, want, n),
          "the compiled circuit differs from the interpreter (%zu bytes)", n);
    if (got && n == 200 * OUT_BITS && !memcmp(got, want, n))
        printf("exported C circuit: identical to the interpreter on 200 inputs (%d bytes out each)\n", OUT_BITS);
    free(got);
    free(want);
    learner_free(l);
    memory_tests();
    printf("%s\n", fails ? "learner_test: FAILED" : "learner_test: all checks passed");
    return fails ? 1 : 0;
}
