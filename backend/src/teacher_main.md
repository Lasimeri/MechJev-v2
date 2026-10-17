# teacher_main.c

The `teacher` command: Qwen3.8-35B teaching MechJev from outside
(teach.md). The engine (`make serve`) and MechJev's server (`learner
serve`, which the TUI or `mjev2` start) must be up; `mjev2 teach` starts
both first.

| command | does |
| --- | --- |
| `lines [LESSON...]` | a line Qwen writes for each lesson, printed; nothing is sent to MechJev |
| `freeze [K]` | K test lines a lesson (3) into `teacher_tests.jsonl`, never taught; refused while the file exists |
| `measure [R]` | MechJev alone on every test line, R times (1); a row each in `lessons.jsonl`, passes a lesson and in all |
| `teach [N] [--lesson NAME]` | N lessons (1), the engine choosing each (or NAME every time) |

A failed lesson is reported and the next one taught; two in a row stop the
run (the engine or MechJev down, say).

**Where.** The engine at `MJ2_ENGINE`, else `XKS_BIND` (127.0.0.1:8090);
MechJev at `MJ2_BIND` (127.0.0.1:8092); the lessons in `MJ2_LESSONS`, else
`lessons.txt` in the checkout; the data in `MJ2_DATA`, else
`$XDG_DATA_HOME/mechjev-v2`, else `~/.local/share/mechjev-v2`
(`lessons.jsonl`, `teacher_tests.jsonl`).

**What it prints** is a line a step, for a person or a monitor to follow:
`lesson NAME: Qwen says "..."`, `MechJev alone: "..." PASS|fail (check P)`,
`demonstration K (taught turn, the engine choosing among MechJev's words):
"..."`.
