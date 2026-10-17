# learner_main.c

The `learner` command. It links no llama.cpp (`ldd backend/build/learner`
lists no llama library); it reaches the engine over HTTP at `MJ2_ENGINE`,
else `XKS_BIND`.

| command | does |
| --- | --- |
| `talk [MESSAGE]` | one turn, or a conversation on the terminal; the store saved after each turn |
| `hear FILE...` | English text read sentence by sentence (observational learning) |
| `stats` | lexicon, speakable words, variable slots, circuit shape, turns, replay, the most used words |
| `export FILE.c` | the discrete circuit as a C function |
| `serve [--bind]` | HTTP for the harness on 127.0.0.1:8092: `GET /health`, `GET /v1/stats`, `POST /v1/talk {message}`, `POST /v1/hear {text}` |
| `reset --yes` | forget everything |

One process at a time writes the learner's data: `talk`, `hear`, `serve` and
`reset` hold an exclusive lock (flock on `lock` in the data directory, the
holder's pid in it), and a second one is refused with that pid. `stats` and
`export` only read: the store is replaced by rename, never half written.

Data: `$MJ2_DATA`, else `$XDG_DATA_HOME/mechjev-v2`, else
`~/.local/share/mechjev-v2`:
- `learner.store`;
- `history.json` (the last 8 turns);
- `curves.jsonl`, one row per turn: lexicon, speakable, words, agreement with and without the variables or the previous word, requirements, cost;
- `turns.jsonl`, the whole record with every word's options, draw and seeds.


`learner probe` (also `POST /v1/probe`, `mjev2 probe`, the MCP `probe` tool)
is the acquisition curve's instrument. The questions in `probes.txt`
(`MJ2_PROBES`) are put to the circuit alone, each from an empty
conversation. The engine perceives and judges as in any turn, but the
learner hears nothing, learns nothing and counts no turn, so probing
changes nothing it measures. Each answer goes to `probes.jsonl` with the
time, the turns the learner had had, its vocabulary, the reply, the
coherence and the requirements.
