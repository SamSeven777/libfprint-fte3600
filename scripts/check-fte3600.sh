#!/usr/bin/env bash
# SPDX-FileCopyrightText: 2026 FTE3600 Linux contributors
# SPDX-License-Identifier: LGPL-2.1-or-later

set -euo pipefail

fte_repo=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)

for fte_config in spi-false spi-true usb combined-false combined-true; do
  fte_auth=false
  fte_drivers=fte3600
  fte_tests=(fpi-device fpi-device-cancel fpi-spi-transfer)
  case "$fte_config" in
    spi-true|combined-true) fte_auth=true ;;
  esac
  case "$fte_config" in
    usb) fte_drivers=focaltech0752 ;;
    combined-*) fte_drivers=fte3600,focaltech0752 ;;
  esac
  if [[ "$fte_config" != usb ]]; then
    fte_tests+=(fte3600-driver fte3600-lifecycle fte3600-brisk fte3600-template)
  fi
  if [[ "$fte_config" == usb || "$fte_config" == combined-* ]]; then
    fte_tests+=(focaltech0752 focaltech0752-lifecycle)
  fi
  fte_build="$fte_repo/build-fte3600-ci-$fte_config"
  fte_args=(
    -Ddrivers="$fte_drivers"
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

  meson compile -j "${FTE_BUILD_JOBS:-4}" -C "$fte_build"
  meson test -C "$fte_build" --print-errorlogs --no-rebuild "${fte_tests[@]}"
done
