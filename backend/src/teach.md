# teach.c: the teacher

Qwen3.8-35B teaches MechJev from outside, as a person at the TUI would:
the user's ask (2026-10-17), "MechJev shouldn't question/teach itself,
Qwen3.8-35B should do that for MechJev, similar to what you were doing
initially to talk to the MechJev architecture". The program is `teacher`
(teacher_main.c); this file is its library. It is a client of two servers
and links no llama.cpp:

- **the engine** (xks, `MJ2_ENGINE`, else `XKS_BIND`): Qwen writes the
  teacher's lines (`POST /v1/generate`) and judges MechJev's replies (a
  Noul);
- **MechJev** (learner serve, `MJ2_BIND`, 127.0.0.1:8092), through the TUI
  a person watches: the teacher types its lines there (below, step 4),
  after `POST /v1/next` has set the turn's settings. Without a TUI
  (`MJEV2_PANE=`) a check is `POST /v1/probe {"message"}` (from an empty
  conversation, nothing heard, learned or counted) and a taught turn
  `POST /v1/talk {"message", "show"}`; `teacher measure` always takes the
  probe path.

**The line it holds.** Qwen writes only the teacher's side. MechJev's
replies are always its own: in a taught turn the engine chooses each
word among the words MechJev offers (its circuit's, the line's, the
lesson's shown words), as in any taught turn. MechJev never learns from a
reply Qwen wrote.

## The ladder (`lessons.txt`)

The user's asks (2026-10-17): "Qwen needs to prompt increasingly complex
prompts", "MechJev needs to learn to communicate first" (code comes after
it), and "the repetitions aren't needed". The stages are the lines of
`lessons.txt` in order, each harder than the one before, 18 now: greet,
feeling, yes-no, unknown, name-tell, name, thanks, goodbye, likes, day,
weather, why, two-part, describe, follow-up, about-me, learning, explain.
`unknown` (the user's ask: MechJev "needs to communicate when it doesn't
know something") has Qwen ask about a made-up thing as if everyone knew
it; the check holds when MechJev says it does not know or asks what it is
(R-Tuning, arXiv:2311.09677: saying so is a skill that carries over).

`NAME | GOAL | CHECK [| HELD-OUT GOAL [| SHOWN WORDS]]`, a line each:

- **GOAL** is what Qwen's line should do ("Ask MechJev how it feels
  today.").
- **CHECK** is a statement about MechJev's reply, judged as a Noul: a turn
  passes when it holds (0.5 or more) and every requirement in
  `restrictions.txt` holds (MechJev's own judging, in its turn record).
- **HELD-OUT GOAL** is what a check's line should do instead (the name
  stage asks without saying the name).
- **SHOWN WORDS** are the answer's words a taught turn may offer, which
  the line itself may not hold ("What is your name?" holds no "MechJev");
  MechJev hears them first, so they are in its lexicon.

The learner's name, for the name stages, is MechJev (the user's).

## A turn on the ladder (`teacher teach N`)

1. **Where it is**, read from `lessons.jsonl` (the only state): the stage
   the last `promote` or `stage` row reached, else the first; the taught
   turns and checks at it since.
2. **A check or a taught turn.** A stage opens with a check of MechJev
   alone, so a stage it has mastered is climbed past at once; after that a
   check comes after every 2 taught turns (`CHECK_EVERY`).
3. **Qwen writes a new line** (`teacher_write`, then `next_line`): the
   system text makes it a patient English teacher talking with MechJev, a
   beginner; the user text holds the stage's goal (a check's: the held-out
   goal), the stage's last 6 exchanges (its lines and MechJev's replies)
   and its last 8 lines ("Write a new line toward the same goal, in other
   words than every line above"). At most 48 tokens, a clock seed, the
   user's sampling settings (engine.md); the first line, quotation marks
   taken off. A line that is a test line or one it has said already
   (marks, spaces and case aside) is written again, the refused lines shown
   to it, 4 times at most; so is a near-repeat, a line sharing 0.75 or more
   of its words with one said (`teacher_word_overlap`: "How are you
   today?" against "How are you?" 0.833, against "How are you feeling
   today?" 0.857; "Are you okay?" against "How are you today?" 0.375),
   or a line whose words all lie in a said line's (or a said line's in
   its: "How are you today?" inside "Hi MechJev. How are you today?",
   overlap 0.667), unless every rewrite was one, when the last is said
   rather than none. Each row keeps the lines refused and why (`refused`),
   and the teacher prints them.
   The user's ask that made this: "Stop the Qwen
   model from looping when teaching MechJev": one line had been typed for
   every try and demonstration of a lesson, 4 times in the greet lesson.
4. **Typed in the TUI** (`teacher_say_in_tui`), as the user asked: "The
   local teaching model needs to show up in the harness just like when you
   type for me". It tells MechJev's server the next turn's settings
   (`POST /v1/next {alone, show, speaker: "Qwen"}`, used by that turn
   once), puts the TUI on its talk screen and in the mode the turn needs
   (F2, seen on the screen: the circuit alone for a check, taught
   otherwise), types the line and Enter, and reads the turn from
   `turns.jsonl`. The TUI shows the line, labelled `qwen`, while MechJev
   answers, and keeps the label in the conversation (the record's
   `speaker`). Under the driver lock, so no two drivers type at once.
   `MJEV2_PANE` names the pane (`jev2.0`); empty, the turns go through the
   API, unseen.
5. **Judged**: the stage's check on MechJev's reply, and its requirements.
6. **Up a stage** when 2 of its last 3 checks passed (`PROMOTE`): a
   `promote` row.

Each turn is a row in `lessons.jsonl` (mode `taught` or `check`, `lesson`
the stage's name, `stage` its number): the line, the reply, the check's
probability, the requirements, the coherence, passed or not.
`teacher status` prints where it is; `teacher stage NAME` moves it.

Before the ladder (2026-10-17, f02ffbd to the ladder): the engine chose
each lesson (a Choice over the names with MechJev's record), and a lesson
was a try alone, then up to 3 demonstrations each followed by another try,
all on one line. The user called that repetition and asked for rising
complexity instead.

## The test lines (`teacher freeze`, `teacher measure`)

`teacher freeze [K]` has Qwen write K lines a lesson (3) toward the
held-out goal, each new against the ones already written, into
`teacher_tests.jsonl`. They are never taught, and they do not change: it
refuses to write them again while the file exists, so measurements before
and after teaching use the same lines. `teacher measure [R]` has MechJev
try every test line alone R times (1), judged, a row each in
`lessons.jsonl` (mode `measure`), and prints passes a lesson and in all.

## Measured so far

- **Generation** (`POST /v1/generate`, the live engine): "Hello MechJev!
  How are you?" (9 tokens, 8.2 s) and "What is your name, MechJev?" (5.6
  s), from a goal each.
- **The teacher end to end has not been run yet** (written and built
  2026-10-17, f02ffbd).

## Before: the teacher inside the learner (removed)

The first teacher ran inside the learner (`learner teach`) and composed
its lines a word at a time by Choice from MechJev's own vocabulary and a
word list. The user rejected it: MechJev was questioning itself, in its
own clipped words ("Who name please?", "MechJev. Hello."). What it
measured, still true of the lessons:

- **Composing by Choice** needed every fix the learner's replies had
  needed: words judged as an unfinished start, not "the best way to go
  on" ("Hello how?" won); every word a group gave 0.05 or more, not each
  group's top ("you" and "do" never reached the finalists); the end by a
  Noul, two sentences at most. Six of nine lines read well after that
  ("How do you feel today?"), at 8 to 25 s a line.
- **Context against looping** (the user's ask, "The teaching model needs
  to have ctx to prevent looping"): without its last lessons and lines it
  taught one lesson twice running, greeted the same way each time, and
  demonstrated "I am." twice to "What name?".
- **Shown words**: with the answer's words offered, the name lesson's
  demonstrations went from "What?", "I am.", "I am." to "My name is
  mechjev." twice; MechJev alone then said "Yes, mechjev." (check 0.81) and
  "My name is mechjev?" (0.76), and on a line never taught "My name is
  mechjev am." (0.71, against "Yes!" 0.01 before), the requirements still
  failing it on form.
- **What a demonstration teaches is the line's**: to "MechJev. Hello."
  every demonstration answered the greeting, and the name was not
  learned. The lines' quality decides the lesson.

The baseline taken then (one fresh line a lesson, 10 lessons): 1 of 10
passed, yes-no only ("Yes!", 0.97). It used composed lines, not the test
lines above, so it is not comparable with `teacher measure`.
