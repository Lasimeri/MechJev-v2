# main.rs

`mjev2` alone, in a terminal, opens the TUI. The commands:

| command | does |
| --- | --- |
| `tui [--file F]` | the TUI (`t` opens the talk screen) |
| `talk MESSAGE [--json]` | one turn: the reply, each word with its score; the engine and the learner are started when they are not up |
| `probe` | the fixed probe questions to the circuit alone: passed of the repeats, mean coherence, what it said |
| `teach [N] [--stage NAME] [--measure]` | the teacher (`backend/build/teacher`): N turns up the ladder, typed in the TUI (after moving it to NAME), or MechJev measured on the frozen test lines; the engine and the learner started first, the teacher's output live |
| `stats` | what the learner knows |
| `serve`, `stop` | the decision engine |
| `mcp` | the MCP server on stdio |

Defaults come from the backend's files (`config.rs`).
