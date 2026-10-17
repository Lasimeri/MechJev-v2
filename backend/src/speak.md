# speak.c

One turn of conversation, every word a decision of the engine (generation
as classification: jev-llm, talktojev).

1. **Hear.** The person's words are counted and learned from.
2. **How much to say.** The engine answers a Score: a word or two, one
   sentence, two or three, a paragraph (3, 12, 30, 60 words). The cap is
   the learner's stage, 3 + speakable / 4, so the replies grow as the
   vocabulary does.
3. **Each word:**
   - **Propose.** The learner proposes its best 8 words. Its rules: no
     punctuation first or doubled, no repeated word or trigram, the end
     only after `. ? !`.
   - **Rerank.** The engine is asked one Choice. Each option is the
     reply's last 8 words with a candidate added, as English (jev-llm's
     rerank), plus `(none of these)`.
   - **Widen.** If none fits, the speakable lexicon goes to the engine in
     groups of 25 with `(not listed)`. The groups' winners are reranked.
   - **Learn.** The pick is the learner's label, and its NLL is taken
     before the update.
4. **Restrictions.** `restrictions.txt`: each line is a Noul the engine
   judges about the finished reply (Aster's restrictions list). The
   probabilities go into the turn's record.

The conversation, meaning the earlier turns and the person's message, is
the request's state. It is the same for every word of a turn, so the
engine keeps it on sequence 0, and each word costs only its question's
tokens. The reply so far goes in the question.
