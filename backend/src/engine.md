# engine.c

ARTICHOKE in C, and the only file of MechJev-v2 that links llama.cpp
(`~/llama.cpp/build`: CUDA, static backends). It never lets the subject
write; it reads the next-token distribution over the option labels.

One request:

1. Every fingerprint's prompt is tokenized segment by segment (template
   text with special tokens parsed, user text without, the start token
   first).
2. The session is the longest token prefix they share, never past the end
   of the state's own tokens.
3. Sequence 0, the rolling buffer, keeps the last session. A request with
   the same state decodes only its questions. A partial match on the
   hybrid 35B cannot be cut back (`llama_memory_seq_rm` refuses), so the
   cache is cleared and the session prefilled again.
4. One-token labels are read in rounds of `XKS_FORKS` (15) sequences:
   `llama_memory_seq_cp(0, k)` for each (the Hologram), then one batch
   with every suffix and an output row only at each suffix's last token.
5. Several-token labels (past 26 options) are read as a trie. The suffix
   goes on sequence 1, every proper label prefix is forked from it in
   BTreeMap order, and a label's log-probability is the sum along the trie.
6. `read_row` takes full-vocabulary log-probabilities: the max as f32,
   the sum of exp in f64 left to right, as the Rust does, so the floats are
   the same.

`check_limits` (REBAL) refuses, before any prefill, a request past
TypeSafe's limits (32,000 tokens for the state and a question, 64,000 for
the request) or past the context.

Context: `n_ctx` 16384, `n_batch` 2048, `n_ubatch` 512,
`n_seq_max = n_outputs_max = forks + 1`, `kv_unified`, 16 threads,
repacking off. `XKS_GPU_LAYERS` takes a count or a share (`50%` is 21 of
41 layers).

Measured (2026-10-17, the 35B-A3B Q4_K_M, RTX 3090 Ti, 50% of the layers):
- **Parity with the Rust xks:** 16 requests and 67 questions
  (`backend/tests/parity/`), the 30-option trie and four long sessions of
  2.5k to 2.7k tokens among them. Answers, usage and raw log-probabilities
  are byte-identical.
- **The copy:** forked against split with `XKS_FORKS=1` is exactly 0 on 15
  fingerprints of 3 requests, sessions of 61 to 1,400 tokens
  (`xks polygraph`).
