# MechJev-v2: plan

## What it is

Two parts, and only the first touches llama.cpp.

1. **The decision engine** (`backend/`, C, binary `xks`): the XKEYSCORE
   portion of Intel Phi Jev ported from Rust. It serves `POST /v1/systemone`
   on 127.0.0.1:8090: Noul, Choice (up to 255 options), Score (2 to 10
   levels). It works the way the Rust xks does:
   - The session (the request's state) is prefilled once on sequence 0 and
     kept as the rolling buffer.
   - Each fingerprint (one question in one option rotation) is forked from
     it with `llama_memory_seq_cp`, and all the suffixes decode in one batch.
   - The answer is the next-token distribution over the option labels, read
     as full-vocabulary log-probabilities. Labels past 26 options are read
     as a trie of forks.

   The subject is Qwen3.8-35B-A3B Q4_K_M with 50% of its layers on the RTX
   3090 Ti. The 35B only ever decides; it never writes text.
2. **The language learner** (`backend/`, C, no llama.cpp): a conceptually
   grounded logic gate circuit (see backend/src/learner.md), not
   a chain over words. It starts knowing a handful of words and
   acquires language from its interactions with the engine. Its weights are
   its own; the 35B's never change.
3. **The harness** (`frontend/`, Rust): Mechanical Jev's TUI (crossterm,
   the seaof.glass theme) reused as the management interface. It has an MCP
   side so Claude drives the same interface a person does.

## Sources and what each contributes

| source | idea taken |
| --- | --- |
| Code-Forge-AU/jev-llm, and its r/LLM post "Jev, the new AI that can only pick from a list, has no way to output text. I made it chat anyway." | Generation as repeated Choice: a fan-out over word groups, then a rerank of the rendered continuations. The decision model is poor at guessing words and good at judging sentences. END is allowed only after sentence-final punctuation. Rules where the model is weak (a/an, capitals, trigram block). |
| xucian/talktojev, "Generation is Classification" | Grains: reply (how much to say), sentence (what it does, is the reply complete), word, and a check after the reply. Every answer is written back into the state; a fix is a better state or option description, never a template. |
| VladimerKhasia/booleanclass | The learner's arithmetic. 8192-bit hypervectors: XOR binding, cyclic-shift permutation for order, majority bundling, prototypes as bipolar int32 sums, a mistake-driven update (`proto[wrong] -= x; proto[right] += x`). A new class is learned from a few shots without forgetting the old ones. |
| Aster, "An LLM Made from Logic Gates" | A language model built entirely of circuits that compile to C, with no matrix multiply: learned embeddings, fixed attention offsets, a gate circuit over the previous tokens. Training is judged instead of benchmarked: a judge checks the restrictions, then prefers lower cross-entropy after a fixed time. The restrictions list grows from feedback. |
| arxiv 2405.15658 (HDC for referring segmentation) | The paper is vision-language segmentation, not hyperdimensional computing; the shared acronym is the likely link. Taken from it: decoding at several grains, aligned grain by grain, and an explicit count (how many sentences the reply needs). |

## How acquisition works (the design to build and measure)

- **Lexicon.** It starts tiny: a few function words and punctuation. A word
  enters it when it is heard (in the person's turns) or confirmed by the
  engine's rerank. Every word has a count; a word becomes speakable once it
  has been seen N times. The lexicon's size over interactions is the first
  curve.
- **Learner.** The context (the last k words, each word's hypervector
  rotated by its distance back) is bundled into one hypervector. The
  prototypes of the candidate next words are compared to it by Hamming
  distance, computed with XOR and popcount (AVX-512 VPOPCNTQ where the CPU
  has it). The learner proposes the top candidates.
- **Teacher.** The engine judges the learner's candidates. They are shown
  as rendered continuations, and Jev picks the best, or "(none of these)".
  The pick is the label: it gets the mistake-driven update, and a "none"
  sends a widened fan-out over the lexicon.
- **Restrictions.** A file of Noul checks the engine runs on each finished
  reply (grammatical? relevant? repeats?). The harness can add to it,
  following Aster's outer agent.
- **Curves**, written to a JSONL per interaction:
  - lexicon size;
  - the learner's top-1 agreement with the engine's pick;
  - the share of words the learner produced without a rerank;
  - "don't know" (abstain) rate;
  - cross-entropy of the learner on held-out picks;
  - words per second.

## Gates (each blocks the next)

1. **C engine parity** with the Rust xks on the same subject, config and
   requests:
   - forked against split, with one fork per round, is 0.000;
   - the probabilities match;
   - Mechanical Jev's `make closeness` gives 28 of 28.
2. **Cost per word** on the real 35B (nonces, interleaved). It decides how
   many questions a word can afford.
3. **Learner store:**
   - the header records the seed, D and the algorithm version, and a
     mismatch is refused;
   - encoding is bit-exact across runs;
   - a new word is learned from a few shots with the old ones kept.
4. **The loop**, with the curves above.

## Changes since this plan was written

- **2026-10-17.** The first learner was a word-window (trigram) model in
  hyperdimensional vectors, taught by the engine's picks. It was
  rejected: "Don't just copy markov chains, that's cheating with this,
  and any language produced will be incoherent". Its gate 2 replies
  ("Are? You", "Thank you, you are like") showed exactly that.
- **The replacement** is the user's direction: "Conceptually grounded
  Aster-style circuit model with the local LLM decision model as
  probability decider for variables and boolean reqs". See
  `backend/src/learner.md`, `circuit.md`, `speak.md`, `variables.txt`
  and `restrictions.txt`.
- **Learned from the rejected version:** at 3.2 to 4.9 s a word, "(none of
  these)" made silence the easy answer. The rerank now always picks a
  word, and the lexicon is widened when the circuit is unsure.
- **The word said** is the coherence lookahead (the user's choice): the
  rerank's best 3 scored as a good start (viability), not as a finished
  reply (speak.md).
- **Stopping and keeping coherence** (the user's): after a sentence ends
  the engine is asked whether the reply says all it should; the coherence
  keeper ends a reply that falls 0.05 below its best sentence end and cuts
  it back there. Block 8 against block 7: 9/10 against 6/10 passed,
  coherence 0.725 against 0.585 (speak.md).
- **A fast memory** after Khasia (SSRN 6459123) learns every passed reply
  at once; speaking, it cost probe passes, so it learns but is silent
  (learner.md, hdmem.md).
- **Why acquisition is slow** (measured): the variables decided the
  engine's words worse than always guessing "Yes"; three variables were
  added (variables.txt). Four input bits a variable are a build option.
- **The teacher** is Qwen3.8-35B from outside (the user's): it writes its
  lines in plain English (`POST /v1/generate`, its lines only), chooses and
  judges by decisions, and talks to MechJev through MechJev's own server;
  MechJev's replies stay its own (teach.md). A first teacher inside the
  learner, composing its lines word by word, was removed.
- **The ladder** (the user's, 2026-10-17: "Qwen needs to prompt
  increasingly complex prompts", "MechJev needs to learn to communicate
  first", "the repetitions aren't needed"): `lessons.txt` is 18 stages in
  order of difficulty; each turn Qwen writes a new line (never one it has
  said) and types it in the TUI, labelled `qwen`; a stage opens with a
  check of MechJev alone, then a check after every 2 taught turns, and 2
  of its last 3 checks move it up (teach.md). The engine's choice of
  lessons and the try-demonstrate-retry loop on one line were removed.
- **Code after communication** (the user's end goal: MechJev writing
  simple, functional programs): parked until the ladder's communication
  stages are climbed. The design measured so far: a C program through the
  tokenizer loses every bracket and quote (`int main(){puts("hi");return
  0;}` became 8 tokens, rendered `Int main puts hi; return 0;`, which tcc
  rejects), so code needs its own marks, renderer, end rule and
  requirements (compiled and run by tcc in bwrap, the output compared with
  a reference program's).
- **The meaning-gated n-gram memory** (the user's ask: repurpose LLM
  n-grams for real-time coherence; the user's one exception to the
  no-Markov rule, which otherwise bans Markov chains in any form, ternary
  included): Engram's design, entries keeping the meaning they were said
  under, learned at once from MechJev's own passed replies, mixed at weight
  1 (ngram.md). Offline, meaning gated it (0.504 against 0.29 shuffled);
  live, 18 of 24 probes against 15 off.
- **The harness, wired for every turn**: `turn.json` while any turn runs
  (TUI, teacher, API, probe), so the TUI shows what is being answered.
- **What it knows** (the user's: say when it doesn't know; imprint on the
  user like a child, confused by the unknown): the parent's words heard
  twice; the novelty and P(IK) as inputs; the ladder's `unknown` and `told`
  stages (speak.md, teach.md).
- **Opinions apart from experiences and facts** (the user's): a Choice
  about what is asked, and a truth requirement from three plain statements
  (12 of 12 on known cases; one compound statement passed everything).
