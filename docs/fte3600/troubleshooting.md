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

## The first open works, then SPI reads become all-zero or `0x95`

This state was reproduced on the One-Netbook A1 when either the Intel LPSS
parent or its pxa2xx SPI child was allowed to runtime-suspend. Confirm that the
DMI-gated workaround was installed and completed before `fprintd`:

```sh
systemctl status fte3600-a1-spi-power.service --no-pager
cat /sys/devices/pci0000:00/0000:00:1e.3/power/control
cat /sys/devices/pci0000:00/0000:00:1e.3/pxa2xx-spi.4/power/control
```

On the verified A1 both controls should report `on` after the service starts.
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
