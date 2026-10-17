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
