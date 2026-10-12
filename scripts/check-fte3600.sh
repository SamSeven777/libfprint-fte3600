#!/usr/bin/env bash
# SPDX-FileCopyrightText: 2026 FTE3600 Linux contributors
# SPDX-License-Identifier: LGPL-2.1-or-later

set -euo pipefail

fte_repo=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
fte_common_args=(
  -Ddrivers=fte3600
  -Dgtk-examples=false
  -Ddoc=false
  -Dintrospection=false
  -Dinstalled-tests=false
  -Dwerror=true
)

fte_check_build() {
  local fte_mode=$1
  local fte_backend=$2
  shift 2
  local fte_build="$fte_repo/build-fte3600-ci-$fte_mode"
  local fte_args=("${fte_common_args[@]}" "-Dfte3600-test-backend=$fte_backend")

  if [[ -f "$fte_build/meson-private/coredata.dat" ]]; then
    meson setup --wipe "$fte_build" "$fte_repo" "${fte_args[@]}"
  else
    meson setup "$fte_build" "$fte_repo" "${fte_args[@]}"
  fi

  meson compile -C "$fte_build"
  # Enumeration depends on optional umockdev packages. Fail explicitly if a
  # requested check was not configured, rather than silently losing coverage.
  meson introspect --tests "$fte_build" | python3 -c '
import json, sys
available = {item["name"] for item in json.load(sys.stdin)}
missing = set(sys.argv[1:]) - available
if missing:
    sys.exit("Missing required Meson tests: " + ", ".join(sorted(missing)))
' "$@"
  meson test -C "$fte_build" --print-errorlogs --no-rebuild "$@"
}

fte_check_build production false fte3600-auth fp-context-udev fpi-device fpi-ssm \
  fte3600-install-firmware fte3600-setup fte3600-pair \
  fte3600-linux-board fte3600-device-session fte3600-fw9369-release \
  fte3600-fw9369-runtime fte3600-open-bench
fte_check_build test-backend true fte3600-public-api

# This host-side policy check needs no kernel headers or attached device.
make -C "$fte_repo/kernel/fte3600" check
