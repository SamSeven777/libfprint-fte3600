#!/usr/bin/env bash
# SPDX-FileCopyrightText: 2026 FTE3600 Linux contributors
# SPDX-License-Identifier: LGPL-2.1-or-later

set -euo pipefail

fte_repo=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)

for fte_auth in false true; do
  fte_build="$fte_repo/build-fte3600-ci-$fte_auth"
  fte_args=(
    -Ddrivers=fte3600
    -Dfte3600_personal_auth="$fte_auth"
    -Dgtk-examples=false
    -Ddoc=false
    -Dintrospection=false
    -Dinstalled-tests=false
    -Dwerror=true
  )

  if [[ -f "$fte_build/meson-private/coredata.dat" ]]; then
    meson setup --wipe "$fte_build" "$fte_repo" "${fte_args[@]}"
  else
    meson setup "$fte_build" "$fte_repo" "${fte_args[@]}"
  fi

  meson compile -C "$fte_build"
  meson test -C "$fte_build" --print-errorlogs --no-rebuild \
    fpi-spi-transfer fte3600-driver fte3600-brisk fte3600-template
done
