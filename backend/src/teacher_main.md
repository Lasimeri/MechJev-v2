# teacher_main.c

The `teacher` command: Qwen3.8-35B teaching MechJev from outside
(teach.md), up a ladder of stages each harder than the last. The engine
(`make serve`) and MechJev's server (`learner serve`, which the TUI or
`mjev2` start) must be up, and for the turns to be seen, the TUI
(`make watch`); `mjev2 teach` starts the engine and MechJev first.

| command | does |
| --- | --- |
| `teach [N]` | N turns on the ladder (1): a check of MechJev alone when one is due, else a taught turn; each a new line Qwen writes, typed in the TUI |
| `status` | the stage it is at, its taught turns and checks, and its last exchanges |
| `stage NAME` | the ladder moved to a stage (a `stage` row) |
| `lines [STAGE...]` | a line Qwen writes for each stage, printed; nothing is sent to MechJev |
| `freeze [K]` | K test lines a stage (3) into `teacher_tests.jsonl`, never taught; refused while the file exists |
| `measure [R]` | MechJev alone on every test line, R times (1), on the probe path; a row each in `lessons.jsonl`, passes a stage and in all |

A failed turn is reported and the next one taken; two in a row stop the
run (the engine or MechJev down, say).

**Where.** The engine at `MJ2_ENGINE`, else `XKS_BIND` (127.0.0.1:8090);
MechJev at `MJ2_BIND` (127.0.0.1:8092); the TUI in tmux pane `MJEV2_PANE`
(`jev2.0`; empty: the API, unseen); the stages in `MJ2_LESSONS`, else
`lessons.txt` in the checkout; the data in `MJ2_DATA`, else
`$XDG_DATA_HOME/mechjev-v2`, else `~/.local/share/mechjev-v2`
(`lessons.jsonl`, the ladder's only state; `teacher_tests.jsonl`).

**What it prints** is two lines a turn, for a person or a monitor to
follow:

```
stage 1/17 greet, a check of MechJev alone: Qwen says "Hi MechJev. How are you today?"
  MechJev alone: "Hi! I am fine?" check 0.82, requirements met: PASS
```

and `promoted: greet -> feeling (after K taught turns, C checks)` when a
stage is climbed.
