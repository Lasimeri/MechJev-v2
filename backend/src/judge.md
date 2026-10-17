# judge.c

Intel Phi Jev's `judge.rs` and `score.rs`.

- Every question in every rotation (`XKS_PERMUTATIONS`, Hemi-Sync: a
  Choice's options rotated cyclically, the per-key probabilities averaged)
  is rendered first, so the engine forks one session for all of them.
- Conditioning (`XKS_CONDITIONING`, a JSON file `{default, by_bucket}`)
  sets a temperature per bucket: `noul:2`, `choice:3-5` and so on.
- Answers keep serde's field order:
  - noul: `{type, noul}`;
  - choice: `{type, choice, probabilities, confidence}`;
  - score: `{type, score, legend, probabilities, confidence}`.
  Values are rounded to 4 places.
- Confidence uses TypeSafe's system-one-adapter formulas.
- `usage.input_tokens` counts only the tokens decoded.
- `--debug` adds the raw log-probabilities, label mass and token counts.
