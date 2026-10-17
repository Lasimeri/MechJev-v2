# learner.rs

The client for the learner's HTTP API (`learner serve`): `MJ2_BIND`,
default 127.0.0.1:8092.
- **Calls:** `healthy`, `stats`, `probe` (the fixed probes), `talk(message)` and `talk_as(message, alone)` (one turn: the record
  with the reply and every word's options, score and seeds) and
  `hear(text)`.
- **Timeout.** The read timeout is an hour: a turn is many decisions of
  the 35B.
- **Starting it.** `ensure` starts `learner serve` in its own process
  group, with its output in `$XDG_RUNTIME_DIR/mjev2/learner.log`. Only one
  process writes the learner's data. While a `learner talk` holds it, the
  start fails, and the learner's own refusal (with the holder's pid) is
  the error shown.
