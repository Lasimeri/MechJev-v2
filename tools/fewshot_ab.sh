#!/bin/sh
# fewshot_ab.sh WEIGHT SHOTS MESSAGE: the paper's few-shot protocol on the
# live learner. Restores the snapshot (tools/fewshot_ab.sh snapshot makes
# it), starts learner serve with the fast memory at WEIGHT and the probes
# in tools/fewshot_probes.txt, probes, then SHOTS times: teaches MESSAGE
# through the watched TUI (tui_say.sh) and probes again. Every probe row
# carries memory_weight and the turns the learner had had.
set -eu
here=$(cd "$(dirname "$0")" && pwd)
D=${MJ2_DATA:-${XDG_DATA_HOME:-$HOME/.local/share}/mechjev-v2}
S=$HOME/.cache/mechjev-v2-fewshot/snapshot
files="learner.store history.json turns.jsonl curves.jsonl probes.jsonl"
stop_serve() {
  p=$(cat "$D/lock" 2>/dev/null || true)
  if [ -n "$p" ] && ps -o args= -p "$p" | grep -q 'learner serve'; then
    kill "$p"
    while kill -0 "$p" 2>/dev/null; do sleep 0.2; done
  fi
}
if [ "${1:-}" = snapshot ]; then
  stop_serve
  mkdir -p "$S"
  for f in $files; do cp -p "$D/$f" "$S/$f"; done
  ls -la "$S"
  exit 0
fi
w=$1 shots=$2 msg=$3
stop_serve
for f in $files; do cp -p "$S/$f" "$D/$f"; done
MJ2_MEMORY=$w MJ2_PROBES=$here/fewshot_probes.txt setsid "$here/../backend/build/learner" serve \
  >> "${XDG_RUNTIME_DIR:-/tmp}/mjev2/learner.log" 2>&1 < /dev/null &
until curl -s -m 2 http://127.0.0.1:8092/health > /dev/null; do sleep 0.5; done
probe() {
  echo "--- weight $w, after $1 shots"
  curl -s -m 1800 -X POST http://127.0.0.1:8092/v1/probe -H 'content-type: application/json' -d '{}' |
    jq -r 'group_by(.probe)[] | "\(.[0].probe): passed \(map(select(.failed_requirement == false)) | length)/\(length) weight \(.[0].memory_weight): \(map(.reply) | join(" | "))"'
}
probe 0
k=1
while [ "$k" -le "$shots" ]; do
  "$here/tui_say.sh" "$msg"
  probe "$k"
  k=$((k + 1))
done
echo "--- arm done"
