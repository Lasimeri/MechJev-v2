//! The learner's HTTP API (`learner serve`, 127.0.0.1:8092 by default,
//! `MJ2_BIND`): `GET /health`, `GET /v1/stats`, `POST /v1/talk {message}`,
//! `POST /v1/hear {text}`. Started on demand like the engine, in its own
//! process group, its output in `$XDG_RUNTIME_DIR/mjev2/learner.log`. See
//! learner.md.

use std::os::unix::process::CommandExt;
use std::path::PathBuf;
use std::process::{Command, Stdio};
use std::time::{Duration, Instant};

use serde_json::{json, Value};

/// The learner's address.
pub fn bind() -> String {
    std::env::var("MJ2_BIND")
        .ok()
        .filter(|v| !v.trim().is_empty())
        .unwrap_or_else(|| "127.0.0.1:8092".into())
}

/// Where a learner the front end started writes its output.
pub fn log_path() -> PathBuf {
    std::env::var_os("XDG_RUNTIME_DIR")
        .map(PathBuf::from)
        .unwrap_or_else(std::env::temp_dir)
        .join("mjev2")
        .join("learner.log")
}

pub struct Learner {
    pub base: String,
    agent: ureq::Agent,
}

impl Learner {
    pub fn new() -> Self {
        Self {
            base: format!("http://{}", bind()),
            // A turn is many decisions of the 35B: minutes, not seconds.
            agent: ureq::AgentBuilder::new()
                .timeout_connect(Duration::from_secs(2))
                .timeout_read(Duration::from_secs(3600))
                .build(),
        }
    }

    pub fn healthy(&self) -> bool {
        self.agent
            .get(&format!("{}/health", self.base))
            .timeout(Duration::from_secs(2))
            .call()
            .is_ok()
    }

    fn get(&self, path: &str) -> Result<Value, String> {
        self.agent
            .get(&format!("{}{path}", self.base))
            .call()
            .map_err(|e| self.error(e))?
            .into_json()
            .map_err(|e| e.to_string())
    }

    fn post(&self, path: &str, body: Value) -> Result<Value, String> {
        self.agent
            .post(&format!("{}{path}", self.base))
            .send_json(body)
            .map_err(|e| self.error(e))?
            .into_json()
            .map_err(|e| e.to_string())
    }

    fn error(&self, e: ureq::Error) -> String {
        match e {
            ureq::Error::Status(code, r) => {
                let v: Value = r.into_json().unwrap_or(Value::Null);
                let m = v["message"].as_str().unwrap_or("(no message)");
                format!("the learner said {code}: {m}")
            }
            ureq::Error::Transport(t) => format!("the learner at {} does not answer: {t}", self.base),
        }
    }

    pub fn stats(&self) -> Result<Value, String> {
        self.get("/v1/stats")
    }

    /// One turn: the person's message in, the turn's record out (the reply,
    /// every word with its options and score, the measures).
    pub fn talk(&self, message: &str) -> Result<Value, String> {
        self.post("/v1/talk", json!({ "message": message }))
    }

    pub fn hear(&self, text: &str) -> Result<Value, String> {
        self.post("/v1/hear", json!({ "text": text }))
    }

    /// Start `learner serve` unless it answers. It refuses to start while
    /// another learner (a `learner talk` on a terminal) holds the data; its
    /// own message says so, and is returned.
    pub fn ensure(&self) -> Result<(), String> {
        if self.healthy() {
            return Ok(());
        }
        let bin = crate::engine::learner();
        if !bin.is_file() {
            return Err(crate::engine::not_built(&bin));
        }
        let log = log_path();
        if let Some(d) = log.parent() {
            std::fs::create_dir_all(d).map_err(|e| format!("{}: {e}", d.display()))?;
        }
        let out = std::fs::File::create(&log).map_err(|e| format!("{}: {e}", log.display()))?;
        let err = out.try_clone().map_err(|e| e.to_string())?;
        let mut child = Command::new(&bin)
            .args(["serve", "--bind", &bind()])
            .stdin(Stdio::null())
            .stdout(out)
            .stderr(err)
            .process_group(0)
            .spawn()
            .map_err(|e| format!("{}: {e}", bin.display()))?;
        let t0 = Instant::now();
        while t0.elapsed() < Duration::from_secs(20) {
            if let Ok(Some(status)) = child.try_wait() {
                let said = std::fs::read_to_string(&log).unwrap_or_default();
                return Err(format!(
                    "the learner did not start ({status}): {}",
                    said.lines().last().unwrap_or("").trim_start_matches("error: ")
                ));
            }
            if self.healthy() {
                // Reaped when it ends, so it never lingers as a zombie of
                // this process (seen 2026-10-17 after a learner restart).
                std::thread::spawn(move || {
                    let _ = child.wait();
                });
                return Ok(());
            }
            std::thread::sleep(Duration::from_millis(200));
        }
        Err(format!("the learner did not answer in 20 s; log {}", log.display()))
    }
}

impl Default for Learner {
    fn default() -> Self {
        Self::new()
    }
}
