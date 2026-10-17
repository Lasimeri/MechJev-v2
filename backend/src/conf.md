# conf.c

Defaults from files, as Intel Phi Jev's `config.rs` reads them. The first
to set a key wins:
1. the environment;
2. `$XKS_CONFIG`;
3. `<checkout>/xks.local.conf` (not tracked);
4. `~/.config/xks/xks.conf` (shared with Intel Phi Jev's xks);
5. `<checkout>/xks.conf`.

Values are read as a shell reads them: quotes, `~/`, `$HOME` and
`${HOME}`, a trailing comment. The checkout is the first parent of the
binary that holds `xks.conf`.
