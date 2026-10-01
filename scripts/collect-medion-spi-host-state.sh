#!/usr/bin/env bash
# SPDX-License-Identifier: LGPL-2.1-or-later
# Bounded, read-only host metadata. Never open a SPI/GPIO device or wake it.
set -uo pipefail

fte_usage() {
  local fte_help_line
  while IFS= read -r fte_help_line; do
    printf '%s\n' "$fte_help_line"
  done <<'USAGE'
Usage: bash scripts/collect-medion-spi-host-state.sh [--help]

Print a local, read-only snapshot for MEDION E3224 SPI investigation.
No root requirement: unavailable or unreadable information is reported as a gap.
An already mounted and accessible debugfs can provide pinctrl metadata; this
script never mounts it, elevates privileges, or changes permissions.

Only selected DMI, FTE3600/INT3453 ACPI, SPI/PCI/controller sysfs attributes and
existing pinctrl pins, pinmux-pins and gpio-ranges text are read. No serial
numbers, templates, journals, firmware, GPIO line values or device nodes are
read. No service, driver, runtime-PM or GPIO state is changed; no ACPI method
is explicitly evaluated. Output is stdout; nothing is uploaded or saved here.

The snapshot may show a state after autosuspend, not the state during a transfer.
MUX UNCLAIMED means no recorded mux owner, not necessarily an unconfigured pin.
This cannot establish sensor power, physical signal levels or earlier activity.
Missing evidence does not make collection fail. Exit 0: collection/help complete;
1: a required host utility is missing; 2: invalid arguments.
USAGE
}

if (( $# )); then
  if (( $# == 1 )) && [[ $1 == --help || $1 == -h ]]; then
    fte_usage
    exit 0
  fi
  printf 'Unexpected arguments. Use --help.\n' >&2
  exit 2
fi

for fte_command in readlink awk uname; do
  if ! command -v "$fte_command" >/dev/null 2>&1; then
    printf 'Missing dependency: %s\n' "$fte_command" >&2
    exit 1
  fi
done

# Fixed production root. Regression tests redirect only a temporary script copy.
# No environment variable or command-line argument can broaden the read scope.
fte_sys_root=/sys
shopt -s nullglob

fte_resolve_sys() {
  local fte_resolved
  fte_resolved=$(readlink -f -- "$1" 2>/dev/null) || return 1
  case $fte_resolved in
    "$fte_sys_root"/*)
      [[ -e $fte_resolved ]] || return 1
      printf '%s\n' "$fte_resolved"
      ;;
    *) return 1 ;;
  esac
}

fte_display_path() {
  printf '/sys%s' "${1#"$fte_sys_root"}"
}

fte_field() {
  local fte_label=$1 fte_path fte_value=
  if ! fte_path=$(fte_resolve_sys "$2") || [[ ! -f $fte_path || ! -r $fte_path ]]; then
    printf '%s: unavailable (absent, unreadable or outside sysfs)\n' "$fte_label"
    return
  fi
  if IFS= read -r fte_value < "$fte_path" 2>/dev/null || [[ -n $fte_value ]]; then
    printf '%s: %s\n' "$fte_label" "${fte_value:-(empty)}"
  else
    printf '%s: unavailable (empty or read failed)\n' "$fte_label"
  fi
}

fte_link() {
  local fte_target
  if [[ -L $2 ]] && fte_target=$(fte_resolve_sys "$2"); then
    printf '%s: ' "$1"
    fte_display_path "$fte_target"
    printf '\n'
  else
    printf '%s: unavailable (no resolvable sysfs link)\n' "$1"
  fi
}

fte_node() {
  local fte_node_path fte_attribute
  printf '\n[%s]\n' "$1"
  if ! fte_node_path=$(fte_resolve_sys "$2") || [[ ! -d $fte_node_path ]]; then
    printf 'Node: unavailable\n'
    return
  fi
  printf 'Node: '
  fte_display_path "$fte_node_path"
  printf '\n'
  fte_link Driver "$fte_node_path/driver"
  for fte_attribute in modalias driver_override power/control power/runtime_status \
      power/runtime_enabled power/runtime_usage power/runtime_active_time \
      power/runtime_suspended_time; do
    fte_field "$fte_attribute" "$fte_node_path/$fte_attribute"
  done
}

fte_pinctrl_file() {
  local fte_kind=$1 fte_path fte_output
  printf '\n  %s:\n' "$fte_kind"
  if ! fte_path=$(fte_resolve_sys "$2") || [[ ! -f $fte_path || ! -r $fte_path ]]; then
    printf '  unavailable (debugfs not present, not accessible, or file absent)\n'
    return
  fi
  if [[ $fte_kind == gpio-ranges ]]; then
    # Mapping metadata only; never inspect debugfs/gpio or a GPIO value file.
    if ! fte_output=$(LC_ALL=C awk 'NR <= 64 { print }
        END { if (NR == 0) print "unavailable (empty gpio-ranges)";
              if (NR > 64) print "[truncated after 64 mapping lines]" }' "$fte_path" 2>/dev/null); then
      printf '  unavailable (read failed)\n'
      return
    fi
  else
    # Keep pin numbers exact: e.g. 108 must never be mistaken for pin 8.
    if ! fte_output=$(LC_ALL=C awk -v kind="$fte_kind" '
        /^[[:space:]]*pin[[:space:]]+(8|9|10|11|12|13)([[:space:]]|:)/ {
          found++; if (found <= 24) print;
          if ($0 ~ /LPSS_SPI_2/) named = 1;
        }
        END {
          if (!found) print "unavailable (no entries for pins 8..13)";
          if (found > 24) print "[truncated after 24 selected pin lines]";
          if (found && kind == "pins" && !named)
            print "NOTE: selected names do not confirm LPSS_SPI_2; do not assume the mapping.";
        }' "$fte_path" 2>/dev/null); then
      printf '  unavailable (read failed)\n'
      return
    fi
  fi
  printf '%s\n' "$fte_output"
}

printf 'MEDION SPI host-state snapshot v1 (read-only; local output)\n'
printf 'Kernel: '
uname -srm || printf 'unavailable\n'
printf '\n[DMI: non-unique platform fields only]\n'
for fte_attribute in sys_vendor product_name product_version board_vendor \
    board_name bios_vendor bios_version bios_date; do
  fte_field "$fte_attribute" "$fte_sys_root/class/dmi/id/$fte_attribute"
done
printf 'Expected target: MEDION / E3224; compare the reported fields, do not infer a match.\n'

fte_spi_nodes=("$fte_sys_root"/bus/spi/devices/spi-FTE3600:*)
if (( ${#fte_spi_nodes[@]} == 0 )); then
  printf '\n[FTE3600 SPI]\nNode: unavailable (no spi-FTE3600:* entry)\n'
fi
for fte_spi_node in "${fte_spi_nodes[@]}"; do
  fte_node "FTE3600 SPI ${fte_spi_node##*/}" "$fte_spi_node"
  fte_link 'ACPI firmware node' "$fte_spi_node/firmware_node"
done

fte_acpi_nodes=("$fte_sys_root"/bus/acpi/devices/FTE3600:*)
if (( ${#fte_acpi_nodes[@]} == 0 )); then
  printf '\n[FTE3600 ACPI]\nNode: unavailable\n'
fi
for fte_acpi_node in "${fte_acpi_nodes[@]}"; do
  printf '\n[FTE3600 ACPI %s]\n' "${fte_acpi_node##*/}"
  for fte_attribute in hid uid path status modalias; do
    fte_field "$fte_attribute" "$fte_acpi_node/$fte_attribute"
  done
  fte_link 'Physical node' "$fte_acpi_node/physical_node"
done

fte_pci_path="$fte_sys_root/bus/pci/devices/0000:00:19.0"
fte_node 'PCI 0000:00:19.0' "$fte_pci_path"
for fte_attribute in vendor device subsystem_vendor subsystem_device class revision power_state; do
  fte_field "$fte_attribute" "$fte_pci_path/$fte_attribute"
done
fte_pxa_found=0
if fte_pci_real=$(fte_resolve_sys "$fte_pci_path") && [[ -d $fte_pci_real ]]; then
  for fte_pxa_node in "$fte_pci_real"/pxa2xx-spi.*; do
    [[ -d $fte_pxa_node ]] || continue
    fte_pxa_found=1
    fte_node "PXA controller ${fte_pxa_node##*/}" "$fte_pxa_node"
    for fte_master in "$fte_pxa_node"/spi_master/spi*; do
      [[ -d $fte_master ]] || continue
      fte_node "SPI master ${fte_master##*/}" "$fte_master"
    done
  done
fi
if (( ! fte_pxa_found )); then
  printf '\n[PXA controller]\nNode: unavailable (no pxa2xx-spi.* child of PCI 0000:00:19.0)\n'
fi

printf '\n[Pinctrl: INT3453 UID 2; expected LPSS_SPI_2 pins 8..13]\n'
fte_uid2_found=0
for fte_gpio_acpi in "$fte_sys_root"/bus/acpi/devices/INT3453:*; do
  fte_field "${fte_gpio_acpi##*/} uid" "$fte_gpio_acpi/uid"
  fte_uid=
  if ! fte_uid_path=$(fte_resolve_sys "$fte_gpio_acpi/uid") || [[ ! -f $fte_uid_path || ! -r $fte_uid_path ]]; then
    continue
  fi
  IFS= read -r fte_uid < "$fte_uid_path" 2>/dev/null || true
  [[ $fte_uid == 2 ]] || continue
  fte_uid2_found=1
  fte_link 'UID 2 physical node' "$fte_gpio_acpi/physical_node"
  if ! fte_gpio_physical=$(fte_resolve_sys "$fte_gpio_acpi/physical_node") || [[ ! -d $fte_gpio_physical ]]; then
    printf 'Pinctrl: unavailable (cannot map UID 2 to a physical controller)\n'
    continue
  fi
  fte_debug_path="$fte_sys_root/kernel/debug/pinctrl/${fte_gpio_physical##*/}"
  printf 'Selected pinctrl directory: '
  fte_display_path "$fte_debug_path"
  printf '\n'
  for fte_debug_kind in pins pinmux-pins gpio-ranges; do
    fte_pinctrl_file "$fte_debug_kind" "$fte_debug_path/$fte_debug_kind"
  done
done
if (( ! fte_uid2_found )); then
  printf 'Pinctrl: unavailable (no readable INT3453 UID 2; not guessing another controller)\n'
fi

printf '\n[Interpretation limits]\n'
printf '%s\n' \
  'This is a non-atomic snapshot; the state may already be after autosuspend.' \
  'No device was intentionally woken or kept active; this is not a transfer-time capture.' \
  'MUX UNCLAIMED does not mean the pin is unconfigured; firmware may have configured it.' \
  'Pinctrl text is kernel-reported metadata/configuration, not an electrical measurement.' \
  'Missing debugfs/permissions/entries are evidence gaps, not evidence of a hardware fault.' \
  'No SPI/GPIO device node or GPIO line-value API was used; no firmware or ACPI execution requested.' \
  'Collection complete; no host state was intentionally changed.'
exit 0
