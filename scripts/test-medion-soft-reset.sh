#!/usr/bin/env bash
# SPDX-License-Identifier: LGPL-2.1-or-later
# Compile a bounded diagnostic; run an explicitly selected experiment on request.
set -euo pipefail
umask 077

fte_usage() {
  cat <<'USAGE'
Usage: bash scripts/test-medion-soft-reset.sh [--check-only|--run|--legacy-id-run|--legacy-ctfdavis-id-run|--legacy-historical-id-run]

  --check-only  Check dependencies, print limited metadata, and compile locally
                in a temporary directory (the default). No device access or
                service changes. This is not a strictly read-only operation.
  --run         Requires root. Temporarily isolate fprintd, then run
                --compare-soft-reset: active SPI reads and a software reset.
                No GPIO request, firmware upload, installation, or reboot.
  --legacy-id-run
                Requires root. Temporarily isolate fprintd, then send exactly
                one old-protocol TX6 + RX4 identity candidate read. No GPIO,
                reset, C6/FD/FE/1a84 write, retry, firmware, or recovery.
  --legacy-historical-id-run
                Requires root. Test the later unverified attachment candidate:
                raw Pin 39
                low->10ms->high twice, Mode 0 at 4 MHz, up to four C6
                handshakes, then one FW9362 identity read.
                Stops before 1a84, FD/FE, FDT, capture, or firmware upload.
  --legacy-ctfdavis-id-run
                Requires root. Test the ctfdavis candidate without GPIO access:
                Mode 0, speed capped at 1 MHz, up to four C6 handshakes, then
                one identity read. Stops before 1a84, FD/FE, FDT or capture.

An existing fprintd mask is preserved. A mask created here is runtime-only and
removed on exit; a previously active service is restarted. Output stays local.
Exit 0/2 preserves the selected diagnostic result, not fingerprint functionality;
setup/cleanup failures return 1, interruption returns 128 + the signal number.
This wrapper cannot prove that nothing accessed the sensor earlier this boot.
Its lock excludes both Medion wrappers; direct diagnostic programs bypass it.
USAGE
}

fte_mode=--check-only
if (( $# > 1 )); then
  fte_usage >&2
  exit 1
fi
if (( $# )); then
  fte_mode=$1
fi
case $fte_mode in
  --help|-h) fte_usage; exit 0 ;;
  --check-only|--run|--legacy-id-run|--legacy-historical-id-run|--legacy-ctfdavis-id-run) ;;
  *) fte_usage >&2; exit 1 ;;
esac

for fte_command in cc pkg-config git systemctl mktemp rm readlink uname id flock; do
  if ! command -v "$fte_command" >/dev/null 2>&1; then
    printf 'Missing dependency: %s\n' "$fte_command" >&2
    exit 1
  fi
done
if [[ $fte_mode != --check-only && $(id -u) != 0 ]]; then
  printf 'Use sudo bash scripts/test-medion-soft-reset.sh %s for the diagnostic.\n' "$fte_mode" >&2
  exit 1
fi

fte_repo=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
fte_runtime_unit=/run/systemd/system/fprintd.service
fte_lockfile=/run/fte3600-medion-diagnostic.lock
fte_work=
fte_child=
fte_mask_attempted=0
fte_existing_mask=0
fte_was_active=0
fte_service_touched=0

fte_read_service() {
  local fte_properties fte_key fte_value
  fte_load=
  fte_active=
  if ! fte_properties=$(systemctl show fprintd.service -p LoadState -p ActiveState); then
    echo 'Cannot query fprintd state.' >&2
    return 1
  fi
  while IFS='=' read -r fte_key fte_value; do
    case $fte_key in
      LoadState) fte_load=$fte_value ;;
      ActiveState) fte_active=$fte_value ;;
    esac
  done <<< "$fte_properties"
  [[ -n $fte_load && -n $fte_active ]]
}

fte_cleanup() {
  local fte_status=$? fte_restore_ok=1
  trap - EXIT
  # Do not interrupt restoration a second time, or restart fprintd while the
  # diagnostic is still running. SIGKILL cannot be handled by either process.
  trap '' INT TERM
  if [[ -n $fte_child ]]; then
    kill -TERM "$fte_child" 2>/dev/null || true
    wait "$fte_child" 2>/dev/null || true
    fte_child=
  fi
  if (( fte_mask_attempted )); then
    if [[ -L $fte_runtime_unit && $(readlink -- "$fte_runtime_unit") == /dev/null ]]; then
      if ! systemctl unmask --runtime fprintd.service; then
        echo 'Could not remove our runtime mask; inspect fprintd manually.' >&2
        fte_restore_ok=0
      fi
    elif [[ -e $fte_runtime_unit || -L $fte_runtime_unit ]]; then
      echo 'Runtime unit changed during the test; leaving it untouched. Inspect fprintd manually.' >&2
      fte_restore_ok=0
    fi
    if (( fte_restore_ok && fte_was_active )); then
      if ! fte_read_service || [[ $fte_load == masked ]]; then
        echo 'Cannot safely restart fprintd; inspect its current unit configuration.' >&2
        fte_restore_ok=0
      elif ! systemctl start fprintd.service; then
        echo 'Could not restart the previously active fprintd service.' >&2
        fte_restore_ok=0
      fi
    fi
  fi
  if (( fte_service_touched )); then
    if fte_read_service; then
      printf 'fprintd after: LoadState=%s ActiveState=%s\n' "$fte_load" "$fte_active"
      if (( fte_mask_attempted && fte_restore_ok )); then
        if [[ $fte_load == masked || ( $fte_was_active == 1 && $fte_active != active ) ]]; then
          fte_restore_ok=0
        fi
      fi
      if (( fte_existing_mask )) && [[ $fte_load != masked || ( $fte_active != inactive && $fte_active != failed ) ]]; then
        echo 'The pre-existing masked state changed during the diagnostic; inspect fprintd manually.' >&2
        fte_restore_ok=0
      fi
    else
      fte_restore_ok=0
    fi
    if (( fte_restore_ok )); then
      echo 'fprintd cleanup: complete (pre-existing masks were preserved).'
    else
      echo 'fprintd cleanup: incomplete; manual review is required.' >&2
    fi
  fi
  if [[ -n $fte_work ]]; then
    # mktemp created this exact private directory; never delete a parent or glob.
    if ! rm -rf -- "$fte_work"; then
      fte_restore_ok=0
    fi
  fi
  if (( ! fte_restore_ok )); then
    fte_status=1
  fi
  exit "$fte_status"
}

fte_signal() {
  exit "$((128 + $1))"
}
trap fte_cleanup EXIT
trap 'fte_signal 2' INT
trap 'fte_signal 15' TERM

# Do not source os-release or print identifiers, biometric data, or journals.
printf 'Kernel: %s\n' "$(uname -srm)"
if [[ -r /etc/os-release ]]; then
  while IFS='=' read -r fte_key fte_value; do
    case $fte_key in
      ID|VERSION_ID|PRETTY_NAME) printf 'OS %s=%s\n' "$fte_key" "$fte_value" ;;
    esac
  done < /etc/os-release
fi
if [[ -r /proc/uptime ]]; then
  read -r fte_uptime fte_unused < /proc/uptime
  printf 'Boot uptime (seconds): %s\n' "$fte_uptime"
fi
for fte_dmi_field in bios_version bios_date; do
  fte_dmi_value=
  if [[ -r /sys/class/dmi/id/$fte_dmi_field ]]; then
    IFS= read -r fte_dmi_value < "/sys/class/dmi/id/$fte_dmi_field" || true
  fi
  printf 'DMI %s: %s\n' "$fte_dmi_field" "${fte_dmi_value:-not exposed}"
done
fte_revision=$(git --no-optional-locks -c safe.directory="$fte_repo" -C "$fte_repo" rev-parse HEAD) || exit 1
fte_changes=$(git --no-optional-locks -c safe.directory="$fte_repo" -C "$fte_repo" status --porcelain --untracked-files=normal) || exit 1
printf 'Source commit: %s\n' "$fte_revision"
if [[ -n $fte_changes ]]; then
  echo 'Source worktree dirty: yes'
else
  echo 'Source worktree dirty: no'
fi
fte_gpiod_version=$(pkg-config --modversion libgpiod) || exit 1
printf 'libgpiod build version: %s\n' "$fte_gpiod_version"
fte_read_service
printf 'fprintd before preflight: LoadState=%s ActiveState=%s\n' "$fte_load" "$fte_active"

fte_work=$(mktemp -d /tmp/fte3600-medion-soft-reset.XXXXXX) || exit 1
fte_flags=$(pkg-config --cflags --libs glib-2.0 libgpiod gudev-1.0) || exit 1
read -r -a fte_cc_flags <<< "$fte_flags"
if ! cc -O2 -Wall -Wextra -Werror "$fte_repo/tools/test_medion_e3224.c" \
    "$fte_repo/libfprint/drivers/fte3600-legacy-proto.c" \
    "${fte_cc_flags[@]}" -o "$fte_work/test-medion-e3224"; then
  echo 'Diagnostic compilation failed; no service was changed.' >&2
  exit 1
fi
echo 'Diagnostic compilation: complete'
if [[ $fte_mode == --check-only ]]; then
  echo 'Preflight complete: no diagnostic was run and no service state was changed.'
  exit 0
fi

# /run itself is root-owned; do not use a predictable file in world-writable
# /tmp or /run/lock. Keep the lock file after exit to avoid inode/unlink races.
if [[ -L $fte_lockfile || ( -e $fte_lockfile && ! -f $fte_lockfile ) ]]; then
  echo 'Unexpected diagnostic lock file; refusing to replace it.' >&2
  exit 1
fi
exec {fte_lock_fd}>>"$fte_lockfile"
if ! flock --nonblock "$fte_lock_fd"; then
  echo 'Another Medion diagnostic wrapper is running.' >&2
  exit 1
fi

fte_read_service
printf 'fprintd before isolation: LoadState=%s ActiveState=%s\n' "$fte_load" "$fte_active"
if [[ -e $fte_runtime_unit || -L $fte_runtime_unit ]]; then
  if [[ ! -L $fte_runtime_unit || $(readlink -- "$fte_runtime_unit") != /dev/null ]]; then
    echo 'Existing runtime fprintd unit override; leaving it untouched.' >&2
    exit 1
  fi
fi
if [[ $fte_load == masked ]]; then
  if [[ $fte_active != inactive && $fte_active != failed ]]; then
    echo 'fprintd is masked but not stopped; cannot preserve that state automatically.' >&2
    exit 1
  fi
  fte_existing_mask=1
  echo 'Preserving the existing fprintd mask.'
elif [[ $fte_load != loaded ]]; then
  printf 'fprintd is not a loaded unit (LoadState=%s); resolve it before testing.\n' "$fte_load" >&2
  exit 1
elif [[ $fte_active == active || $fte_active == inactive ]]; then
  if [[ -e $fte_runtime_unit || -L $fte_runtime_unit ]]; then
    echo 'Runtime mask does not match effective fprintd state; refusing to change it.' >&2
    exit 1
  fi
  [[ $fte_active != active ]] || fte_was_active=1
  # Set before systemctl: even a partially successful command needs cleanup.
  fte_mask_attempted=1
  fte_service_touched=1
  systemctl mask --runtime --now fprintd.service
else
  echo 'fprintd is in a failed or transitional state; resolve it before testing.' >&2
  exit 1
fi

fte_service_touched=1
fte_read_service
if [[ $fte_load != masked || ( $fte_active != inactive && $fte_active != failed ) ]]; then
  printf 'Cannot isolate fprintd (LoadState=%s ActiveState=%s).\n' "$fte_load" "$fte_active" >&2
  exit 1
fi
if [[ $fte_mode == --legacy-historical-id-run ]]; then
  echo 'Running the later unverified attachment candidate; full initialization and capture do not follow.'
  fte_tool_mode=--legacy-historical-id
elif [[ $fte_mode == --legacy-ctfdavis-id-run ]]; then
  echo 'Running the ctfdavis candidate prefix without GPIO; full initialization and capture do not follow.'
  fte_tool_mode=--legacy-ctfdavis-id
elif [[ $fte_mode == --legacy-id-run ]]; then
  echo 'Running one bounded legacy-protocol identity candidate read; no initialization or recovery follows.'
  fte_tool_mode=--legacy-id-no-init
else
  echo 'Running the explicit soft-reset comparison; earlier sensor state is not proven by this wrapper.'
  fte_tool_mode=--compare-soft-reset
fi
"$fte_work/test-medion-e3224" "$fte_tool_mode" &
fte_child=$!
if wait "$fte_child"; then
  fte_result=0
else
  fte_result=$?
fi
fte_child=
printf 'Diagnostic exit: %s\n' "$fte_result"
exit "$fte_result"
