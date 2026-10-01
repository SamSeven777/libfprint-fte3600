#!/usr/bin/env bash
# SPDX-License-Identifier: LGPL-2.1-or-later
# Black-box fixtures only: no root, host sysfs, debugfs, GPIO or SPI required.
set -euo pipefail

fte_test_repo=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
fte_test_root=$(mktemp -d /tmp/fte3600-host-state-regression.XXXXXX)
fte_test_cleanup() {
  local fte_status=$?
  trap - EXIT
  # This exact private directory was created by mktemp, not supplied by a user.
  rm -rf -- "$fte_test_root"
  exit "$fte_status"
}
trap fte_test_cleanup EXIT
trap 'exit 130' INT
trap 'exit 143' TERM

fte_test_bash=$(command -v bash)
fte_test_real_awk=$(command -v awk)
mkdir -p "$fte_test_root/bin"
cat > "$fte_test_root/bin/forbidden" <<'MOCK'
#!/usr/bin/env bash
printf 'Forbidden command: %s %s\n' "${0##*/}" "$*" >> "$FTE_TEST_EVENTS"
exit 99
MOCK
chmod +x "$fte_test_root/bin/forbidden"
for fte_command in mount umount sudo systemctl modprobe rmmod gpioget gpioset \
    gpioinfo devmem setpci udevadm dd tee timeout chmod chown chgrp install \
    cp mv rm mkdir touch ln python python3 perl sh cc gcc; do
  ln -s forbidden "$fte_test_root/bin/$fte_command"
done
cat > "$fte_test_root/bin/awk" <<'MOCK'
#!/usr/bin/env bash
if [[ ${FTE_TEST_FAIL_AWK:-0} == 1 ]]; then
  exit 13
fi
exec "$FTE_TEST_REAL_AWK" "$@"
MOCK
chmod +x "$fte_test_root/bin/awk"
export FTE_TEST_REAL_AWK="$fte_test_real_awk"

fte_test_number=0
fte_test_new() {
  fte_test_number=$((fte_test_number + 1))
  fte_case="$fte_test_root/case-$fte_test_number"
  fte_sys="$fte_case/sys"
  fte_pci="$fte_sys/devices/pci0000:00/0000:00:19.0"
  fte_pxa="$fte_pci/pxa2xx-spi.12"
  fte_spi="$fte_pxa/spi_master/spi1/spi-FTE3600:00"
  fte_gpio="$fte_sys/devices/platform/INT3453:00"
  fte_debug="$fte_sys/kernel/debug/pinctrl/INT3453:00"
  mkdir -p "$fte_sys/class/dmi/id" "$fte_sys/bus/spi/devices" \
    "$fte_sys/bus/spi/drivers/spidev" "$fte_sys/bus/pci/devices" \
    "$fte_sys/bus/acpi/devices/FTE3600:00" \
    "$fte_sys/bus/acpi/devices/INT3453:00" \
    "$fte_sys/bus/acpi/devices/INT3453:01" \
    "$fte_sys/kernel/debug/pinctrl/INT3453:01" \
    "$fte_spi/power" "$fte_pxa/power" "$fte_pci/power" \
    "$fte_gpio" "$fte_debug"
  printf 'MEDION\n' > "$fte_sys/class/dmi/id/sys_vendor"
  printf 'E3224\n' > "$fte_sys/class/dmi/id/product_name"
  printf 'YS13G\n' > "$fte_sys/class/dmi/id/board_name"
  printf 'SERIAL-MUST-NOT-APPEAR\n' > "$fte_sys/class/dmi/id/product_serial"
  printf 'UUID-MUST-NOT-APPEAR\n' > "$fte_sys/class/dmi/id/product_uuid"
  printf 'acpi:FTE3600:FTE3600:\n' > "$fte_spi/modalias"
  printf 'auto\n' > "$fte_spi/power/control"
  printf 'suspended\n' > "$fte_spi/power/runtime_status"
  printf 'active\n' > "$fte_pxa/power/runtime_status"
  printf 'active\n' > "$fte_pci/power/runtime_status"
  printf 'D0\n' > "$fte_pci/power_state"
  printf 'FTE3600\n' > "$fte_sys/bus/acpi/devices/FTE3600:00/hid"
  printf '\\_SB_.PCI0.SPI1.FP05\n' > "$fte_sys/bus/acpi/devices/FTE3600:00/path"
  printf '15\n' > "$fte_sys/bus/acpi/devices/FTE3600:00/status"
  printf '2\n' > "$fte_sys/bus/acpi/devices/INT3453:00/uid"
  printf '1\n' > "$fte_sys/bus/acpi/devices/INT3453:01/uid"
  ln -s "$fte_spi" "$fte_sys/bus/spi/devices/spi-FTE3600:00"
  ln -s "$fte_pci" "$fte_sys/bus/pci/devices/0000:00:19.0"
  ln -s "$fte_sys/bus/spi/drivers/spidev" "$fte_spi/driver"
  ln -s "$fte_sys/bus/acpi/devices/FTE3600:00" "$fte_spi/firmware_node"
  ln -s "$fte_spi" "$fte_sys/bus/acpi/devices/FTE3600:00/physical_node"
  ln -s "$fte_gpio" "$fte_sys/bus/acpi/devices/INT3453:00/physical_node"
  cat > "$fte_debug/pins" <<'PINS'
registered pins: 48
pin 7 (OTHER-PIN-SECRET)
pin 8 (LPSS_SPI_2_CLK)
pin 9 (LPSS_SPI_2_FS0)
pin 10 (LPSS_SPI_2_FS1)
pin 11 (LPSS_SPI_2_RXD)
pin 12 (LPSS_SPI_2_TXD)
pin 13 (LPSS_SPI_2_IO2)
pin 108 (OUT-OF-RANGE-SECRET)
PINS
  cat > "$fte_debug/pinmux-pins" <<'PINS'
pin 8 (LPSS_SPI_2_CLK): (MUX UNCLAIMED) (GPIO UNCLAIMED)
pin 9 (LPSS_SPI_2_FS0): device spi1 function spi2 group spi2_grp
pin 108 (OUT-OF-RANGE-SECRET): device other
PINS
  printf '0: INT3453:00 GPIOS [440 - 487] PINS [0 - 47]\n' > "$fte_debug/gpio-ranges"
  printf 'pin 8 (WRONG-UID-SECRET)\n' > "$fte_sys/kernel/debug/pinctrl/INT3453:01/pins"
  printf 'GPIO-VALUES-MUST-NOT-APPEAR\n' > "$fte_sys/kernel/debug/gpio"
  : > "$fte_case/events"
  # The only redirection is the fixed sysfs root in an isolated script copy.
  sed "s@^fte_sys_root=/sys\$@fte_sys_root='$fte_sys'@" \
    "$fte_test_repo/scripts/collect-medion-spi-host-state.sh" > "$fte_case/collector.sh"
}

fte_test_snapshot() {
  (cd "$fte_sys" && find . -type f -exec sha256sum {} + | LC_ALL=C sort)
  (cd "$fte_sys" && find . -printf '%y %m %p -> %l\n' | LC_ALL=C sort)
}

fte_test_run() {
  local fte_expected=$1
  shift
  fte_test_snapshot > "$fte_case/before"
  if PATH="${FTE_TEST_PATH_OVERRIDE:-$fte_test_root/bin:$PATH}" FTE_TEST_EVENTS="$fte_case/events" \
      "$fte_test_bash" "$fte_case/collector.sh" "$@" > "$fte_case/output" 2>&1; then
    fte_actual=0
  else
    fte_actual=$?
  fi
  if [[ $fte_actual != "$fte_expected" ]]; then
    cat "$fte_case/output" >&2
    printf 'Expected exit %s; got %s\n' "$fte_expected" "$fte_actual" >&2
    exit 1
  fi
  [[ ! -s $fte_case/events ]] || { cat "$fte_case/events" >&2; exit 1; }
  fte_test_snapshot > "$fte_case/after"
  cmp "$fte_case/before" "$fte_case/after"
}

fte_test_has() { grep -Fq -- "$1" "$fte_case/output"; }
fte_test_lacks() { ! grep -Fq -- "$1" "$fte_case/output"; }
fte_test_ok() { printf 'ok %s - %s\n' "$fte_test_number" "$1"; }

fte_test_new
fte_test_run 0
fte_test_has 'product_name: E3224'
fte_test_has 'modalias: acpi:FTE3600:FTE3600:'
fte_test_has 'Driver: /sys/bus/spi/drivers/spidev'
fte_test_has 'power/runtime_status: suspended'
fte_test_has '[PXA controller pxa2xx-spi.12]'
fte_test_has '[SPI master spi1]'
fte_test_has 'power_state: D0'
fte_test_has 'pin 8 (LPSS_SPI_2_CLK)'
fte_test_has 'pin 13 (LPSS_SPI_2_IO2)'
fte_test_has 'PINS [0 - 47]'
fte_test_has 'after autosuspend'
fte_test_has 'MUX UNCLAIMED does not mean the pin is unconfigured'
for fte_secret in SERIAL UUID OTHER-PIN OUT-OF-RANGE WRONG-UID GPIO-VALUES; do
  fte_test_lacks "$fte_secret-"
done
fte_test_ok 'bounded topology, UID 2 selection, privacy and interpretation limits'

fte_test_new
rm -rf -- "$fte_sys/kernel/debug"
fte_test_run 0
fte_test_has 'debugfs not present, not accessible, or file absent'
fte_test_has 'Collection complete'
fte_test_ok 'missing debugfs remains a reported gap without mounting'

fte_test_new
rm -- "$fte_sys/bus/acpi/devices/INT3453:00/uid"
fte_test_run 0
fte_test_has 'no readable INT3453 UID 2; not guessing another controller'
fte_test_lacks 'WRONG-UID-SECRET'
fte_test_ok 'missing UID never falls back to another GPIO controller'

fte_test_new
rm -- "$fte_sys/bus/acpi/devices/INT3453:00/physical_node"
fte_test_run 0
fte_test_has 'cannot map UID 2 to a physical controller'
fte_test_ok 'missing physical link does not guess from the ACPI instance name'

fte_test_new
rm -- "$fte_spi/power/runtime_status"
fte_test_run 0
fte_test_has 'power/runtime_status: unavailable'
fte_test_has 'pin 8 (LPSS_SPI_2_CLK)'
fte_test_ok 'missing PM attributes do not prevent other collection'

fte_test_new
rm -- "$fte_sys/bus/spi/devices/spi-FTE3600:00" "$fte_sys/bus/pci/devices/0000:00:19.0"
fte_test_run 0
fte_test_has 'no spi-FTE3600:* entry'
fte_test_has 'no pxa2xx-spi.* child'
fte_test_ok 'absent SPI and PCI topology is explicit'

fte_test_new
printf 'pin 108 (OUT-OF-RANGE-SECRET)\n' > "$fte_debug/pins"
fte_test_run 0
fte_test_has 'no entries for pins 8..13'
fte_test_lacks 'OUT-OF-RANGE-SECRET'
fte_test_ok 'pin-number matching excludes 108'

fte_test_new
printf 'pin 8 (UNEXPECTED-NAME)\n' > "$fte_debug/pins"
fte_test_run 0
fte_test_has 'selected names do not confirm LPSS_SPI_2'
fte_test_ok 'unconfirmed pin names are not represented as a verified SPI mapping'

fte_test_new
printf 'pin 8 (OUTSIDE-SYSFS-SECRET)\n' > "$fte_case/outside"
rm -- "$fte_debug/pins"
ln -s "$fte_case/outside" "$fte_debug/pins"
fte_test_run 0
fte_test_lacks 'OUTSIDE-SYSFS-SECRET'
fte_test_has 'debugfs not present, not accessible, or file absent'
fte_test_ok 'resolved files outside fixed sysfs root are not read'

fte_test_new
export FTE_TEST_FAIL_AWK=1
fte_test_run 0
unset FTE_TEST_FAIL_AWK
fte_test_has 'unavailable (read failed)'
fte_test_has 'Collection complete'
fte_test_ok 'debugfs read errors preserve partial evidence'

fte_test_new
fte_test_run 0 --help
fte_test_has 'Usage:'
fte_test_lacks 'Kernel:'
fte_test_ok 'help does not collect host state'

fte_test_new
fte_test_run 2 --run
fte_test_has 'Unexpected arguments'
fte_test_lacks 'Kernel:'
fte_test_ok 'there is no active-operation option'

fte_test_new
fte_test_run 2 --help --run
fte_test_ok 'extra arguments are rejected'

fte_test_new
mkdir -p "$fte_sys/devices/platform/INT3453:07"
rm -- "$fte_sys/bus/acpi/devices/INT3453:00/physical_node"
ln -s "$fte_sys/devices/platform/INT3453:07" "$fte_sys/bus/acpi/devices/INT3453:00/physical_node"
mv -- "$fte_debug" "$fte_sys/kernel/debug/pinctrl/INT3453:07"
fte_test_run 0
fte_test_has 'Selected pinctrl directory: /sys/kernel/debug/pinctrl/INT3453:07'
fte_test_has 'pin 8 (LPSS_SPI_2_CLK)'
fte_test_ok 'pinctrl selection follows the physical-node name, not ACPI instance numbering'

fte_test_new
rm -rf -- "$fte_sys"
mkdir -p "$fte_sys"
fte_test_run 0
fte_test_has 'no spi-FTE3600:* entry'
fte_test_has 'no readable INT3453 UID 2'
fte_test_has 'Collection complete'
fte_test_ok 'an entirely missing platform yields bounded explicit gaps'

fte_test_new
mkdir -p "$fte_case/minimal-bin"
ln -s "$(command -v readlink)" "$fte_case/minimal-bin/readlink"
ln -s "$(command -v uname)" "$fte_case/minimal-bin/uname"
export FTE_TEST_PATH_OVERRIDE="$fte_case/minimal-bin"
fte_test_run 1
unset FTE_TEST_PATH_OVERRIDE
fte_test_has 'Missing dependency: awk'
fte_test_lacks 'Kernel:'
fte_test_ok 'missing required utility fails before collecting'

printf '1..%s\n' "$fte_test_number"
