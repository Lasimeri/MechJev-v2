# learner_main.c

The `learner` command: MechJev. It links no llama.cpp (`ldd
backend/build/learner` lists no llama library); it reaches the engine over
HTTP at `MJ2_ENGINE`, else `XKS_BIND`.

| command | does |
| --- | --- |
| `talk [MESSAGE]` | one turn, or a conversation on the terminal; the store saved after each turn (`--trace`: the whole record; `--alone`: the circuit by itself) |
| `hear FILE...` | English text read sentence by sentence (its words counted; nothing else is learned from hearing) |
| `probe` | the fixed probe questions to the circuit alone (below) |
| `stats` | lexicon, speakable words, variable slots, circuit shape, turns, replay, the most used words |
| `export FILE.c` | the discrete circuit as a C function |
| `memory-rebuild` | the fast memory made again from turns.jsonl (below) |
| `rejudge` | past passed taught turns judged by today's grammar and truth; failures marked in turns.jsonl and the replay, the memory rebuilt (speak.md) |
| `circuit-rebuild [P]` | the circuit made new and retrained from the replay (below) |
| `serve [--bind B]` | the HTTP API on 127.0.0.1:8092 (below) |
| `reset --yes` | forget everything |

**The HTTP API** (`learner serve`), for the TUI, `mjev2`, its MCP tools and
the teacher:

| route | does |
| --- | --- |
| `GET /health` | `{status, engine}` |
| `GET /v1/stats` | what `stats` prints |
| `POST /v1/talk {message, alone, show}` | one turn in the conversation, its record back. `alone`: the circuit by itself. `show`: words the turn offers the engine besides MechJev's and the person's (heard first, so they are in its lexicon): the teacher's demonstration of an answer whose words the line does not hold |
| `POST /v1/hear {text}` | English text read |
| `POST /v1/probe [{ngram_weight}]` | the fixed probes, a row each (`probes.jsonl`); `ngram_weight` sets the n-gram memory's weight for these probes only (tools/ngram_ab.sh) |
| `POST /v1/probe {message}` | that message to the circuit alone, from an empty conversation, nothing heard, learned or counted: the teacher's try; the turn record back |
| `POST /v1/next {show, alone, speaker, check}` | the next `/v1/talk` turn's settings, used by it once: the teacher, typing its line in the TUI as a person would, sets its answer words (`show`), MechJev alone for a check, who speaks (`speaker`, "Qwen": the turn record's `speaker`, the TUI's label), and its stage check (`check`, one more requirement of that turn) |

One process at a time writes the learner's data: `talk`, `hear`, `probe`,
`serve`, the rebuilds and `reset` hold an exclusive lock (flock on `lock` in
the data directory, the holder's pid in it), and a second one is refused
with that pid. `stats` and `export` only read: the store is replaced by
rename, never half written. At start it warns when it found no
requirements (every reply would pass unjudged: a copy of the binary outside
the checkout finds no `restrictions.txt`; `MJ2_CHECKS` names one) and when
its circuit was made new at load (another input layout, `VAR_BITS`).

Data: `$MJ2_DATA`, else `$XDG_DATA_HOME/mechjev-v2`, else
`~/.local/share/mechjev-v2`:
- `learner.store`;
- `history.json` (the last 8 turns);
- `curves.jsonl`, one row per turn: lexicon, speakable, words, agreement
  with and without the variables or the previous word, requirements, cost;
- `turns.jsonl`, the whole record with every word's options, draw and
  seeds;
- `probes.jsonl`, the probe rows;
- `lessons.jsonl` and `teacher_tests.jsonl`, the teacher's (teach.md);
- `speaking.txt`, the reply so far while a turn runs, and `speaker.txt`, who
  is speaking when it is not the person (written at `/v1/next`, removed when
  the turn ends): the TUI shows both while MechJev answers.

`learner probe` (also `POST /v1/probe`, `mjev2 probe`, the MCP `probe` tool)
is the acquisition curve's instrument. The questions in `probes.txt`
(`MJ2_PROBES`) are put to the circuit alone, each from an empty
conversation, `MJ2_PROBE_REPEATS` times (3). The engine perceives and
judges as in any turn, but the learner hears nothing, learns nothing and
counts no turn, so probing changes nothing it measures. Each answer goes to
`probes.jsonl` with the time, the turns the learner had had, its
vocabulary, the reply, the coherence, the requirements and the memory's
weight.

`learner memory-rebuild` empties the fast memory and makes it again from
turns.jsonl (each passed taught turn, in order; speak.md), prints what it
would have chosen before each turn (overall, by tens of turns, on words and
on marks) and the margin between its first and second word, and saves the
store. `MJ2_MEMORY` is the memory's weight in a word's score (0, the
default: it learns but does not speak).

`learner circuit-rebuild [P]` makes the circuit new and retrains it from
the replay, P passes (by default as many as the old circuit had), holding
out the last 10 taught turns first to compare the new circuit with what the
old one measured on them (learner.md, "Four input bits a variable").

The other settings: `MJ2_NGRAM` (the n-gram memory's weight, 0; ngram.md),
`MJ2_KEEP_DROP` (the coherence keeper, 0.05),
`MJ2_LOOKAHEAD` (3), `MJ2_RECENT_SHARE` (0), the sampling settings
(`MJ2_TEMP` and the rest), `MJ2_MIN_HEARD`, `MJ2_VARS`, `MJ2_CHECKS`; the
README's table has them all.
