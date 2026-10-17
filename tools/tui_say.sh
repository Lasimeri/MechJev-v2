#!/bin/sh
# tui_say.sh MESSAGE...: say each message through the TUI's talk screen (the
# tmux pane MJEV2_PANE, default jev2.0: `make watch` opens it), waiting for
# each turn in the learner's turns.jsonl, and print it with every word's
# per-token score and the measures. One driver at a time: two typing into
# the TUI merge their messages (seen 2026-10-17), so this holds a lock.
set -u
T=${MJEV2_PANE:-jev2.0}
exec 9> "${XDG_RUNTIME_DIR:-/tmp}/mjev2-driver.lock"
flock -n 9 || { echo "another driver is typing into the TUI"; exit 1; }
D=${MJ2_DATA:-${XDG_DATA_HOME:-$HOME/.local/share}/mechjev-v2}
F=$D/turns.jsonl
for m in "$@"; do
  n=$(wc -l < "$F" 2>/dev/null || echo 0)
  # a person scrolling the window leaves the pane in copy mode
  [ "$(tmux display -p -t "$T" '#{pane_in_mode}')" = 1 ] && tmux send-keys -t "$T" -X cancel
  tmux send-keys -t "$T" -l "$m"
  tmux send-keys -t "$T" Enter
  t=0
  until [ "$(wc -l < "$F" 2>/dev/null || echo 0)" -gt "$n" ] || [ $t -ge 900 ]; do sleep 3; t=$((t+3)); done
  tail -1 "$F" | jq -r '"you: \(.person)\nit:  \(.reply)   [\([.trace[] | select(.reward != null) | "\(.pick) \(.reward*100|round/100)"] | join(", "))]  coherence \(.coherence_final) agree \(.eval.agree) (without variables \(.eval_no_variables.agree), without the word before \(.eval_no_prev_word.agree)) requirements \(if .failed_requirement then "failed" else "passed" end) \((.ms_per_word/1000)|floor) s a word"'
done
