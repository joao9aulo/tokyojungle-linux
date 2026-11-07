#!/usr/bin/env bash
# Tokyo Jungle Recompiled (Linux) — build & run helper.
#
# Builds and runs the headless port. Prerequisites (see README.md):
#   cmake, ninja, gcc/g++ 12+, SDL2 dev, and a decrypted input/EBOOT.ELF
#   plus the generated/ppu_recomp_*.cpp from the analysis pipeline.
#
# Usage:
#   ./pandacode-run.sh            # run the already-built binary
#   ./pandacode-run.sh build      # configure + build, then run
#   ./pandacode-run.sh quick      # run with a 20 s cap and bounded output
set -euo pipefail
cd "$(dirname "$0")"

NINJA="$(command -v ninja || command -v ninja-build || true)"
BUILD_DIR="build-linux"
BIN="$BUILD_DIR/tokyojungle"

if [ "${1:-}" = "build" ]; then
    [ -n "$NINJA" ] || { echo "ninja not found; install ninja-build" >&2; exit 1; }
    cmake -B "$BUILD_DIR" -G Ninja \
        -DCMAKE_MAKE_PROGRAM="$NINJA" \
        -DCMAKE_BUILD_TYPE=Release \
        -DCMAKE_C_FLAGS_RELEASE="-O1 -DNDEBUG" \
        -DCMAKE_CXX_FLAGS_RELEASE="-O1 -DNDEBUG"
    "$NINJA" -C "$BUILD_DIR" tokyojungle -j2
fi

if [ ! -x "$BIN" ]; then
    echo "binary not built — run: $0 build" >&2
    exit 1
fi

# PS3_VERBOSE=0 forces quiet HLE logging (the default turns verbose when
# stderr is redirected to a file).
export PS3_VERBOSE=0

if [ "${1:-}" = "quick" ]; then
    exec timeout 20 "$BIN" 2>&1 | head -200
fi

exec "$BIN"
