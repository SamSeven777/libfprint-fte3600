#!/usr/bin/env bash
# SPDX-License-Identifier: LGPL-2.1-or-later
# User-facing entry point for the bounded old-protocol identity experiment.
set -euo pipefail

fte_repo=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
fte_usage='Usage: bash scripts/test-medion-legacy-id.sh [--check-only|--run|--ctfdavis-run|--historical-run]'

(( $# <= 1 )) || { echo "$fte_usage" >&2; exit 1; }

case ${1:---check-only} in
  --check-only)
    exec bash "$fte_repo/scripts/test-medion-soft-reset.sh" --check-only
    ;;
  --run)
    exec bash "$fte_repo/scripts/test-medion-soft-reset.sh" --legacy-id-run
    ;;
  --historical-run)
    exec bash "$fte_repo/scripts/test-medion-soft-reset.sh" --legacy-historical-id-run
    ;;
  --ctfdavis-run)
    exec bash "$fte_repo/scripts/test-medion-soft-reset.sh" --legacy-ctfdavis-id-run
    ;;
  --help|-h)
    cat <<'USAGE'
Usage: bash scripts/test-medion-legacy-id.sh [--check-only|--run|--ctfdavis-run|--historical-run]

  --check-only  Compile-only preflight; no sensor or service access (default).
  --run         Preserve the original bounded behavior: isolate fprintd and
                send one 1 MHz TX6 + RX4 read without GPIO or C6. This is a
                comparison, not the historical stack.
  --ctfdavis-run
                Candidate corresponding to the code named in the Mint success
                report: no GPIO request, Mode 0, at most 1 MHz, C6 handshake,
                then identity. Stops before full initialization or capture.
                Exact installed Mint source/hash has not been recovered.
  --historical-run
                Explicit later unverified attachment candidate:
                raw Pin 39 low->10ms->high twice, Mode 0 at 4 MHz, C6
                handshake, then FW9362 identity read. Stops before full
                initialization, capture, or firmware.

Read docs/medion-legacy-id-test.md before the active run.
USAGE
    ;;
  *)
    echo "$fte_usage" >&2
    exit 1
    ;;
esac
