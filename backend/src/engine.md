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

## pad_equal (an xks extension)

**The cost.** A hybrid model (the 35B is one) splits a batch into ubatches
with the same number of tokens from every sequence (llama.cpp
`llama-batch.cpp` `split_equal`), so suffixes of different lengths in one
round cost a forward pass each. Measured 2026-10-17 on the live engine (the
35B, 50% of its layers on the GPU, ubatch 512):

| fingerprints | 1 | 3 | 6 | 12 |
| --- | --- | --- | --- | --- |
| one length | 0.97 to 1.15 s | 1.07 to 1.08 s | 1.09 to 1.20 s | 9.41 to 13.65 s |
| different lengths | 0.99 to 1.00 s | 1.61 to 3.51 s | 4.44 to 6.53 s | 12.47 to 12.89 s |

**The extension.** A request with `"xks": {"pad_equal": true}` (the Rust xks
and TypeSafe ignore the field) has every suffix of a round left-padded with
newline tokens to the round's longest. Its prompts change by those leading
newlines; requests without it are unchanged (parity request 1 re-checked:
identical answers).

**On the learner's word step** (the rerank in 3 rotations, 5 step Nouls,
the coherence Score; 3 interleaved pairs each):

| | time | answers |
| --- | --- | --- |
| unpadded, ubatch 512 | 7.20 to 10.20 s | |
| padded, ubatch 512 | 5.40 to 5.51 s | same choice; Nouls within 0.07 |
| padded, ubatch 2048 | 1.99 to 2.04 s | within 0.04 of padded at 512; repeats identical; 42 MiB more VRAM |

MechJev-v2's `xks.conf` sets `XKS_UBATCH=2048`, and the parity scripts pin
512.

## Generation (`engine_generate`, `POST /v1/generate`)

For the teacher's lines only (teach.md): MechJev's replies are never
generated. Under the engine's lock the model's memory is cleared, the whole
rendered chat decoded on sequence 0, and the reply sampled a token at a
time until an end-of-generation token or `max_tokens`, each sampled token
decoded in turn. The sampler chain is llama.cpp's (`llama_sampler_chain`,
`~/llama.cpp` f5b9bd3): repeat penalty over the last 64 tokens, top-k,
top-p, min-p, temperature, then `dist` with the caller's seed; defaults the
user's settings (temperature 0.5, top-k 75, top-p 0.95, min-p 0.05, repeat
penalty 1.2). `llama_sampler_sample` accepts the token into the chain
itself.

Afterwards the memory is cleared again and the decision path's cached
session forgotten (`cached.n = 0`): the subject is hybrid (Gated Delta
Net), its recurrent state cannot share a sequence with the cached prefix,
so the next decision prefills its session anew. Generation changes no code
the decisions run (the engine's diff for it adds lines only).

Measured on the live engine: "Hello MechJev! How are you?" (9 tokens, 8.2
s) and "What is your name, MechJev?" (5.6 s), prompt and all.

**Parity, not yet re-checked.** A run of the parity requests with a
generation before each one differed from the recording in every answer,
but that run had the user's `~/.config/xks/xks.conf` in force
(`XKS_SYSTEM_FILE=~/CLAUDE.md`, about 1,850 tokens of system text, its
date line changing every minute) and the recording did not: parity runs
must set `XKS_SYSTEM_FILE=` empty. To be run again so.
