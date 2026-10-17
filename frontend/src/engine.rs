//! The two C programs of MechJev-v2's backend, found and run for `mjev2`:
//! the decision engine (`xks`, started on demand with `xks serve --detach`,
//! stopped with `xks stop`) and the learner (`learner serve`, its HTTP API
//! on 127.0.0.1:8092). Adapted from Mechanical Jev's `phi.rs`. See
//! engine.md.

use std::path::{Path, PathBuf};
use std::process::{Command, Stdio};

use crate::client::Client;

/// A backend program: `MJEV2_<NAME>` (or `MJEV_XKS` for the engine), else
/// this checkout's `backend/build/<name>`, else on PATH. The checkout comes
/// before PATH: an `xks` on PATH is usually Intel Phi Jev's Rust one.
fn program(name: &str, env: &[&str]) -> PathBuf {
    for e in env {
        if let Some(p) = std::env::var_os(e) {
            return PathBuf::from(p);
        }
    }
    let here = Path::new(env!("CARGO_MANIFEST_DIR"));
    let built = here
        .parent()
        .map(|r| r.join("backend/build").join(name))
        .filter(|p| p.is_file());
    built
        .or_else(|| on_path(name))
        .unwrap_or_else(|| here.with_file_name("backend/build").join(name))
}

/// The decision engine's binary.
pub fn xks() -> PathBuf {
    program("xks", &["MJEV2_XKS", "MJEV_XKS"])
}

/// The learner's binary.
pub fn learner() -> PathBuf {
    program("learner", &["MJEV2_LEARNER"])
}

/// An executable `name` on PATH.
pub fn on_path(name: &str) -> Option<PathBuf> {
    use std::os::unix::fs::PermissionsExt;
    std::env::split_paths(&std::env::var_os("PATH")?)
        .map(|d| d.join(name))
        .find(|p| {
            p.metadata()
                .is_ok_and(|m| m.is_file() && m.permissions().mode() & 0o111 != 0)
        })
}

/// `host:port` when `base` is a server on this machine (only those are
/// started on demand).
pub fn local_bind(base: &str) -> Option<String> {
    let rest = base.strip_prefix("http://")?;
    let hostport = rest.split('/').next()?;
    let host = hostport.split(':').next()?;
    matches!(host, "127.0.0.1" | "localhost").then(|| hostport.to_string())
}

/// Start the engine if it is on this machine and does not answer.
pub fn ensure(client: &Client) -> Result<(), String> {
    ensure_as(client, Say::Terminal).map(drop)
}

/// `ensure`, saying what it does where `say` says; the log's last lines
/// when it started one.
pub fn ensure_as(client: &Client, say: Say) -> Result<String, String> {
    let Some(bind) = local_bind(&client.base) else {
        return Ok(String::new());
    };
    if client.healthy() {
        return Ok(String::new());
    }
    if std::env::var("MJEV_AUTOSTART").as_deref() == Ok("0") {
        return Err(format!(
            "{} does not answer (MJEV_AUTOSTART=0; start it with mjev2 serve)",
            client.base
        ));
    }
    start_as(&bind, say)
}

/// `mjev2 serve`: start the engine unless it answers already.
pub fn serve(client: &Client) -> Result<(), String> {
    let Some(bind) = local_bind(&client.base) else {
        return Err(format!(
            "{} is not this machine; nothing to start",
            client.base
        ));
    };
    if client.healthy() {
        eprintln!("mjev2: {} already answers", client.base);
        return Ok(());
    }
    start(&bind)
}

/// Where the engine's messages go: the terminal, or a log the TUI reads.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum Say {
    Terminal,
    Log,
}

/// `$XDG_RUNTIME_DIR/mjev2/xks.log`: what `xks` said when the TUI ran it.
pub fn log_path() -> PathBuf {
    std::env::var_os("XDG_RUNTIME_DIR")
        .map(PathBuf::from)
        .unwrap_or_else(std::env::temp_dir)
        .join("mjev2")
        .join("xks.log")
}

/// The detached engine's own log, which `xks serve --detach` writes.
pub fn serve_log() -> PathBuf {
    std::env::var_os("XDG_RUNTIME_DIR")
        .map(PathBuf::from)
        .unwrap_or_else(std::env::temp_dir)
        .join("xks")
        .join("serve.log")
}

fn log_tail(n: usize) -> String {
    let text = std::fs::read_to_string(log_path()).unwrap_or_default();
    let lines: Vec<&str> = text.lines().filter(|l| !l.trim().is_empty()).collect();
    lines[lines.len().saturating_sub(n)..].join("\n")
}

/// `xks` with `args`, its output where `say` says. A file, not a pipe, for
/// the log: `status` returns when `xks` does, even if something it
/// started keeps an inherited descriptor open.
fn run_xks(args: &[&str], say: Say) -> Result<bool, String> {
    let bin = xks();
    let mut cmd = Command::new(&bin);
    cmd.args(args).stdin(Stdio::null());
    match say {
        Say::Terminal => {
            cmd.stdout(Stdio::from(std::io::stderr()));
        }
        Say::Log => {
            let p = log_path();
            if let Some(d) = p.parent() {
                std::fs::create_dir_all(d).map_err(|e| format!("{}: {e}", d.display()))?;
            }
            let out = std::fs::File::create(&p).map_err(|e| format!("{}: {e}", p.display()))?;
            let err = out.try_clone().map_err(|e| e.to_string())?;
            cmd.stdout(out).stderr(err);
        }
    }
    cmd.status()
        .map(|s| s.success())
        .map_err(|e| format!("{}: {e}", bin.display()))
}

fn with_tail(msg: &str, say: Say) -> String {
    match say {
        Say::Terminal => msg.into(),
        Say::Log => format!("{msg}\n{}", log_tail(6)),
    }
}

/// Start the engine on `bind` (`host:port`).
pub fn start(bind: &str) -> Result<(), String> {
    start_as(bind, Say::Terminal).map(drop)
}

/// `start`, with `xks`'s messages sent where `say` says.
pub fn start_as(bind: &str, say: Say) -> Result<String, String> {
    let bin = xks();
    if !bin.is_file() {
        return Err(not_built(&bin));
    }
    if say == Say::Terminal {
        eprintln!("mjev2: starting the decision engine on {bind} (loads the 35B; a minute or so)");
    }
    if run_xks(&["serve", "--detach", "--bind", bind], say)? {
        Ok(if say == Say::Log { log_tail(6) } else { String::new() })
    } else {
        Err(with_tail(
            "the decision engine did not start (its log: $XDG_RUNTIME_DIR/xks/serve.log)",
            say,
        ))
    }
}

/// What to do when a backend binary is missing.
pub fn not_built(bin: &Path) -> String {
    let checkout = Path::new(env!("CARGO_MANIFEST_DIR"))
        .parent()
        .map_or_else(|| "MechJev-v2".into(), |p| p.display().to_string());
    format!(
        "{} is not built (cd \"{checkout}\" && make), or set MJEV2_XKS / MJEV2_LEARNER",
        bin.display()
    )
}

/// Stop the detached engine.
pub fn stop() -> Result<(), String> {
    let bin = xks();
    let status = Command::new(&bin)
        .arg("stop")
        .status()
        .map_err(|e| format!("{}: {e}", bin.display()))?;
    if status.success() {
        Ok(())
    } else {
        Err("xks stop failed".into())
    }
}

/// `stop`, with `xks`'s messages sent where `say` says.
pub fn stop_as(say: Say) -> Result<String, String> {
    if say == Say::Terminal {
        return stop().map(|_| String::new());
    }
    if run_xks(&["stop"], say)? {
        Ok(log_tail(6))
    } else {
        Err(with_tail("xks stop failed", say))
    }
}

#[cfg(test)]
mod tests {
    #[test]
    fn only_this_machine_is_started() {
        assert_eq!(
            super::local_bind("http://127.0.0.1:8090"),
            Some("127.0.0.1:8090".into())
        );
        assert_eq!(super::local_bind("https://api.typesafe.ai"), None);
    }

    #[test]
    fn the_checkout_comes_before_path() {
        // MechJev-v2's own build, when it is there, is what runs: an `xks`
        // on PATH is usually Intel Phi Jev's.
        let built = std::path::Path::new(env!("CARGO_MANIFEST_DIR"))
            .parent()
            .unwrap()
            .join("backend/build/xks");
        if built.is_file() && std::env::var_os("MJEV2_XKS").is_none() && std::env::var_os("MJEV_XKS").is_none()
        {
            assert_eq!(super::xks(), built);
        }
    }
}
