# main.c

The `xks` command. Its command line is the one Mechanical Jev consumes from
Intel Phi Jev:
- **`serve [--bind] [--kill-date] [--debug]`** routes `GET /`,
  `GET /health` (`status`, `subject`, `site`, `cards`, `kill_date_s`,
  `idle_s`), `GET /v1/models`, `POST /v1/systemone` and `POST /v1/generate`
  (below). A wrong method gets
  405 with `Allow`; a refusal gets 422; an engine failure gets 502.
- **`serve --detach`** re-runs the command line without `--detach` in its
  own process group, its output going to `$XDG_RUNTIME_DIR/xks/serve.log`.
  While it waits for `/health` it relays the `xks:` lines, then prints the
  URL.
- **`stop`** ends the process group named in `serve.pid` (SIGTERM, then
  SIGKILL after 30 s), but only if that pid is still an `xks ... serve`.
- **`query`** sends a request to the server at `XKS_BIND` when one
  answers; with `--local`, it runs in this process instead.
- **`polygraph`** reads every fingerprint forked and split (the copy's
  exactness check).
- **`doctor`** reports whether this checkout is ready: exit 0 ready, 1 not.

The kill date counts only questions. `/health` polls do not keep a server
alive.

**`POST /v1/generate {system, user, max_tokens?, temperature?, top_k?,
top_p?, min_p?, repeat_penalty?, seed?}`** answers `{text, tokens, ms}`:
text written by the subject, for the teacher's lines only (teach.md).
MechJev's replies are never generated: they are its own words, chosen by
decisions. The chat is rendered as ChatML with thinking off (a subject on
another template gets 422); `max_tokens` is 64 by default, 512 at most; the
sampling settings default to the user's (engine.md). It counts as a
question for the kill date.
