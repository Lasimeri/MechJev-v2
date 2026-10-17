#!/bin/sh
# Gate 1, the rest: the copy's exactness (forked against split, one fork per
# round) on three requests, then Mechanical Jev's closeness (Jev's 28
# published questions) through the C engine started with serve --detach.
set -u
X=~/MechJev-v2/backend/build/xks
export XKS_SYSTEM_FILE= XKS_GPU_LAYERS=50%
for n in 1 3 7; do
  sed -n "${n}p" requests.jsonl > /tmp/claude-1000/-home-lasimeri/61db23af-45f6-4a9e-958a-1ea227ab44e4/scratchpad/poly.json
  echo "polygraph request $n:"
  XKS_FORKS=1 $X polygraph --file /tmp/claude-1000/-home-lasimeri/61db23af-45f6-4a9e-958a-1ea227ab44e4/scratchpad/poly.json 2>/dev/null
done
cd ~/"Mechanical Jev" && MJEV_XKS=$X make closeness 2>&1 | tail -25
