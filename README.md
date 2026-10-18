# MechJev-v2

A decision model that learns to speak English from its own decisions, and
a teacher that talks to it.

MechJev-v2 has three parts, and only the first touches a language model:

1. **The decision engine**, `xks`, in C. It is the XKEYSCORE portion of
   [Mechanical Jev](https://github.com/Lasimeri/Mechanical-Jev) and
   [Intel Phi Jev](https://github.com/Lasimeri/Intel-Phi-Jev), ported from
   Rust. It serves a local Jev (TypeSafe's System One wire,
   `POST /v1/systemone`) on 127.0.0.1:8090 and answers typed questions as
   probabilities:
   - **Noul**: P(true) for a statement.
   - **Choice**: up to 255 options.
   - **Score**: 2 to 10 ordered levels.

   It reads them out of Qwen3.8-35B-A3B (Q4_K_M) through llama.cpp. The 35B
   writes one thing besides: the teacher's lines (`POST /v1/generate`).
   It never writes a word of MechJev's.
2. **MechJev, the learner**, in C with no llama.cpp: a logic gate circuit,
   conceptually grounded.
   - **Inputs.** Variables whose probabilities the decision engine decides:
     typed questions about the conversation and about the reply so far.
   - **What it does.** It proposes the next word of a reply.
   - **How it is taught.** The engine chooses among its proposals, as
     English continuations, and judges every finished reply against a list
     of requirements.
   - **Learning.** The circuit learns from those choices. Its vocabulary
     grows from the English it hears, so its replies grow from silence to
     words to sentences.
3. **The teacher**, in C: Qwen3.8-35B talking to MechJev from outside, as
   a person at its TUI would. It writes its lines in plain English, chooses
   what to teach and judges the answers by decisions, and demonstrates
   through MechJev's own taught turns.

The language is acquired by the circuit. Nothing in the 35B changes.

```
╔═══════════════════════ this machine (Ryzen 7 5800X, RTX 3090 Ti) ════════════════════════╗
║                                                                                          ║
║  ┌─────────────────────────────────────┐  /v1/systemone  ┌─────────────────────────────┐ ║
║  │ MechJev: learner serve (C, :8092)   │ ──────────────▶ │ xks, the decision engine    │ ║
║  │                                     │ variables,      │ (C, :8090)                  │ ║
║  │  heard English ─▶ lexicon           │ rerank, checks  │                             │ ║
║  │  variables (P) ──┐                  │ ◀────────────── │ session on sequence 0, a    │ ║
║  │  word before ────┼─▶ gate circuit   │ probabilities   │ fork per question, label    │ ║
║  │  position ───────┘   106>256>256>128│                 │ log-probabilities           │ ║
║  │  lookahead, stop Noul, keeper       │                 │                             │ ║
║  │  fast memory (silent)               │                 │ /v1/generate: the teacher's │ ║
║  │  learner.store, turns.jsonl         │                 │ lines only                  │ ║
║  └──────────────────▲──────────────────┘                 │              │              │ ║
║                     │ /v1/talk: the typed turns          │              ▼              │ ║
║  ┌──────────────────┴──────────────────┐                 │ llama.cpp (CUDA build)      │ ║
║  │ mjev2 TUI (Rust, tmux jev2.0)       │                 │ Qwen3.8-35B-A3B Q4_K_M      │ ║
║  │ talk screen: qwen/you, solo/it      │                 │ 21 of 41 layers on the GPU  │ ║
║  └──────────────────▲──────────────────┘                 │                             │ ║
║                     │ tmux send-keys: F2, line, Enter    │                             │ ║
║  ┌──────────────────┴──────────────────┐  /v1/generate   │                             │ ║
║  │ teacher (C): Qwen, from outside     │ ──────────────▶ │                             │ ║
║  │ the ladder: 18 stages, a new line   │  /v1/systemone  │                             │ ║
║  │ a turn; /v1/next to MechJev         │ ◀────────────── │                             │ ║
║  │ lessons.txt ─▶ lessons.jsonl        │                 │                             │ ║
║  └─────────────────────────────────────┘                 └─────────────────────────────┘ ║
║                                                                                          ║
╚══════════════════════════════════════════════════════════════════════════════════════════╝
```

## Use it

You need:
- llama.cpp built with CUDA at `~/llama.cpp/build` (`LLAMA_BUILD` and
  `LLAMA_CPP_DIR` choose another);
- the subject, `XKS_SUBJECT` in `xks.conf`;
- gcc and tcc (the export check compiles with tcc).

```sh
make            # build the engine, MechJev, the teacher and the front end (mjev2)
make check      # the checks that need no model
make watch      # the TUI in tmux session jev2, opened in a Konsole window
make tui        # the TUI on this terminal (t: the talk screen)
make serve      # start the engine on 127.0.0.1:8090 (loads the 35B)
make talk       # a bare conversation with MechJev on this terminal
make stats      # what it knows
make stop       # stop the engine
```

**The TUI** (`frontend/target/release/mjev2`) is Mechanical Jev's,
reused as the management interface, with a talk screen added:
- **Left:** the conversation, each reply with its words' per-token scores
  under it, the line being answered and the reply growing word by word
  while a turn runs. The teacher types its lines here, labelled `qwen`.
- **Right:** what MechJev knows and how it is learning: agreement with the
  engine, also without the variables or the word before; coherence;
  requirements; the agreement turn by turn; the last probe.
- **Starting.** The first thing said starts the engine and MechJev's
  server if they are not up. F2 lets the circuit speak by itself.
- **The rest.** Mechanical Jev's other screens (ask, examples, server)
  still put typed questions to the engine.

**`mjev2`** also has `talk`, `probe` (the fixed probe questions to the
circuit alone) and `teach` (the teacher; below).

**MCP.** `mjev2 mcp` lets an agent drive the same TUI a person watches
(`screen`, `type`, `keys`), talk to MechJev (`talk`, `hear`, `probe`,
`stats`), run the teacher (`teach`) and ask the engine (`judge`). Register
it with
`claude mcp add --scope user mjev2 -- <checkout>/frontend/target/release/mjev2 mcp`.

**MechJev** is `backend/build/learner` (backend/src/learner_main.md):
- `talk --trace` prints each turn's whole record, and `talk --alone` lets the
  circuit speak by itself (the engine still perceives and judges, but
  chooses no word: what it has acquired, unaided);
- `hear FILE` reads English text (words only: hearing grows the lexicon,
  it never teaches what follows what);
- `probe` asks the fixed probe questions (`probes.txt`) of the circuit alone;
- `export FILE.c` writes the discrete circuit as a C function;
- `memory-rebuild` and `circuit-rebuild` make the fast memory or the circuit
  again from the record;
- `serve` gives the TUI, the MCP tools and the teacher an HTTP API on
  127.0.0.1:8092 (`/v1/talk`, `/v1/hear`, `/v1/probe`, `/v1/stats`);
- `reset --yes` forgets everything.

**The teacher** is `backend/build/teacher` (backend/src/teach.md):
`teach N` takes N turns up the ladder, typed in the TUI; `status` says
where MechJev is on it, `stage NAME` moves it; `freeze` writes the test
lines once, `measure` puts them to MechJev alone; `lines` shows the lines
Qwen writes. `mjev2 teach [N]` starts the engine and MechJev first.

**Data** is in `~/.local/share/mechjev-v2`:
- `learner.store`;
- `curves.jsonl`, one row a turn;
- `turns.jsonl`, every word's options, the draw and its seed;
- `history.json`;
- `probes.jsonl`, the probes;
- `lessons.jsonl` and `teacher_tests.jsonl`, the teacher's.

**Configuration** comes from `xks.conf`, an untracked `xks.local.conf`, and
`~/.config/xks/xks.conf` (shared with Intel Phi Jev, read before
`xks.conf`). Environment variables win:

| key | default | what |
| --- | --- | --- |
| `XKS_SUBJECT` | the 35B Q4_K_M | the decision engine's model |
| `XKS_GPU_LAYERS` | `50%` | a count, or a share of the layers, on the GPU |
| `XKS_BIND` | `127.0.0.1:8090` | where the engine answers, and where MechJev and the teacher ask |
| `XKS_KILL_DATE` | `1800` | seconds without a question before the engine exits (0: never) |
| `XKS_PERMUTATIONS` | `3` | option rotations averaged per Choice |
| `XKS_UBATCH` | `2048` | tokens in one physical step (engine.md) |
| `XKS_SYSTEM_FILE` | unset | a file's text ahead of the engine's system text (this machine's `~/.config/xks/xks.conf` sets `~/CLAUDE.md`; parity runs set it empty) |
| `MJ2_TEMP`, `MJ2_TOP_K`, `MJ2_TOP_P`, `MJ2_MIN_P`, `MJ2_REPEAT_PENALTY` | 0.5, 75, 0.95, 0.05, 1.2 | how each word is drawn from the engine's distribution |
| `MJ2_MIN_HEARD` | `2` | times a word is heard before it may be said |
| `MJ2_LOOKAHEAD` | `3` | the engine's best words scored for coherence before one is said (0: its first is said) |
| `MJ2_KEEP_DROP` | `0.05` | the coherence keeper: how far below its best sentence end a reply may fall (below 0: off) |
| `MJ2_MEMORY` | `0` | the fast memory's weight in a word's score (0: it learns but does not speak) |
| `MJ2_NGRAM` | `1` | the meaning-gated n-gram memory's weight (times its gate); 0: it learns, silent (backend/src/ngram.md) |
| `MJ2_GATED` | `0` | MechJev's gates make every word in every turn and the engine only judges: the pointer, the n-gram among the circuit's own best, the circuit's code match; a taught turn tries up to three replies judged whole; learning by a margin, never toward the engine's probabilities (backend/src/speak.md, "Gated") |
| `MJ2_COPY` | `1` | adaptive attention over the person's line: a sentinel-gated pointer that may name a word of the line MechJev never said, in a turn it answers alone; 0 off (backend/src/pointer.md) |
| `MJ2_TRUTH` | `1` | judge the truth: no fact it was not told, no experience it could not have had; opinions free (speak.md); 0 off |
| `MJ2_GRAMMAR` | `1` | judge the grammar: no sentence without its subject, none broken (speak.md); 0 off |
| `MJ2_PHRASES` | `0` | the lookahead judges two-word phrases (measured worse and slower; speak.md) |
| `MJ2_RECENT_SHARE` | `0` | share of rehearsal passes drawn from the newest examples |
| `MJ2_PROBES`, `MJ2_PROBE_REPEATS` | `probes.txt`, `3` | the probe questions, and times each is asked |
| `MJ2_VARS`, `MJ2_CHECKS` | `variables.txt`, `restrictions.txt` | the variables and the requirements |
| `MJ2_LESSONS` | `lessons.txt` | the teacher's ladder, its stages in order |
| `MJ2_BIND` | `127.0.0.1:8092` | MechJev's server |
| `MJ2_DATA` | `~/.local/share/mechjev-v2` | the learner's and the teacher's data |
| `MJ2_HARNESS` | the default `MJ2_DATA` | where the TUI watches everything done with MechJev: a learner on another data directory and the teacher write their activity there too (backend/src/activity.md) |
| `MJ2_NAME` | its directory and address | a learner's name in the harness when it runs elsewhere (a test, an A/B) |

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
     never shown. In a turn the teacher teaches, the stage's answer words
     are offered as well. The options are shuffled with a clock seed, so
     the engine never sees the circuit's ranking.
   - **Choice.** The engine chooses among them as continuations of the
     reply: one Choice, with the `step` variables about the reply so far
     in the same request.
   - **The word said** (the coherence lookahead, the user's choice): the
     engine's 3 best after the sampling settings above are scored for how
     good a start each makes, and the best is said. After a sentence ends,
     the engine is asked whether the reply says all a good reply should;
     it stops only if so (the user's stop rule).
   - **Keeping coherence** (the coherence keeper, the user's ask): at
     every sentence end the reply's coherence is held against its best
     sentence end so far; falling more than 0.05 below it ends the reply,
     cut back to that best end. Without it a reply that never "said all"
     ran on and decayed ("Yes!" 0.772, then "Yes! You too? Like to talk?"
     0.558).
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

## The teacher

Qwen3.8-35B teaches MechJev from outside, the way a person at the TUI would
(the user's ask: "MechJev shouldn't question/teach itself, Qwen3.8-35B
should do that for MechJev"), up a ladder: "Qwen needs to prompt
increasingly complex prompts", communication first. `backend/build/teacher`
is a client of the engine and of MechJev's server:

- **The ladder** is `lessons.txt` in order, 18 stages from greetings to
  explaining a thing in its own words: greet, feeling, yes-no, unknown (a
  made-up thing, which it should say it does not know), name-tell, name,
  thanks, goodbye, likes, day, weather, why, two-part, describe, follow-up,
  about-me, learning, explain. Each stage opens with a check of
  MechJev alone (what it has mastered is climbed past at once), then
  taught turns, a check after every 2; 2 of its last 3 checks passed move
  it up.
- **Qwen writes a new line every turn** in plain English (`POST
  /v1/generate`), shown the stage's last 6 exchanges and its own recent
  lines; a line it has said already is written again, so it never loops
  ("Hi MechJev. How are you today?", "Hello MechJev. Are you okay?", "Hi
  MechJev. How are you doing?").
- **It types the line in the TUI**, labelled `qwen` there while MechJev
  answers: a taught turn, the engine choosing each of MechJev's words among
  its own (and the stage's answer words, "MechJev" for "What is your
  name?"), or for a check, MechJev alone (F2).
- **The engine judges** the reply: every requirement, and the stage's own
  check as a Noul.

Qwen writes only the teacher's side: MechJev's replies are always its own,
and it never learns from a reply Qwen wrote. `teacher status` says where it
is; each turn is a row in `lessons.jsonl`, the ladder's only state. Test
lines Qwen writes once (`teacher freeze`) and never teaches measure it
before and after (`teacher measure`). Details: `backend/src/teach.md`.

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
| the coherence keeper: block 8 (keeper on) against block 7 (stop rule alone), the same ten messages | passed every requirement 9/10 against 6/10; mean coherence 0.725 against 0.585; probe 9/12 against 4/12 (not interleaved; block 8 was the fourth pass over the messages, block 7 the third) | `backend/src/speak.md`, `turns.jsonl`, `probes.jsonl` |
| the fast memory (Khasia), learning only: one context a word, 12 words | recalled 12/12, through 10% of the bits flipped 12/12, none lost to a 13th | `backend/tests/learner_test.c` |
| the fast memory speaking (weight 25) against silent, the probe interleaved | 10/24 against 15/24 passed: it pulls first words to "Yes", so it stays silent | `tools/harm_ab.sh`, `backend/src/learner.md` |
| do the turn variables decide the engine's first word? (82 taught turns, leave one out) | nearest neighbour 0.390, nearest centroid 0.366, always "Yes" 0.500: no; "answer a question" covered 51 of them | `backend/src/learner.md` |
| four input bits a variable against one (three words on two variables, 8 seeds, 3 seed sets) | each seed's worst word at 1,000 passes 0.604 against 0.442; a circuit retrained so lost live, 12/24 against 15/24, so one bit stays | `learner_test three`, `backend/src/learner.md` |
| generation for the teacher's lines | "Hello MechJev! How are you?" 9 tokens, 8.2 s; "What is your name, MechJev?" 5.6 s | `backend/src/engine.md` |
| learning after the teacher was moved out (live) | a probe changed nothing (turns 127 to 127); a taught turn trained 15,661 passes in 3 s, saved the store, turns 127 to 128 | `turns.jsonl` |

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
| 102 turns (block 7, the stop rule without the keeper) | 2/3, 0.62: "I am too am too.", "Yes.", "Hello!" | 0/3, 0.33: "I am too am happy.", "I am too am?", "I did? Do?" | 0/3, 0.28: "I did? Walking?", "Cold.", "I am too am?" | 2/3, 0.57: "Thank you? Music you?", "Yes.", "Thank you." |
| 112 turns (block 8, the keeper on) | 3/3, 0.82: "Yes!", "Yes. I am happy.", "Yes. I am happy." | 3/3, 0.82: "Yes!" three times | 0/3, 0.46 | 3/3, 0.75: "Thank you!", "Thank you.", "Thank you!" |

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
- **The ladder is at its first stage.** It ran in the harness (the TUI)
  for its first turns; no stage above greet has been reached yet, and no
  before-and-after on the test lines exists.
- **Code.** The user's end goal, MechJev writing simple, functional
  programs, comes after communication; MechJev cannot yet even hear a
  program whole (its tokenizer drops brackets and quotes; docs/plan.md).
- **Parity after generation** is not re-checked. A run with a generation
  before each parity request differed in every answer, but it ran with the
  user's CLAUDE.md as the engine's system text and the recording did not;
  it has to be run again with `XKS_SYSTEM_FILE=` empty. The engine's code
  for generation only adds lines.
- **The stop rule accepts unfinished replies.** "Mechjev is." stopped at
  0.51 on "says all" with the grammar check at 0.17. Asking whether the
  reply is "a complete, well-formed English reply, and says all ..." (as
  the teacher's own end question was) is proposed, not done: the rule is
  the user's.
- **Cost per word** (4 to 13 s on the live learner) is measured on single
  turns, not interleaved against a baseline.
- **Acquisition** is early (about 130 turns): before learning a turn, the
  circuit's first word was the engine's on 0.490 of the steps, against
  0.461 for the word most often said at that position. The fast memory
  gets 0.403 (0.432 on words, level with both) and, speaking, cost probe
  passes, so it learns but does not speak.
- **The variables did not decide the words.** Over 82 taught turns the
  turn variables predicted the engine's first word worse than always
  guessing "Yes" (0.390 against 0.500): "answer a question" covered 51 of
  them, yes-or-no and "what" questions alike. Three variables were added
  to tell those apart (`variables.txt`); their effect is not measured yet.
  Four input bits a variable make a variable learnable but lost live, so
  the default stays one bit (`backend/src/learner.md`).
- **The step variables arrive a word late.** A step sees the answers the
  step before was given, about the reply one word shorter; asking first
  would cost a request per word (the user chose not now).

## Where the ideas come from

| source | what MechJev-v2 takes |
| --- | --- |
| [Code-Forge-AU/jev-llm](https://github.com/Code-Forge-AU/jev-llm), and its r/LLM post "Jev, the new AI that can only pick from a list, has no way to output text. I made it chat anyway." | generation as repeated choice: a rerank over rendered continuations, the end only after a sentence ends |
| [xucian/talktojev](https://github.com/xucian/talktojev), "Generation is Classification" (doi:10.5281/zenodo.22940945) | choosing at several grains: how much to say, then each word; checks after the reply |
| [Aster, "An LLM Made from Logic Gates"](https://www.asterlab.ai/research/an_llm_made_from_logic_gates) | a language model entirely of circuits that compile to C: tied embeddings added or subtracted at the output, a fixed look at the token before, training judged against a growing list of restrictions |
| Petersen, Borgelt, Kuehne, Deussen, "Deep Differentiable Logic Gate Networks" (NeurIPS 2022, arXiv:2210.08277) | the circuit itself: the 16 relaxed gates, their training, their discretization |
| Petersen, Kuehne, Borgelt, Welzel, Ermon, "Convolutional Differentiable Logic Gate Networks" (NeurIPS 2024, arXiv:2411.04732) | residual initialization, measured against the Gaussian (it keeps the signal; at three layers it was not needed) |
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
