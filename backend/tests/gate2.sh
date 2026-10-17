#!/bin/sh
# Gate 2: a fresh learner, eight short English turns through the running
# engine (make serve), per-word cost and the first acquisition numbers in
# $MJ2_DATA/curves.jsonl.
set -u
export MJ2_DATA=${MJ2_DATA:-$HOME/.cache/mechjev-v2-gate2}
L=$(dirname "$0")/../build/learner
$L reset --yes
while IFS= read -r line; do
  echo "you> $line"
  $L talk "$line"
done <<'TURNS'
Hello. How are you today?
I am fine, thank you. How are you?
Hello again. I am happy today. Are you happy?
Yes, I am happy too. What do you like?
I like music. Do you like music?
I like music and I like talking with you.
What is your favorite thing to do today?
Thank you for talking with me today. Goodbye.
TURNS
