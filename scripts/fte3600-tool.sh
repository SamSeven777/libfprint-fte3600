#!/usr/bin/env bash
# SPDX-License-Identifier: LGPL-2.1-or-later
set -euo pipefail
fte_repo=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
fte_build=${FTE3600_BUILD_DIR:-$fte_repo/build}
usage() {
  cat <<'HELP'
Usage: scripts/fte3600-tool.sh COMMAND [arguments]
  info                         Read-only ACPI/GPIO/SPI topology report
  check                        Check installed kernel/service prerequisites
  probe /dev/spidevB.C [dir]    Identify chip through full production startup
  bench [cycles]               Measure driver open/close time
Set FTE3600_BUILD_DIR for a build directory other than ./build.
Probe needs root and the ABI 2 bridge; keep the sensor untouched.
Stop other sensor clients before probe/bench. No automatic service changes.
See docs/fte3600/tools.md for build instructions and developer tools.
HELP
}
fte_cmd=${1:---help}
if (($#)); then shift; fi
case "$fte_cmd" in
  info)
    if (($#)); then usage >&2; exit 2; fi
    exec bash "$fte_repo/scripts/probe-fte3600.sh" ;;
  check)
    if (($#)); then usage >&2; exit 2; fi
    exec bash "$fte_repo/scripts/setup-fte3600.sh" check ;;
  probe|bench)
    if [[ $fte_cmd == probe ]]; then fte_bin=fte3600-probe; else fte_bin=fte3600-open-bench; fi
    if [[ ! -x "$fte_build/tools/$fte_bin" ]]; then
      printf 'Missing %s/tools/%s. Build the production tools first; see docs/fte3600/tools.md.\n' "$fte_build" "$fte_bin" >&2
      exit 2
    fi
    exec "$fte_build/tools/$fte_bin" "$@" ;;
  -h|--help|help) usage ;;
  *) usage >&2; exit 2 ;;
esac
