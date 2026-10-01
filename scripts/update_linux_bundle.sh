#!/bin/bash
# Refreshes release-artifacts/Di-111_linux/Standalone (the local Linux bundle: D-110, D-50, Nonet-Seq)
# from the CMake build tree. plugin/build/ is only the build tree; this folder is what gets run/shipped.
# Builds are niced and capped at 3 jobs. Nonet-Seq lives in its own repo: it is copied, not built here.
set -e

ROOT=$(cd "$(dirname "$0")/.." && pwd)
BUILD="${BUILD:-$ROOT/plugin/build}"
BUNDLE="$ROOT/release-artifacts/Di-111_linux/Standalone"
NONET_SEQ_BIN="${NONET_SEQ_BIN:-$HOME/src/nonet-sequencer/build/Nonet-Seq}"

nice -n 19 cmake --build "$BUILD" --target D110EmulatorNative_Standalone D50Emulator_Standalone -- -j3

mkdir -p "$BUNDLE"
install_stripped() { # source destination
    cp "$1" "$2"
    strip "$2"
    echo "updated $2"
}
install_stripped "$BUILD/D110EmulatorNative_artefacts/Release/Standalone/Di-111" "$BUNDLE/Di-111"
install_stripped "$BUILD/D50Emulator_artefacts/Release/Standalone/D-50 Emulator" "$BUNDLE/D-50_Emulator"

if [ -x "$NONET_SEQ_BIN" ]; then
    install_stripped "$NONET_SEQ_BIN" "$BUNDLE/Nonet-Seq"
else
    echo "Nonet-Seq not updated: $NONET_SEQ_BIN not found (set NONET_SEQ_BIN)"
fi
