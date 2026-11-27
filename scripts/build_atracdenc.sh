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
cp "$ROOT/scripts/atracdenc_pcm_io_simple.cpp" "$SRC/src/pcm_io_simple.cpp"

python3 - "$SRC/src/CMakeLists.txt" <<'PY'
from pathlib import Path
import sys

path = Path(sys.argv[1])
text = path.read_text()
text = text.replace(
    'set_property(CACHE ATRACDENC_PCM_IO_BACKEND PROPERTY STRINGS auto mediafoundation libsndfile)',
    'set_property(CACHE ATRACDENC_PCM_IO_BACKEND PROPERTY STRINGS auto mediafoundation libsndfile simplewav)'
)
text = text.replace(
    'elseif (NOT ATRACDENC_PCM_IO_BACKEND STREQUAL "mediafoundation" AND\n        NOT ATRACDENC_PCM_IO_BACKEND STREQUAL "libsndfile")',
    'elseif (NOT ATRACDENC_PCM_IO_BACKEND STREQUAL "mediafoundation" AND\n        NOT ATRACDENC_PCM_IO_BACKEND STREQUAL "libsndfile" AND\n        NOT ATRACDENC_PCM_IO_BACKEND STREQUAL "simplewav")'
)
old = '''else()
    INCLUDE(FindLibSndFile)
    if (NOT LIBSNDFILE_FOUND)
        message(FATAL_ERROR "libsndfile PCM I/O backend requires libsndfile")
    endif()
    include_directories(${LIBSNDFILE_INCLUDE_DIR})
    set(PCM_IO_LIBRARIES ${SNDFILE_LIBRARIES})
    set(SOURCE_PCM_IO_LIB
        pcm_io_sndfile.cpp
    )
endif()'''
new = '''elseif (ATRACDENC_PCM_IO_BACKEND STREQUAL "libsndfile")
    INCLUDE(FindLibSndFile)
    if (NOT LIBSNDFILE_FOUND)
        message(FATAL_ERROR "libsndfile PCM I/O backend requires libsndfile")
    endif()
    include_directories(${LIBSNDFILE_INCLUDE_DIR})
    set(PCM_IO_LIBRARIES ${SNDFILE_LIBRARIES})
    set(SOURCE_PCM_IO_LIB
        pcm_io_sndfile.cpp
    )
else()
    set(SOURCE_PCM_IO_LIB
        pcm_io_simple.cpp
    )
endif()'''
if old not in text:
    raise SystemExit("Could not locate libsndfile backend block in ATRACDEnc CMakeLists.txt")
text = text.replace(old, new)
path.write_text(text)
PY

cmake -S "$SRC" -B "$SRC/build" \
  -DCMAKE_BUILD_TYPE=Release \
  -DATRACDENC_PCM_IO_BACKEND=simplewav
cmake --build "$SRC/build" --target atracdenc -j2

mkdir -p "$(dirname "$OUT")"
cp "$SRC/build/src/atracdenc" "$OUT"
chmod +x "$OUT"
cp "$SRC/LICENSE" "${OUT}.LICENSE"
tar -czf "${OUT}-source.tar.gz" --exclude=./.git --exclude=./build -C "$SRC" .
printf 'Built patched ATRACDEnc %s with 744-byte ATRAC3+ frame-size support.\n' "$COMMIT"
printf 'Binary: %s\n' "$OUT"
printf 'Patched source bundle: %s-source.tar.gz\n' "$OUT"
