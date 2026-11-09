#!/usr/bin/env bash
# Build the firmware and drop a copy in ./dev-output/.
#
#   ./build-firmware.sh            # x4-pro (default)
#   ./build-firmware.sh default    # any env from platformio.ini
#
# Each build gets its own file (env, date-time, git commit, "-dirty" for uncommitted changes),
# so older builds are never overwritten. Do not run two builds of this repo at once: they share
# the rebuilt ESP-IDF libraries (see .claude/CONTEXT.md).
#
# Optional second copy: put `FIRMWARE_COPY_DIR=/some/folder` in .build-firmware.local (gitignored)
# and each build is also copied there as the next firmware-<env>-scenes-vN.bin.
set -euo pipefail

ENV="${1:-x4-pro}"
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
OUT="$ROOT/dev-output"
cd "$ROOT"
FIRMWARE_COPY_DIR=""
# shellcheck disable=SC1091
if [[ -f .build-firmware.local ]]; then source .build-firmware.local; fi

if pgrep -f "pio run" >/dev/null 2>&1; then
  echo "Another 'pio run' is already running; wait for it to finish." >&2
  exit 1
fi

pio run -e "$ENV"

BIN=".pio/build/$ENV/firmware.bin"
if [[ ! -f "$BIN" ]]; then
  echo "Build finished but $BIN was not found." >&2
  exit 1
fi

SHA="$(git rev-parse --short HEAD 2>/dev/null || echo nogit)"
DIRTY=""
if [[ -n "$(git status --porcelain 2>/dev/null)" ]]; then DIRTY="-dirty"; fi
NAME="firmware-$ENV-$(date +%Y%m%d-%H%M%S)-$SHA$DIRTY.bin"

mkdir -p "$OUT"
cp "$BIN" "$OUT/$NAME"
echo
echo "Firmware: $OUT/$NAME ($(du -h "$OUT/$NAME" | cut -f1))"

if [[ -n "$FIRMWARE_COPY_DIR" ]]; then
  mkdir -p "$FIRMWARE_COPY_DIR"
  last=0
  shopt -s nullglob
  for f in "$FIRMWARE_COPY_DIR"/firmware-"$ENV"-scenes-v*.bin; do
    n="${f##*-v}"
    n="${n%.bin}"
    if [[ "$n" =~ ^[0-9]+$ ]] && ((n > last)); then last=$n; fi
  done
  COPY="$FIRMWARE_COPY_DIR/firmware-$ENV-scenes-v$((last + 1)).bin"
  cp -n "$BIN" "$COPY"
  echo "Copy:     $COPY"
fi
