# Troubleshooting

## The device is not listed

Confirm the exact hardware profile and device nodes:

```sh
cat /sys/class/dmi/id/sys_vendor
cat /sys/class/dmi/id/product_name
grep -H . /sys/bus/acpi/devices/FTE3600:*/hid
ls -l /dev/spidev* /dev/gpiochip*
fprintd-list "$USER"
```

An unknown DMI profile is rejected intentionally. Do not bypass that check;
open an issue with the sanitized evidence requested in `CONTRIBUTING.md`.

## A capture or firmware recovery says the SPI message is too large

```sh
cat /sys/module/spidev/parameters/bufsiz
```

The result must be at least 10403 for firmware recovery (5128 for images).
Install `config/modprobe.d/fte3600-spidev.conf` through the package and reboot;
it sets 32768. Each 10,403-byte firmware upload and 5,128-byte full-duplex image
read must retain chip select for its entire transaction.

## Cold boot fails with MCU `00 00`

On the verified A1, reset recovery alone did not restore a running sensor
after cold boot. The driver now attempts one upload of the pinned FT9361
firmware to RAM, followed by the validated reset/startup sequence. The owner
confirmed cold-boot initialization and fingerprint recognition with this fix.

Check that the current driver is installed, the SPI buffer meets the limit
above, and the owner's firmware is installed at
`/usr/lib/firmware/fte3600/ft9361.bin`. Follow the
[firmware installation instructions](install.md#firmware-for-cold-boot-recovery)
for the exact size and SHA256. The repository and Arch package do not include
this file. An all-zero reply by itself does not distinguish missing firmware
from a transport or power failure; the runtime-power checks below still apply.
Do not use the superseded FT9338 unlock sequence for FT9361 recovery.

## fprintd cannot open GPIO

Check the installed service drop-in and restart after fixing the package:

```sh
systemctl cat fprintd.service
systemctl status fprintd.service --no-pager
```

The effective unit must allow `char-gpiochip rw`. Keep the service sandbox in
place and do not grant broad access to all devices merely to bypass a GPIO
configuration error.

If the error reports `Permission denied` on Fedora 43/44 with enforcing
SELinux, check the audit log and current policy contexts:

```sh
sudo ausearch -m AVC,USER_AVC,SELINUX_ERR,USER_SELINUX_ERR \
  -ts recent -c fprintd -i
ls -lZ /dev/gpiochip*
ps -eZ | grep '[f]printd'
```

Only if the denial and commands show source type `fprintd_t` and target type
`gpio_device_t`, install and verify the provided optional local module:

```sh
sudo semodule -i config/selinux/fte3600-gpio.cil
sudo semodule -lfull | grep -F fte3600-gpio
sudo systemctl restart fprintd.service
```

Fedora assigns `gpio_device_t` to every `/dev/gpiochip*` node. The module
therefore permits the listed operations on all GPIO character devices, not
only FTE3600 lines; the systemd device allow-list remains in force. Do not
disable SELinux or install this Fedora-specific module on other policies to
bypass a denial. Arch and Ubuntu do not load it automatically. Remove it with
`sudo semodule -r fte3600-gpio` when uninstalling the driver.

## The first open works, then SPI reads become all-zero or `0x95`

Earlier A1 experiments associated these reads with allowing the Intel LPSS
parent or its pxa2xx SPI child to runtime-suspend. Those experiments predated
the corrected firmware startup path, and the helper also rebinds the SPI
controller. They do not isolate runtime PM as an independent cause. The
service was already active during repeated `00 00` failures, so a successful
service status is not proof that the sensor works.

The existing packages retain this DMI-gated precaution. Inspect its state:

```sh
systemctl status fte3600-a1-spi-power.service --no-pager
cat /sys/devices/pci0000:00/0000:00:1e.3/power/control
cat /sys/devices/pci0000:00/0000:00:1e.3/pxa2xx-spi.4/power/control
```

On the verified A1 both controls should report `on` after the service starts.
Whether this precaution remains necessary with corrected firmware recovery
is unverified: the successful cold-boot test still had the service active.
Removing it requires a controlled new-driver on/auto comparison, followed by
a direct Linux cold boot and real authentication without the helper. Stopping
the service alone is not such a test; the fprintd dependency can start it again.
Do not copy these fixed controller paths to another computer. Reinstall the
package and reboot if the unit is missing; if its topology check fails, attach
the sanitized unit status to a hardware report instead of forcing a rebind.
TLP, powertop autotuning, or another power manager can overwrite these values
later. Disable that conflicting rule, stop `fprintd`, and restart
`fte3600-a1-spi-power.service`; never rebind the controller while a fingerprint
operation is active.

## Enrollment repeatedly asks for another press

Lift the finger completely, pause briefly, and vary position slightly on the
next press. Low contrast, too few features, a duplicate placement, or an
inconsistent finger are deliberately rejected. Do not weaken the matcher to
make enrollment complete.

## Omarchy does not show fingerprint unlock

First confirm `fprintd-verify` succeeds. Then check that
`/etc/pam.d/omarchy-lock-fingerprint` exists and restart only the shell:

```sh
omarchy restart shell
omarchy system lock
```

Do not use the general Omarchy fingerprint setup command with this package.

## Collect a safe log

Start with versions and non-biometric state:

```sh
uname -a
pkg-config --modversion libfprint-2 libgpiod
systemctl status fprintd.service --no-pager
```

Before posting, redact usernames, hostnames, serial numbers, paths, and any
binary dump. Never upload a fingerprint image, template, or raw capture.
