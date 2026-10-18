//! The talk screen's state: the conversation as the learner records it
//! (`turns.jsonl` in its data directory, so turns from any client show:
//! this screen, `mjev2 mcp`, `learner talk`), the reply growing word by
//! word while a turn runs (`speaking.txt`, which the learner rewrites after
//! every word), and the line being typed. Drawing is view.rs. See talk.md.

use std::path::PathBuf;
use std::time::{Duration, Instant};

use serde_json::Value;

use super::editor::Editor;

/// How many turns are kept on screen.
const KEEP: usize = 40;
/// How often the learner's files are looked at.
pub const POLL_FILES: Duration = Duration::from_millis(400);

/// The learner's data directory, as the learner finds it (learner_main.c):
/// `$MJ2_DATA`, else `$XDG_DATA_HOME/mechjev-v2`, else
/// `~/.local/share/mechjev-v2`.
pub fn data_dir() -> PathBuf {
    if let Some(d) = std::env::var_os("MJ2_DATA").filter(|v| !v.is_empty()) {
        return PathBuf::from(super::app::expand_home(&d.to_string_lossy()));
    }
    if let Some(x) = std::env::var_os("XDG_DATA_HOME").filter(|v| !v.is_empty()) {
        return PathBuf::from(x).join("mechjev-v2");
    }
    PathBuf::from(std::env::var_os("HOME").unwrap_or_default()).join(".local/share/mechjev-v2")
}

pub struct Talk {
    pub input: Editor,
    /// The next turns spoken by the circuit alone (F2).
    pub alone: bool,
    /// The last turns' records, oldest first.
    pub turns: Vec<Value>,
    /// The last probe's rows (probes.jsonl): the circuit alone on the fixed
    /// questions.
    pub probes: Vec<Value>,
    probes_len: u64,
    /// The line a turn in progress answers (shown above its reply so far),
    /// and who said it (`speaker.txt`: the teacher; else the person).
    pub pending: Option<String>,
    pub pending_by: Option<String>,
    /// The turn the learner is answering now, whoever asked (`turn.json`:
    /// {line, speaker, mode}): the teacher's API turns and the probes too.
    pub live: Option<Value>,
    /// The reply so far of a turn in progress.
    pub speaking: Option<String>,
    /// Rows scrolled up from the bottom of the conversation.
    pub back: usize,
    turns_len: u64,
    polled: Option<Instant>,
}

impl Default for Talk {
    fn default() -> Self {
        Self {
            input: Editor::new(true),
            alone: false,
            turns: Vec::new(),
            probes: Vec::new(),
            probes_len: u64::MAX,
            pending: None,
            pending_by: None,
            live: None,
            speaking: None,
            back: 0,
            turns_len: u64::MAX,
            polled: None,
        }
    }
}

impl Talk {
    /// Read the learner's files when due; whether anything shown changed.
    pub fn poll(&mut self) -> bool {
        if self.polled.is_some_and(|t| t.elapsed() < POLL_FILES) {
            return false;
        }
        self.polled = Some(Instant::now());
        let dir = data_dir();
        let mut changed = false;
        let len = std::fs::metadata(dir.join("turns.jsonl")).map_or(0, |m| m.len());
        if len != self.turns_len {
            self.turns_len = len;
            let text = std::fs::read_to_string(dir.join("turns.jsonl")).unwrap_or_default();
            let lines: Vec<&str> = text.lines().filter(|l| !l.trim().is_empty()).collect();
            self.turns = lines[lines.len().saturating_sub(KEEP)..]
                .iter()
                .filter_map(|l| serde_json::from_str(l).ok())
                .collect();
            changed = true;
        }
        let plen = std::fs::metadata(dir.join("probes.jsonl")).map_or(0, |m| m.len());
        if plen != self.probes_len {
            self.probes_len = plen;
            let text = std::fs::read_to_string(dir.join("probes.jsonl")).unwrap_or_default();
            let rows: Vec<Value> = text.lines().filter_map(|l| serde_json::from_str(l).ok()).collect();
            // the last probe: the rows sharing the newest time stamp
            let last = rows.last().map(|r| r["time"].clone());
            self.probes = rows.into_iter().filter(|r| Some(&r["time"]) == last.as_ref()).collect();
            changed = true;
        }
        let live = std::fs::read_to_string(dir.join("turn.json")).ok().and_then(|s| serde_json::from_str::<Value>(&s).ok());
        if live != self.live {
            self.live = live;
            changed = true;
        }
        let by = std::fs::read_to_string(dir.join("speaker.txt")).ok().map(|s| s.trim().to_string());
        if by != self.pending_by {
            self.pending_by = by;
            changed = true;
        }
        let speaking = std::fs::read_to_string(dir.join("speaking.txt")).ok();
        if speaking != self.speaking {
            self.speaking = speaking;
            changed = true;
        }
        changed
    }

    /// The last turn's record, the numbers the side panel shows.
    pub fn last(&self) -> Option<&Value> {
        self.turns.last()
    }
}

/// A turn's words with their per-token scores: `morning +0.22  ? -0.02`.
pub fn scored_words(turn: &Value) -> String {
    let mut out = Vec::new();
    for s in turn["trace"].as_array().map(Vec::as_slice).unwrap_or_default() {
        if let (Some(w), Some(r)) = (s["pick"].as_str(), s["reward"].as_f64()) {
            out.push(format!("{w} {r:+.2}"));
        }
    }
    out.join("  ")
}

/// One probe question's repeats: how many passed every requirement, the mean
/// coherence, and the answers.
pub struct ProbeGroup {
    pub question: String,
    pub passed: usize,
    pub coherence: f64,
    pub replies: Vec<String>,
}

/// Probe rows grouped by question, in the order asked.
pub fn probe_groups(rows: &[Value]) -> Vec<ProbeGroup> {
    let mut out: Vec<ProbeGroup> = Vec::new();
    for r in rows {
        let q = r["probe"].as_str().unwrap_or("").to_string();
        let i = match out.iter().position(|g| g.question == q) {
            Some(i) => i,
            None => {
                out.push(ProbeGroup { question: q, passed: 0, coherence: 0.0, replies: Vec::new() });
                out.len() - 1
            }
        };
        let g = &mut out[i];
        let reply = r["reply"].as_str().filter(|s| !s.is_empty()).unwrap_or("(no words)");
        g.replies.push(reply.to_string());
        g.passed += (r["failed_requirement"].as_bool() == Some(false)) as usize;
        g.coherence += r["coherence"].as_f64().unwrap_or(0.0);
    }
    for g in &mut out {
        g.coherence /= g.replies.len().max(1) as f64;
    }
    out
}
