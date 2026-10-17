# learner.c

The language learner: a lexicon and one prototype per word.

- **Lexicon.** It starts with only `<s>`, `</s>` and `. , ? !`. Every
  other word is acquired by hearing it in the person's English or in text
  read in. A word may be said once it has been heard `min_heard` (2)
  times, or once the engine has chosen it.
- **Context.** The next word's context is the previous word rotated once;
  the previous two bound (XOR, rotated by distance); and the previous
  three bound. These are bundled with weights 3, 2, 2, together with the
  person's words (weight 1, a bag). Near contexts share the first
  component, the same phrase shares all three.
- **Prediction.** The speakable words are ranked by similarity of their
  prototype to the context; ties go to the more often heard.
- **Learning.** On a label: `proto[label] += ctx`, and when the learner's
  own first guess was another word, `proto[guess] -= ctx` (booleanclass's
  mistake-driven update). Labels come from two places: hearing (every next
  word of what was heard) and the engine (the word it chose).
- **NLL.** `learner_nll` is the label's -log p under a softmax over
  similarity / tau (tau 128), taken BEFORE learning the label, so it is a
  held-out (prequential) number.
- **The store.** `learner.store` holds a header (magic, version, bits,
  seed) and then each word: its counts and its int16 accumulators. Another
  version or dimension is refused, never converted.

Measured (`backend/tests/learner_test.c`, 2026-10-17, no model):
- encoding is bit-exact across learners with one seed;
- a store reloads to identical prototypes and predictions;
- a version-99 header is refused;
- four 5-word phrases heard 3 times score 1.000 next-word accuracy;
- a fifth phrase goes from 0.000 to 1.000 in 5 shots with the old ones
  kept at 1.000.
