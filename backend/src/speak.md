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
     good a start is it...", with levels an unfinished reply can top). After
     a sentence ends, the reply stops if no word makes it read better *as a
     whole* (the coherence question, the completeness score RED also uses)
     than the reply as it is. That choice is also what the circuit learns.
     `MJ2_LOOKAHEAD=0` restores the rule below. The trace keeps the rerank's
     own top (`rerank_top`) and both sets of scores (`lookahead.start`,
     `lookahead.whole`).

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
