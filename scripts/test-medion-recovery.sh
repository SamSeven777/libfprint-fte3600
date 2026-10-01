#!/usr/bin/env bash
# SPDX-License-Identifier: LGPL-2.1-or-later
# Build the current diagnostic, isolate it from fprintd, then restore the daemon.
set -euo pipefail
umask 077

if [[ $(id -u) != 0 ]]; then
  echo "Run with sudo: sudo bash $0 [firmware-file]" >&2
  exit 1
fi
if (( $# > 1 )); then
  echo "Usage: sudo bash $0 [firmware-file]" >&2
  exit 1
fi

fte_repo=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
fte_work=$(mktemp -d /tmp/fte3600-medion-test.XXXXXX)
fte_lockfile=/run/fte3600-medion-diagnostic.lock
fte_child=
fte_masked=0
fte_was_active=0

fte_cleanup() {
  local fte_status=$?
  trap - EXIT
  trap '' INT TERM
  if [[ -n $fte_child ]]; then
    kill -TERM "$fte_child" 2>/dev/null || true
    wait "$fte_child" 2>/dev/null || true
    fte_child=
  fi
  if (( fte_masked )); then
    systemctl unmask --runtime fprintd.service || fte_status=1
    if (( fte_was_active )); then
      systemctl start fprintd.service || fte_status=1
    fi
  fi
  rm -rf -- "$fte_work"
  exit "$fte_status"
}
trap fte_cleanup EXIT
trap 'exit 130' INT
trap 'exit 143' TERM

# Only non-biometric diagnostic output is printed. Never enable SPI image dumps.
fte_revision=$(git -c safe.directory="$fte_repo" -C "$fte_repo" rev-parse --short HEAD 2>/dev/null || true)
printf 'Source commit: %s\n' "${fte_revision:-unknown}"
fte_flags=$(pkg-config --cflags --libs glib-2.0 libgpiod gudev-1.0)
read -r -a fte_cc_flags <<< "$fte_flags"
cc -O2 -Wall -Wextra -Werror "$fte_repo/tools/test_medion_e3224.c" \
  "$fte_repo/libfprint/drivers/fte3600-legacy-proto.c" \
  "${fte_cc_flags[@]}" -o "$fte_work/test-medion-e3224"

# Both wrappers hold this same root-owned lock through child and service
# cleanup. Keep the inode after exit; unlinking it permits competing locks.
if [[ -L $fte_lockfile || ( -e $fte_lockfile && ! -f $fte_lockfile ) ]]; then
  echo 'Unexpected diagnostic lock file; refusing to replace it.' >&2
  exit 1
fi
exec {fte_lock_fd}>>"$fte_lockfile"
if ! flock --nonblock "$fte_lock_fd"; then
  echo 'Another Medion diagnostic wrapper is running.' >&2
  exit 1
fi

fte_load=$(systemctl show fprintd.service -p LoadState --value)
if systemctl is-active --quiet fprintd.service; then
  fte_was_active=1
fi
if [[ $fte_load == masked ]]; then
  if (( fte_was_active )); then
    echo 'fprintd is active but already masked; cannot restore this state automatically.' >&2
    exit 1
  fi
else
  if [[ -e /run/systemd/system/fprintd.service || -L /run/systemd/system/fprintd.service ]]; then
    echo 'Existing runtime fprintd unit override; leaving it untouched.' >&2
    exit 1
  fi
  fte_masked=1
  systemctl mask --runtime --now fprintd.service
fi

# /etc unit overrides outrank /run. Check that the mask actually took effect.
fte_load=$(systemctl show fprintd.service -p LoadState --value)
fte_active=$(systemctl show fprintd.service -p ActiveState --value)
if [[ $fte_load != masked || ( $fte_active != inactive && $fte_active != failed ) ]]; then
  echo "Cannot isolate fprintd (LoadState=$fte_load ActiveState=$fte_active)." >&2
  exit 1
fi

"$fte_work/test-medion-e3224" --test-vendor-recovery \
  "${1:-/usr/lib/firmware/fte3600/ft9361.bin}" &
fte_child=$!
if wait "$fte_child"; then fte_result=0; else fte_result=$?; fi
fte_child=
exit "$fte_result"
