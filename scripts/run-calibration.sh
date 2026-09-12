#!/usr/bin/env bash
#
# Compile and run the offline calibration benchmark for FTE3600 BRISK matcher
#
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_DIR="$(cd "${SCRIPT_DIR}/.." && pwd)"

BIN_DIR="${REPO_DIR}/build-calibration"
BIN="${BIN_DIR}/calibrate-matcher"

mkdir -p "${BIN_DIR}"

echo "==> Compiling FTE3600 calibration benchmark..."
gcc -O3 -DFTE3600_ENABLE_PERSONAL_AUTH=1 \
  -I"${REPO_DIR}/libfprint/drivers" -I"${REPO_DIR}" \
  "${REPO_DIR}/scripts/calibrate-matcher.c" \
  "${REPO_DIR}/libfprint/drivers/fte3600-brisk.c" \
  $(pkg-config --cflags --libs glib-2.0) -lm \
  -o "${BIN}"

echo "==> Executing calibration benchmark..."
echo
"${BIN}" "$@"
