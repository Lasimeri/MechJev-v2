# teach.c: the decision engine as the learner's teacher

The user's ask (2026-10-17): "Setup the LLM I'm using for decisions to not
only decide, but also teaching this architecture." The engine still only
decides (Noul, Choice, Score); it never writes text, and it never writes
the learner's replies. As the teacher it:

1. **chooses the lesson** (`teacher_choose`): a Choice over the lessons in
   `lessons.txt`, with the learner's record in the question (each lesson's
   times taught, how often it passed on lines never taught, the last
   result; from `lessons.jsonl`);
2. **composes its own line** to the learner (`teacher_compose`), a word at
   a time by Choice (the jev-llm way; the user chose this over a bank of
   written lines);
3. **judges** the learner's reply (`teacher_check`): a Noul with the
   lesson's own check, beside the requirements in `restrictions.txt`.

The loop is in learner_main.c (`learner teach`, `POST /v1/teach`,
`mjev2 teach`, the MCP `teach` tool): compose a line; the learner tries it
alone (the probe path: from an empty conversation, nothing heard or
learned); passed only if every requirement holds and the lesson's check is
0.5 or more. Failed: a demonstration (a taught turn on the same line: the
engine chooses each word among the learner's own, the keeper on; the turn
goes to turns.jsonl like any other) and another try, 3 demonstrations at
most. Then mastery: a fresh line toward the lesson's held-out goal (or its
goal), never taught, tried alone. `--measure` does only that, for every
lesson. Each lesson is a row in `lessons.jsonl`.

## Composing a line

The vocabulary is `teacher_words.txt` (simple, common English, written by
the developer; edit freely) and every word the learner has heard, once each
(the teacher's spelling first: "MechJev"). Each word:

- **Groups.** Every candidate (all words but the one before, and the marks
  after a word), shuffled by a clock seed, in Choices of 25 plus "(not
  listed)": "Which of these should come next in it?". One option rotation
  (`{"xks": {"permutations": 1}}`, judge.md): the shuffle already cancels
  the order's pull, and it is a third of the cost.
- **Finalists.** Every word a group gives 0.05 or more, the 12 best. Only
  each group's top, at first, lost the right word: after "Hello how are"
  the finalists were "is", "do" and "?", no "you"; and "do" fell under 0.10.
- **The final Choice**, among the line as it would go on with each, framed
  as unfinished ("It is unfinished: more words will follow. Which of these
  is the best start of clear, correct English that does what the line
  should do?"). Asked "the best way to go on", a line that already looked
  finished won ("Hello how?", "MechJev?"), the myopia the learner's own
  word choice had (speak.md); asked toward the goal alone, the word about
  the goal won over grammar ("What weather" over "What is").
- **The end**, once a sentence has ended, by a Noul: "It is a complete,
  well-formed English line, and it does all that this line should do:
  GOAL" (0.5 or more), never as an option among words. A line has two
  sentences at most (tell, then ask) and is cut back to the sentence end
  where that Noul was highest (the keeper, for the teacher: the lines ran
  on and decayed, "Can you like to walk? Would want to go? Yes? Go? No?").
  A line cut at its length ends with the mark the engine chooses.
- A line under 0.5 on its goal is composed again, once
  (`compose_best`), and the line that tests mastery is never the taught
  line's text.

Measured, `learner compose` (about 4 s a choice, 8 to 25 s a line once
fixed): "Hello how are you?", "How do you feel today?", "What is the
weather like?", "Would you like to play?", "Thanks for talking!",
"Goodbye."; weaker: "What you like?", and the two-part name lesson
("Tell MechJev to tell name your?"), since split into `name-tell` and
`name`. A run varies with the clock seeds.

## Context, so it does not loop

The user's ask: "The teaching model needs to have ctx to prevent looping".
Without it the teacher taught name-tell twice running, greeted with "Hello
how are you?" each time, and demonstrated "I am." twice to "What name?".
Now:

- choosing, it reads its last 6 lessons in order with what came of them,
  and is told a lesson that has not worked the same way twice running can
  wait;
- composing, its state holds the last 8 lines it said to the learner, and
  it is asked to say this one in a new way; the line that tests mastery is
  composed knowing this lesson's exchanges ("In this lesson you said ...
  Alone it answered ... You showed it ...");
- demonstrating, the turn's conversation is this lesson's demonstrations so
  far, and it may offer the lesson's SHOWN WORDS (lessons.txt), the
  answer's words, which the line itself may not hold ("What name?" holds no
  "MechJev").

The learner's tries alone keep an empty conversation (their judging is the
measure). The name lesson, before and after: demonstrations "What?", "I
am.", "I am." against "My name is mechjev." twice; the learner alone after
them "Yes I too.", "Name?" against "Yes, mechjev." (check 0.81) and "My
name is mechjev?" (0.76); on a line never taught "Yes!" (0.01) against "My
name is mechjev am." (0.71, the requirements failing on form).
