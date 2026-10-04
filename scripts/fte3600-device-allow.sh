#!/bin/sh
# SPDX-License-Identifier: MIT
# Print a service drop-in for currently present bridge devices. No writes.
set -eu
fte_found=0
printf '[Service]\n'
for fte_node in /sys/class/misc/fte3600-*; do
  [ -r "$fte_node/fte3600_abi" ] || continue
  [ "$(cat "$fte_node/fte3600_abi")" = 1 ] || continue
  [ "$(basename "$(readlink -f "$fte_node/device/driver")")" = fte3600 ] || continue
  fte_name=$(basename "$fte_node")
  case "$fte_name" in *[!a-zA-Z0-9_.:-]*) exit 1 ;; esac
  [ -c "/dev/$fte_name" ] || continue
  printf 'DeviceAllow=/dev/%s rw\n' "$fte_name"
  fte_found=1
done
if [ "$fte_found" = 0 ]; then
  echo 'No FTE3600 bridge node found; load the module first.' >&2
  exit 1
fi
