#!/usr/bin/env bash
# SPDX-License-Identifier: LGPL-2.1-or-later
# User-facing entry point for the bounded old-protocol identity experiment.
set -euo pipefail

fte_repo=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)

case ${1:---check-only} in
  --check-only)
    (( $# <= 1 )) || { echo 'Usage: bash scripts/test-medion-legacy-id.sh [--check-only|--run]' >&2; exit 1; }
    exec bash "$fte_repo/scripts/test-medion-soft-reset.sh" --check-only
    ;;
  --run)
    (( $# == 1 )) || { echo 'Usage: bash scripts/test-medion-legacy-id.sh [--check-only|--run]' >&2; exit 1; }
    exec bash "$fte_repo/scripts/test-medion-soft-reset.sh" --legacy-id-run
    ;;
  --help|-h)
    (( $# == 1 )) || { echo 'Usage: bash scripts/test-medion-legacy-id.sh [--check-only|--run]' >&2; exit 1; }
    cat <<'USAGE'
Usage: bash scripts/test-medion-legacy-id.sh [--check-only|--run]

  --check-only  Compile-only preflight; no sensor or service access (default).
  --run         Isolate fprintd and send one old-protocol TX6 + RX4 candidate
                identity read. No GPIO, reset, register configuration, retry,
                firmware upload or recovery.

Read docs/medion-legacy-id-test.md before the active run.
USAGE
    ;;
  *)
    echo 'Usage: bash scripts/test-medion-legacy-id.sh [--check-only|--run]' >&2
    exit 1
    ;;
esac
