# learner_main.c

The `learner` command. It links no llama.cpp (`ldd backend/build/learner`
lists no llama library); it reaches the engine over HTTP at `MJ2_ENGINE`,
else `XKS_BIND`.

| command | does |
| --- | --- |
| `talk [MESSAGE]` | one turn, or a conversation on the terminal; the store saved after each turn |
| `hear FILE...` | English text read sentence by sentence (observational learning) |
| `stats` | lexicon, speakable words, prototypes, turns, the most used words |
| `serve [--bind]` | HTTP for the harness on 127.0.0.1:8092: `GET /health`, `GET /v1/stats`, `POST /v1/talk {message}`, `POST /v1/hear {text}` |
| `reset --yes` | forget everything |

Data: `$MJ2_DATA`, else `$XDG_DATA_HOME/mechjev-v2`, else
`~/.local/share/mechjev-v2`:
- `learner.store`;
- `history.json` (the last 8 turns);
- `curves.jsonl`, one row per turn: lexicon, speakable, words, agreement
  with the engine, NLL, cost, checks.
