# util.c

Growable byte strings (`sb_t`, always NUL-terminated) and int32 arrays
(`ivec_t`, tokens), allocation that aborts on out-of-memory with a message
instead of returning NULL, whole-file reads, a monotonic clock in
milliseconds, `~/` expansion, `mkdir -p`, and `run_dir()`:
`$XDG_RUNTIME_DIR/xks`, the directory the engine keeps `serve.pid` and
`serve.log` in. It is the same directory the Rust xks uses, so either
`xks stop` finds a server started by the other.
