# prompt.c

Intel Phi Jev's `src/prompt.rs` (26866cb) in C, string for string. A
prompt is a list of segments, template text (special tokens parsed) or user
text (never parsed, the Energy Conversion Box), adjacent segments of one
kind merged, so the tokens are those of the Rust.

- `prompt_prefix`: the system turn (the `XKS_SYSTEM_FILE` text first when
  set, then the fixed SYSTEM or JEV_PREAMBLE text, the Gateway
  Affirmation), then `<document>` around the state.
- `prompt_render`: one fingerprint.
  - In the `letters` layout: the question, the options lettered A to Z
    (past 26, `001` to `255`), the answer cue `Answer:`.
  - In the `jev` layout: the question as compact JSON, then the answer's
    JSON opened up to its value (Patterning).
- `parse_questions`: the request's questions in order. Noul (criteria
  optional, with `true` and `false`), Choice (at least one option), Score
  (2 to 10 levels). A malformed question is refused with its id.
- `render_state`: a string verbatim, anything else as pretty JSON
  (json.c).
- `trim`: Rust's `str::trim` (Unicode whitespace), used on the system file
  and on option descriptions.
