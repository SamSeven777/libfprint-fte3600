#!/bin/sh
# SPDX-FileCopyrightText: 2026 FTE3600 Linux contributors
# SPDX-License-Identifier: LGPL-2.1-or-later

set -eu

helper=${1:?missing helper path}
unit=${2:?missing systemd unit path}
test_root=$(mktemp -d "${TMPDIR:-/tmp}/fte3600-power-test.XXXXXX")
trap 'rm -rf -- "$test_root"' EXIT HUP INT TERM

fail()
{
  printf 'test-fte3600-a1-spi-power: %s\n' "$*" >&2
  exit 1
}

assert_file()
{
  expected=$1
  path=$2
  actual=$(cat "$path")
  [ "$actual" = "$expected" ] ||
    fail "$path: expected '$expected', got '$actual'"
}

grep -Fqx \
  'ConditionFirmware=smbios-field(sys_vendor = "ONE-NETBOOK TECHNOLOGY CO., LTD.")' \
  "$unit" || fail 'systemd unit does not quote the DMI vendor value'

sys=$test_root/sys
pci=$sys/devices/pci0000:00/0000:00:1e.3
platform=$pci/pxa2xx-spi.4
spi=$platform/spi_master/spi0/spi-FTE3600:00
acpi=$sys/bus/acpi/devices/FTE3600:00
pxa_driver=$sys/bus/platform/drivers/pxa2xx-spi
spidev_driver=$sys/bus/spi/drivers/spidev

touch "$test_root/.fte3600-test-root"
mkdir -p \
  "$sys/class/dmi/id" \
  "$pci/power" \
  "$platform/power" \
  "$spi" \
  "$acpi" \
  "$pxa_driver" \
  "$spidev_driver" \
  "$sys/bus/pci/devices" \
  "$sys/bus/pci/drivers/intel-lpss" \
  "$sys/bus/platform/devices" \
  "$sys/bus/platform" \
  "$sys/bus/spi" \
  "$sys/bus/spi/drivers/future-fte3600"

printf '%s\n' 'ONE-NETBOOK TECHNOLOGY CO., LTD.' > "$sys/class/dmi/id/sys_vendor"
printf '%s\n' A1 > "$sys/class/dmi/id/product_name"
printf '%s\n' FTE3600 > "$acpi/hid"
printf '%s\n' 15 > "$acpi/status"
printf '%s\n' 'acpi:FTE3600:' > "$acpi/modalias"
printf '%s\n' '\_SB_.PCI0.SPI1.FPRT' > "$acpi/path"
printf '%s\n' 0x8086 > "$pci/vendor"
printf '%s\n' 0x9d2a > "$pci/device"
printf '%s\n' 0x118000 > "$pci/class"
printf '%s\n' 'acpi:FTE3600:' > "$spi/modalias"
printf '%s\n' auto > "$pci/power/control"
printf '%s\n' auto > "$platform/power/control"
: > "$pxa_driver/unbind"
: > "$pxa_driver/bind"

ln -s "$pci" "$sys/bus/pci/devices/0000:00:1e.3"
ln -s "$sys/bus/pci/drivers/intel-lpss" "$pci/driver"
ln -s "$platform" "$sys/bus/platform/devices/pxa2xx-spi.4"
ln -s "$pxa_driver" "$platform/driver"
ln -s "$sys/bus/platform" "$platform/subsystem"
ln -s "$spi" "$acpi/physical_node"
ln -s "$spidev_driver" "$spi/driver"
ln -s "$sys/bus/spi" "$spi/subsystem"

FTE3600_TEST_ROOT=$test_root "$helper" start
assert_file on "$pci/power/control"
assert_file on "$platform/power/control"
assert_file auto "$test_root/run/libfprint-fte3600/parent-control"
assert_file auto "$test_root/run/libfprint-fte3600/child-control"
[ "$(stat -c %a "$test_root/run/libfprint-fte3600")" = 700 ] ||
  fail 'state directory mode is not 0700'
[ "$(stat -c %a "$test_root/run/libfprint-fte3600/parent-control")" = 600 ] ||
  fail 'saved parent policy mode is not 0600'
assert_file pxa2xx-spi.4 "$pxa_driver/unbind"
assert_file pxa2xx-spi.4 "$pxa_driver/bind"

# Model an interrupted start that left the platform device unbound. The
# ExecStopPost path must rebind it before restoring the saved policies.
rm "$platform/driver"
: > "$pxa_driver/bind"
FTE3600_TEST_ROOT=$test_root "$helper" stop
assert_file pxa2xx-spi.4 "$pxa_driver/bind"
[ "$(readlink -f "$platform/driver")" = "$(readlink -f "$pxa_driver")" ] ||
  fail 'stop did not recover an unbound pxa2xx controller'
assert_file auto "$pci/power/control"
assert_file auto "$platform/power/control"

# A mixed administrator policy survives a repeated recovery start.
rm -rf -- "$test_root/run/libfprint-fte3600"
printf '%s\n' on > "$pci/power/control"
printf '%s\n' auto > "$platform/power/control"
: > "$pxa_driver/unbind"
: > "$pxa_driver/bind"
FTE3600_TEST_ROOT=$test_root "$helper" start
assert_file on "$pci/power/control"
assert_file on "$platform/power/control"
assert_file pxa2xx-spi.4 "$pxa_driver/unbind"
assert_file pxa2xx-spi.4 "$pxa_driver/bind"
: > "$pxa_driver/unbind"
: > "$pxa_driver/bind"
FTE3600_TEST_ROOT=$test_root "$helper" start
assert_file pxa2xx-spi.4 "$pxa_driver/unbind"
assert_file pxa2xx-spi.4 "$pxa_driver/bind"
FTE3600_TEST_ROOT=$test_root "$helper" stop
assert_file on "$pci/power/control"
assert_file auto "$platform/power/control"
[ ! -e "$test_root/run/libfprint-fte3600/parent-control" ] ||
  fail 'successful stop left stale saved policy state'
FTE3600_TEST_ROOT=$test_root "$helper" stop

# A future native kernel driver must take precedence over this workaround.
rm -rf -- "$test_root/run/libfprint-fte3600"
rm "$spi/driver"
ln -s "$sys/bus/spi/drivers/future-fte3600" "$spi/driver"
printf '%s\n' auto > "$pci/power/control"
printf '%s\n' auto > "$platform/power/control"
FTE3600_TEST_ROOT=$test_root "$helper" start
assert_file auto "$pci/power/control"
assert_file auto "$platform/power/control"
[ ! -e "$test_root/run/libfprint-fte3600/parent-control" ] ||
  fail 'native-driver path unexpectedly saved or changed power policy'

# A near-match DMI profile must not write any controller state.
rm "$spi/driver"
ln -s "$spidev_driver" "$spi/driver"
printf '%s\n' A2 > "$sys/class/dmi/id/product_name"
FTE3600_TEST_ROOT=$test_root "$helper" start
assert_file auto "$pci/power/control"
assert_file auto "$platform/power/control"

# Once DMI matches exactly, a changed ACPI/topology contract is a hard error.
printf '%s\n' A1 > "$sys/class/dmi/id/product_name"
printf '%s\n' '\_SB_.PCI0.SPI2.FPRT' > "$acpi/path"
if FTE3600_TEST_ROOT=$test_root "$helper" start >/dev/null 2>&1; then
  fail 'an exact-A1 device with the wrong ACPI path was accepted'
fi
assert_file auto "$pci/power/control"
assert_file auto "$platform/power/control"
printf '%s\n' '\_SB_.PCI0.SPI1.FPRT' > "$acpi/path"

# Preflight must validate both power controls before changing either one.
mv "$platform/power/control" "$platform/power/control.missing"
if FTE3600_TEST_ROOT=$test_root "$helper" start >/dev/null 2>&1; then
  fail 'a missing child power control was accepted'
fi
assert_file auto "$pci/power/control"
mv "$platform/power/control.missing" "$platform/power/control"

# Count every SPI child, including conventional names such as spi0.1.
extra_spi=$platform/spi_master/spi0/spi0.1
mkdir -p "$extra_spi"
ln -s "$sys/bus/spi" "$extra_spi/subsystem"
: > "$pxa_driver/unbind"
: > "$pxa_driver/bind"
if FTE3600_TEST_ROOT=$test_root "$helper" start >/dev/null 2>&1; then
  fail 'a controller with a second SPI child was accepted'
fi
assert_file auto "$pci/power/control"
assert_file auto "$platform/power/control"
assert_file '' "$pxa_driver/unbind"
assert_file '' "$pxa_driver/bind"
rm -rf -- "$extra_spi"

# Saved state is data, never shell input, and accepts only auto/on.
mkdir -p "$test_root/run/libfprint-fte3600"
printf '%s\n' '$(touch should-not-exist)' > \
  "$test_root/run/libfprint-fte3600/parent-control"
printf '%s\n' auto > "$test_root/run/libfprint-fte3600/child-control"
if FTE3600_TEST_ROOT=$test_root "$helper" start >/dev/null 2>&1; then
  fail 'an invalid saved power policy was accepted'
fi
if FTE3600_TEST_ROOT=$test_root "$helper" stop >/dev/null 2>&1; then
  fail 'stop discarded an invalid saved power policy'
fi
[ ! -e "$test_root/should-not-exist" ] || fail 'saved state was evaluated as shell'
assert_file auto "$pci/power/control"
assert_file auto "$platform/power/control"

# Test mode itself is fail-closed and cannot point at an unmarked directory.
unmarked=$test_root/unmarked
mkdir -p "$unmarked"
if FTE3600_TEST_ROOT=$unmarked "$helper" start >/dev/null 2>&1; then
  fail 'an unmarked FTE3600_TEST_ROOT was accepted'
fi
