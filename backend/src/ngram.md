# ngram.c: the meaning-gated n-gram memory

The user's ask (2026-10-17): "Use exa to search up the function of n-grams
in LLM's, and repurpose their design in the analog/logic gate part of this
design, in order to provide some real-time contextual coherence as those
'weights' (for lack of a better term) update in real-time". The user's
earlier rule stands: MechJev does not copy Markov chains (next word from
previous words). So the user chose, of three designs, the meaning-gated
one.

## Where the design comes from

| work | what the n-grams do there | taken here |
| --- | --- | --- |
| Cheng et al., Engram (arXiv:2601.07372) | hashed suffix n-grams look up static memory; a context-aware gate `alpha = sigmoid(h . k)` silences what contradicts the context | the suffixes, the orders, the gate (the context is the meaning) |
| Grave et al. 2017 (arXiv:1612.04426), Kuhn and De Mori 1990 | a cache updated online, mixed with the model: `p = (1 - lambda) p_model + lambda p_cache` | the mix; learned at once, a turn at a time |
| Liu et al., infini-gram (arXiv:2401.17377) | the longest matching suffix weighs most | suffixes of 1 to 3 words weighing 1, 2, 4 |
| Olsson et al., induction heads (arXiv:2209.11895) | an n-gram seen in the context continues as it did | (not taken: that copies heard text) |

Exact suffixes, not hashes: at 150 turns the table holds 491 entries,
scanned in full each step.

## What it is

Each entry is a suffix (1 to 3 words said), the word said after it, how
often, and the sum of the meanings it was said under: the variable slots
the circuit saw at that step. Asked at a step (the words said before it,
the slots now), each matching entry counts as much as `max(0, cos)` of the
two meanings, each slot z-scored over everything learned (a slot that
hardly varies is left out). Each suffix length gives a distribution; they
are mixed 1, 2, 4. The gate it returns is Engram's alpha: the gated share
of the matching counts, 0 when the meaning contradicts every one.

In `learner_propose` the circuit's distribution over the candidates is
mixed with the n-gram's, `lambda = min(1, MJ2_NGRAM x gate)`, the cache
language model's interpolation, and the scores become log probabilities.

It learns only MechJev's own passed taught turns (the words up to the
keeper's cut, the end there), at once, never anything heard. It is made
again from `turns.jsonl` at every start (`speak_ngram_rebuild`): nothing of
it is in the store.

The trace keeps, every step, the n-gram's own first word and its gate
(`ngram_top`, `ngram_gate`); the record keeps `ngram` {weight, learned,
entries}.

## Measured

**Does meaning gate it?** (2026-10-17, before it was built: a C program
over a snapshot of `turns.jsonl`, read-only, `~/.cache/mechjev-v2-ngram-check.txt`.)
81 passed taught turns, 383 steps, 33 of 64 slots varying; each turn left
out in turn, the word learned predicted from the others:

| | right of 383 |
| --- | --- |
| the n-gram ungated (the Markov chain the rule forbids) | 206 (0.538) |
| gated by the step's own meaning | 193 (0.504) |
| gated by the meaning of a random other step (5 draws) | 109 to 118 (0.285 to 0.308) |

The meaning matters (0.504 against 0.29, the draws' spread 0.023); gating
costs 0.034 against the chain. START then "yes": gate 0.055 on yes/no
questions, 0.000 on wh-questions.

**Live gates are small**: 0.06 to 0.12 at the three steps of the first
probe reply ("Yes."), so `MJ2_NGRAM` 1 mixes in about a tenth.

**Its weight**, chosen by the probe A/B (2 rounds, 12 probes an arm a
round, interleaved, live): 0 passed 15 of 24 (coherence 0.611), 1 passed 18
(0.654), 3 passed 19 (0.657); not significant (one weak run at 0). At 3 the
chain showed: "I am." to "Do you like music?" twice and to "What is your
name?" five times. So 1, the default. The A/B:
`tools/ngram_ab.sh` runs the probe at each weight, interleaved, against the
live learner (`POST /v1/probe {"ngram_weight": W}`).
