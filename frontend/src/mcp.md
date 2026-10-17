# mcp.rs

`mjev2 mcp`: JSON-RPC 2.0 over stdio, one message per line (the framing of
Intel Phi Jev's `mcp.rs`). Registered with Claude Code at user scope:
`claude mcp add --scope user mjev2 -- .../frontend/target/release/mjev2 mcp`.

| tool | does |
| --- | --- |
| `screen` | the TUI's screen as text (`tmux capture-pane` of `MJEV2_PANE`, default `jev2.0`) |
| `type` | text into the TUI (`send-keys -l`), `enter: true` to press Enter after it |
| `keys` | tmux key names into the TUI |
| `talk` | one turn through the learner's API, the record back (`alone: true`: the circuit by itself); the TUI shows it too, since it reads the learner's `turns.jsonl` |
| `hear` | English text for the learner to read |
| `probe` | the fixed probe questions to the circuit alone (`probes.txt`), a row each, kept in `probes.jsonl`; the learner hears, learns and counts nothing |
| `teach` | runs the teacher (`backend/build/teacher`): `lessons` from Qwen (1), or `lesson` NAME, or `measure: true` on the frozen test lines; its printed output back. Minutes a lesson |
| `stats` | what the learner knows |
| `judge` | a System One request straight to the decision engine |

A person scrolling the window puts the pane in tmux's copy mode, where
typed keys would go to copy mode. `type` and `keys` leave it first (seen
2026-10-17: `send-keys` then failed with "not in a mode").

Checked 2026-10-17 over stdio: `initialize` (`mjev2` 0.1.0), `tools/list`
(7 tools at the time; 9 now), `screen` (the talk screen's text), and `stats` (44 words heard,
23 it can say, 12 turns).
