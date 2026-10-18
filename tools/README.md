# tools

Scripts for talking to MechJev and measuring it on the live system.

- `tui_say.sh MESSAGE...`: talk to the learner through the TUI a person is
  watching (`make watch`), one message after another, each turn printed
  with its words' per-token scores and the measures. It holds a lock, so
  only one driver types at a time (two had merged their messages).
- `block5.txt`: ten messages, one a line, used for blocks 5 to 8 (the same
  messages, so each change was compared on them):
  `tr '\n' '\0' < tools/block5.txt | xargs -0 tools/tui_say.sh`.
- `harm_ab.sh ROUNDS WEIGHT...`: the probe with the fast memory at each
  weight, interleaved, ROUNDS times, on one store (a probe changes
  nothing). `learner serve` must be stopped: the probe takes the writer
  lock.
- `arms_ab.sh ROUNDS NAME=BINARY:DATA_DIR...`: the probe with each learner
  binary on its own data, interleaved (old circuit against new). Both
  scripts pass the checkout's `restrictions.txt` and `variables.txt` to
  every binary: a binary copied outside the checkout had found none and
  passed every reply.
- `fewshot_ab.sh snapshot`, then `fewshot_ab.sh WEIGHT SHOTS MESSAGE`: the
  few-shot protocol (Khasia's) on the live learner. Restores the snapshot
  of the learner's data, starts `learner serve` with the memory's weight
  and `fewshot_probes.txt`, probes, then teaches MESSAGE SHOTS times through
  the TUI, probing after each. Not run: the harm check stopped the memory
  first (learner.md).

- `ngram_ab.sh ROUNDS WEIGHT...`: the probe with the n-gram memory at each
  weight, interleaved, against the live `learner serve` (`POST /v1/probe
  {"ngram_weight": W}`, for those probes only); a line a run, the replies
  that passed every requirement (ngram.md).

The teacher has its own program, `backend/build/teacher` (teach.md).
