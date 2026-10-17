# MechJev-v2

A decision model that learns to speak English from its own decisions.

MechJev-v2 has two halves, and only the first touches a language model:

1. **The decision engine**, `xks`, in C. It is the XKEYSCORE portion of
   [Mechanical Jev](https://github.com/Lasimeri/Mechanical-Jev) and
   [Intel Phi Jev](https://github.com/Lasimeri/Intel-Phi-Jev), ported from
   Rust. It serves a local Jev (TypeSafe's System One wire,
   `POST /v1/systemone`) on 127.0.0.1:8090 and answers typed questions as
   probabilities:
   - **Noul**: P(true) for a statement.
   - **Choice**: up to 255 options.
   - **Score**: 2 to 10 ordered levels.

   It reads them out of Qwen3.8-35B-A3B (Q4_K_M) through llama.cpp, and the
   35B never writes a word.
2. **The learner**, in C with no llama.cpp: a logic gate circuit,
   conceptually grounded.
   - **Inputs.** Variables whose probabilities the decision engine decides:
     typed questions about the conversation and about the reply so far.
   - **What it does.** It proposes the next word of a reply.
   - **Teacher.** The engine chooses among its proposals, as English
     continuations, and judges every finished reply against a list of
     requirements.
   - **Learning.** The circuit learns from those choices. Its vocabulary
     grows from the English it hears, so its replies grow from silence to
     words to sentences.

The language is acquired by the circuit. Nothing in the 35B changes.

```
╔═════════════════════════════ this machine (Ryzen 7 5800X, RTX 3090 Ti) ════════════════════════════╗
║                                                                                                    ║
║  ┌──────────────────────────────────────────┐  POST /v1/systemone  ┌────────────────────────────┐  ║
║  │ learner (C, no llama.cpp)                │ ───────────────────▶ │ xks, the decision engine   │  ║
║  │                                          │ variables, rerank,   │ (C, :8090)                 │  ║
║  │  heard English ─▶ lexicon                │ requirements         │                            │  ║
║  │                                          │                      │ session on sequence 0,     │  ║
║  │  variables (P) ──┐                       │ ◀─────────────────── │ a fork per question,       │  ║
║  │  word before ────┼─▶ gate circuit ──┐    │ probabilities        │ label log-probabilities    │  ║
║  │  position ───────┘   106 > 256 >    │    │                      │            │               │  ║
║  │                      256 > 128      ▼    │                      │            ▼               │  ║
║  │  ternary tied embeddings ─▶ word scores  │                      │ llama.cpp (CUDA build)     │  ║
║  │                                          │                      │ Qwen3.8-35B-A3B Q4_K_M     │  ║
║  │  learner.store, curves.jsonl             │                      │ 21 of 41 layers on the GPU │  ║
║  └──────────────────────────────────────────┘                      └────────────────────────────┘  ║
║                                                                                                    ║
╚════════════════════════════════════════════════════════════════════════════════════════════════════╝
```

## Use it

You need:
- llama.cpp built with CUDA at `~/llama.cpp/build` (`LLAMA_BUILD` and
  `LLAMA_CPP_DIR` choose another);
- the subject, `XKS_SUBJECT` in `xks.conf`;
- gcc and tcc (the export check compiles with tcc).

```sh
make            # build backend/build/xks and backend/build/learner
make check      # the checks that need no model
make serve      # start the engine on 127.0.0.1:8090 (loads the 35B)
make talk       # talk with the learner on this terminal
make stats      # what it knows
make stop       # stop the engine
```

`backend/build/learner`:
- `talk --trace` prints each turn's whole record, and `talk --alone` lets the
  circuit speak by itself (the engine still perceives and judges, but
  chooses no word: what it has acquired, unaided);
- `hear FILE` reads English text (words only: hearing grows the lexicon,
  it never teaches what follows what);
- `export FILE.c` writes the discrete circuit as a C function;
- `serve` gives the harness an HTTP API on 127.0.0.1:8092;
- `reset --yes` forgets everything.

**Data** is in `~/.local/share/mechjev-v2`:
- `learner.store`;
- `curves.jsonl`, one row a turn;
- `turns.jsonl`, every word's options, the draw and its seed;
- `history.json`.

**Configuration** comes from `xks.conf`, an untracked `xks.local.conf`, and
`~/.config/xks/xks.conf` (shared with Intel Phi Jev). Environment
variables win:

| key | default | what |
| --- | --- | --- |
| `XKS_SUBJECT` | the 35B Q4_K_M | the decision engine's model |
| `XKS_GPU_LAYERS` | `50%` | a count, or a share of the layers, on the GPU |
| `XKS_BIND` | `127.0.0.1:8090` | where the engine answers, and where the learner asks |
| `XKS_KILL_DATE` | `1800` | seconds without a question before the engine exits (0: never) |
| `XKS_PERMUTATIONS` | `3` | option rotations averaged per Choice |
| `XKS_SYSTEM_FILE` | unset | a file's text ahead of the engine's system text |
| `MJ2_TEMP`, `MJ2_TOP_K`, `MJ2_TOP_P`, `MJ2_MIN_P`, `MJ2_REPEAT_PENALTY` | 0.5, 75, 0.95, 0.05, 1.2 | how each word is drawn from the engine's distribution |
| `MJ2_MIN_HEARD` | `2` | times a word is heard before it may be said |
| `MJ2_VARS`, `MJ2_CHECKS` | `variables.txt`, `restrictions.txt` | the variables and the requirements |
| `MJ2_DATA` | `~/.local/share/mechjev-v2` | the learner's data |

## How a reply is made

1. **Hear.** The person's words are counted.
2. **Ground.** One request to the engine answers every `turn` variable in
   `variables.txt` (did the person greet, ask, thank; what should a reply
   do) and how long the reply should be. The length is capped by the
   learner's stage: 3 words, plus 1 for every 4 it can say.
3. **Each word:**
   - **Inputs.** The variables' probabilities are drawn into bits with a
     seed from the realtime clock, so every token has its own seed, kept in
     the trace.
   - **Proposal.** The circuit proposes its 8 best words. When it is
     unsure, the engine adds words from the lexicon.
   - **Choice.** The engine chooses among them as continuations of the
     reply: one Choice, with the `step` variables about the reply so far
     in the same request.
   - **The word said** is drawn from the engine's distribution with the
     settings above.
   - **The example.** The engine's own choice, with its distribution, is
     the example the circuit learns from.
4. **Score each word.** The engine scores the coherence of the reply after
   every word; a word's reward is the change it made (RED, arXiv:2411.08302),
   so a word that makes the reply read better is reinforced even where the
   engine's choice at that step was weak, and one that breaks it is pushed
   down.
5. **Judge.** Each line of `restrictions.txt` is a requirement the engine
   judges about the finished reply. A reply that fails one teaches only
   which shown word the engine preferred, never that its word was right.
6. **Learn.** The circuit is measured on the turn's words before learning
   them: with every input, with the variables blanked, and with the word
   before blanked. If blanking the variables cost nothing, the circuit
   would be a chain over words. Then it learns and rehearses.

The details are in the sibling `.md` of each source file in `backend/src/`.

## Measured

Every number comes with the command or file that produced it.

| what | result | where |
| --- | --- | --- |
| the C engine against the Rust xks: 16 requests, 67 questions (a 30-option trie and four 2.5-2.7k-token sessions among them) | answers, usage and raw log-probabilities byte-identical | `backend/tests/parity/` (`run.sh`, `rust.jsonl`, `c.jsonl`) |
| the session copy (forked against split, one fork) | exactly 0 on 15 fingerprints, sessions of 61 to 1,400 tokens | `xks polygraph` |
| Jev's 28 published questions (Mechanical Jev's `make closeness`) | 27 of 28 of Jev's decisions, mean difference 0.118. Today's Rust xks gives the same rows (difference 0.0); its 28 of 28 dates from September, on an older llama.cpp and site | `backend/tests/parity/closeness-*.jsonl` |
| JSON against serde_json 1.0.151 | byte-identical on 8 cases and 20,000 generated numbers | `make check`, `backend/tests/gen_numbers.c` |
| the circuit's 16 gates | equal their truth tables | `backend/tests/learner_test.c` |
| grounding toy: the right word depends on one variable only (60 examples, 8 seeds, 6,000 passes each; mean, min to max) | held-out agreement 0.912 (0.825 to 0.950); with the variable blanked 0.553 (0.525 to 0.575, chance 0.5); with the word before blanked 0.881 (0.725 to 0.950). One seed trained twice: identical circuits | `backend/tests/learner_test.c` |
| relaxed against discrete circuit | the same word on 0.981 of held-out inputs (0.900 to 1.000 over the 8 seeds) | `backend/tests/learner_test.c` |
| the exported C circuit, compiled | identical to the interpreter on 200 inputs | `backend/tests/learner_test.c` |
| per-token scoring (RED): one context, the engine weakly preferring the wrong word (0.4 to 0.3), 8 seeds | the right word first among the shown: 0.200 (0.025 to 0.500) with the KL alone, 0.887 (0.725 to 1.000) with the per-token reward | `backend/tests/learner_test.c` |
| a word of a reply, live (35B at 50% on the GPU) | 5 to 12 s a step, 15 s a word on average (the third turn of the first conversation) | `turns.jsonl` |

The live learner is at its very beginning. Its first replies were
"(no words yet)" twice, then "Are you? How are". The acquisition curve
(vocabulary, agreement with the engine, the ablations, requirements
passed) is recorded turn by turn in `curves.jsonl` and will be reported
here as it grows.

## Not done yet

- **The Rust harness** (`frontend/`, not yet in the repository):
  Mechanical Jev's TUI reused as the management interface, with a talk
  screen and an MCP side so the same interface can be driven by a person
  and by an agent.
- **No Xeon Phi cards** yet; the engine runs on this machine's CPU and
  GPU only.
- **Cost per word** is measured on single turns, not yet interleaved
  against a baseline.

## Where the ideas come from

| source | what MechJev-v2 takes |
| --- | --- |
| [Code-Forge-AU/jev-llm](https://github.com/Code-Forge-AU/jev-llm), and its r/LLM post "Jev, the new AI that can only pick from a list, has no way to output text. I made it chat anyway." | generation as repeated choice: a rerank over rendered continuations, the end only after a sentence ends |
| [xucian/talktojev](https://github.com/xucian/talktojev), "Generation is Classification" (doi:10.5281/zenodo.22940945) | choosing at several grains: how much to say, then each word; checks after the reply |
| [Aster, "An LLM Made from Logic Gates"](https://www.asterlab.ai/research/an_llm_made_from_logic_gates) | a language model entirely of circuits that compile to C: tied embeddings added or subtracted at the output, a fixed look at the token before, training judged against a growing list of restrictions |
| Petersen, Borgelt, Kuehne, Deussen, "Deep Differentiable Logic Gate Networks" (NeurIPS 2022, arXiv:2210.08277) | the circuit itself: the 16 relaxed gates, their training, their discretization |
| [VladimerKhasia/booleanclass](https://github.com/VladimerKhasia/booleanclass) (doi:10.5281/zenodo.19170384) | learning in Boolean space without forgetting: bits, XOR and popcount, few-shot additions |
| arXiv:2405.15658 (HDC for referring segmentation) | the paper is vision-language segmentation, not hyperdimensional computing; taken from it: decoding at several grains, and a count of how much |
| [TypeSafe AI's Jev](https://typesafe.ai) and its system-one-adapter | the wire format and the confidence formulas |

## Naming

The family's foundation is the Revelation to John, and its lenses are
XKEYSCORE, MKULTRA, Stuxnet and the Gateway Process (Intel Phi Jev's
README). Two names are new here:

- **The little book (Revelation 10:9-11)** is the learner. John takes the
  little book and eats it, and then is told "Thou must prophesy again":
  first it is eaten, then it is spoken.
- **The restrictions** are Aster's: the list a judge holds every candidate
  to, grown from what slipped through.

## The family

| repository | what |
| --- | --- |
| [Intel-Phi-Jev](https://github.com/Lasimeri/Intel-Phi-Jev) | `xks` in Rust, on this host and the Xeon Phi cards; the reference this engine is measured against |
| [Mechanical-Jev](https://github.com/Lasimeri/Mechanical-Jev) | the asking side: `mjev`, its TUI, the evidence and closeness checks |
| [Intel-Phi-AVX512](https://github.com/Lasimeri/Intel-Phi-AVX512) | the card backend Intel Phi Jev uses |

## License

MIT or Apache-2.0, at your option. See `NOTICE` for what descends from
jev-rs (Yijun Yu and contributors) and TypeSafe's system-one-adapter.
