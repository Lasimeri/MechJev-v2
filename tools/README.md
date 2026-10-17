# tools

- `tui_say.sh MESSAGE...`: talk to the learner through the TUI a person is
  watching (`make watch`), one message after another, each turn printed
  with its words' per-token scores and the measures. It holds a lock, so
  only one driver types at a time.
