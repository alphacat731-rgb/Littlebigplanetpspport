#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
OUT="${1:-$ROOT/build/atracdenc}"
SRC="${RUNNER_TEMP:-/tmp}/lbppsp_atracdenc"
COMMIT="cb170709f836cb43dc0312455188c791159d81dc"

rm -rf "$SRC"
git clone https://github.com/dcherednik/atracdenc.git "$SRC"
git -C "$SRC" checkout "$COMMIT"
git -C "$SRC" submodule update --init --recursive
git -C "$SRC" apply "$ROOT/scripts/atracdenc-744.patch"

cmake -S "$SRC" -B "$SRC/build" \
  -DCMAKE_BUILD_TYPE=Release \
  -DATRACDENC_PCM_IO_BACKEND=libsndfile
cmake --build "$SRC/build" --target atracdenc -j2

mkdir -p "$(dirname "$OUT")"
cp "$SRC/build/src/atracdenc" "$OUT"
chmod +x "$OUT"
cp "$SRC/LICENSE" "${OUT}.LICENSE"
printf 'Built patched ATRACDEnc %s with 744-byte ATRAC3+ frame-size support.\n' "$COMMIT"
printf 'Binary: %s\n' "$OUT"
