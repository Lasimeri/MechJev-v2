# hdc.c

The learner's arithmetic, after booleanclass (Khasia 2026,
doi:10.5281/zenodo.19170384): 8192-bit binary hypervectors.

| operation | bits | role |
| --- | --- | --- |
| `hv_word` | splitmix64 stream seeded by seed XOR FNV-1a(word) | a word's vector, recomputed, never stored |
| `hv_xor` | XOR | binding: a phrase as one vector |
| `hv_rot` | cyclic rotation of all 8192 bits | order: a word k places back is rotated k times |
| `bundle_*` | weighted majority vote, ties to a fixed vector | superposition of several contexts |
| `hv_hamming` | XOR then popcount | similarity: D - 2 x Hamming |
| `proto_*` | saturating int16 bipolar sums, signs packed | a word's prototype, the only learned state |

There is no multiply anywhere, as in Aster's circuit-only language model.
This host's CPU has no AVX-512, so popcount is POPCNT through
`__builtin_popcountll`.
