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
make            # build the engine, the learner and the front end (mjev2)
make check      # the checks that need no model
make watch      # the TUI in tmux session jev2, opened in a Konsole window
make tui        # the TUI on this terminal (t: the talk screen)
make serve      # start the engine on 127.0.0.1:8090 (loads the 35B)
make talk       # a bare conversation with the learner on this terminal
make stats      # what it knows
make stop       # stop the engine
```

**The TUI** (`frontend/target/release/mjev2`) is Mechanical Jev's,
reused as the management interface, with a talk screen added:
- **Left:** the conversation, each reply with its words' per-token scores
  under it, the reply growing word by word while a turn runs.
- **Right:** what the learner knows and how it is learning: agreement with
  the engine, also without the variables or the word before; coherence;
  requirements; and the agreement turn by turn.
- **Starting.** The first thing said starts the engine and the learner if
  they are not up.
- **The rest.** Mechanical Jev's other screens (ask, examples, server)
  still put typed questions to the engine.

**MCP.** `mjev2 mcp` lets an agent drive the same TUI a person watches
(`screen`, `type`, `keys`), talk to the learner (`talk`, `hear`, `stats`)
and ask the engine (`judge`). Register it with
`claude mcp add --scope user mjev2 -- <checkout>/frontend/target/release/mjev2 mcp`.

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
| `MJ2_LOOKAHEAD` | `3` | the engine's best words scored for coherence before one is said (0: its first is said) |
| `MJ2_MEMORY` | `0` | the fast memory's weight in a word's score (0: it learns but does not speak) |
| `MJ2_PROBE_REPEATS` | `3` | times `learner probe` asks each question |
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
   - **Proposal.** While everything the learner can say fits in one Choice
     (24 words), the engine is shown all of it, ranked by the circuit.
     Past that, the circuit proposes its 8 best and the engine adds its 10
     best from the whole vocabulary. The words the person just said are
     always offered too: before, 24 of 34 content words they used were
     never shown. The options are shuffled with a clock seed, so the engine
     never sees the circuit's ranking.
   - **Choice.** The engine chooses among them as continuations of the
     reply: one Choice, with the `step` variables about the reply so far
     in the same request.
   - **The word said** (the coherence lookahead, the user's choice): the
     engine's 3 best after the sampling settings above are scored for how
     good a start each makes, and the best is said. After a sentence ends,
     the reply stops when no word makes it read better as a whole.
   - **The example.** That decision, with the engine's distribution, is
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
   would be a chain over words. Then it learns and rehearses: the KL to
   the engine's choices, the engine's choice against the whole vocabulary
   (replies that passed), and each word's per-token reward against the
   mean at its position. Half the training passes blank the word before
   (input dropout), so the circuit cannot lean on it alone.
7. **Remember.** A reply that passed is also learned at once by the fast
   memory (after Khasia, SSRN 6459123): no gradient, each step's context
   (the variables, the position, the person's words, never the reply's own
   words) superposed into its word's prototype, then corrected where it
   would have chosen another word. It is measured every turn; it speaks
   only when `MJ2_MEMORY` is above 0.

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
| grounding toy: the right word depends on one variable only (60 examples, 8 seeds, 6,000 passes each; mean, min to max) | held-out agreement 0.906 (0.800 to 0.925); with the variable blanked 0.544 (0.525 to 0.550, chance 0.5); with the word before blanked 0.919 (0.850 to 0.950), with half the training passes blanking the word before (input dropout). One seed trained twice: identical circuits | `backend/tests/learner_test.c` |
| relaxed against discrete circuit | the same word on 0.981 of held-out inputs (0.875 to 1.000 over the 8 seeds) | `backend/tests/learner_test.c` |
| the exported C circuit, compiled | identical to the interpreter on 200 inputs | `backend/tests/learner_test.c` |
| per-token scoring (RED): one context, the engine weakly preferring the wrong word (0.4 to 0.3), 8 seeds | the right word first among the shown: 0.172 (0.000 to 0.400) with the KL alone, 0.803 (0.450 to 0.925) with the per-token reward | `backend/tests/learner_test.c` |
| a word's step on the engine (rerank in 3 rotations, 5 step variables, coherence; 3 interleaved pairs) | 7.2 to 10.2 s unpadded; 5.4 to 5.5 s padded (`pad_equal`); 2.0 s padded at ubatch 2048 | `backend/src/engine.md` |
| a word of a reply, live (35B at 50% on the GPU) | 16 to 25 s a word before the padding; 4 to 5 s after | `turns.jsonl` |

**The live learner, its first day** (`~/.local/share/mechjev-v2/curves.jsonl`):
- **First replies:** "(no words yet)" twice, then "Are you? How are" and
  "I! Am. You?".
- **Fixes along the way:** the requirements deadlock, the engine shown
  every word it can say, the judge naming the newest reply, the
  per-token scores, the word said being the engine's choice, padding, and
  always widening.
- **Taught:** after those, "Hello! Are you happy today?" got "Yes. I am
  happy." and "I am happy too. Do you like music?" got "Yes. I like
  music.", both passing every requirement (coherence 0.74 and 0.72).
- **Alone:** the circuit by itself (no word chosen by the engine) answered
  "Hello! Are you happy today?" with "Yes, hello." (every requirement
  passed, coherence 0.45).

**The acquisition curve** is `learner probe` (`mjev2 probe`, the MCP
`probe` tool): the questions in `probes.txt` put to the circuit alone, each
from an empty conversation. The learner hears, learns and counts nothing,
so a probe changes nothing it measures. The rows go to `probes.jsonl`.

Each question is asked 3 times (the variable bits are drawn with a new clock
seed each time): passed of 3, mean coherence, and what the circuit alone
said.

| after | Hello! Are you happy today? | Do you like music? | What is your name? | Thank you for talking with me. |
| --- | --- | --- | --- | --- |
| 59 turns | 3/3, 0.79: "Yes. I am.", "Yes. I am.", "I am happy too." | 2/3, 0.68: "Hello. I do.", "Yes. I do music.", "Yes." | 0/3, 0.30 (no name was ever taught) | 3/3, 0.73: "Yes.", "Thank you.", "Hello." |

Two earlier single-sample probes (49 and 59 turns, 3 and 1 of 4 passed)
fall inside this spread, which is why each question is now asked three
times.

Before the probe instrument, the same questions
put through the conversation (heard and counted) got word salad from the
circuit alone ("Yes? Too music? Do, asking! Today. I too. Too."). It has
since learned to stop after a sentence. It still says "Yes." where "Yes."
is not an answer.


## Not done yet

- **No Xeon Phi cards** yet; the engine runs on this machine's CPU and
  GPU only.
- **Cost per word** (4 to 5 s on the live learner) is measured on
  single turns, not yet interleaved against a baseline.
- **Acquisition** is early (about 100 turns): before learning a turn, the
  circuit's first word is the engine's on 0.490 of the steps, against
  0.461 for the word most often said at that position. The fast memory
  gets 0.403 (0.432 on words, level with both) and, speaking, cost probe
  passes (10/24 against 15/24), so it learns but does not speak.
- **The variables do not decide the words yet.** Over 82 taught turns
  the turn variables predict the engine's first word worse than always
  guessing "Yes" (0.390 nearest neighbour, against 0.500): "answer a
  question" covers 51 of them, yes-or-no and "what" questions alike. Four
  input bits a variable make a variable learnable (a three-word test from
  0.794 to 0.898), but a circuit retrained so lost live (12/24 against
  15/24), so the default stays one bit (`backend/src/learner.md`).
- **Replies end at the first sentence.** In block 6, at 11 points where
  the reply could stop, it stopped 7 times: a one-word continuation is
  unfinished, so it never reads better as a whole than "Yes." does. That
  leaves no room for anything about what the person said.
- **The step variables arrive a word late.** A step sees the answers the
  step before was given, about the reply one word shorter; asking first
  would cost a request per word.

## Where the ideas come from

| source | what MechJev-v2 takes |
| --- | --- |
| [Code-Forge-AU/jev-llm](https://github.com/Code-Forge-AU/jev-llm), and its r/LLM post "Jev, the new AI that can only pick from a list, has no way to output text. I made it chat anyway." | generation as repeated choice: a rerank over rendered continuations, the end only after a sentence ends |
| [xucian/talktojev](https://github.com/xucian/talktojev), "Generation is Classification" (doi:10.5281/zenodo.22940945) | choosing at several grains: how much to say, then each word; checks after the reply |
| [Aster, "An LLM Made from Logic Gates"](https://www.asterlab.ai/research/an_llm_made_from_logic_gates) | a language model entirely of circuits that compile to C: tied embeddings added or subtracted at the output, a fixed look at the token before, training judged against a growing list of restrictions |
| Petersen, Borgelt, Kuehne, Deussen, "Deep Differentiable Logic Gate Networks" (NeurIPS 2022, arXiv:2210.08277) | the circuit itself: the 16 relaxed gates, their training, their discretization |
| [VladimerKhasia/booleanclass](https://github.com/VladimerKhasia/booleanclass) (doi:10.5281/zenodo.19170384) and its paper, Khasia, "Backpropagation-Free Continual Learning in Boolean Space for Extreme Edge Intelligence" (SSRN 6459123, 2026) | the fast memory: level hypervectors, XOR binding, majority bundling, class prototypes by superposition, error-driven correction, recall by Hamming distance |
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
