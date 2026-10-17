# engine.rs

Finds and runs the backend's two C programs.

- **`xks()`** is the decision engine: `MJEV2_XKS` (or `MJEV_XKS`), else this
  checkout's `backend/build/xks`, else `xks` on PATH. The checkout comes
  first: an `xks` on PATH is usually Intel Phi Jev's Rust one.
- **`learner()`** is the learner: `MJEV2_LEARNER`, else
  `backend/build/learner`, else PATH.
- **Starting and stopping.** `ensure` and `serve` start the engine with
  `xks serve --detach` when it is on this machine and does not answer
  (`MJEV_AUTOSTART=0` turns that off); `stop` runs `xks stop`.
- **Logs.** The TUI runs them with their messages in
  `$XDG_RUNTIME_DIR/mjev2/xks.log`; the detached server's own log is
  `$XDG_RUNTIME_DIR/xks/serve.log`.

Adapted from Mechanical Jev's `phi.rs`.
