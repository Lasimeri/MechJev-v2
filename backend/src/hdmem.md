# hdmem: the fast memory

Backpropagation-free continual learning in Boolean space, after Vladimer
Khasia, "Backpropagation-Free Continual Learning in Boolean Space for
Extreme Edge Intelligence" (SSRN 6459123, 2026; the code is
[booleanclass](https://github.com/VladimerKhasia/booleanclass)). The module
is generic: features in, classes out. What a context is made of is the
learner's business (`learner_context`, learner.md).

| the paper | here |
| --- | --- |
| Definition 1, Lemma 1: level hypervectors by progressive flips of a random permutation, `d_H(L_i, L_j) = abs(i - j) D / (2 (L - 1))` | `make_levels`, D = 8192, L = 16; the test checks the distances exactly |
| eq 2: a feature's level XOR its identifier | `hd_level`, `hd_id`, `hd_xor` |
| eq 3: majority bundling, `Theta(sum - F/2)` | `hd_bundle_*` (ties go to 0, as in the paper) |
| eq 5: a class prototype superposes its bipolar contexts | `hd_superpose(+1)`, `hd_learn` |
| eq 6: the least Hamming distance to the binarized prototypes | `hd_distance`, `hd_sim` = 1 - 2 d_H / D |
| eq 7-8: on a miss, the true class gains the context and the predicted one loses it; binarize at the epoch's end | `learner_memorize` (learner.c), at most 3 epochs, stop at 95% right |

Identifiers come from the seed through a splitmix64 stream whose start is
mixed first (`mix64`): the raw seeds of neighbouring identifiers differ by
splitmix64's own step, so unmixed they would be one stream shifted by a
word. Neighbouring identifiers are 3800 to 4392 bits apart (D/2 within 6.6
sigma), which the test checks.

Prototypes are int16, saturating at +-32767. The last 4096 contexts are
kept (a ring) for the correction epochs. In the store: each class that
holds anything (its count and its 8192 counts), then the ring. The levels
and identifiers are not stored; the seed makes them again.

What it does well, measured in `learner_test`: twelve words, each learned
from ONE context, are all recalled from it, and all through 10% of the bits
flipped; one more context leaves all of them recalled. What it does not do,
measured on the live record (learner.md): predict the engine's word for
contexts it has not seen better than the circuit.
