#!/usr/bin/env bash
# Host-Tests (Linux/WSL, g++): uebersetzt die Tests mit AddressSanitizer und
# UndefinedBehaviorSanitizer und fuehrt sie aus.
#   bash tests/host/run.sh
set -euo pipefail
cd "$(dirname "$0")"
ROOT=../..
OUT=$(mktemp -d)
trap 'rm -rf "$OUT"' EXIT
CXX=${CXX:-g++}
FLAGS="-std=c++17 -g -O1 -fsanitize=address,undefined -fno-sanitize-recover=undefined -Wall -Wno-format -Istubs"

status=0
for t in test_*.cpp; do
  name=${t%.cpp}
  echo "== $name"
  $CXX $FLAGS -I$ROOT/lib/ArduinoSIP -I$ROOT/src "$t" -o "$OUT/$name"
  "$OUT/$name" || status=1
done
exit $status
