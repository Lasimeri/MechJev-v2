#!/bin/sh
# run.sh OUT PORT CMD...: start CMD (an xks server on 127.0.0.1:PORT with
# --debug), POST every line of requests.jsonl to it in order, one response
# per line into OUT, then stop the server. The same order on both engines,
# so the rolling buffer (sequence 0) holds the same session at each step.
set -u
out=$1; port=$2; shift 2
here=$(dirname "$0")
log="$out.log"
XKS_UBATCH=${XKS_UBATCH:-512} "$@" > "$log" 2>&1 &
pid=$!
t0=$(date +%s)
until curl -sf "http://127.0.0.1:$port/health" > /dev/null; do
  if ! kill -0 "$pid" 2>/dev/null; then echo "server exited; see $log"; exit 1; fi
  sleep 1
done
echo "up after $(( $(date +%s) - t0 )) s"
: > "$out"
n=0
while IFS= read -r line; do
  n=$((n + 1))
  s=$(date +%s.%N)
  printf '%s' "$line" | curl -s -X POST -H 'Content-Type: application/json' --data-binary @- "http://127.0.0.1:$port/v1/systemone" >> "$out"
  echo >> "$out"
  e=$(date +%s.%N)
  echo "request $n: $(echo "$e - $s" | bc) s"
done < "$here/requests.jsonl"
kill "$pid"; wait "$pid" 2>/dev/null
echo done
