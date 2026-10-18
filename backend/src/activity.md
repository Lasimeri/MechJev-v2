# activity.c: everything done with MechJev, where the harness shows it

The user's ask (2026-10-18): "The harness should show all activity". The
TUI watches one directory, the harness's: `MJ2_HARNESS`, else the default
data directory (`$XDG_DATA_HOME/mechjev-v2`, else
`~/.local/share/mechjev-v2`), whatever `MJ2_DATA` says. Before, a learner
run on another data directory (the pointer's A/B on :8093 and :8094) was
invisible there, and a single probe (`POST /v1/probe {"message"}`, the
teacher's tries) showed while it ran and was then recorded nowhere the TUI
reads.

What goes there:

| who | file | what |
| --- | --- | --- |
| every learner | `activity.jsonl` | each finished turn (taught, alone, probe, every probe of a set): `{kind: "turn", instance, mode, speaker, person, reply, failed_requirement, coherence_final, ms_per_word, trace: [{pick, reward, from}]}` |
| a learner elsewhere | `turn.json`, `speaking.txt` | the turn it is answering and its reply so far, `instance` its name; removed when its turn ends while they are still its own |
| the teacher | `activity.jsonl` | each row it writes to `lessons.jsonl` (verdicts, measures, promotions, stage moves), `kind: "lesson"` |

`instance` is null for the learner on the harness's own directory; else
`MJ2_NAME`, else its directory's name and, served, its address
(`mechjev-v2-scratch-pointer-8093 127.0.0.1:8093`).

An entry is one line in one `write` on a file opened `O_APPEND`, so
learners and the teacher may append at once. Each gets `time` (local).
The activity is shown, never read back by the learner: losing a line
loses nothing it learned.

A limit: the learner on the harness's own directory removes its live files
there without asking whose they are (they always were its own). A test
learner answering while it ends a turn has its live display blanked until
its next word. Nothing runs at once today.
