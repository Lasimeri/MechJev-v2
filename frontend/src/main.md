# main.rs

`mjev2` alone, in a terminal, opens the TUI. The commands:

| command | does |
| --- | --- |
| `tui [--file F]` | the TUI (`t` opens the talk screen) |
| `talk MESSAGE [--json]` | one turn: the reply, each word with its score; the engine and the learner are started when they are not up |
| `stats` | what the learner knows |
| `serve`, `stop` | the decision engine |
| `mcp` | the MCP server on stdio |

Defaults come from the backend's files (`config.rs`).
