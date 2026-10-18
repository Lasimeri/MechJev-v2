//! `mjev2 mcp`: an MCP server over stdio, so an agent uses MechJev-v2 the
//! way a person does. The TUI runs in a tmux pane (`MJEV2_PANE`, default
//! `jev2.0`, the window a person watches): `screen` reads it, `type` and
//! `keys` type into it. `talk`, `hear` and `stats` go to the learner's API
//! (a turn said here shows in the TUI too, which reads the learner's
//! turns), `judge` straight to the decision engine.
//!
//! JSON-RPC 2.0, one message per line: `initialize`, `ping`, `tools/list`,
//! `tools/call`; notifications are read and ignored. See mcp.md.

use std::io::{self, BufRead, Write};
use std::process::Command;

use serde_json::{json, Value};

use crate::client::Client;
use crate::learner::Learner;
use crate::protocol::Request;

const PROTOCOL_VERSION: &str = "2025-06-18";

fn pane() -> String {
    std::env::var("MJEV2_PANE")
        .ok()
        .filter(|v| !v.trim().is_empty())
        .unwrap_or_else(|| "jev2.0".into())
}

fn tools() -> Value {
    json!([
        {"name": "screen", "description": "The TUI's screen as text: what the person watching sees.",
         "inputSchema": {"type": "object", "properties": {}}},
        {"name": "type", "description": "Type text into the TUI (the focused field; on the talk screen, the line to say). Add {\"enter\": true} to press Enter after it.",
         "inputSchema": {"type": "object", "properties": {"text": {"type": "string"}, "enter": {"type": "boolean"}}, "required": ["text"]}},
        {"name": "keys", "description": "Press keys in the TUI, tmux names: Enter, Escape, Tab, Up, Down, PageUp, PageDown, F1, F5, C-u, t (the talk screen from home).",
         "inputSchema": {"type": "object", "properties": {"keys": {"type": "array", "items": {"type": "string"}}}, "required": ["keys"]}},
        {"name": "talk", "description": "Say a message to the learner and wait for its reply (minutes: every word is a decision of the 35B). Returns the reply, each word with its options, per-token score and seeds, and the measures. The TUI shows the turn as well.",
         "inputSchema": {"type": "object", "properties": {"message": {"type": "string"}, "alone": {"type": "boolean", "description": "the circuit speaks by itself: no word chosen by the engine, nothing learned"}}, "required": ["message"]}},
        {"name": "hear", "description": "Let the learner read English text (its words counted; nothing else is learned from hearing).",
         "inputSchema": {"type": "object", "properties": {"text": {"type": "string"}}, "required": ["text"]}},
        {"name": "probe", "description": "The acquisition curve: the fixed probe questions (probes.txt) put to the circuit alone from an empty conversation; the learner hears, learns and counts nothing. One row each (reply, coherence, requirements), also kept in probes.jsonl.",
         "inputSchema": {"type": "object", "properties": {}}},
        {"name": "teach", "description": "Turns up the teacher's ladder: Qwen3.8-35B teaching MechJev from outside (backend/build/teacher), the stages of lessons.txt in order, each harder than the last (greetings first, then reasons, two-part questions, describing, explaining). Each turn Qwen writes a new line and types it in the TUI (labelled qwen): a check of MechJev alone when one is due, else a taught turn (the engine choosing among MechJev's own words); 2 of a stage's last 3 checks passed move it up. measure: MechJev alone on the frozen test lines. About a minute or two a turn; rows in lessons.jsonl; the teacher's printed output returned.",
         "inputSchema": {"type": "object", "properties": {"turns": {"type": "integer", "description": "how many turns on the ladder (1)"}, "measure": {"type": "boolean", "description": "measure MechJev on the frozen test lines (teacher freeze first); teach nothing"}, "stage": {"type": "string", "description": "move the ladder to this stage first (a name in lessons.txt)"}}}},
        {"name": "stats", "description": "What the learner knows: lexicon, speakable words, variable slots, circuit, turns, replay, most used words.",
         "inputSchema": {"type": "object", "properties": {}}},
        {"name": "judge", "description": "Typed questions to the decision engine (System One: noul, choice, score) about a state; calibrated probabilities back, no text.",
         "inputSchema": {"type": "object", "properties": {"state": {}, "questions": {"type": "object"}}, "required": ["state", "questions"]}}
    ])
}

/// tmux with `args`, its output or its error.
fn tmux(args: &[&str]) -> Result<String, String> {
    let out = Command::new("tmux")
        .args(args)
        .output()
        .map_err(|e| format!("tmux: {e}"))?;
    if out.status.success() {
        Ok(String::from_utf8_lossy(&out.stdout).into_owned())
    } else {
        Err(format!(
            "tmux {}: {} (is the TUI's pane {} up? make watch)",
            args.first().unwrap_or(&""),
            String::from_utf8_lossy(&out.stderr).trim(),
            pane()
        ))
    }
}

/// A person scrolling the window puts the pane in copy mode, where typed
/// keys go to copy mode instead of the TUI: leave it first.
fn leave_copy_mode(p: &str) {
    if tmux(&["display", "-p", "-t", p, "#{pane_in_mode}"]).is_ok_and(|m| m.trim() == "1") {
        let _ = tmux(&["send-keys", "-t", p, "-X", "cancel"]);
    }
}

fn call(name: &str, args: &Value) -> Result<Value, String> {
    let p = pane();
    match name {
        "screen" => Ok(json!(tmux(&["capture-pane", "-t", &p, "-p"])?)),
        "type" => {
            let text = args["text"].as_str().ok_or("text is a string")?;
            leave_copy_mode(&p);
            tmux(&["send-keys", "-t", &p, "-l", text])?;
            if args["enter"].as_bool() == Some(true) {
                tmux(&["send-keys", "-t", &p, "Enter"])?;
            }
            Ok(json!("typed"))
        }
        "keys" => {
            let keys: Vec<String> = args["keys"]
                .as_array()
                .ok_or("keys is an array of tmux key names")?
                .iter()
                .filter_map(|k| k.as_str().map(String::from))
                .collect();
            leave_copy_mode(&p);
            let mut a: Vec<&str> = vec!["send-keys", "-t", &p];
            a.extend(keys.iter().map(String::as_str));
            tmux(&a)?;
            Ok(json!("pressed"))
        }
        "talk" => {
            let m = args["message"].as_str().ok_or("message is a string")?;
            let client = Client::from_env();
            if !client.healthy() {
                crate::engine::ensure(&client)?;
            }
            let l = Learner::new();
            l.ensure()?;
            l.talk_as(m, args["alone"].as_bool() == Some(true))
        }
        "hear" => {
            let t = args["text"].as_str().ok_or("text is a string")?;
            let l = Learner::new();
            l.ensure()?;
            l.hear(t)
        }
        "probe" => {
            let l = Learner::new();
            l.ensure()?;
            l.probe()
        }
        "teach" => {
            /* the teacher program: Qwen teaching MechJev from outside */
            let client = Client::from_env();
            if !client.healthy() {
                crate::engine::ensure(&client)?;
            }
            Learner::new().ensure()?;
            let run = |a: &[&str]| std::process::Command::new(crate::engine::teacher()).args(a).output();
            let mut output = String::new();
            let mut errors = String::new();
            if let Some(name) = args["stage"].as_str() {
                let st = run(&["stage", name]).map_err(|e| e.to_string())?;
                output.push_str(&String::from_utf8_lossy(&st.stdout));
                errors.push_str(&String::from_utf8_lossy(&st.stderr));
                if !st.status.success() {
                    return Ok(json!({"ok": false, "output": output, "errors": errors}));
                }
            }
            let n = args["turns"].as_u64().unwrap_or(1).to_string();
            let out = if args["measure"].as_bool() == Some(true) { run(&["measure"]) } else { run(&["teach", &n]) }
                .map_err(|e| e.to_string())?;
            output.push_str(&String::from_utf8_lossy(&out.stdout));
            errors.push_str(&String::from_utf8_lossy(&out.stderr));
            Ok(json!({"ok": out.status.success(), "output": output, "errors": errors}))
        }
        "stats" => {
            let l = Learner::new();
            l.ensure()?;
            l.stats()
        }
        "judge" => {
            let req: Request = serde_json::from_value(args.clone()).map_err(|e| format!("invalid request: {e}"))?;
            let client = Client::from_env();
            crate::engine::ensure(&client)?;
            client
                .system_one(&req)
                .map(|(r, _)| serde_json::to_value(r).unwrap_or(Value::Null))
                .map_err(|e| e.to_string())
        }
        _ => Err(format!("unknown tool: {name}")),
    }
}

pub fn serve() -> io::Result<()> {
    let stdin = io::stdin();
    let mut out = io::stdout().lock();
    for line in stdin.lock().lines() {
        let line = line?;
        if line.trim().is_empty() {
            continue;
        }
        let msg: Value = match serde_json::from_str(&line) {
            Ok(v) => v,
            Err(e) => {
                write_msg(&mut out, &error(Value::Null, -32700, &format!("parse error: {e}")))?;
                continue;
            }
        };
        let Some(id) = msg.get("id").cloned() else {
            continue; // a notification
        };
        let method = msg.get("method").and_then(Value::as_str).unwrap_or("");
        let params = msg.get("params").cloned().unwrap_or(Value::Null);
        let reply = match method {
            "initialize" => result(
                id,
                json!({
                    "protocolVersion": params.get("protocolVersion").cloned().unwrap_or(json!(PROTOCOL_VERSION)),
                    "capabilities": {"tools": {"listChanged": false}},
                    "serverInfo": {"name": "mjev2", "version": env!("CARGO_PKG_VERSION")},
                    "instructions": "MechJev-v2: a decision engine (the 35B's typed readout) and a language learner it teaches. `screen`, `type` and `keys` drive the TUI a person watches; `talk` says something to the learner and returns its reply with every word's score."
                }),
            ),
            "ping" => result(id, json!({})),
            "tools/list" => result(id, json!({ "tools": tools() })),
            "tools/call" => {
                let name = params.get("name").and_then(Value::as_str).unwrap_or("");
                let args = params.get("arguments").cloned().unwrap_or(json!({}));
                match call(name, &args) {
                    Ok(v) => {
                        let text = match &v {
                            Value::String(s) => s.clone(),
                            other => serde_json::to_string_pretty(other).unwrap_or_default(),
                        };
                        result(id, json!({"content": [{"type": "text", "text": text}], "isError": false}))
                    }
                    Err(e) => result(id, json!({"content": [{"type": "text", "text": e}], "isError": true})),
                }
            }
            _ => error(id, -32601, &format!("method not found: {method}")),
        };
        write_msg(&mut out, &reply)?;
    }
    Ok(())
}

fn result(id: Value, r: Value) -> Value {
    json!({"jsonrpc": "2.0", "id": id, "result": r})
}

fn error(id: Value, code: i64, msg: &str) -> Value {
    json!({"jsonrpc": "2.0", "id": id, "error": {"code": code, "message": msg}})
}

fn write_msg(out: &mut impl Write, v: &Value) -> io::Result<()> {
    out.write_all(v.to_string().as_bytes())?;
    out.write_all(b"\n")?;
    out.flush()
}
