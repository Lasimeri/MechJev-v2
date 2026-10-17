# teach.c: the teacher

Qwen3.8-35B teaches MechJev from outside, as a person at the TUI would:
the user's ask (2026-10-17), "MechJev shouldn't question/teach itself,
Qwen3.8-35B should do that for MechJev, similar to what you were doing
initially to talk to the MechJev architecture". The program is `teacher`
(teacher_main.c); this file is its library. It is a client of two servers
and links no llama.cpp:

- **the engine** (xks, `MJ2_ENGINE`, else `XKS_BIND`): Qwen writes the
  teacher's lines (`POST /v1/generate`), chooses the lesson (a Choice) and
  judges MechJev's replies (a Noul);
- **MechJev** (learner serve, `MJ2_BIND`, 127.0.0.1:8092): a try alone is
  `POST /v1/probe {"message"}` (the probe path: from an empty
  conversation, nothing heard, learned or counted); a demonstration is
  `POST /v1/talk {"message", "show"}`, a taught turn in its conversation.

**The line it holds.** Qwen writes only the teacher's side. MechJev's
replies are always its own: in a demonstration the engine chooses each
word among the words MechJev offers (its circuit's, the line's, the
lesson's shown words), as in any taught turn. MechJev never learns from a
reply Qwen wrote.

## The lessons (`lessons.txt`)

`NAME | GOAL | CHECK [| HELD-OUT GOAL [| SHOWN WORDS]]`, a line each:

- **GOAL** is what the teacher's line should do ("Ask the learner how it
  feels today.").
- **CHECK** is a statement about MechJev's reply, judged as a Noul: a try
  passes when it holds (0.5 or more) and every requirement in
  `restrictions.txt` holds (MechJev's own judging, in its turn record).
- **HELD-OUT GOAL** is what the test lines should do instead (the name
  lesson asks without saying the name).
- **SHOWN WORDS** are the answer's words a demonstration may offer, which
  the line itself may not hold ("What is your name?" holds no "MechJev");
  MechJev hears them first, so they are in its lexicon.

The learner's name, for the name lessons, is MechJev (the user's).

## A lesson (`teacher teach`)

1. **The engine chooses the lesson**, a Choice over the lesson names with
   MechJev's record in the question: each lesson's times taught, its passes
   on the test lines, the last lesson's result, and the last 6 lessons in
   order ("a lesson that has not worked the same way twice running can
   wait while you teach another").
2. **Qwen writes the line** (`teacher_write`): the system text makes it a
   patient English teacher talking with MechJev, a beginner; the user text
   holds the goal and its last 8 lines ("Say it in your own words, not as
   you have said it before"). At most 48 tokens, a clock seed, the user's
   sampling settings (engine.md). The first line is kept, quotation marks
   taken off. A line that is a test line (marks, spaces and case aside) is
   written again, 3 times at most.
3. **MechJev tries alone**, judged.
4. **Failed: a demonstration**, a taught turn on the same line in
   MechJev's conversation (the TUI shows it), then another try; 3
   demonstrations at most.

Each lesson is a row in `lessons.jsonl` (mode `teach`): the line, every
try and demonstration, the demonstrations it took, passed or not.

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
