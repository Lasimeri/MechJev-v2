# learner.c

The language learner: a conceptually grounded logic gate circuit
(`circuit.c`). The user's direction (2026-10-17): "Conceptually grounded
Aster-style circuit model with the local LLM decision model as probability
decider for variables and boolean reqs".

**Inputs: 106 bits.**
- **64 variable slots.** The decision engine gives each variable's
  probability. Each bit is drawn from that probability, with the clock's
  seed at inference and the example's seed in training, so both see the
  same kind of input. An unused slot reads 0.5.
- **32 bits of the previous word's code**, the sign of its embedding.
  This is Aster's fixed attention to the token before.
- **8 position bits** (1 2 3 5 8 12 20 30 words in) and the constants 0
  and 1.

**Body: 256, 256 and 128 gates.**

**Output.** The 128 output bits are added or subtracted by each word's
ternary embedding, tied with the input code (Aster's unembedding), and
divided by TAU (8). The result is the word's score. Rounding a latent
float to ternary uses the threshold 0.33; the straight-through estimator
carries the gradient back through that rounding.

**Lexicon.** It starts as `<s> </s> . , ? !`. Hearing the person's English
only counts their words, and a word may be said once heard twice.
Hearing never teaches what follows what (the user's rule: no Markov
chains).

**Teaching.** Every word the engine decided is an example: the inputs the
circuit saw, the words shown, the engine's distribution over them and its
choice.
- **Loss:** the KL from the engine's distribution to the circuit's over the
  shown words, plus 0.5 times the cross-entropy of the engine's choice
  against every speakable word.
- **When:** examples are learned only from replies that passed every
  requirement in full. A failed reply's examples teach only the KL term (which
  shown word the engine preferred), never the cross-entropy that says the
  chosen word was right: a hard gate deadlocked at birth (every early reply
  failed "grammatical English", measured 2026-10-17, so nothing was ever
  learned).
- **Rehearsal:** after each turn, the replay buffer (20,000 at most) is
  rehearsed for a time budget (3 s).

**Measures** (`learner_eval`), before learning, so they are held out:
- agreement (the circuit's first word is the engine's choice);
- -log p of the choice;
- whether the relaxed and discrete circuits pick the same word.

Each measure is taken three ways: with every input, with the variables
blanked, and with the previous word blanked. If blanking the variables
costs nothing, the circuit is a chain over words again.

**The store.** `learner.store`, version 2 (magic `MJ2CIRC`), holds the
lexicon with embeddings and Adam moments, the variable slot names
(append-only), the circuit's gate weights and Adam moments (its wiring is
recomputed from the seed), and the replay buffer. Version 1 (the retired
word-window learner) and any other shape are refused, never converted.

Measured (`backend/tests/learner_test.c`, 2026-10-17, no model):
- **Grounding toy** (8 seeds, 6,000 passes each; mean, min to max). The right
  word depends on one variable only: held-out agreement 0.912 (0.825 to
  0.950); blanking the variable gives 0.553 (0.525 to 0.575, chance 0.5);
  blanking the word before gives 0.881 (0.725 to 0.950); relaxed and
  discrete circuits pick the same word on 0.981 (0.900 to 1.000). A seed
  trained twice gives identical circuits. An earlier single run read
  0.925; training bounded by time made that number vary from 0.800 to
  0.925 between runs, so tests now cap the passes instead.
- **The store** reloads identical; version 1 is refused.

## Per-token scoring (RED)

The user, 2026-10-17: "implement a scoring per token solution/method ...
in order for the model to gain coherency while learning". The method
chosen (after an Exa search) is RED, Li et al., "Unleashing Token-Level
Rewards from Holistic Feedback via Reward Redistribution",
arXiv:2411.08302.
- **Reward.** A word's reward is the change it makes in the reward
  model's score of the prefix, r_t = R(x, y<=t) - R(x, y<=t-1).
- **Properties.** The rewards add up to R(full) - R(empty), and this is
  potential-based shaping (Ng et al. 1999), which keeps the optimal policy.
- **Why not the others.** RED needs only a black-box judge. Grad2Reward
  (judge gradients) and Dense Reward for Free (judge attention) would need
  the 35B's internals, which the typed wire does not give and the project's
  rule keeps out of the learner.

Here:
- **R** is the engine's coherence of the reply so far: a Score over 5
  levels, normalized to [0, 1], asked in each word's request, plus once
  after the last word.
- **The term.** Each example records the word said and its reward. The
  term is -r log p(said) over the words shown at that step: a negative
  reward moves probability to the alternatives the engine weighed, never to
  words nobody offered. Over the whole lexicon it raised punctuation never
  shown; a reward-only toy fell to 0.275.
- **Measured** (`backend/tests/learner_test.c`, 8 seeds). One context, in
  which the engine weakly prefers the wrong word (0.4 against 0.3), and
  every reply failed (no cross-entropy):
  - with the KL alone, the right word is first among the shown on 0.200
    (0.025 to 0.500);
  - with the per-token reward added, on 0.887 (0.725 to 1.000).

  The per-token score overrules a step's local preference when the word
  makes the reply more coherent.

The store is now version 3 (each example has the word said and its
reward). A version 2 store is read with the said word set to the engine's
choice and no reward.

## Input dropout on the word before

Half of the training passes blank the previous word's code (drawn from
each pass's seed, so a seed still trains the same circuit). The live
learner leaned on it: on 2026-10-17 its agreement with the engine was 0.86,
0.71 with the variables blanked and 0.14 with the word before blanked. That
is a chain over words, against the user's rule.

With the dropout, `learner_test` (8 seeds):
- grounding 0.906 (was 0.912);
- with the variable blanked 0.544 (0.553);
- with the word before blanked 0.919 (0.881);
- per-token reward A/B 0.172 against 0.803 (0.200 against 0.887).

## A baseline for the per-token reward

The RED term trains on the reward minus a baseline: the mean reward of the
words said in the replay buffer, by position (the first word, the second,
the third and fourth, the rest). RED's paper uses PPO or RLOO, which have
baselines of their own; this is REINFORCE with one. Raw, every first word
earned about +0.3 for beating the empty reply (whose coherence is about
0.25). The word the engine already favoured, "yes", was reinforced turn
after turn, and the learner answered "Yes." to three different statements
(2026-10-17, block 1). The baseline is computed from the replay at
learning time, so the store does not change.

## The fast memory (hdmem.md)

The user's ask (2026-10-17): adapt faster, and answer more to the point.
Two measured causes:

- A turn's new examples are trained once, then the whole replay is
  rehearsed uniformly for 3 s. At turn 92, with 387 examples kept, a new
  turn is about 1% of the training.
- The circuit's inputs hold nothing of what the person said: the engine's
  variables, the word before, the position.

So beside the circuit sits a memory after Khasia (SSRN 6459123): no
gradient, a word learned from one context. Its context for a step
(`learner_context`) is a majority bundle of every variable (its
probability's level XOR its slot's identifier) and every distinct word the
person said (its identifier XOR the role "heard"). The whole bundle is
then bound to an identifier per position. **Never the reply's own words:**
what follows what is not learned (the no-chain rule). A reply that passed
every requirement is learned at once (`learner_memorize`): each step's
context into its word's prototype, then correction epochs over the last
4096 contexts.

How the context was chosen: `learner memory-rebuild` replays the record
(turns.jsonl) through the memory, and before each turn is learned it says
what it would have chosen. Over the 47 passed taught turns (200 steps),
against the engine's word:

| predictor | all | words (95) | marks and the end (103) |
| --- | --- | --- | --- |
| memory, one flat bundle, the position one level in it | 0.301 | 0.200 | 0.396 |
| memory, flat, the step variables as of this word | 0.265 | | |
| memory, turn bundle XOR step bundle | 0.291 (0.327 as of this word) | | |
| memory, bundle XOR a position identifier, position level kept | 0.398 (0.418 as of this word) | 0.411 | 0.386 |
| **memory, bundle XOR a position identifier (kept)** | **0.403** | **0.432** | 0.376 |
| the circuit (its own first word, before learning) | 0.490 | 0.402 | 0.573 |
| the word most often said at that position so far | 0.461 | 0.432 | 0.490 |

As one bundled level among ~30 features the position was drowned out;
bound to an orthogonal identifier, each word keeps a prototype per
position. On marks and the end the memory loses by construction: without
the reply's own words it cannot tell "after a word" from "after a mark".
On words it is level with the circuit and the position baseline. So it has
a say on words only (`memory_speaks`), its score `MJ2_MEMORY` times its
similarity less the mean over those words; and that weight is **0 by
default** (it learns but does not speak) until a live A/B shows what it
does. Its measured strength is recall from few examples (the test: 12 of
12 from one context each, 12 of 12 through 10% noise); whether that makes
the learner adapt faster on the real conversation is the A/B's question.

"As of this word": the step variables a step sees were answered at the
step before, about the reply one word shorter (the rerank asks them with
its Choice, after the circuit has proposed). The record lets the rebuild
try them as if asked first; live, that would cost a request per word.
