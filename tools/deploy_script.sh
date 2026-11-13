#!/usr/bin/env bash
# Put the play script the phone should run into iCloud Drive -> "Retro Bowl mods"/rbmod_script.txt.
# It is the Hail Mary script with the step 1 script below it (the combination the checker passes).
# From there, move it into the game's RetroBowl folder in the Files app.
set -euo pipefail
cd "$(dirname "$0")/.."
out="${RB_ICLOUD:-$HOME/iCloudDrive/Retro Bowl mods}"
mkdir -p "$out"
# refuse to ship a script the game would reject (checker errors or over the block/variable limits)
node="${NODE:-C:/Program Files/nodejs/node.exe}"
check=$("$node" tools/check_script.js examples/step1_script.txt 2>&1 || true)
echo "$check"
if ! echo "$check" | grep -q "^deployed (Hail Mary + step 1): loads" || echo "$check" | grep -q "OVER THE LIMIT"; then
  echo "NOT deployed: fix the script first" >&2; exit 1
fi
{ cat examples/rbmod_script.txt; printf '\n\n'; cat examples/step1_script.txt; } > "$out/rbmod_script.txt"
echo "deployed $(wc -c < "$out/rbmod_script.txt") bytes to $out/rbmod_script.txt"
