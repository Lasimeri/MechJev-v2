# tui/talk.rs

The talk screen's state.
- **The conversation.** It is read from the learner's `turns.jsonl` (last
  40 turns), so turns from any client show: this screen, `mjev2 mcp`,
  `mjev2 talk`, `learner talk`.
- **The reply as it grows.** It comes from `speaking.txt`, which the
  learner rewrites after every word and removes when the turn ends.
- **Polling.** Both files are read every 400 ms while the screen is open
  or a talk job runs.

The screen (view.rs `talk`):
- **Left:** the conversation; under each reply, its words with their
  per-token scores, the coherence, seconds a word, and whether it failed a
  requirement.
- **Right** (terminals 96 wide or more): words heard and sayable, turns,
  examples kept; the last turn's agreement with the engine, also without
  the variables or the word before; coherence; requirements; and the
  agreement turn by turn as bars.
- **Bottom:** the line to say.

Keys: Enter says it, Esc goes home, PgUp and PgDn scroll.

Checked 2026-10-17 in tmux: the conversation and the panel drawn from 11
turns of the learner's file. "Hello again! Do you want to talk about
music?" was answered "Hello? Do" in 54 s, the reply growing on screen
("Hello", "Hello?", "Hello? Do"). The words and their scores were shown
under it, and the screen started `learner serve` itself.

F2 switches the next turns to the circuit alone: the API's `"alone": true`.
The engine perceives (the variables) and judges (requirements, coherence),
but chooses no word, and nothing is learned. Such turns are marked `solo`.
First one, 2026-10-17: "Hello! Are you happy today?" got "Yes, hello."
(every requirement passed, coherence 0.45, 3 s a word).
