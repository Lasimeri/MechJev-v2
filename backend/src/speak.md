# speak.c

One turn of conversation, every word a decision of the engine.

1. **Hear.** The person's words are counted (the lexicon grows); nothing
   else is learned from hearing.
2. **The turn's variables.** Every `turn` line of `variables.txt`, and
   "how long should the reply be" (3, 12, 30 or 60 words), go to the
   engine in one request. The conversation is that request's state, so
   the engine keeps it prefilled for the whole turn. The length is capped
   at the learner's stage, 3 + speakable / 4: the replies grow as the
   vocabulary does.
3. **Each word:**
   - **Inputs.** The variables' probabilities are drawn into bits with a
     seed from the realtime clock (the user's rule: the current time
     seeds each token).
   - **What the engine is shown.** While every word the learner may say
     fits in one Choice (24), the engine is shown all of them, ranked by
     the circuit (a teacher sees a beginner's whole vocabulary; measured
     2026-10-17: with only the circuit's 8, the word the reply needed was
     often missing and the engine chose the least bad). Past 24:
   - **Proposals.** The circuit proposes its best 8 allowed words: no
     punctuation first or doubled, no repeated word or trigram, the end
     only after `. ? !`.
   - **Widening.** Past one Choice's worth of words (24) the engine always
     looks over the whole vocabulary too (in groups of 25, each with
     `(not listed)`), and its 10 best join the circuit's 8: with the
     circuit's 8 alone, "am" was missing after "Hello! I". (Was: if the
     circuit's first word has probability under 0.25,
     it is unsure, and the engine picks up to 6 more from the lexicon (in
     groups of 25, each with `(not listed)`).
   - **The rerank.** The options are shuffled with a clock seed first: they
     arrive ranked by the circuit, and the engine must not see that order.
     One Choice, each option the reply's last 8 words with
     a candidate added, as English (jev-llm's rerank). The `step`
     variables about the reply so far go in the same request and feed the
     next word's inputs.
   - **The coherence lookahead** (the user's choice): the rerank's top 3, in
     the order the user's settings give, are scored by the engine in one
     padded request. The word said is the one that makes the best *start*
     (the viability question: "It is unfinished: more words will follow. How
     good a start is it...", with levels an unfinished reply can top).
     After a sentence ends, the same request asks a Noul about the reply as
     it is, "It says all that a good reply to the person's last message
     should say.", and the reply stops only if it holds (P >= 0.5: the
     user's stop rule, 2026-10-17). Before it, the reply stopped when no
     single next word made it read better as a whole; one more word is
     always unfinished, so in block 6 it stopped at 7 of the 11 points where
     it could, nearly always at the first sentence. That choice is also
     what the circuit learns. `MJ2_LOOKAHEAD=0` restores the rule below.
     The trace keeps the rerank's own top (`rerank_top`), the viability
     scores (`lookahead.start`) and the Noul (`lookahead.complete`).

     Why two questions: the completeness levels only let a finished reply
     reach the top, so choosing the word on them favoured whatever finished
     the reply at once. In block 5 the lookahead overrode the rerank in 29 of
     56 steps; "I" was the rerank's first word 5 times and never kept (0.31 to
     0.39 against "Yes" 0.39 to 0.75). The 13 recorded first-word sets, both
     wordings, the same state (`~/.cache/mechjev-v2-viab-ab.jsonl`):

     | | completeness | viability |
     | --- | --- | --- |
     | "I" first where the rerank wanted it (5 turns) | 1 | 4 |
     | "Yes" kept on yes/no questions (5 turns) | 5 | 5 |
     | "Yes" first, of 13 | 7 | 5 (all yes/no questions) |
     | mean score of "I" | 0.458 | 0.643 |
   - **(without the lookahead)** the word said is, by the user's choice, the most likely one after
     their settings (repeat penalty 1.2, top-k 75, top-p 0.95, min-p 0.05,
     temp 0.5, in llama.cpp's order), the clock seed breaking exact ties
     (`MJ2_DRAW=sample` draws from the distribution instead: under it, 3 of
     15 scored words departed from the engine's choice and averaged -0.057
     reward against +0.074).
   - **The example** is the word said (the lookahead's choice, or without it
     the engine's own), with the rerank's distribution
     over the shown words.
   - **Every request** asks the engine for `pad_equal` (engine.md): a word's
     whole step is one forward pass, 2.0 s instead of 6 to 10.
4. **Requirements.** Each line of `restrictions.txt` is a Noul about the
   newest reply, named as such in the state ("The newest reply, the one to
   judge: ..."): the conversation holds earlier `Reply:` lines too, and a
   bare "the reply" let the engine judge an older one. Measured on "Thank
   you." in the same conversation: grammatical 0.0348, responds 0.7618,
   no repetition 0.395 before; 0.9074, 0.656, 0.8605 after.
   finished reply. One below 0.5, or an empty reply, fails it: the turn's
   examples teach only which shown word the engine preferred, never that
   the word was right (Aster's judge, softened so a beginner can start).
5. **Per-token scores.** Each word's reward is the change in the engine's
   coherence of the reply (a Score asked with every word, and once after
   the last): RED, learner.md.
6. **Measure, then learn.** The circuit is measured on the turn's examples
   before learning them: with all inputs, with the variables blanked, with
   the previous word blanked. Then it learns, and the record goes to
   `curves.jsonl` and `turns.jsonl`.

`learner talk --alone`: the circuit speaks by itself. The engine still
answers the variables (its perception of the conversation and of the reply
so far) and judges the requirements, but chooses no word. Nothing is
learned. The words are the circuit's own first word at each step, so
this shows what it has acquired.

## The person's words, offered

Every step in a taught turn also offers the engine the words the person
just said that may come next and are not shown yet (`offer_heard`), up to
24 options in all, a word heard only now included (the person has just
used it). In blocks 5 and 6 (20 turns) the person used 34 content words;
19 were speakable, 10 were ever shown, 4 were said; widen added a word in
5 of 88 steps (its groups answer "not listed" for a word that cannot come
next yet). The trace marks a word decided from these as `from: heard`.

## The fast memory in a turn

Each step's context (`learner_context`: the variables as the circuit saw
them, the position, the person's words) goes to `learner_propose`, which
adds the memory's score when `MJ2_MEMORY` > 0; the trace's `memory_top` is
the memory's own word. After the requirements, the record's `memory` says
what the memory would have chosen before learning (`agree_before`), and a
reply that passed every requirement is learned (`learned`, `epochs`,
`fit`). `speak_memory_rebuild` makes it again from turns.jsonl.

## The coherence keeper

The user's ask (2026-10-17): "a way of managing coherence, and keeping
it". Under the stop rule a reply ends when it says all it should; a weak
reply never does, so it ran on and decayed. Block 7, the engine's
coherence of the reply as a whole at each sentence end:

| reply | its best sentence end | at the end |
| --- | --- | --- |
| Yes! You too? Like to talk? | "Yes!" 0.772 | 0.558 |
| How are you now? Good! Yes! Too. I feel. Right? Now. Now too? | "How are you now?" 0.676 | 0.547 |
| I too am happy. You? Doing what today? Did? Work. I did? | "... Doing what today?" 0.660 | 0.498 |

At every sentence end the reply's coherence (measured every step for RED
anyway, so no extra request) is held against its best sentence end so
far. Falling more than `keep_drop` below it (`MJ2_KEEP_DROP`, 0.05; below 0
turns it off) ends the reply, and a reply that ends that far below its
best, said all or at its length, too. Either way it is cut back to the
best sentence end: the reply said is that prefix, and the circuit learns
the words up to it and the end there, nothing of the cut words (they were
not said). 0.05 lets the dips this block called noise go on (0.017, 0.037)
and cuts the falls (0.051, 0.117, 0.18). The record's `kept` holds the cut
(`cut_at`, `coherence_best`, `coherence_cut`); the step where it fell is
marked `keeper` in the trace.

Block 8 (block 5's ten messages again, the keeper on), against block 7
(the stop rule without it):

| | block 7 | block 8 |
| --- | --- | --- |
| taught replies passing every requirement | 6/10 | 9/10 |
| mean final coherence | 0.585 | 0.725 |
| the probe, the circuit alone | 4/12 | 9/12 |
| how replies ended | | said all 4 (P 0.51 to 0.55), cut by the keeper 2, the length cap 4 |

Block 7's run-ons taught the circuit to run on (its probe answers: "I am
too am happy.", "I did? Do?"); the keeper teaches the end at the best
sentence instead. Not interleaved, and block 8 was the fourth pass over
these messages (block 7, the third, got worse, not better).
