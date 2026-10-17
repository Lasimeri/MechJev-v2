#!/bin/sh
# arms_ab.sh ROUNDS NAME=BINARY:DATA_DIR...: the probe with each learner
# binary on its own data, interleaved, ROUNDS times (a probe changes
# nothing). Each arm's learner serve must be stopped (the probe takes the
# writer lock).
set -eu
# every binary judges by the checkout's files, wherever it sits (a binary
# outside the checkout found no restrictions.txt and passed everything)
repo=$(cd "$(dirname "$0")/.." && pwd)
export MJ2_CHECKS="${MJ2_CHECKS:-$repo/restrictions.txt}" MJ2_VARS="${MJ2_VARS:-$repo/variables.txt}"
rounds=$1
shift
r=1
while [ "$r" -le "$rounds" ]; do
  for arm in "$@"; do
    name=${arm%%=*} rest=${arm#*=}
    bin=${rest%%:*} data=${rest#*:}
    echo "--- round $r $name"
    MJ2_DATA=$data "$bin" probe 2>&1
  done
  r=$((r + 1))
done
echo "--- done"
