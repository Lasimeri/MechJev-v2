# pointer.c: adaptive attention over the person's line

The user's ask (2026-10-18): "Keep going, and the model isn't adaptive
enough, look up methods for 'attention' on ArXiv using Exa", then
"Adaptive attention". The gap it closes: MechJev alone could not answer
with a word from the person's line. Told "The moon is round. Is the moon
round?", it said a word of the last lesson ("White.", "Small!"); asked
about a word it never heard, it could not say that word back ("What
zin?"). The circuit says only words it has learned, and a word heard once
has no trained embedding.

## Where the design comes from

| work | what it does there | taken here |
| --- | --- | --- |
| Lu et al., Knowing When to Look (arXiv:1612.01887) | a sentinel gate `beta` decides at each word whether to attend to the input or rely on the language model; content words attend, function words do not | the gate: per word, the line or the circuit |
| See et al., pointer-generator (arXiv:1704.04368) | `P(w) = p_gen P_vocab(w) + (1 - p_gen) sum_{i: w_i = w} a_i`, the input's words in the vocabulary, so an unseen word can be said | the copy distribution, the training objective |
| Vinyals et al., Pointer Networks (arXiv:1506.03134) | attention over input positions is the output | the attention names a token of the line |
| Schlag et al. (arXiv:2102.11174) | attention as fast weights updated online | (later: per-turn updates; see below) |

## What it is

Keys: a token of the person's current line as ten features, not its word
(a new word would have no key): new before this turn, the line's last
content word, how many content words follow it (0, 1, 2 to 3, 4 or more),
a mark, often heard (10 or more hearings), already said in this reply, a
bias. The query: the circuit's 128 output bits at this step, `h = V^T a' +
W0` (`a' = 2a - 1`); the score `h . f_j`, a softmax over the line.

The gate: `beta = sigmoid(B0 + Cn novelty + TP[position] + G . f_att +
GS[first or later] . (slots - 0.5))`, `novelty` the share of the line's
content words never heard, the reply position (first word, second, later),
`f_att` the features the attention landed on, `slots` the meaning the
engine gave the turn and the reply so far (what the circuit saw at that
step), weighed apart for the first word and the later ones. The meaning
decides whether to look at the line; the learner learns where (the user's
split: the engine decides meaning, the learner learns form). Without it
the gate copied the word at "Do you know the Zin?" where "What" comes
first ("Zin it!"). The circuit's 128 bits are not in the gate: fitted on 87
turns they took over and made it copy where it should not; nor is the
circuit's confidence (both measured below).

The decision is outright (`pt_choose`): under 0.5 the attended word is
MechJev's word, unless it is a mark or a word heard 10 times or more: those
the circuit says itself, and copying them made parrots live ("Thank you!"
answered "You!", both words the pointer's). Else the circuit's word. A soft
mix was measured and lost: the circuit's distribution over a few hundred
words is flat, so a little copy mass outvoted it on steps that were not
copies.

It attends only over the person's line, never over MechJev's own reply
(the no-Markov rule). It acts only in a turn MechJev answers alone; a
taught turn offers the person's words to the engine already. The trace
keeps `pointer_gate` and `pointer_word` each step, and `from: "pointer"`
when it named the word.

It is fitted again at every start (`speak_pointer_rebuild`), nothing of it
in the store: each passed taught turn's line with the hearings replayed as
the turns made them (the parent's twice, a probe's none; words heard
through `/v1/hear` are missed), each learned word with the circuit's output
bits for it now and the slots, 25 passes in order at learning rate 0.05,
L2 1e-4 (1e-3 on the meaning weights), starting
with `B0 = 2` (trust the circuit) and a new word drawing the attention a
little (`W0[0] = 1`). Off unless `MJ2_COPY=1`.

## Measured

**Does it say the person's word where the circuit does not?** (2026-10-18,
offline, read-only over a snapshot, `~/.cache/mechjev-v2-snapshot-20261018-0630-pointer`.)
87 passed taught turns, 311 learned words, each turn left out in turn and
the pointer fitted to the others; the label is the engine's word. The
shipped code (`speak_pointer_data`, `pt_fit`, `pt_forward`, `pt_choose`) measured by a
scratch program, `~/.cache/mechjev-v2-pointer-loo3.txt`:

| | right, pointer | right, circuit |
| --- | --- | --- |
| the word is in the person's line (122) | 66 (0.541) | 61 (0.500) |
| of them the teacher's lines (44) | 25 | 18 |
| the word is not in the line (189) | 83 (0.439) | 83 (0.439) |

The pointer named a word wrongly on 1 of the 189. By stage (copy right, pointer /
circuit; other steps lost):

| stage | copy | other lost |
| --- | --- | --- |
| before the teacher (78 copy, 132 other) | 41 / 43 | 0 |
| greet | 4 / 4 of 6 | 0 of 10 |
| unknown | 14 / 10 of 17 | 0 of 12 |
| told | 7 / 4 of 19 | 0 of 8 |

Offline cannot test a word MechJev never said: the circuit now has every
learned label speakable (0 of the 11 hard copies). The gates measured on
the way, same data (net: right words gained over the circuit; wrong names:
steps not in the line where the pointer named a word, for a soft mix the
steps its gate was under 0.5):

| gate | copy | other | wrong names | net |
| --- | --- | --- | --- | --- |
| soft mix, with the circuit's bits | 0.598 | 0.349 | 28 | -5 |
| soft mix, small | 0.713 | 0.233 | 17 | -13 |
| outright, with the circuit's bits | 0.525 | 0.376 | 32 | -9 |
| outright, small | 0.582 | 0.392 | 18 | +1 |
| outright, small + the circuit's confidence | 0.516 | 0.413 | 28 | -3 |
| outright, small + the meaning | 0.607 | 0.423 | 7 | +10 |
| the same, marks and often-heard words left to the circuit (shipped) | 0.541 | 0.439 | 1 | +5 |

`learner_test` checks the mechanism on a toy: two meanings in one slot,
told answering with the line's new word first, unknown saying the
circuit's "what" first and the new word second; on a line it never saw the
gate opens at 0.08 for told, stays at 0.91 for unknown's first word and
opens at 0.02 for its second. (A first version of the toy gave the two
meanings different circuit bits too; the gate then learned the meaning
through where the attention landed, and failed when asked with the other
bits.)
