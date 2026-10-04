#!/usr/bin/env bash
# SPDX-FileCopyrightText: 2026 FTE3600 Linux contributors
# SPDX-License-Identifier: LGPL-2.1-or-later
#
# Automated setup, permission, and diagnostic tool for FTE3600 fingerprint sensor.

set -euo pipefail

REPO_DIR=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
KERNEL_SRC="$REPO_DIR/kernel/fte3600"
DKMS_VERSION="0.1"
DKMS_DEST="/usr/src/fte3600-${DKMS_VERSION}"
SYSTEMD_OVERRIDE_DIR="/etc/systemd/system/fprintd.service.d"
SYSTEMD_OVERRIDE_FILE="${SYSTEMD_OVERRIDE_DIR}/10-fte3600-bridge.conf"
SELINUX_CIL="${REPO_DIR}/config/selinux/fte3600-bridge.cil"
SELINUX_POLICY_NAME="fte3600-bridge"

# Colors for terminal output
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
BLUE='\033[0;34m'
BOLD='\033[1m'
NC='\033[0m' # No Color

log_info()    { echo -e "${BLUE}[INFO]${NC} $*"; }
log_ok()      { echo -e "${GREEN}[ OK ]${NC} $*"; }
log_warn()    { echo -e "${YELLOW}[WARN]${NC} $*"; }
log_fail()    { echo -e "${RED}[FAIL]${NC} $*"; }
log_header()  { echo -e "\n${BOLD}=== $* ===${NC}"; }

check_root() {
  if [ "$(id -u)" -ne 0 ]; then
    log_fail "This action requires root privileges. Please re-run with sudo:"
    echo "  sudo $0 $*"
    exit 1
  fi
}

module_loaded() {
  # Read the complete output, even with pipefail enabled.
  lsmod | grep '^fte3600 ' >/dev/null
}

relabel_bridge_devices() {
  local expected_type="${1:-}" node context label_user label_role label_type label_range
  for node in /dev/fte3600-*; do
    [ -c "$node" ] || continue
    if [ -n "$expected_type" ]; then
      context=$(matchpathcon -n "$node") || return 1
      IFS=: read -r label_user label_role label_type label_range <<< "$context"
      if [ "$label_type" != "$expected_type" ]; then
        log_fail "SELinux expects '$label_type' for $node, not '$expected_type'. Review local file-context overrides."
        return 1
      fi
    fi
    restorecon -v "$node" || return 1
    matchpathcon -V "$node" || return 1
  done
}

detect_distro() {
  if [ -f /etc/os-release ]; then
    # shellcheck source=/dev/null
    . /etc/os-release
    DISTRO_ID="${ID:-unknown}"
    DISTRO_NAME="${PRETTY_NAME:-Linux}"
  else
    DISTRO_ID="unknown"
    DISTRO_NAME="Generic Linux"
  fi
}

cmd_check() {
  detect_distro
  log_header "FTE3600 System & Hardware Health Check"
  echo "Distribution: $DISTRO_NAME"
  echo "Kernel:       $(uname -r)"

  local missing_pkgs=0

  # 1. Check Kernel Headers
  local kdir="/lib/modules/$(uname -r)/build"
  if [ -d "$kdir" ]; then
    log_ok "Kernel build headers found ($kdir)"
  else
    log_fail "Kernel headers NOT found for current kernel $(uname -r)!"
    missing_pkgs=1
    case "$DISTRO_ID" in
      fedora)
        echo "  Install with: sudo dnf install kernel-devel-$(uname -r)"
        ;;
      ubuntu|debian|linuxmint|pop)
        echo "  Install with: sudo apt install linux-headers-$(uname -r)"
        ;;
      arch|manjaro)
        echo "  Install with: sudo pacman -S linux-headers"
        ;;
      *)
        echo "  Install headers corresponding to $(uname -r) from your package manager."
        ;;
    esac
  fi

  # 2. Check Build Tools
  for tool in make gcc dkms; do
    if command -v "$tool" >/dev/null 2>&1; then
      log_ok "Build tool available: $tool"
    else
      if [ "$tool" = "dkms" ]; then
        log_warn "Tool '$tool' is recommended for automated kernel rebuilds on kernel updates."
      else
        log_fail "Required tool '$tool' is missing."
        missing_pkgs=1
      fi
    fi
  done

  # 3. Check Hardware ACPI Detection
  log_header "Hardware Discovery"
  local acpi_found=0
  for dev in /sys/bus/acpi/devices/*; do
    [ -d "$dev" ] || continue
    if [ -f "$dev/hid" ] && [ "$(cat "$dev/hid" 2>/dev/null)" = "FTE3600" ]; then
      acpi_found=1
      log_ok "Detected ACPI FTE3600 device: $(basename "$dev")"
      [ -f "$dev/path" ] && echo "  ACPI Path: $(cat "$dev/path")"
      [ -f "$dev/status" ] && echo "  ACPI Status: $(cat "$dev/status")"
      break
    fi
  done
  if [ "$acpi_found" -eq 0 ]; then
    log_warn "No ACPI device with HID 'FTE3600' found in /sys/bus/acpi/devices/."
    echo "  (Note: If testing on a development host without FTE3600 sensor, this is expected)"
  fi

  # 4. Check Kernel Module & Bridge Device
  log_header "Bridge Module & Device Node"
  if module_loaded; then
    log_ok "Kernel module 'fte3600' is loaded."
  else
    log_warn "Kernel module 'fte3600' is not loaded."
  fi

  local bridge_found=0
  for node in /sys/class/misc/fte3600-*; do
    if [ -r "$node/fte3600_abi" ]; then
      bridge_found=1
      local abi
      abi=$(cat "$node/fte3600_abi")
      local dev_node="/dev/$(basename "$node")"
      log_ok "Bridge device node: $dev_node (ABI version: $abi)"
    fi
  done
  if [ "$bridge_found" -eq 0 ]; then
    log_warn "No active /dev/fte3600-* character device found."
  fi

  # 5. Check systemd Service Sandbox
  log_header "systemd fprintd Device Sandbox"
  if [ -f "$SYSTEMD_OVERRIDE_FILE" ]; then
    log_ok "systemd drop-in found: $SYSTEMD_OVERRIDE_FILE"
    echo "  Contents:"
    sed 's/^/    /' "$SYSTEMD_OVERRIDE_FILE"
  else
    log_warn "systemd drop-in NOT found: $SYSTEMD_OVERRIDE_FILE"
    echo "  fprintd will be blocked by systemd device cgroup unless this drop-in is installed."
    echo "  Run: sudo $0 --install-systemd"
  fi

  # 6. Check SELinux Status
  log_header "SELinux Enforcement"
  if command -v getenforce >/dev/null 2>&1; then
    local selinux_mode
    selinux_mode=$(getenforce)
    echo "SELinux Mode: $selinux_mode"
    if [ "$selinux_mode" = "Enforcing" ]; then
      if command -v semodule >/dev/null 2>&1 && semodule -l 2>/dev/null | grep -q "^${SELINUX_POLICY_NAME}\b"; then
        log_ok "SELinux module '${SELINUX_POLICY_NAME}' is installed."
      else
        log_warn "SELinux is Enforcing, but '${SELINUX_POLICY_NAME}' policy is NOT installed."
        echo "  fprintd will be denied by SELinux when opening /dev/fte3600-*."
        echo "  Run: sudo $0 --install-selinux"
      fi
    else
      log_ok "SELinux is $selinux_mode (no blocking)."
    fi
  else
    log_ok "SELinux is not installed on this system."
  fi

  echo ""
  if [ "$missing_pkgs" -eq 1 ]; then
    log_warn "Some dependencies are missing. Install them and run this check again."
  else
    log_ok "Prerequisite inspection complete. You can proceed with installation."
  fi
}

cmd_install_kernel() {
  check_root
  log_header "Installing FTE3600 Kernel Bridge"

  local kdir="/lib/modules/$(uname -r)/build"
  if [ ! -d "$kdir" ]; then
    log_fail "Kernel headers not found at $kdir. Please install kernel-devel or linux-headers first."
    exit 1
  fi

  if command -v dkms >/dev/null 2>&1; then
    log_info "Using DKMS for automated module lifecycle..."
    mkdir -p "$DKMS_DEST"
    cp -f "$KERNEL_SRC/dkms.conf" "$DKMS_DEST/"
    cp -f "$KERNEL_SRC/Makefile" "$DKMS_DEST/"
    cp -f "$KERNEL_SRC/fte3600.c" "$DKMS_DEST/"
    cp -f "$KERNEL_SRC/fte3600-policy.h" "$DKMS_DEST/"
    cp -f "$KERNEL_SRC/fte3600-bridge.h" "$DKMS_DEST/"
    cp -f "$KERNEL_SRC/fte3600-policy-test.c" "$DKMS_DEST/"

    log_info "Registering DKMS package fte3600/${DKMS_VERSION}..."
    local dkms_status
    dkms_status=$(dkms status -m fte3600 -v "$DKMS_VERSION")
    if [ -n "$dkms_status" ]; then
      dkms remove -m fte3600 -v "$DKMS_VERSION" --all
    fi
    dkms add -m fte3600 -v "${DKMS_VERSION}"
    dkms build -m fte3600 -v "${DKMS_VERSION}"
    dkms install -m fte3600 -v "${DKMS_VERSION}" --force
    log_ok "DKMS installation completed."
  else
    log_warn "DKMS not found, performing direct in-tree build and installation..."
    make -C "$KERNEL_SRC" KDIR="$kdir"
    local dest_dir="/lib/modules/$(uname -r)/extra"
    mkdir -p "$dest_dir"
    cp -f "$KERNEL_SRC/fte3600.ko" "$dest_dir/"
    depmod -a
    log_ok "Installed fte3600.ko to $dest_dir"
  fi

  log_info "Attempting to load module..."
  if ! modprobe fte3600; then
    log_fail "Module installation finished, but loading failed. Check the error above and any Secure Boot signing requirements."
    return 1
  fi
  if ! "$REPO_DIR/scripts/fte3600-device-allow.sh" >/dev/null; then
    log_fail "Module installed, but no usable FTE3600 bridge was found. Installation is incomplete."
    return 1
  fi
  log_ok "Kernel bridge is bound and its device node is available."
}

cmd_install_systemd() {
  check_root
  log_header "Configuring systemd fprintd Device Allow"
  "$REPO_DIR/scripts/fte3600-device-allow.sh" --install
  log_ok "systemd sandbox override installed."
}

cmd_install_selinux() {
  check_root
  log_header "Configuring SELinux Policy"
  if ! command -v getenforce >/dev/null 2>&1 || [ "$(getenforce)" = "Disabled" ]; then
    log_info "SELinux is disabled or not present. No policy installation needed."
    return
  fi

  if [ ! -f "$SELINUX_CIL" ]; then
    log_fail "SELinux policy file not found: $SELINUX_CIL"
    exit 1
  fi

  local tool
  for tool in semodule restorecon matchpathcon; do
    if ! command -v "$tool" >/dev/null 2>&1; then
      log_fail "'$tool' utility not found. Install the SELinux policy and labeling tools first."
      return 1
    fi
  done

  log_info "Installing SELinux CIL policy ($SELINUX_CIL)..."
  semodule -i "$SELINUX_CIL"
  relabel_bridge_devices fte3600_device_t
  log_ok "SELinux policy '${SELINUX_POLICY_NAME}' installed successfully."
}

cmd_install_all() {
  check_root
  if ! command -v systemctl >/dev/null 2>&1; then
    log_fail "install-all requires systemd. Use the individual installation commands on other systems."
    return 1
  fi
  cmd_install_kernel
  cmd_install_systemd
  cmd_install_selinux

  log_header "Restarting fprintd Service"
  if ! systemctl restart fprintd.service; then
    log_fail "fprintd failed to restart; installation is incomplete. Inspect 'systemctl status fprintd.service'."
    return 1
  fi
  log_ok "fprintd service restarted."

  echo ""
  log_ok "All FTE3600 prerequisites, kernel bridge, systemd, and SELinux patches applied!"
  echo "Next step: Run 'sudo $0 --status' to inspect the live status."
}

cmd_uninstall() {
  check_root
  log_header "Uninstalling FTE3600 System Integration"

  # Check removal prerequisites before stopping the service or deleting files.
  local dkms_status="" policy_list="" remove_policy=0 selinux_live=0
  local service_state="not-found" restart_service=0 loaded_modules tool
  if command -v dkms >/dev/null 2>&1; then
    dkms_status=$(dkms status -m fte3600 -v "$DKMS_VERSION")
  elif [ -e "$DKMS_DEST" ]; then
    log_fail "DKMS sources are present, but dkms is unavailable. Restore dkms before uninstalling."
    return 1
  fi
  if command -v getenforce >/dev/null 2>&1 && [ "$(getenforce)" != "Disabled" ]; then
    selinux_live=1
  fi
  # The module store persists when SELinux (or the module itself) is disabled.
  # Remove only priority 400, where this helper installs its local policy.
  if command -v semodule >/dev/null 2>&1; then
    policy_list=$(semodule --list-modules=full)
    if printf '%s\n' "$policy_list" | grep -E "^[[:space:]]*400[[:space:]]+${SELINUX_POLICY_NAME}([[:space:]]|$)" >/dev/null; then
      remove_policy=1
    fi
  elif [ "$selinux_live" = 1 ]; then
    log_fail "semodule is required to inspect and remove the SELinux policy."
    return 1
  fi
  if [ "$remove_policy" = 1 ] && [ "$selinux_live" = 1 ]; then
    for tool in restorecon matchpathcon; do
      if ! command -v "$tool" >/dev/null 2>&1; then
        log_fail "'$tool' is required to restore device labels after policy removal."
        return 1
      fi
    done
  fi
  if ! loaded_modules=$(lsmod); then
    log_fail "Cannot inspect loaded modules; uninstall has not changed installed files."
    return 1
  fi

  # Stop the daemon before attempting to release its module reference. Other
  # clients can still hold a descriptor; a failed unload must abort deletion.
  if command -v systemctl >/dev/null 2>&1; then
    service_state=$(systemctl show --property=LoadState --value fprintd.service)
    if [ "$service_state" != "not-found" ]; then
      if systemctl is-active --quiet fprintd.service; then
        restart_service=1
      fi
      systemctl stop fprintd.service
    fi
  fi

  # Refresh after stopping fprintd. A failed query is not evidence of absence.
  if ! loaded_modules=$(lsmod); then
    log_fail "Cannot inspect loaded modules; installed files have been retained."
    return 1
  fi
  if printf '%s\n' "$loaded_modules" | grep '^fte3600 ' >/dev/null; then
    log_info "Unloading fte3600 kernel module..."
    if ! rmmod fte3600; then
      log_fail "Module is still loaded. Close fingerprint clients and retry; installed files have been retained."
      if [ "$restart_service" = 1 ]; then
        systemctl start fprintd.service || log_warn "Could not restore the previously active fprintd service."
      fi
      return 1
    fi
    if ! loaded_modules=$(lsmod); then
      log_fail "Cannot confirm that the module was unloaded; installed files have been retained."
      return 1
    fi
    if printf '%s\n' "$loaded_modules" | grep '^fte3600 ' >/dev/null; then
      log_fail "Module is still present after the unload request; installed files have been retained."
      return 1
    fi
  fi

  # 2. DKMS removal
  if [ -n "$dkms_status" ]; then
    log_info "Removing DKMS module..."
    dkms remove -m fte3600 -v "$DKMS_VERSION" --all
  fi
  rm -rf "$DKMS_DEST"
  rm -f "/lib/modules/$(uname -r)/extra/fte3600.ko"
  depmod -a

  # 3. systemd removal
  log_info "Removing systemd drop-in override..."
  "$REPO_DIR/scripts/fte3600-device-allow.sh" --remove

  # 4. SELinux removal
  if [ "$remove_policy" = 1 ]; then
    log_info "Removing SELinux module '${SELINUX_POLICY_NAME}'..."
    if [ "$selinux_live" = 1 ]; then
      semodule -X 400 -r "$SELINUX_POLICY_NAME"
      relabel_bridge_devices
    else
      semodule -n -X 400 -r "$SELINUX_POLICY_NAME"
    fi
  fi

  # Restore a previously active daemon; do not start one that was stopped.
  if [ "$restart_service" = 1 ]; then
    systemctl start fprintd.service
  fi

  log_ok "Uninstall and cleanup completed."
}

cmd_status() {
  cmd_check
}

show_help() {
  echo -e "${BOLD}FTE3600 Automated Setup & Diagnostic Helper${NC}"
  echo "Usage: $0 [COMMAND]"
  echo ""
  echo "Commands:"
  echo "  check             Run system, prerequisite, and hardware detection checks (default)"
  echo "  install-all       Install kernel module (DKMS), systemd drop-in, and SELinux policy"
  echo "  install-kernel    Install and build the kernel bridge driver"
  echo "  install-systemd   Install the fprintd service device sandbox override"
  echo "  install-selinux   Install the SELinux policy module for /dev/fte3600-*"
  echo "  uninstall         Remove all installed modules, systemd overrides, and policies"
  echo "  status            Alias for check"
  echo "  --help, -h        Show this help message"
  echo ""
}

ACTION="${1:-check}"
case "$ACTION" in
  check|--check)
    cmd_check
    ;;
  install-all|--install-all)
    cmd_install_all
    ;;
  install-kernel|--install-kernel)
    cmd_install_kernel
    ;;
  install-systemd|--install-systemd)
    cmd_install_systemd
    ;;
  install-selinux|--install-selinux)
    cmd_install_selinux
    ;;
  uninstall|--uninstall|remove|--remove)
    cmd_uninstall
    ;;
  status|--status)
    cmd_status
    ;;
  help|--help|-h)
    show_help
    ;;
  *)
    log_fail "Unknown action: $ACTION"
    show_help
    exit 1
    ;;
esac
