//! `mjev2`: MechJev-v2's front end. Alone in a terminal it opens the TUI
//! (Mechanical Jev's, with a talk screen for the learner); the rest are
//! the few commands a person or a script needs. See main.md.

use std::io::IsTerminal;
use std::path::PathBuf;

use clap::{Parser, Subcommand};

use mjev2::client::Client;
use mjev2::learner::Learner;
use mjev2::{config, engine};

#[derive(Parser)]
#[command(
    name = "mjev2",
    version,
    about = "MechJev-v2: the decision engine (the 35B's typed readout) and the language learner it teaches; `mjev2` alone, in a terminal, opens the TUI"
)]
struct Cli {
    #[command(subcommand)]
    cmd: Option<Cmd>,
}

#[derive(Subcommand)]
enum Cmd {
    /// The TUI (also `mjev2` alone in a terminal).
    Tui {
        /// A request to open on the ask screen.
        #[arg(long)]
        file: Option<PathBuf>,
    },
    /// Say MESSAGE to the learner; its reply, and each word with its score.
    Talk {
        message: Vec<String>,
        /// The whole turn record as JSON.
        #[arg(long)]
        json: bool,
    },
    /// The fixed probe questions to the circuit alone (probes.jsonl).
    Probe,
    /// Turns up the teacher's ladder, Qwen teaching MechJev from outside
    /// (backend/build/teacher, lessons.txt in order, lessons.jsonl): each
    /// turn a new line typed in the TUI, a check of MechJev alone or a
    /// taught turn.
    Teach {
        /// How many turns.
        #[arg(default_value_t = 1)]
        turns: u64,
        /// Measure MechJev on the frozen test lines (teacher freeze); teach nothing.
        #[arg(long)]
        measure: bool,
        /// Move the ladder to this stage first (a name in lessons.txt).
        #[arg(long)]
        stage: Option<String>,
    },
    /// What the learner knows.
    Stats,
    /// Start the decision engine (and nothing else).
    Serve,
    /// Stop the decision engine.
    Stop,
    /// The MCP server on stdio: the TUI's screen, typing into it, talk, judge.
    Mcp,
}

fn main() {
    config::load();
    let cli = Cli::parse();
    let r = match cli.cmd {
        None if std::io::stdin().is_terminal() => mjev2::tui::app::run(Client::from_env(), None),
        None => Err("mjev2 alone opens the TUI in a terminal; see mjev2 --help".into()),
        Some(Cmd::Tui { file }) => mjev2::tui::app::run(Client::from_env(), file),
        Some(Cmd::Talk { message, json }) => talk(&message.join(" "), json),
        Some(Cmd::Teach { turns, measure, stage }) => teach(turns, measure, stage.as_deref()),
        Some(Cmd::Probe) => probe(),
        Some(Cmd::Stats) => Learner::new()
            .ensure()
            .and_then(|_| Learner::new().stats())
            .map(|v| println!("{}", serde_json::to_string_pretty(&v).unwrap_or_default())),
        Some(Cmd::Serve) => engine::serve(&Client::from_env()),
        Some(Cmd::Stop) => engine::stop(),
        Some(Cmd::Mcp) => mjev2::mcp::serve().map_err(|e| e.to_string()),
    };
    if let Err(e) = r {
        eprintln!("mjev2: {e}");
        std::process::exit(1);
    }
}

fn talk(message: &str, json: bool) -> Result<(), String> {
    if message.trim().is_empty() {
        return Err("mjev2 talk MESSAGE".into());
    }
    let client = Client::from_env();
    if !client.healthy() {
        engine::ensure(&client)?;
    }
    let l = Learner::new();
    l.ensure()?;
    let rec = l.talk(message)?;
    if json {
        println!("{}", serde_json::to_string_pretty(&rec).unwrap_or_default());
        return Ok(());
    }
    let reply = rec["reply"].as_str().unwrap_or("");
    println!("learner: {}", if reply.is_empty() { "(no words yet)" } else { reply });
    let words = mjev2::tui::talk::scored_words(&rec);
    if !words.is_empty() {
        eprintln!("  per word: {words}; coherence {:.2}", rec["coherence_final"].as_f64().unwrap_or(0.0));
    }
    Ok(())
}

fn probe() -> Result<(), String> {
    let client = Client::from_env();
    if !client.healthy() {
        engine::ensure(&client)?;
    }
    let l = Learner::new();
    l.ensure()?;
    let rows = l.probe()?;
    for g in mjev2::tui::talk::probe_groups(rows.as_array().map(Vec::as_slice).unwrap_or_default()) {
        println!("{}", g.question);
        println!(
            "  passed {} of {}, coherence {:.2}; the circuit alone said: {}",
            g.passed,
            g.replies.len(),
            g.coherence,
            g.replies.join(" | ")
        );
    }
    Ok(())
}

/// The teacher, Qwen teaching MechJev from outside (backend/build/teacher):
/// the engine and MechJev's server up first, then its own output, live.
fn teach(turns: u64, measure: bool, stage: Option<&str>) -> Result<(), String> {
    let client = Client::from_env();
    if !client.healthy() {
        engine::ensure(&client)?;
    }
    Learner::new().ensure()?;
    let run = |args: &[&str]| -> Result<std::process::ExitStatus, String> {
        std::process::Command::new(engine::teacher())
            .args(args)
            .status()
            .map_err(|e| format!("{}: {e}", engine::teacher().display()))
    };
    if let Some(name) = stage {
        let st = run(&["stage", name])?;
        if !st.success() {
            return Err(format!("the teacher has no stage {name}: {st}"));
        }
    }
    let n = turns.to_string();
    let status = if measure { run(&["measure"])? } else { run(&["teach", &n])? };
    if status.success() {
        Ok(())
    } else {
        Err(format!("the teacher stopped: {status}"))
    }
}
