#!/bin/sh
# harm_ab.sh ROUNDS WEIGHT...: the probe with the fast memory at each weight,
# interleaved, ROUNDS times (a probe changes nothing, so the arms share one
# store). learner serve must be stopped: the probe takes the writer lock.
set -eu
# every binary judges by the checkout's files, wherever it sits (a binary
# outside the checkout found no restrictions.txt and passed everything)
repo=$(cd "$(dirname "$0")/.." && pwd)
export MJ2_CHECKS="${MJ2_CHECKS:-$repo/restrictions.txt}" MJ2_VARS="${MJ2_VARS:-$repo/variables.txt}"
L=$(dirname "$0")/../backend/build/learner
rounds=$1
shift
r=1
while [ "$r" -le "$rounds" ]; do
  for w in "$@"; do
    echo "--- round $r weight $w"
    MJ2_MEMORY=$w "$L" probe 2>&1
  done
  r=$((r + 1))
done
echo "--- done"
