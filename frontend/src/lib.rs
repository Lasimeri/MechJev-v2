//! mjev2: MechJev-v2's front end. Mechanical Jev's TUI and client, reused
//! as the harness for the two C programs of the backend: the decision
//! engine (`xks`, TypeSafe's System One wire on 127.0.0.1:8090) and the
//! language learner (`learner serve`, 127.0.0.1:8092). An MCP side lets an
//! agent drive the same interface a person does.
//!
//! - [`protocol`], [`client`], [`confidence`]: the System One wire.
//! - [`engine`]: the backend's programs, found, started and stopped.
//! - [`learner`]: the learner's HTTP API.
//! - [`config`]: defaults from the same files the backend reads.
//! - [`evidence`]: Jev's published examples, for the TUI.
//! - [`tui`]: the terminal interface.
//! - [`mcp`]: the MCP server.

pub mod client;
pub mod confidence;
pub mod config;
pub mod engine;
pub mod evidence;
pub mod learner;
pub mod mcp;
pub mod protocol;
pub mod tui;
