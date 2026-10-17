# json.c

A JSON tree read and written exactly as serde_json 1.0.151 does with
`preserve_order`, the build the Rust xks uses. Exactness matters because a
state given as JSON is rendered as pretty JSON into the prompt, and a
different byte is a different token.

| behaviour | as serde_json |
| --- | --- |
| object order | insertion order; a repeated key replaces the value in the first key's place |
| integers | u64 when non-negative and fitting, i64 when negative, `-0` and overflow as f64 |
| float reading | serde's fast path without `float_roundtrip`: a u64 significand times or divided by 10^e from a table (1e308 at a time beyond it), digits past the significand's overflow dropped. Not always the nearest double past 17 digits (`1.2345678901234567e+30` reads as `...568e+30`; strtod gives `...567`) |
| float writing | zmij 1.0.23: shortest round-trip digits, fixed notation for decimal exponents -5..=15, else `1e+21`, `1e-7` (sign always, no padding) |
| strings | `\"`, `\\`, `\b \t \n \f \r`, other controls as `\u00XX` (lower-case hex), nothing else escaped |
| pretty | two-space indent, `": "`, empty containers `{}` and `[]` |

Measured (2026-10-17): byte-identical to serde_json on
`backend/tests/json_cases.jsonl` (8 cases, 84 output lines) and on 20,000
generated numbers (`backend/tests/gen_numbers.c`, 1,000 lines; 213 lines
refused as out of range by both). `make check` repeats the first.
