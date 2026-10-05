#!/usr/bin/env bash
# SPDX-License-Identifier: LGPL-2.1-or-later
# Experimental GPIO glue + distribution spidev integration.
set -euo pipefail
REPO_DIR=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
KERNEL_SRC="$REPO_DIR/kernel/fte3600"
DKMS_VERSION=0.2
DKMS_DEST="/usr/src/fte3600-${DKMS_VERSION}"
PAIR_HELPER="$REPO_DIR/scripts/fte3600-pair.py"
INSTALLED_HELPER=/usr/libexec/fte3600-pair
RULES=/etc/udev/rules.d/70-fte3600-acpi-spidev.rules
LABEL_RULES=/etc/udev/rules.d/71-fte3600-acpi-spidev-selinux.rules
MODPROBE_CONF=/etc/modprobe.d/fte3600-acpi-spidev.conf
SELINUX_POLICY_NAME=fte3600-acpi-spidev
SELINUX_CIL="$REPO_DIR/config/selinux/$SELINUX_POLICY_NAME.cil"
fail() { echo "FTE3600: $*" >&2; return 1; }
info() { echo "FTE3600: $*"; }
check_root() { [ "$(id -u)" = 0 ] || fail 'This action requires root.'; }
pair_data() { python3 "$PAIR_HELPER"; }
require_tools() {
  local tool
  for tool in "$@"; do command -v "$tool" >/dev/null || { fail "Missing tool: $tool"; return 1; }; done
}
check_legacy() {
  local path policies
  for path in /usr/src/fte3600-0.1 /etc/systemd/system/fprintd.service.d/10-fte3600-bridge.conf /etc/systemd/system/fprintd.service.d/10-fte3600-gpio.conf; do
    if [ -e "$path" ] || [ -L "$path" ]; then
      fail "Old installation: $path. Save rollback copies and follow docs/fte3600/acpi-spidev.md first."; return 1
    fi
  done
  for path in /sys/class/misc/fte3600-*; do
    if [ -e "$path/fte3600_abi" ]; then
      fail 'Old SPI bridge is bound. Migrate and reboot; this helper will not forcibly unbind it.'; return 1
    fi
  done
  if command -v semodule >/dev/null; then
    policies=$(semodule --list-modules=full) || return 1
    if printf '%s\n' "$policies" | grep -E '(^|[[:space:]])fte3600-(gpio|bridge)([[:space:]]|$)' >/dev/null; then
      fail 'Old FTE3600 SELinux module remains; the new policy cannot revoke its permissions. Review and migrate it first.'; return 1
    fi
  fi
}
check_buffer() {
  local size
  size=$(cat /sys/module/spidev/parameters/bufsiz) || return 1
  case "$size" in ''|*[!0-9]*) fail 'Invalid spidev bufsiz.'; return 1 ;; esac
  if [ "$size" -lt 32768 ]; then
    fail "spidev bufsiz=$size, need at least 32768. Reboot to apply the saved module setting; spidev was not unloaded."; return 1
  fi
}
trigger_spi() {
  local spi
  for spi in /sys/bus/spi/devices/*; do
    [ -r "$spi/firmware_node/hid" ] || continue
    [ "$(cat "$spi/firmware_node/hid")" = FTE3600 ] || continue
    udevadm trigger --action=change "$spi"
  done
  udevadm settle --timeout=10
}
cmd_check() {
  local failed=0
  info "Experimental ACPI glue/spidev; kernel $(uname -r)"
  require_tools make gcc python3 systemd-escape || failed=1
  [ -d "/lib/modules/$(uname -r)/build" ] || { info 'Matching kernel headers missing.'; failed=1; }
  check_legacy || failed=1
  check_buffer || failed=1
  pair_data || failed=1
  info 'Inspection does not compile the module or prove capture, service-domain access or signing compatibility.'
  return "$failed"
}
cmd_install_kernel() {
  check_root
  check_legacy
  require_tools make gcc python3 modprobe udevadm
  local kdir="/lib/modules/$(uname -r)/build" file status
  [ -d "$kdir" ] || { fail "Matching kernel headers missing: $kdir"; return 1; }
  # These exact destinations belong in the rollback inventory. No global unload.
  install -Dm755 "$PAIR_HELPER" "$INSTALLED_HELPER"
  install -Dm644 "$REPO_DIR/config/udev/70-fte3600-acpi-spidev.rules" "$RULES"
  install -Dm644 "$REPO_DIR/config/modprobe.d/fte3600-acpi-spidev.conf" "$MODPROBE_CONF"
  modprobe spidev bufsiz=32768
  check_buffer
  if command -v dkms >/dev/null; then
    status=$(dkms status -m fte3600 -v "$DKMS_VERSION")
    if [ -n "$status" ]; then dkms remove -m fte3600 -v "$DKMS_VERSION" --all; fi
    mkdir -p "$DKMS_DEST"
    for file in dkms.conf Makefile fte3600.c fte3600-policy-test.c; do cp -f "$KERNEL_SRC/$file" "$DKMS_DEST/"; done
    for file in "$KERNEL_SRC/"*.h; do [ ! -f "$file" ] || cp -f "$file" "$DKMS_DEST/"; done
    dkms add -m fte3600 -v "$DKMS_VERSION"
    dkms build -m fte3600 -v "$DKMS_VERSION"
    dkms install -m fte3600 -v "$DKMS_VERSION" --force
  else
    make -C "$KERNEL_SRC" KDIR="$kdir" W=1
    install -Dm644 "$KERNEL_SRC/fte3600.ko" "/lib/modules/$(uname -r)/extra/fte3600.ko"
    depmod -a
  fi
  udevadm control --reload
  modprobe fte3600 || { fail 'Module load failed. Check kernel compatibility and Secure Boot signing; no key enrollment is performed here.'; return 1; }
  trigger_spi
  pair_data >/dev/null || { fail 'No ready pair. A replaced loaded module needs reboot; restarting fprintd cannot replace it.'; return 1; }
  info 'Glue/spidev pair validated. Capture is not yet verified.'
}
cmd_install_systemd() {
  check_root
  check_legacy
  "$REPO_DIR/scripts/fte3600-device-allow.sh" --install
}
cmd_install_selinux() {
  check_root
  check_legacy
  if ! command -v getenforce >/dev/null || [ "$(getenforce)" = Disabled ]; then
    info 'SELinux not active; no policy or label rule installed.'; return
  fi
  require_tools semodule udevadm stat
  local pairs role node alias context expected devpath
  pairs=$(pair_data)
  semodule -i "$SELINUX_CIL"
  install -Dm644 "$REPO_DIR/config/udev/71-fte3600-acpi-spidev-selinux.rules" "$LABEL_RULES"
  udevadm control --reload
  # No wildcard file contexts for dynamic node numbers. udev revalidates identity.
  while read -r role node alias; do
    if [ "$role" = spi ]; then udevadm trigger --action=change "/sys/class/spidev/${node##*/}";
    elif [ "$role" = gpio ]; then udevadm trigger --action=change "/sys/bus/gpio/devices/${node##*/}"
    else udevadm trigger --action=change "/sys/class/uio/${node##*/}"; fi
  done <<< "$pairs"
  udevadm settle --timeout=10
  [ "$(pair_data)" = "$pairs" ] || { fail 'Pair changed during labeling.'; return 1; }
  while read -r role node alias; do
    # Invoke synchronously too: udev RUN failures are not trigger's exit status.
    # The same restricted operation handles metadata-late CHANGE events at boot.
    devpath=$(udevadm info --query=path --name="$node")
    python3 "$PAIR_HELPER" --relabel "$devpath"
    expected=fte3600_gpio_t
    [ "$role" != irq ] || expected=fte3600_irq_t
    [ "$role" != spi ] || expected=fte3600_spidev_t
    context=$(stat -c %C -- "$node") || return 1
    case "$context" in *:object_r:"$expected":*) ;; *) fail "Wrong actual SELinux label on $node: $context"; return 1 ;; esac
  done <<< "$pairs"
  info 'Validated companion labels applied. fprintd-domain access still needs a runtime test.'
}
cmd_install_all() {
  check_root
  require_tools systemctl
  cmd_install_kernel
  cmd_install_systemd
  cmd_install_selinux
  systemctl restart fprintd.service || { fail 'fprintd restart failed; integration incomplete.'; return 1; }
  info 'System integration completed. Test actual identity and capture separately.'
}
cmd_uninstall() {
  check_root
  local dkms_status="" policies="" remove_policy=0 active=0 live=0 loaded
  if command -v dkms >/dev/null; then dkms_status=$(dkms status -m fte3600 -v "$DKMS_VERSION");
  elif [ -e "$DKMS_DEST" ]; then fail 'DKMS sources exist but dkms is unavailable; files retained.'; return 1; fi
  if command -v getenforce >/dev/null && [ "$(getenforce)" != Disabled ]; then live=1; fi
  if command -v semodule >/dev/null; then
    policies=$(semodule --list-modules=full)
    if printf '%s\n' "$policies" | grep -E "^[[:space:]]*400[[:space:]]+$SELINUX_POLICY_NAME([[:space:]]|$)" >/dev/null; then remove_policy=1; fi
  elif [ "$live" = 1 ]; then fail 'semodule is needed before policy removal.'; return 1; fi
  if [ "$live" = 1 ]; then require_tools python3 matchpathcon; fi
  loaded=$(lsmod)
  if command -v systemctl >/dev/null; then
    if [ "$(systemctl show --property=LoadState --value fprintd.service)" != not-found ]; then
      if systemctl is-active --quiet fprintd.service; then active=1; fi
      systemctl stop fprintd.service
    fi
  fi
  loaded=$(lsmod)
  if printf '%s\n' "$loaded" | grep '^fte3600 ' >/dev/null; then
    if ! rmmod fte3600; then
      [ "$active" = 0 ] || systemctl start fprintd.service
      fail 'Glue busy; installed files retained. Close clients and retry.'; return 1
    fi
    loaded=$(lsmod)
    if printf '%s\n' "$loaded" | grep '^fte3600 ' >/dev/null; then fail 'Glue still loaded; files retained.'; return 1; fi
  fi
  # An already-running udev helper may have validated metadata before rmmod.
  # Drain those writers before restoring labels, including when the module was
  # absent on entry. A queued SPI event may instead have loaded it again.
  udevadm settle --timeout=10
  loaded=$(lsmod)
  if printf '%s\n' "$loaded" | grep '^fte3600 ' >/dev/null; then
    fail 'Glue reloaded during device-event cleanup; files and policy retained. Close clients and retry.'; return 1
  fi
  if [ -n "$dkms_status" ]; then dkms remove -m fte3600 -v "$DKMS_VERSION" --all; fi
  # spidev survives glue removal. Restore its pinned inode while our types still
  # exist in policy, even if GPIO/UIO/aliases disappeared before this invocation.
  # Failure retains the policy/rules/helper for an explicit retry.
  if [ "$live" = 1 ]; then python3 "$PAIR_HELPER" --restore-labels; fi
  rm -rf "$DKMS_DEST"
  rm -f "/lib/modules/$(uname -r)/extra/fte3600.ko"
  depmod -a
  "$REPO_DIR/scripts/fte3600-device-allow.sh" --remove
  rm -f "$RULES" "$LABEL_RULES" "$MODPROBE_CONF" "$INSTALLED_HELPER"
  udevadm control --reload
  if [ "$remove_policy" = 1 ]; then
    if [ "$live" = 1 ]; then semodule -X 400 -r "$SELINUX_POLICY_NAME";
    else semodule -n -X 400 -r "$SELINUX_POLICY_NAME"; fi
  fi
  [ "$active" = 0 ] || systemctl start fprintd.service
  info 'Experimental integration removed. Global spidev was not unloaded; restore saved configuration and reboot for rollback.'
}
case "${1:-check}" in
  check|--check|status|--status) cmd_check ;;
  install-kernel|--install-kernel) cmd_install_kernel ;;
  install-systemd|--install-systemd) cmd_install_systemd ;;
  install-selinux|--install-selinux) cmd_install_selinux ;;
  install-all|--install-all) cmd_install_all ;;
  uninstall|--uninstall|remove|--remove) cmd_uninstall ;;
  --help|-h) echo 'Usage: setup-fte3600.sh [check|install-kernel|install-systemd|install-selinux|install-all|uninstall]' ;;
  *) fail 'Unknown command. See --help.' ;;
esac
