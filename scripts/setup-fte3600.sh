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
  if lsmod | grep -q "^fte3600 "; then
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
    dkms remove -m fte3600 -v "${DKMS_VERSION}" --all >/dev/null 2>&1 || true
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
  modprobe fte3600 2>/dev/null || insmod "$KERNEL_SRC/fte3600.ko" 2>/dev/null || true
  if lsmod | grep -q "^fte3600 "; then
    log_ok "Kernel module loaded successfully."
  else
    log_warn "Module built, but could not be loaded immediately (normal if ACPI device is not present or Secure Boot is active)."
  fi
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

  if ! command -v semodule >/dev/null 2>&1; then
    log_fail "'semodule' utility not found. Install policycoreutils."
    exit 1
  fi

  log_info "Installing SELinux CIL policy ($SELINUX_CIL)..."
  semodule -i "$SELINUX_CIL"
  log_ok "SELinux policy '${SELINUX_POLICY_NAME}' installed successfully."
}

cmd_install_all() {
  check_root
  cmd_install_kernel
  cmd_install_systemd
  cmd_install_selinux

  log_header "Restarting fprintd Service"
  if command -v systemctl >/dev/null 2>&1; then
    systemctl restart fprintd 2>/dev/null || true
    log_ok "fprintd service restarted."
  fi

  echo ""
  log_ok "All FTE3600 prerequisites, kernel bridge, systemd, and SELinux patches applied!"
  echo "Next step: Run 'sudo $0 --status' to inspect the live status."
}

cmd_uninstall() {
  check_root
  log_header "Uninstalling FTE3600 System Integration"

  # 1. Unload module
  if lsmod | grep -q "^fte3600 "; then
    log_info "Unloading fte3600 kernel module..."
    rmmod fte3600 2>/dev/null || true
  fi

  # 2. DKMS removal
  if command -v dkms >/dev/null 2>&1; then
    log_info "Removing DKMS module..."
    dkms remove -m fte3600 -v "${DKMS_VERSION}" --all >/dev/null 2>&1 || true
    rm -rf "$DKMS_DEST"
  fi
  rm -f "/lib/modules/$(uname -r)/extra/fte3600.ko"
  depmod -a

  # 3. systemd removal
  log_info "Removing systemd drop-in override..."
  "$REPO_DIR/scripts/fte3600-device-allow.sh" --remove || true

  # 4. SELinux removal
  if command -v semodule >/dev/null 2>&1 && semodule -l 2>/dev/null | grep -q "^${SELINUX_POLICY_NAME}\b"; then
    log_info "Removing SELinux module '${SELINUX_POLICY_NAME}'..."
    semodule -r "${SELINUX_POLICY_NAME}" 2>/dev/null || true
  fi

  # 5. Restart fprintd
  if command -v systemctl >/dev/null 2>&1; then
    systemctl restart fprintd 2>/dev/null || true
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
