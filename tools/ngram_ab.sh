#!/bin/sh
# ngram_ab.sh ROUNDS WEIGHT...: the probe with the n-gram memory at each
# weight, interleaved, ROUNDS times, against the live learner serve: POST
# /v1/probe {"ngram_weight": W} sets the weight for those probes only, and a
# probe changes nothing, so the arms share one state and the TUI stays up.
# Each run's rows go to probes.jsonl (ngram_weight in each); a line a run
# here: the weight, the replies that passed every requirement, the time.
set -eu
B=${MJ2_BIND:-127.0.0.1:8092}
rounds=$1
shift
r=1
while [ "$r" -le "$rounds" ]; do
  for w in "$@"; do
    t0=$(date +%s)
    out=$(curl -s -X POST -H 'content-type: application/json' -d "{\"ngram_weight\": $w}" "http://$B/v1/probe")
    total=$(printf '%s' "$out" | grep -o '"failed_requirement":[a-z]*' | wc -l)
    passed=$(printf '%s' "$out" | grep -o '"failed_requirement":false' | wc -l)
    echo "round $r weight $w: passed $passed of $total, $(($(date +%s) - t0)) s"
  done
  r=$((r + 1))
done
echo "done"
