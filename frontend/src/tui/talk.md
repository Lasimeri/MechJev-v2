# tui/talk.rs

The talk screen's state.
- **The conversation.** It is read from `activity.jsonl` in the
  harness's directory (activity.md; last 80 entries): every learner's
  turns and probes, a learner run on another data directory too (a test,
  an A/B, labelled `on <its name>`), and the teacher's verdicts (`qwen`,
  one line each: `told check, alone: check 0.88, passed (stage 5)`). The
  user's ask (2026-10-18): "The harness should show all activity". Words
  the pointer copied from the person's line are named under the reply
  (pointer.md). Before the log existed: the learner's `turns.jsonl` (last
  40 turns), so turns from any client show: this screen, `mjev2 mcp`,
  `mjev2 talk`, `learner talk`.
- **The line being answered**, shown above the reply as it grows: the
  line said (kept from Enter until the turn ends), labelled by
  `speaker.txt` when the learner has one (the teacher's turns: `qwen`),
  else `you`. In the conversation a turn's line is labelled by its
  record's `speaker` the same way. The user's ask (2026-10-17): "THe TUI
  needs to show the initial "you" prompts the (local) training model
  sends"; before, the line only appeared once the turn was over.
- **The reply as it grows.** It comes from `speaking.txt`, which the
  learner rewrites after every word and removes when the turn ends.
- **Polling.** The files are read every 400 ms while the screen is open
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

The side panel also shows the last probe (`probes.jsonl`): each fixed
question with what the circuit alone answered, ✓ or ✗ for the
requirements, and the turns the learner had had.

Checked 2026-10-17 20:21 with the teacher typing (teach.md): while
MechJev answered alone, the screen showed `qwen Hi MechJev. How are you
today?` above `it Hi! I ⠼`, the input line `say (circuit alone) ›` (the
teacher had pressed F2).
