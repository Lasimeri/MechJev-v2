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
   - **Widening.** If the circuit's first word has probability under 0.25,
     it is unsure, and the engine picks up to 6 more from the lexicon (in
     groups of 25, each with `(not listed)`).
   - **The rerank.** One Choice, each option the reply's last 8 words with
     a candidate added, as English (jev-llm's rerank). The `step`
     variables about the reply so far go in the same request and feed the
     next word's inputs.
   - **The word said** is, by the user's choice, the most likely one after
     their settings (repeat penalty 1.2, top-k 75, top-p 0.95, min-p 0.05,
     temp 0.5, in llama.cpp's order), the clock seed breaking exact ties
     (`MJ2_DRAW=sample` draws from the distribution instead: under it, 3 of
     15 scored words departed from the engine's choice and averaged -0.057
     reward against +0.074).
   - **The example** is the engine's own choice, with its distribution
     over the shown words.
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
